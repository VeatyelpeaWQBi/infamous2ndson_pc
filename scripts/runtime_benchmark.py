"""Measure actual game presentations in an isolated, repeatable saved-game workload."""
import argparse
import csv
from collections import deque
from datetime import datetime, timezone
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

from windows_tools import ROOT, require_windows, tool_environment
from game_profiles import native_environment, select_profile
from performance_sim import check_game_stopped

BASE = ROOT / 'out/runtime-benchmark'
LIMIT = 512 * 1024 * 1024
OWNER = 'infamous-runtime-benchmark-v1'


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save_json(path, value):
    path = Path(path)
    pending = path.with_name(path.name + '.next')
    pending.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    pending.replace(path)


def load_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def files(root):
    root = Path(root).resolve()
    result = {}
    if not root.is_dir():
        raise ValueError(f'Missing existing data directory: {root}')
    for path in root.rglob('*'):
        if path.is_symlink() or path.is_junction():
            raise ValueError(f'Reparse points are not accepted in benchmark inputs: {path}')
        if path.is_file():
            if not path.resolve().is_relative_to(root):
                raise ValueError('Input path escaped its source directory.')
            result[path.relative_to(root).as_posix()] = path
    return result


def fingerprint(root):
    return {name: digest(path) for name, path in files(root).items()}


def check_budget():
    used=sum(path.stat().st_size for path in files(BASE).values())
    if used>LIMIT:
        raise ValueError(f'Benchmark data budget exceeded: {used} bytes; data preserved for review.')
    return used


def plan():
    game = ROOT / 'patches/CUSA00309'
    profile = select_profile(game)
    inputs = {name: ROOT / 'out/CUSA00309' / name for name in
              ('boot-linked.bin', 'patches.bin', 'content.bin')}
    inputs['executable'] = ROOT / 'out/bb-probe.exe'
    for path in inputs.values():
        if not path.is_file():
            raise ValueError(f'Missing prepared artifact: {path}; use existing build/run entry points first.')
    save = ROOT / 'profiles/CUSA00309/user'
    gpu = ROOT / 'profiles/CUSA00309/gpu'
    save_bytes = sum(p.stat().st_size for p in files(save).values())
    cache_bytes = sum(p.stat().st_size for p in files(gpu).values())
    if not save_bytes or 2 * (save_bytes + cache_bytes) + 160 * 1024 * 1024 > LIMIT:
        raise ValueError('Missing save or source data exceeds the 512 MiB benchmark budget.')
    return {'owner': OWNER, 'game': str(game), 'profile': profile,
            'inputs': {name: str(path) for name, path in inputs.items()},
            'save_source': str(save), 'cache_source': str(gpu),
            'save_bytes': save_bytes, 'cache_bytes': cache_bytes, 'limit_bytes': LIMIT,
            'output': str(BASE), 'scope': 'actual game process; static checkpoint and camera rotation'}


def verify_base():
    expected = (ROOT / 'out/runtime-benchmark').resolve()
    if BASE.resolve() != expected or BASE.is_symlink() or BASE.is_junction():
        raise ValueError('Benchmark output is not the expected project directory.')
    marker = BASE / 'owner.json'
    if BASE.exists() and (not marker.is_file() or load_json(marker)['owner'] != OWNER):
        raise ValueError('Refusing to modify an unowned existing benchmark directory.')


def remove_owned_child(name):
    if name not in ('user', 'run', 'previous-run'):
        raise ValueError('Not an owned disposable benchmark child.')
    path = BASE / name
    if path.exists():
        if path.resolve().parent != BASE.resolve() or path.is_symlink() or path.is_junction():
            raise ValueError('Refusing recursive removal outside the benchmark directory.')
        files(path)  # Reject junctions inside the subtree before recursive removal.
        shutil.rmtree(path)


def prepare(allow):
    description = plan()
    if not allow:
        raise ValueError('Creating isolated saves/cache requires --allow-test-data (up to 512 MiB; no game files copied).')
    check_game_stopped()
    verify_base()
    BASE.mkdir(parents=True, exist_ok=True)
    if not (BASE / 'owner.json').exists():
        save_json(BASE / 'owner.json', {'owner': OWNER})
    seed = BASE / 'seed'
    if not seed.exists():
        seed.mkdir()
        shutil.copytree(description['save_source'], seed / 'user')
        shutil.copytree(description['cache_source'], seed / 'gpu')
        save_json(seed / 'manifest.json', {**description, 'save_fingerprint': fingerprint(seed / 'user'),
                                          'cache_fingerprint': fingerprint(seed / 'gpu')})
    seed_manifest = json.loads((seed / 'manifest.json').read_text(encoding='utf-8'))
    if fingerprint(seed / 'user') != seed_manifest['save_fingerprint']:
        raise ValueError('Benchmark save seed has changed; refusing a different starting state.')
    if not (BASE / 'gpu').exists():
        shutil.copytree(seed / 'gpu', BASE / 'gpu')
    config = ROOT / 'profiles/CUSA00309/settings.ini'
    if config.is_file() and not (BASE / 'settings.ini').exists():
        shutil.copy2(config, BASE / 'settings.ini')
    check_budget()
    return description, seed_manifest


class Session:
    """Controls only the child carrying this private command-file path."""
    def __init__(self, description, run_dir, flat_memo):
        self.directory = run_dir
        self.control = run_dir / 'window-control.txt'
        self.pad = run_dir / 'pad.txt'
        self.nonce = 0
        self.error = None
        self.result = None
        self.thread = None
        env = tool_environment()
        native_environment(description['profile'], ROOT, env)
        env.update(BB_USER_DIR=str(BASE / 'user'), BB_GPU_USER_DIR=str(BASE / 'gpu'),
                   BB_CONFIG=str(BASE / 'settings.ini'), BB_PAD_FILE=str(self.pad),
                   BB_BENCHMARK_INPUT='1', BB_BENCH_CONTROL=str(self.control),
                   BB_PAD_RECORD='', BB_PAD_REPLAY='', BB_FRAME_STATS='0',
                   BB_PERF_STATS='1', BB_F10_DEEP='0', BB_FLAT_DATA_MEMO=str(flat_memo))
        # Use already prepared, audited artifacts; no launcher rebuild, game copies or address mods.
        inputs = description['inputs']
        command = [inputs['executable'], inputs['boot-linked.bin'], '--content-profile', inputs['content.bin'],
                   '--patches', inputs['patches.bin'], '--app0', description['game'], '--user', str(BASE / 'user'), '--timeout', '0']
        self.script('')
        def launch():
            try:
                with (run_dir / 'collector.log').open('w', encoding='utf-8') as log:
                    # collect is run in another Python process so its output cannot affect the controller.
                    spec = run_dir / 'launch.json'
                    keys=('BB_GAME_PROFILE','BB_UPSCALER','BB_DEBUG_MOTION','BB_REGION','BB_LANGUAGE','BB_TIMEZONE_MINUTES',
                          'BB_ENTER_BUTTON','BB_DRAW_PIPE','BB_TEXTURE_HELPER','BB_PREP_PRIORITY','BB_SHADER_SOURCE','BB_LIVE_RES','BB_FPS','BB_FPS_LIMIT','BB_VBLANK_HZ',
                          'BB_SHOW_FPS','BB_USER_DIR','BB_GPU_USER_DIR','BB_CONFIG','BB_USER_NAME','BB_PRESENT_MODE',
                          'BB_FLAT_DATA_MEMO','BB_BENCHMARK_INPUT','BB_BENCH_CONTROL','BB_PAD_FILE','BB_PAD_RECORD','BB_PAD_REPLAY',
                          'BB_FRAME_STATS','BB_PERF_STATS','BB_F10_DEEP','BB_BUFFER_PROFILE','BB_CPU_WORD_SUMMARY','BB_FRAMES_AHEAD','BB_FAULT_CHUNK_WORDS','BB_READBACK_WINDOW_KIB','BB_READBACK_TRACE','BB_READBACK_PREFETCH','BB_READBACK_LRU','BB_CLEAN_ARENA_READ','BB_DESCRIPTOR_PACK')
                    save_json(spec, {'command': command, 'env': {k: env[k] for k in keys if k in env},
                                    'base': str(run_dir / 'debug'), 'profile': description['profile']})
                    self.collector = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '_collect', str(spec)],
                                                      cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
                    self.result = self.collector.wait()
            except BaseException as error:
                self.error = error
        self.thread = threading.Thread(target=launch, daemon=True)
        self.thread.start()

    def alive(self):
        return self.thread.is_alive()

    def wait(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if not self.alive():
                raise RuntimeError(f'Benchmark child ended early: {self.result}, {self.error}')
            time.sleep(min(.2, end - time.monotonic()))

    @staticmethod
    def replace_control(pending, destination):
        # Existing Windows readers may briefly omit delete sharing. Retry only
        # sharing/access conflicts, with a deadline; persistent ACL failures still
        # propagate instead of changing permissions or targeting another process.
        deadline = time.monotonic() + 2
        while True:
            try:
                pending.replace(destination)
                return
            except PermissionError as error:
                if error.winerror not in (5, 32, 33) or time.monotonic() >= deadline:
                    raise
                time.sleep(.01)

    def script(self, tokens):
        # Different sizes for hold/release avoid legacy whole-second stat caching.
        pending = self.pad.with_suffix('.next')
        pending.write_text(tokens + '\n', encoding='ascii')
        self.replace_control(pending, self.pad)

    def command(self, action, timeout=20):
        if action not in ('record-start', 'record-stop', 'snapshot', 'quit'):
            raise ValueError('Unknown owned-session control action.')
        self.nonce += 1
        pending = self.control.with_suffix('.next')
        pending.write_text(f'{self.nonce} {action}\n', encoding='ascii')
        self.replace_control(pending, self.control)
        end = time.monotonic() + timeout
        ack = Path(str(self.control) + '.ack')
        while time.monotonic() < end:
            if ack.is_file():
                value = load_json(ack)
                if value.get('id') == self.nonce and value.get('action') == action:
                    return value
            if not self.alive():
                raise RuntimeError('Owned game exited before acknowledging control.')
            time.sleep(.05)
        raise RuntimeError(f'Owned game did not acknowledge {action}; inspect its diagnostics.')

    def session(self):
        path = self.directory / 'debug/active.json'
        if not path.is_file():
            raise RuntimeError('No actual game diagnostic session was created.')
        session = Path(load_json(path)['session']).resolve()
        if session.parent != (self.directory / 'debug').resolve():
            raise ValueError('Child diagnostic path escaped this benchmark run.')
        return session

    def close(self):
        if self.alive():
            self.command('quit')
            self.thread.join(30)
        if self.alive():
            raise RuntimeError('Owned benchmark game has not exited; diagnostic state preserved, no other process was stopped.')
        if self.error or self.result:
            raise RuntimeError(f'Actual benchmark process failed: {self.error or self.result}')


def frame_report(session, start, stop):
    if stop<=start:
        raise ValueError('Measurement boundaries must be increasing.')
    rows = {}
    malformed = 0
    for name in ('present-frames.csv.1', 'present-frames.csv'):
        path = Path(session) / name
        if not path.is_file():
            continue
        with path.open(newline='') as stream:
            for raw in csv.DictReader(stream):
                try:
                    row = {key: int(value) for key, value in raw.items()}
                    if (not {'sequence','tick_ms','interval_ns','width','height','dropped_frames'}.issubset(row) or
                            row['interval_ns'] <= 0 or row['sequence'] <= 0 or row['width']<=0 or row['height']<=0 or
                            any(v<0 for v in row.values())):
                        raise ValueError()
                    rows[row['sequence']] = row
                except (ValueError, TypeError, KeyError):
                    malformed += 1
    # Exclude an interval which started before the measurement command.
    selected = sorted((r for r in rows.values() if start <= r['tick_ms'] - r['interval_ns'] / 1e6
                       and r['tick_ms'] <= stop), key=lambda r: r['sequence'])
    if not selected:
        raise ValueError('No actual fresh game presentations in the measurement interval.')
    intervals = sorted(row['interval_ns'] / 1e6 for row in selected)
    dimensions=[list(d) for d in sorted(set((r['width'],r['height']) for r in selected))]
    pct = lambda p: intervals[min(len(intervals) - 1, math.ceil(len(intervals) * p) - 1)]
    gaps = sum(max(0, b['sequence'] - a['sequence'] - 1) for a, b in zip(selected, selected[1:]))
    coverage = sum(intervals) / (stop - start)
    valid = not malformed and not gaps and not max(r['dropped_frames'] for r in selected) and .95<=coverage<=1.05 and len(dimensions)==1
    # Sliding windows spanning at least one second expose brief scene-dependent
    # drops which an overall average hides. Never infer missing intervals.
    window=deque(); elapsed=0; minimum=None
    for row in selected:
        window.append(row['interval_ns']); elapsed+=window[-1]
        while elapsed-window[0]>=1_000_000_000:
            elapsed-=window.popleft()
        if elapsed>=1_000_000_000:
            score=len(window)*1_000_000_000/elapsed
            minimum=score if minimum is None else min(minimum,score)
    return {'source': 'fresh_game_present_host_clock', 'actual_game_process': True,
            'frames': len(selected), 'fps': len(selected) * 1000 / sum(intervals),
            'minimum_1s_fps': minimum if valid else None,
            'p50_ms': pct(.5), 'p95_ms': pct(.95), 'p99_ms': pct(.99),
            'one_percent_low_fps': 1000 / (sum(intervals[-max(1, math.ceil(len(intervals) * .01)):]) / max(1, math.ceil(len(intervals) * .01))),
            'max_ms': max(intervals), 'over_20ms': sum(v > 20 for v in intervals),
            'over_33ms': sum(v > 33.333333 for v in intervals), 'over_50ms': sum(v > 50 for v in intervals),
            'over_100ms': sum(v > 100 for v in intervals), 'coverage': coverage,
            'sequence_gaps': gaps, 'malformed_rows': malformed, 'valid': valid,
            'dimensions': dimensions,
            'stable_50': valid and max(intervals) <= 20.5,
            'stable_60': valid and max(intervals) <= 17.2}


def run_round(description, manifest, warmup, seconds, flat_memo):
    verify_base()
    check_game_stopped()
    remove_owned_child('user')
    shutil.copytree(BASE / 'seed/user', BASE / 'user')
    remove_owned_child('previous-run')
    if (BASE / 'run').exists():
        (BASE / 'run').replace(BASE / 'previous-run')
    run_dir = BASE / 'run'
    run_dir.mkdir()
    source_before = fingerprint(description['save_source'])
    session = Session(description, run_dir, flat_memo)
    phases = []
    try:
        print('Actual game launching with isolated save; waiting for the startup menu.', flush=True)
        session.wait(15)
        session.script('circle')
        session.wait(.25)
        session.script('')
        print(f'Loading fixed checkpoint; warmup {warmup}s (excluded from FPS).', flush=True)
        session.wait(warmup)
        for name, tokens in (('checkpoint_static', ''), ('checkpoint_camera_rotation', 'rx=180')):
            session.script(tokens)
            session.command('snapshot')
            session.wait(5)  # Include three delayed captures and worker drain before measuring.
            begin = session.command('record-start')
            if begin['recording'] != 1:
                raise ValueError('F11 recording did not start.')
            print(f'Measuring actual game: {name}, {seconds}s.', flush=True)
            session.wait(seconds)
            end = session.command('record-stop')
            if end['recording'] != 0:
                raise ValueError('F11 recording did not stop.')
            session.wait(2)
            value = frame_report(session.session(), begin['tick_ms'], end['tick_ms'])
            value.update(name=name, start_tick_ms=begin['tick_ms'], stop_tick_ms=end['tick_ms'])
            phases.append(value)
            print(f'{name}: actual FPS {value["fps"]:.2f}, p95 {value["p95_ms"]:.2f}ms, valid={value["valid"]}', flush=True)
    finally:
        session.script('')
        session.close()
        if source_before != fingerprint(description['save_source']):
            raise ValueError('Original save changed while benchmark ran; report must not be accepted.')
    native_manifest = load_json(session.session() / 'manifest.json')
    check_budget()
    return {'schema': OWNER, 'created_utc': datetime.now(timezone.utc).isoformat(),
            'executable_sha256': digest(description['inputs']['executable']),
            'game_sha256': description['profile']['source_sha256'], 'save_seed': manifest['save_fingerprint'],
            'warmup_seconds': warmup, 'phase_seconds': seconds, 'flat_data_memo': flat_memo,
            'session': str(session.session()), 'native_manifest': native_manifest, 'phases': phases,
            'host': {'machine': platform.machine(), 'cpu': os.environ.get('PROCESSOR_IDENTIFIER',''), 'logical_cpus': os.cpu_count()},
            'source_save_unchanged': True,
            'scene_verified': False, 'note': 'Actual game FPS. Validate snapshots depict gameplay before accepting scene scores; no offline frame replay.'}


def compare(previous, current):
    for field in ('schema','game_sha256','save_seed','warmup_seconds','phase_seconds','host'):
        if previous.get(field)!=current.get(field):
            raise ValueError(f'Actual runtime reports differ in {field}; comparison refused.')
    if [p['name'] for p in previous['phases']]!=[p['name'] for p in current['phases']]:
        raise ValueError('Actual runtime scenarios differ.')
    comparisons=[]
    for old,new in zip(previous['phases'],current['phases']):
        if not old['valid'] or not new['valid'] or old['dimensions']!=new['dimensions']:
            raise ValueError('Incomplete/resized measurement cannot be used as an optimization comparison.')
        comparisons.append({'name':new['name'],'fps_change_percent':100*(new['fps']/old['fps']-1),
                            'p95_change_percent':100*(new['p95_ms']/old['p95_ms']-1)})
    return comparisons


def write_report(value):
    report = BASE / 'latest.json'
    if report.is_file():
        report.replace(BASE / 'previous.json')
    save_json(report, value)
    lines = ['# 实际游戏运行时帧率测试', '',
             '已运行真实游戏，使用隔离的固定存档；启动等待与预热不计入指定测量区间。', '',
             '| 场景 | FPS | 最低 1秒 FPS | 1% low | p95 ms | p99 ms | >50ms 帧 | 数据完整 |', '|---|---:|---:|---:|---:|---:|---:|---|']
    for p in value['phases']:
        minimum=f'{p["minimum_1s_fps"]:.2f}' if p.get('minimum_1s_fps') is not None else '未测量'
        lines.append(f'| {p["name"]} | {p["fps"]:.2f} | {minimum} | {p["one_percent_low_fps"]:.2f} | {p["p95_ms"]:.2f} | '
                     f'{p["p99_ms"]:.2f} | {p["over_50ms"]} | {p["valid"]} |')
    lines += ['', f'实际会话：{value["session"]}', 'FPS 来源为新游戏帧呈现的主机时间线，非函数微基准或菜单帧率推算。',
              ('截图已人工确认：两段均为人物操作场景，镜头旋转输入生效。' if value.get('scene_verified') else
               '截图尚未确认目标场景；不能将菜单或加载画面的帧率作为游玩验收结果。'),
              '本版自动运行固定存档的静止/镜头旋转场景；不是脱离游戏进程的完整图形帧回放。',
              '该计时不测量显示面板扫描、撕裂或完整画面正确性；实际呈现模式记录在日志中。']
    (BASE / 'latest.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('plan', 'prepare', 'run'))
    parser.add_argument('--allow-test-data', action='store_true', help='authorize isolated saves/cache/logs within 512 MiB')
    parser.add_argument('--warmup', type=int, default=45)
    parser.add_argument('--seconds', type=int, default=20)
    parser.add_argument('--flat-data-memo', type=int, choices=(0, 1), default=1)
    parser.add_argument('--compare',type=Path,help='compare a previous actual runtime report with identical save/workload')
    args = parser.parse_args()
    require_windows()
    if args.action == 'plan':
        print(json.dumps(plan(), ensure_ascii=False, indent=2))
        return 0
    if not 15 <= args.warmup <= 120 or not 10 <= args.seconds <= 90:
        parser.error('warmup 15..120 seconds; measurement 10..90 seconds')
    description, manifest = prepare(args.allow_test_data)
    if args.action == 'prepare':
        print(f'Isolated benchmark seed prepared: {BASE}')
        return 0
    value = run_round(description, manifest, args.warmup, args.seconds, args.flat_data_memo)
    if args.compare:
        try:
            value['comparison']=compare(load_json(args.compare),value)
        except ValueError as error:
            value['comparison_error']=str(error)
            write_report(value)  # Preserve the actual measurements even if comparison is invalid.
            raise
    write_report(value)
    print(f'Actual runtime FPS report: {BASE / "latest.md"}')
    return 0 if all(p['valid'] for p in value['phases']) else 2


if __name__ == '__main__':
    try:
        if sys.argv[1:2] == ['_collect']:
            from debug_session import collect
            spec = load_json(sys.argv[2])
            env = tool_environment(); env.update(spec['env'])
            raise SystemExit(collect(spec['command'], ROOT, spec['base'], spec['profile'], env))
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        raise SystemExit(1)

"""Windows component performance harness; F11 calibrates counts, never supplies a replay."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys

from windows_tools import ROOT, require_windows, tool_environment

MAX_RECORDING_BYTES = 64 * 1024 * 1024
MAX_ROWS = 200_000
CASE_NAMES = ('texture_cache_small_set', 'texture_cache_pressure',
              'image_repeated_partial_read', 'image_view_switch_control',
              'image_mixed_read_write', 'image_full_range_control',
              'flat_upload_repeated', 'flat_upload_mixed', 'flat_upload_changing_control')


def quantile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, math.ceil(len(values) * fraction) - 1)]


def calibration(path):
    """Bounded read-only F11 analysis. No inferred resource identities or access order."""
    path = Path(path).resolve()
    if path.stat().st_size > MAX_RECORDING_BYTES:
        raise ValueError('Recording exceeds the 64 MiB input limit.')
    rows = []
    required = ('sequence', 'tick_ms', 'interval_ns', 'texture_set_hits', 'texture_set_misses')
    with path.open(encoding='utf-8-sig', newline='') as stream:
        reader = csv.DictReader(line for line in stream if not line.startswith('#'))
        if not reader.fieldnames or not set(required).issubset(reader.fieldnames):
            raise ValueError('Input must be an F11 CSV with texture-set counters.')
        previous, previous_tick = 0, 0
        for source in reader:
            if len(rows) >= MAX_ROWS:
                raise ValueError('Recording exceeds the 200,000-row input limit.')
            row = {name: int(source[name]) for name in required}
            if (row['sequence'] <= previous or row['tick_ms'] < previous_tick or
                    any(value < 0 or value >= 2**64 for value in row.values())):
                raise ValueError('Recording has duplicate/out-of-order sequences or negative values.')
            previous, previous_tick = row['sequence'], row['tick_ms']
            rows.append(row)
    if not rows or not sum(row['interval_ns'] for row in rows):
        raise ValueError('Recording contains no measurable frames.')
    # Favor the last minute of real activity over menu-heavy full-session averages.
    end = rows[-1]['tick_ms']
    activity = [row for row in rows if row['tick_ms'] >= end - 60_000]
    activity_ns = sum(row['interval_ns'] for row in activity)
    if not activity_ns:
        raise ValueError('Recording activity window contains no measurable frames.')
    lookups = [row['texture_set_hits'] + row['texture_set_misses'] for row in activity]
    p95 = quantile(lookups, .95)
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'frames': len(rows), 'activity_frames': len(activity),
            'activity_fps': len(activity) * 1e9 / activity_ns,
            'texture_lookups_p95': p95,
            'recommended_operations': max(200_000, min(500_000, p95 * 64)),
            'interpretation': 'Synthetic operation count calibration only; identities, views and writes are assumed.'}


def latest_recording(root=ROOT):
    active = root / 'out/CUSA00309/debug/active.json'
    if not active.is_file():
        return None
    state = json.loads(active.read_text(encoding='utf-8'))
    base = (root / 'out/CUSA00309/debug').resolve()
    session = Path(state.get('session', '')).resolve()
    if session.parent != base:
        raise ValueError('Active session path is outside the project diagnostic directory.')
    paths = list(session.glob('performance-recording-*.csv'))
    paths = [path for path in paths if '.cache.' not in path.name]
    return max(paths, key=lambda path: path.stat().st_mtime) if paths else None


def check_game_stopped(root=ROOT):
    active = root / 'out/CUSA00309/debug/active.json'
    if not active.is_file():
        return
    state = json.loads(active.read_text(encoding='utf-8'))
    if state.get('finished'):
        return
    from debug_session import ProcessMetrics
    try:
        process = ProcessMetrics(int(state['pid']))
    except PermissionError as error:
        # Win32 87 means an absent stale PID; access-denied is NOT an absent process.
        if getattr(error, 'winerror', None) == 87:
            return
        raise RuntimeError(f'Cannot determine whether the game is still running: {error}') from error
    try:
        sample = process.read()
        if not sample.get('ended_filetime'):
            raise RuntimeError('Close the current game before benchmarking to avoid competing CPU/GPU load.')
    finally:
        process.close()


def validate_native(data, cpu_only, operations, rounds, seed):
    if (data.get('schema'), data.get('kind'), data.get('cpu_only'), data.get('operations'),
            data.get('rounds'), data.get('seed')) != (1, 'synthetic_component_benchmark', cpu_only, operations, rounds, seed):
        raise ValueError('Native benchmark parameters/schema do not match the requested run.')
    cases = data.get('cases', [])
    if [case.get('name') for case in cases] != list(CASE_NAMES[:2] if cpu_only else CASE_NAMES):
        raise ValueError('Native report has missing/duplicate/unexpected cases.')
    if data.get('registry_checks', 0) <= 0:
        raise ValueError('Registry correctness validation did not execute.')
    for case in cases:
        if case.get('equivalent') is not True:
            raise ValueError(f'Correctness failed: {case["name"]}')
        for mode in ('baseline', 'candidate'):
            row = case[mode]
            values = row['round_ms']
            if len(values) != rounds or any(not math.isfinite(value) or value <= 0 for value in values):
                raise ValueError('Invalid/incomplete benchmark timing samples.')
            if not math.isclose(row['median_ms'], statistics.median(values), rel_tol=1e-8):
                raise ValueError('Native median does not match its timing samples.')
            if not math.isfinite(row['mad_ratio']) or row['mad_ratio'] < 0:
                raise ValueError('Invalid benchmark dispersion.')
        if case['baseline']['checksum'] != case['candidate']['checksum']:
            raise ValueError('Baseline/candidate checksum mismatch.')
    return data


def assess(data):
    """Performance observations never replace correctness checks or predict game FPS."""
    for case in data['cases']:
        old, new = case['baseline'], case['candidate']
        # MAD and paired ratios reduce single-round noise; p95 is across whole rounds.
        ratios = [b / a for a, b in zip(old['round_ms'], new['round_ms'])]
        case['candidate_over_baseline'] = statistics.median(ratios)
        stable = max(old['mad_ratio'], new['mad_ratio']) <= .10
        measurable = min(old['median_ms'], new['median_ms']) >= .10
        case['measurement_quality'] = 'usable' if stable and measurable else 'noisy_or_too_short'
        ratio = case['candidate_over_baseline']
        if case['measurement_quality'] != 'usable':
            case['performance'] = 'inconclusive'
        elif ratio <= .90 and quantile(ratios, .90) <= .95:
            case['performance'] = 'component_gain'
        elif ratio >= 1.10 and quantile(ratios, .10) >= 1.05:
            case['performance'] = 'possible_regression'
        else:
            case['performance'] = 'no_clear_change'
    data['game_fps_verified'] = False
    production = [case for case in data['cases'] if case['scope'] in ('production_Image_GetBarriers_cpu_time','production_FlatDataMemo_StreamBuffer_Copy_cpu_time')]
    data['stage_gain_confirmed'] = (any(case['performance'] == 'component_gain' for case in production)
                                    and all(case['performance'] not in ('possible_regression', 'inconclusive') for case in production))
    return data


def compare(previous, current):
    for field in ('schema', 'kind', 'operations', 'seed', 'cpu_only', 'gpu', 'host', 'perf_stats'):
        if previous.get(field) != current.get(field):
            raise ValueError(f'Cannot compare reports with different {field}.')
    if [c['name'] for c in previous['cases']] != [c['name'] for c in current['cases']]:
        raise ValueError('Cannot compare reports with different case coverage.')
    result = []
    for old, new in zip(previous['cases'], current['cases']):
        if old['scope'] != new['scope'] or old['candidate']['checksum'] != new['candidate']['checksum']:
            raise ValueError('Cannot compare reports with different semantics/workload outputs.')
        quality = old['measurement_quality'] == new['measurement_quality'] == 'usable'
        ratio = new['candidate']['median_ms'] / old['candidate']['median_ms']
        result.append({'name': new['name'], 'new_over_previous': ratio,
                       'status': 'inconclusive' if not quality else 'possible_regression' if ratio > 1.10
                                 else 'component_gain' if ratio < .90 else 'no_clear_change'})
    return result


def write_report(directory, data):
    directory.mkdir(parents=True, exist_ok=True)
    current = directory / 'latest.json'
    if current.is_file():
        current.replace(directory / 'previous.json')
    pending = directory / 'latest.json.next'
    pending.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    pending.replace(current)
    lines = ['# 引擎组件性能模拟测试', '',
             '这是合成组件基准；没有运行游戏，不代表游戏 FPS。', '',
             '| 场景 | 基线中位耗时 ms | 新版中位耗时 ms | 配对耗时比 | 基线→新版命中 | 判定 |',
             '|---|---:|---:|---:|---:|---|']
    labels = {'component_gain': '组件收益', 'possible_regression': '疑似退化',
              'inconclusive': '波动大/不足以判断', 'no_clear_change': '无明确变化'}
    for case in data['cases']:
        lines.append(f'| {case["name"]} | {case["baseline"]["median_ms"]:.4f} | '
                     f'{case["candidate"]["median_ms"]:.4f} | {case["candidate_over_baseline"]:.3f} | '
                     f'{case["baseline"]["hits"]}→{case["candidate"]["hits"]} | '
                     f'{labels[case["performance"]]} |')
    lines += ['', '所有结果校验一致；p95 是重复轮次总耗时，不是游戏逐帧 p95。',
              'GPU 模式只测图像状态生成的 CPU 开销，没有提交模拟场景绘制。',
              'texture_cache 场景调用生产哈希函数，查找表为同尺寸直接映射模型；不包含完整资源绑定。',
              '完整参数、每轮时间、波动、命中及检查次数见 latest.json。']
    if data.get('calibration'):
        value = data['calibration']
        lines += ['', f'负载数量参考 F11 最后一分钟：{value["activity_frames"]} 帧，'
                  f'约 {value["activity_fps"]:.2f} FPS；访问身份、顺序和读写比例为合成假设。']
    if data['cpu_only']:
        lines += ['', '显式 CPU-only：四项图像状态基准未执行。']
    if data.get('comparison'):
        lines += ['', '## 与上次候选版本对比', '', '| 场景 | 本次/上次耗时 | 判定 |', '|---|---:|---|']
        for case in data['comparison']:
            lines.append(f'| {case["name"]} | {case["new_over_previous"]:.3f} | {labels[case["status"]]} |')
    (directory / 'latest.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return current


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpu-only', action='store_true', help='omit Vulkan-backed image-state benchmarks')
    parser.add_argument('--recording', type=Path, help='read-only F11 input; default latest completed recording')
    parser.add_argument('--synthetic', action='store_true', help='use fixed synthetic load without F11 calibration')
    parser.add_argument('--operations', type=int)
    parser.add_argument('--rounds', type=int, default=9)
    parser.add_argument('--seed', type=lambda value: int(value, 0), default=0x309)
    parser.add_argument('--compare', type=Path, help='previous compatible JSON report; read before rotating harness outputs')
    parser.add_argument('--require-gain', action='store_true', help='exit 3 unless production image cases show a stable gain without detected regression/noise')
    args = parser.parse_args()
    require_windows()
    if args.synthetic and args.recording:
        parser.error('--synthetic and --recording cannot be combined')
    if not 3 <= args.rounds <= 31 or not 0 < args.seed < 2**64:
        parser.error('rounds must be 3..31; seed must be a nonzero uint64')
    check_game_stopped()
    binary = ROOT / 'out/gpu/performance-sim-test.exe'
    if not binary.is_file():
        raise RuntimeError('Missing performance-sim-test.exe; run build.bat --build-tests. No implicit build/download.')
    recording = args.recording or (None if args.synthetic else latest_recording())
    profile = calibration(recording) if recording else None
    operations = args.operations if args.operations is not None else profile['recommended_operations'] if profile else 200_000
    if not 1000 <= operations <= 2_000_000:
        parser.error('operations must be 1000..2000000')
    previous = json.loads(args.compare.read_text(encoding='utf-8')) if args.compare else None
    directory = ROOT / 'out/performance-sim'
    user = directory / 'gpu-user'
    user.mkdir(parents=True, exist_ok=True)
    env = tool_environment()
    env.update(BB_GPU_USER_DIR=str(user), BB_PERF_STATS='1', BB_IMAGE_READ_MEMO='1')
    command = [str(binary), '--operations', str(operations), '--rounds', str(args.rounds), '--seed', str(args.seed)]
    if args.cpu_only:
        command.append('--cpu-only')
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=180)
    (directory / 'native.log').write_text((result.stdout + result.stderr)[-2 * 1024 * 1024:], encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'Native benchmark failed (exit {result.returncode}); see out/performance-sim/native.log.')
    outputs = [line[len('PERF_SIM_JSON '):] for line in result.stdout.splitlines() if line.startswith('PERF_SIM_JSON ')]
    if len(outputs) != 1:
        raise ValueError('Native benchmark did not return exactly one complete report.')
    data = assess(validate_native(json.loads(outputs[0]), args.cpu_only, operations, args.rounds, args.seed))
    data.update(created_utc=datetime.now(timezone.utc).isoformat(), calibration=profile, perf_stats=True,
                host={'machine': platform.machine(), 'cpu': os.environ.get('PROCESSOR_IDENTIFIER', ''), 'logical_cpus': os.cpu_count()},
                executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                omitted_cases=list(CASE_NAMES[2:]) if args.cpu_only else [])
    if previous is not None:
        data['comparison'] = compare(previous, data)
    report = write_report(directory, data)
    print(f'Correctness PASS; measured {len(data["cases"])} cases, {args.rounds} paired rounds. Report: {report}')
    for case in data['cases']:
        print(f'{case["name"]}: {case["baseline"]["median_ms"]:.3f} -> '
              f'{case["candidate"]["median_ms"]:.3f} ms; {case["performance"]}')
    print('Whole-game FPS and brightness correctness are unverified by this component benchmark.')
    if args.require_gain and not data['stage_gain_confirmed']:
        return 3
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        sys.exit(1)

"""Recover an interrupted F11 recording from persisted frame telemetry, never invent frames."""
import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import re
import threading


class RecordingJournal:
    """Collector-side checkpoint; no file operations in the game's render threads."""
    Limit = 32 * 1024 * 1024  # Per session, including all recordings' temporary journals.

    def __init__(self, session):
        self.session = Path(session).resolve()
        self.lock = threading.Lock()
        self.recordings = {}
        self.bytes = 0
        self.last_sequence = {}

    def observe(self, line):
        match = re.search(r'PERF_RECORDING (START|STOP) id=(\d+) tick_ms=(\d+)', line)
        if not match:
            return
        action, number, tick = match.groups()
        with self.lock:
            recording = self.recordings.setdefault(int(number), {})
            recording['start' if action == 'START' else 'stop'] = int(tick)

    def checkpoint(self):
        with self.lock:
            recordings = {number: dict(value) for number, value in self.recordings.items()}
        if not recordings:
            return
        index = self.session / 'performance-recording-index.json'
        pending = index.with_name(index.name + '.next')
        pending.write_text(json.dumps(recordings, indent=2)+'\n', encoding='utf-8')
        pending.replace(index)
        for number, value in recordings.items():
            if 'start' not in value or (self.session / f'performance-recording-{number}.csv').exists():
                continue
            cache = self.session / f'performance-recording-{number}.cache.csv'
            for name in ('frames.csv.1', 'frames.csv'):
                source = self.session / name
                if not source.is_file():
                    continue
                fields, rows, _ = read_frames(source)
                selected = [r for r in rows if r['sequence'] > self.last_sequence.get(number, 0)
                            and r['tick_ms'] - math.ceil(r['interval_ns']/1e6) >= value['start']
                            and ('stop' not in value or r['tick_ms'] <= value['stop'])]
                if not selected:
                    continue
                buffer = io.StringIO(newline='')
                writer = csv.DictWriter(buffer, fields, lineterminator='\n')
                if not cache.exists():
                    writer.writeheader()
                writer.writerows(selected)
                data = buffer.getvalue().encode('ascii')
                if self.bytes + len(data) > self.Limit:
                    continue  # Source telemetry continues; loss is reported on recovery.
                with cache.open('ab') as stream:
                    stream.write(data)
                    stream.flush()
                self.bytes += len(data)
                self.last_sequence[number] = selected[-1]['sequence']


def read_frames(path):
    with Path(path).open(encoding='ascii', newline='') as stream:
        reader = csv.DictReader(line for line in stream if not line.startswith('#'))
        fields = reader.fieldnames or []
        rows, malformed = [], 0
        for raw in reader:
            try:
                row = {key: int(raw[key]) for key in fields}
                if row['sequence'] <= 0 or row['interval_ns'] <= 0 or any(v < 0 for v in row.values()):
                    raise ValueError('invalid frame')
                rows.append(row)
            except (ValueError, TypeError, KeyError):
                malformed += 1
        return fields, rows, malformed


def merge_native_journal(native, journal):
    """Join different sequence origins using unchanged frame content, not millisecond ticks.

    Multiple catch-up flips can share a tick. A tick-keyed merge would silently lose
    samples and can even hide the slow frame preceding those catch-up flips.
    This function is read-only; callers decide where to publish analysis artifacts.
    """
    if not native or not journal:
        return list(native or journal)
    fields=sorted(set(native[0])-{'sequence','dropped_frames'})
    if fields!=sorted(set(journal[0])-{'sequence','dropped_frames'}):
        raise ValueError('Native recording and journal schemas differ')
    signatures={}
    for row in journal:
        signatures.setdefault(tuple(row[k] for k in fields),[]).append(row['sequence'])
    offsets={matches[0]-row['sequence'] for row in native
             if len(matches:=signatures.get(tuple(row[k] for k in fields),[]))==1}
    if len(offsets)!=1:
        raise ValueError('Cannot prove a unique native/journal sequence origin')
    offset=offsets.pop()
    result={row['sequence']:{k:row[k] for k in ('sequence',*fields)} for row in journal}
    for source in native:
        row={k:source[k] for k in ('sequence',*fields)}
        row['sequence']+=offset
        old=result.get(row['sequence'])
        if old is not None and old!=row:
            raise ValueError('Conflicting frame content at matched sequence')
        result[row['sequence']]=row
    return [result[key] for key in sorted(result)]


def recover(session):
    session = Path(session).resolve()
    status = json.loads((session / 'status.json').read_text(encoding='utf-8'))
    if status.get('collector') != 'infamous-debug-v1' or status.get('finished') is not True:
        raise ValueError('Recovery requires a finished diagnostic session')
    starts, stops = {}, {}
    index = session / 'performance-recording-index.json'
    if index.is_file():
        for number, value in json.loads(index.read_text(encoding='utf-8')).items():
            if 'start' in value:
                starts[int(number)] = int(value['start'])
            if 'stop' in value:
                stops[int(number)] = int(value['stop'])
    for name in ('runtime.log.1', 'runtime.log'):
        path = session / name
        if not path.is_file():
            continue
        for action, number, tick in re.findall(
                r'PERF_RECORDING (START|STOP) id=(\d+) tick_ms=(\d+)',
                path.read_text(encoding='utf-8', errors='replace')):
            (starts if action == 'START' else stops)[int(number)] = int(tick)
    fields, source_rows, sources, malformed = [], {}, [], 0
    cache_names = [f'performance-recording-{number}.cache.csv' for number in starts]
    for name in (*cache_names, 'frames.csv.1', 'frames.csv'):
        path = session / name
        if not path.is_file():
            continue
        if path.is_symlink() or path.resolve().parent != session:
            raise ValueError('Telemetry source must stay inside the session')
        current_fields, rows, invalid = read_frames(path)
        if fields and fields != current_fields:
            raise ValueError('Rotated telemetry schemas differ')
        fields = current_fields
        malformed += invalid
        for row in rows:
            old = source_rows.get(row['sequence'])
            if old is not None and old != row:
                raise ValueError('Conflicting duplicate frame sequence')
            source_rows[row['sequence']] = row
        sources.append({'name': name, 'bytes': path.stat().st_size,
                        'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    results = []
    ordered = sorted(source_rows.values(), key=lambda r: r['sequence'])
    for number, start in sorted(starts.items()):
        target = session / f'performance-recording-{number}.csv'
        if target.exists():
            # Normal F11 STOP was saved by the native writer. Only remove a sidecar if every
            # cached sample also exists unchanged in that verified file (native sequences differ).
            cache = session / f'performance-recording-{number}.cache.csv'
            if cache.is_file() and not cache.is_symlink() and cache.resolve().parent == session:
                _, saved, invalid = read_frames(target)
                _, cached, cache_invalid = read_frames(cache)
                by_tick = {r['tick_ms']: r for r in saved}
                if not invalid and not cache_invalid and cached and all(
                        r['tick_ms'] in by_tick and all(by_tick[r['tick_ms']].get(k) == v
                        for k, v in r.items() if k not in ('sequence', 'dropped_frames')) for r in cached):
                    cache.unlink()
            continue  # Never overwrite a native recording or earlier recovery.
        next_start = min((t for t in starts.values() if t > start), default=None)
        end = stops.get(number)
        upper = end if end is not None else next_start
        # Omit the flip straddling START: its counters include work before the key press.
        selected = [r for r in ordered if r['tick_ms'] - math.ceil(r['interval_ns']/1e6) >= start
                    and (upper is None or r['tick_ms'] <= upper)]
        if not selected:
            continue
        gaps = sum(max(0, b['sequence']-a['sequence']-1) for a, b in zip(selected, selected[1:]))
        out_fields = [k for k in fields if k != 'dropped_frames']
        normalized = [{k: (i if k == 'sequence' else r[k]) for k in out_fields}
                      for i, r in enumerate(selected, 1)]
        metadata = {
            'schema': 'infamous-recording-recovery-v1', 'recording_id': number,
            'recovered': True, 'interrupted': end is None,
            'requested_start_tick_ms': start, 'requested_end_tick_ms': end,
            'first_available_tick_ms': selected[0]['tick_ms'],
            'last_available_tick_ms': selected[-1]['tick_ms'],
            'source_first_sequence': selected[0]['sequence'],
            'source_last_sequence': selected[-1]['sequence'], 'frames': len(selected),
            'sequence_gaps': gaps, 'malformed_source_rows': malformed,
            'source_dropped_frames': max(r.get('dropped_frames', 0) for r in selected),
            'start_gap_ms': max(0, selected[0]['tick_ms'] - math.ceil(selected[0]['interval_ns']/1e6) - start),
            'unpersisted_tail_available': False, 'tail_missing_frames': None,
            'interval_source': 'guest_flip_host_clock', 'gpu_execution_time_available': False,
            'sources': sources, 'source_telemetry_preserved': True,
            'temporary_recording_caches_deleted': [],
        }
        buffer = io.StringIO(newline='')
        buffer.write(f'# recording_id={number} start_tick_ms={start} end_tick_ms={selected[-1]["tick_ms"]} '
                     f'dropped_frames={gaps} recovered=1 interrupted={int(end is None)} tail_missing_frames=unknown\n')
        writer = csv.DictWriter(buffer, out_fields, lineterminator='\n')
        writer.writeheader()
        writer.writerows(normalized)
        pending = target.with_name(target.name + '.recovery-next')
        if pending.exists():
            raise ValueError(f'Unverified existing recovery file: {pending}')
        with pending.open('x', encoding='ascii', newline='') as stream:
            stream.write(buffer.getvalue())
        verified_fields, verified, invalid = read_frames(pending)
        if invalid or verified_fields != out_fields or verified != normalized:
            raise ValueError('Recovery validation failed; sources were preserved')
        pending.replace(target)  # Publishes verified data and consumes our temporary cache.
        metadata['output_sha256'] = hashlib.sha256(target.read_bytes()).hexdigest()
        report = target.with_suffix('.recovery.json')
        cache = session / f'performance-recording-{number}.cache.csv'
        if cache.is_file():
            _, cached_rows, cached_invalid = read_frames(cache)
            covered = {r['sequence']: r for r in selected}
            if not cached_invalid and all(covered.get(r['sequence']) == r for r in cached_rows):
                if cache.resolve().parent != session or cache.is_symlink():
                    raise ValueError('Cache deletion target escaped session')
                cache.unlink()  # Only the validated recording-specific cache; retain source logs.
                metadata['temporary_recording_caches_deleted'].append(cache.name)
        report.write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
        results.append(metadata)
    return results


def stats(rows):
    if not rows:
        return {'frames': 0}
    times = sorted(r['interval_ns']/1e6 for r in rows)
    percentile = lambda p: round(times[max(0, math.ceil(len(times)*p)-1)], 3)
    duration = sum(times)/1000
    result = {'frames': len(rows), 'measured_seconds': round(duration, 3),
            'average_flip_fps': round(len(rows)/duration, 2),
            'p50_ms': percentile(.5), 'p90_ms': percentile(.9), 'p95_ms': percentile(.95),
            'p99_ms': percentile(.99), 'max_ms': round(times[-1], 3),
            'over_20ms': sum(t > 20 for t in times), 'over_33_3ms': sum(t > 1000/30 for t in times),
            'over_50ms': sum(t > 50 for t in times), 'over_100ms': sum(t > 100 for t in times),
            'compile_frames': sum(r.get('compile_count', 0) > 0 for r in rows),
            'mean_draws': round(sum(r.get('draws', 0) for r in rows)/len(rows), 1),
            'mean_timers_ms': {k: round(sum(r.get(k, 0) for r in rows)/len(rows)/1e6, 3)
                               for k in rows[0] if k.endswith('_ns') and k != 'interval_ns'}}
    if 'texture_set_hits' in rows[0]:
        hits=sum(r['texture_set_hits'] for r in rows)
        misses=sum(r['texture_set_misses'] for r in rows)
        result['texture_sets']={'hits':hits,'misses':misses,
            'reused_after_unrelated_registry_changes':sum(r.get('texture_set_revalidated',0) for r in rows),
            'hit_rate':round(hits/(hits+misses),4) if hits+misses else None}
    return result


def analyze(path):
    _, rows, invalid = read_frames(path)
    journal=Path(path).with_suffix('.cache.csv')
    journal_joined=False
    if not Path(path).name.endswith('.cache.csv') and journal.is_file():
        _, persisted,journal_invalid=read_frames(journal)
        rows=merge_native_journal(rows,persisted)
        invalid+=journal_invalid
        journal_joined=True
    first = rows[0]['tick_ms'] if rows else 0
    windows = {}
    for row in rows:
        windows.setdefault((row['tick_ms']-first)//10000, []).append(row)
    return {'interval_source': 'guest_flip_host_clock', 'gpu_execution_time_available': False,
            'malformed_rows': invalid, 'journal_joined_read_only':journal_joined,'overall': stats(rows),
            'groups': {'under_20ms': stats([r for r in rows if r['interval_ns'] < 20000000]),
                       '20_to_33_3ms': stats([r for r in rows if 20000000 <= r['interval_ns'] < 1e9/30]),
                       'over_33_3ms': stats([r for r in rows if r['interval_ns'] >= 1e9/30]),
                       'over_50ms_without_compile': stats([r for r in rows if r['interval_ns'] > 50000000
                                                                           and r.get('compile_count', 0) == 0])},
            'windows_10s': [{'offset_seconds': number*10, **stats(samples)}
                            for number, samples in sorted(windows.items())],
            'worst_frames': sorted(rows, key=lambda r: r['interval_ns'], reverse=True)[:20]}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    args = parser.parse_args()
    results = recover(args.session)
    for result in results:
        path = args.session / f'performance-recording-{result["recording_id"]}.csv'
        path.with_suffix('.summary.json').write_text(json.dumps(analyze(path), indent=2)+'\n', encoding='utf-8')
    print(json.dumps(results, indent=2))

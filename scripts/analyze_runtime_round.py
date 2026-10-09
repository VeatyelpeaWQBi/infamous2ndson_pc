"""Preserve F11 phase counters before rotating isolated benchmark sessions."""
import argparse
import csv
import json
from pathlib import Path
import statistics


def summarize(report, session):
    result = {'executable_sha256': report['executable_sha256'], 'game_sha256': report['game_sha256'],
              'save_seed': report['save_seed'], 'settings': report['native_manifest']['settings'], 'phases': []}
    rows = []
    for path in session.glob('performance-recording-*.csv'):
        if path.name.endswith('.cache.csv'):
            continue  # Native journal duplicates the finalized recording.
        with path.open(encoding='utf-8') as stream:
            if stream.readline().startswith('#'):
                rows.extend(csv.DictReader(stream))
            else:
                stream.seek(0); rows.extend(csv.DictReader(stream))
    for phase in report['phases']:
        if not phase.get('valid', False):
            raise ValueError(f"Invalid frame measurement: {phase['name']}")
        selected = [row for row in rows if phase['start_tick_ms'] <= int(row['tick_ms']) <= phase['stop_tick_ms']]
        if not selected:
            raise ValueError(f"No finalized F11 data for {phase['name']} in {session}")
        values = {}
        if selected:
            for name in selected[0]:
                if name in ('sequence', 'tick_ms'):
                    continue
                values[name] = statistics.mean(int(row[name]) for row in selected)
        result['phases'].append({'name': phase['name'], 'fps': phase['fps'], 'p95_ms': phase['p95_ms'],
                                 'frames': len(selected), 'means_per_frame': values})
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('session', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = summarize(json.loads(args.report.read_text(encoding='utf-8')), args.session)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')

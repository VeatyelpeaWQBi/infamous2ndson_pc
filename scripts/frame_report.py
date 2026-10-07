"""Summarize bounded flip-interval telemetry; never equate host waits with GPU execution."""
import csv
import math
from pathlib import Path


def summarize(session):
    session=Path(session)
    rows={}
    malformed=0
    for name in ('frames.csv.1','frames.csv'):
        path=session/name
        if not path.is_file(): continue
        with path.open(encoding='ascii',errors='replace',newline='') as stream:
            for raw in csv.DictReader(stream):
                try:
                    row={key:int(value) for key,value in raw.items()}
                    if row['interval_ns']<=0 or row['sequence']<=0: raise ValueError()
                    rows[row['sequence']]=row
                except (TypeError,ValueError,KeyError): malformed+=1
    ordered=sorted(rows.values(),key=lambda row:row['sequence'])
    marks=[]
    for name in ('input.previous.txt','input.txt'):
        path=session/name
        if path.is_file():
            for line in path.read_text(encoding='utf-8',errors='replace').splitlines():
                if line.startswith('# MARK '):
                    try: marks.append(int(line.split()[2]))
                    except (ValueError,IndexError): pass

    def stats(samples):
        if not samples: return {'frames':0}
        intervals=sorted(row['interval_ns']/1e6 for row in samples)
        percentile=lambda p: round(intervals[max(0,math.ceil(len(intervals)*p)-1)],3)
        count=len(samples)
        # Timers overlap (e.g. pipeline selection includes compilation) and may
        # span flip boundaries; report amounts, not additive CPU percentages.
        timers={key:round(sum(row.get(key,0) for row in samples)/count/1e6,3)
                for key in samples[0] if key.endswith('_ns') and key!='interval_ns'}
        return {'frames':count,'p50_ms':percentile(.5),'p95_ms':percentile(.95),
                'p99_ms':percentile(.99),'max_ms':round(intervals[-1],3),
                'over_33ms':sum(t>33 for t in intervals),'over_50ms':sum(t>50 for t in intervals),
                'over_100ms':sum(t>100 for t in intervals),'mean_timers_ms_per_flip':timers}

    # Limit summary size even if the user presses F10 repeatedly.
    return {'schema':'infamous-frame-metrics-v1','interval_source':'guest_flip_host_clock',
            'gpu_execution_time_available':False,'malformed_rows':malformed,
            'dropped_frames':max((r.get('dropped_frames',0) for r in ordered),default=0),
            'sequence_gaps':sum(max(0,b['sequence']-a['sequence']-1) for a,b in zip(ordered,ordered[1:])),
            'first_tick_ms':ordered[0]['tick_ms'] if ordered else None,
            'last_tick_ms':ordered[-1]['tick_ms'] if ordered else None,
            'overall':stats(ordered),
            'marks':[{'tick_ms':mark,
                      'before':stats([r for r in ordered if mark-2000<=r['tick_ms']<mark]),
                      'after':stats([r for r in ordered if mark<=r['tick_ms']<=mark+2000])}
                     for mark in sorted(set(marks))[-32:]]}

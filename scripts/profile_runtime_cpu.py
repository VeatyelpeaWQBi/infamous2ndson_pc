"""Explicit CPU instruction sampling of the isolated benchmark's GPU thread.

Uses an existing Win32 helper/compiler and LLVM symbolizer. Brief suspension
perturbs execution, so captures are diagnostic observations, never FPS proof.
"""
import argparse
from collections import Counter
import csv
import ctypes
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT=Path(__file__).resolve().parent.parent

def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))

def capture_inputs(session,output):
    session=Path(session).resolve();output=Path(output).resolve()
    if not session.is_relative_to(ROOT/'out/runtime-benchmark/run/debug'):
        raise ValueError('Only the active isolated benchmark can be sampled.')
    if output.suffix!='.csv' or not (output.is_relative_to(ROOT/'out/reverse-engineering') or output.is_relative_to(session)) or output.exists() or output.with_suffix('.json').exists():
        raise ValueError('Use a new output in out/reverse-engineering or the isolated session.')
    state=load(session/'status.json');manifest=load(session/'manifest.json')
    if state.get('finished') or state.get('collector')!='infamous-debug-v1' or state.get('pid')!=manifest.get('pid'):
        raise ValueError('No matching active collector-owned process.')
    executable=ROOT/'out/bb-probe.exe'
    if Path(manifest['command'][0]).resolve()!=executable.resolve():
        raise ValueError('Unexpected executable.')
    if hashlib.sha256(executable.read_bytes()).hexdigest()!=manifest['executable_sha256']:
        raise ValueError('Executable changed since launch.')
    created=state.get('latest',{}).get('created_filetime')
    if not isinstance(created,int) or created<=0:
        raise ValueError('Missing verified process creation time.')
    return state['pid'],created,manifest,output

def capture(session,output,seconds):
    if not .1<=seconds<=10:
        raise ValueError('Capture duration must be 0.1..10 seconds.')
    pid,created,manifest,output=capture_inputs(session,output)
    helper=ROOT/'out/cpu-profile.exe'
    if not helper.is_file():
        raise ValueError('Missing native helper; use build.bat --build-tests.')
    clock=ctypes.WinDLL('kernel32').GetTickCount64;clock.restype=ctypes.c_uint64
    begin=clock()
    with output.open('x',encoding='utf-8') as stream:
        # Do not kill a sampler while it may hold a thread suspension. The
        # native loop bounds capture time and resumes before any output work.
        result=subprocess.run([str(helper),str(pid),str(round(seconds*1000)),
                               str(ROOT/'out/bb-probe.exe'),str(created)],
                              stdout=stream,stderr=subprocess.PIPE,text=True)
    end=clock()
    marker={'record_type':'cpu-sampling-interval','start_tick_ms':begin,'stop_tick_ms':end,'pid':pid,'diagnostic_only':True}
    (Path(session)/('cpu-profile-'+output.stem+'.json')).write_text(json.dumps(marker)+'\n',encoding='utf-8')
    output.with_suffix('.json').write_text(json.dumps({
        'diagnostic_only':True,'seconds_requested':seconds,'pid':pid,'created_filetime':created,
        'session':str(Path(session).resolve()),'exit_code':result.returncode,'error':result.stderr,
        'executable_sha256':manifest['executable_sha256'],**marker,
        'note':'GPU thread briefly suspended to copy registers. RIP observations with prior-interval CPU progress are approximate samples, not exact CPU cost or optimization FPS.'},indent=2)+'\n',encoding='utf-8')
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f'Profiler failed: {result.returncode}')
    return output

def summarize(path):
    path=Path(path);metadata=load(path.with_suffix('.json'))
    executable=ROOT/'out/bb-probe.exe'
    data=executable.read_bytes()
    if metadata['exit_code'] or hashlib.sha256(data).hexdigest()!=metadata['executable_sha256']:
        raise ValueError('Capture incomplete or executable differs; cannot symbolize.')
    pe=struct.unpack_from('<I',data,0x3c)[0];optional=pe+24
    if data[pe:pe+4]!=b'PE\0\0' or struct.unpack_from('<H',data,optional)[0]!=0x20b:
        raise ValueError('Expected Windows x86-64 PE.')
    preferred=struct.unpack_from('<Q',data,optional+24)[0]
    image_size=struct.unpack_from('<I',data,optional+56)[0]
    lines=path.read_text(encoding='utf-8').splitlines()
    base=int(re.search(r'image_base=(0x[0-9a-f]+)',lines[0])[1],16)
    modules=[]
    for line in lines:
        match=re.match(r'# module_base=(0x[0-9a-f]+) bytes=(\d+) name=(.*)',line)
        if match: modules.append((int(match[1],16),int(match[2]),match[3]))
    rows=list(csv.DictReader(line for line in lines if not line.startswith('#')))
    active=[r for r in rows if int(r['cpu_delta_100ns'])>0]
    addresses=Counter(int(r['rip'],16)-base+preferred for r in active
                      if base<=int(r['rip'],16)<base+image_size)
    tool=Path('C:/msys64/clang64/bin/llvm-addr2line.exe')
    if not tool.is_file(): raise ValueError('Existing LLVM symbolizer unavailable.')
    unique=list(addresses);symbols={}
    for start in range(0,len(unique),128):
        batch=unique[start:start+128]
        result=subprocess.run([str(tool),'-e',str(executable),'-a','-f','-C','-i',*[hex(a) for a in batch]],
                              text=True,capture_output=True,check=True)
        current=None;frames=[]
        for line in [*result.stdout.splitlines(),'END']:
            if re.fullmatch(r'0x[0-9a-fA-F]+',line) or line=='END':
                if current is not None:
                    if len(frames)%2: raise ValueError('Unexpected LLVM inline output.')
                    chain=list(zip(frames[::2],frames[1::2]))
                    symbols[current]=next((frame for frame in chain if str(ROOT).replace('\\','/').lower() in frame[1].replace('\\','/').lower()),chain[0])
                current=None if line=='END' else int(line,16);frames=[]
            elif current is not None: frames.append(line)
    groups=Counter()
    for address,count in addresses.items(): groups[symbols[address]]+=count
    module_counts=Counter()
    for row in active:
        rip=int(row['rip'],16)
        module_counts[next((name for start,size,name in modules if start<=rip<start+size),'unknown')]+=1
    report={'diagnostic_only':True,'observations':len(rows),'with_cpu_progress':len(active),
            'engine_observations':sum(addresses.values()),
            'external_observations':len(active)-sum(addresses.values()),
            'note':metadata['note'],'module_observations':dict(module_counts),'hot_locations':[
                {'function':name,'location':location,'observations':count}
                for (name,location),count in groups.most_common(30)]}
    output=path.with_name(path.stem+'-functions.json')
    output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    return report

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='action',required=True)
    live=sub.add_parser('capture');live.add_argument('session',type=Path);live.add_argument('output',type=Path)
    live.add_argument('--seconds',type=float,default=5)
    offline=sub.add_parser('summarize');offline.add_argument('path',type=Path)
    args=parser.parse_args()
    if args.action=='capture': print(capture(args.session,args.output,args.seconds))
    else: print(json.dumps(summarize(args.path),indent=2))

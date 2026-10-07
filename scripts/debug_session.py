"""Local Windows game-session collector. Starts only the requested executable.

Uses installed Python + Win32, never downloads dependencies. Logs rotate; the
latest three completed sessions are retained. The collector never kills a game
because frames stop, and it never starts another instance to diagnose a failure.
"""
import ctypes
from ctypes import wintypes
from datetime import datetime,timezone
import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time
import uuid

MIB=1024*1024

class RotatingWriter:
    def __init__(self,path,limit,backups=1):
        if limit<2 or backups<0: raise ValueError('Invalid diagnostic log limit')
        self.path=Path(path); self.limit=limit; self.backups=backups
        self.file=self.path.open('wb'); self.size=0
    def write(self,text):
        data=text.encode('utf-8',errors='replace')
        # Keep a line bounded even if a driver prints a malformed giant message.
        data=data.rstrip(b'\r\n')[:min(self.limit-1,16383)].decode('utf-8',errors='ignore').encode('utf-8')+b'\n'
        if self.size+len(data)>self.limit:
            self.file.close()
            for index in range(self.backups,0,-1):
                previous=self.path if index==1 else self.path.with_name(self.path.name+f'.{index-1}')
                target=self.path.with_name(self.path.name+f'.{index}')
                if previous.exists(): previous.replace(target)
            self.file=self.path.open('wb'); self.size=0
        self.file.write(data); self.file.flush(); self.size+=len(data)
    def close(self): self.file.close()

def save_json(path,data):
    path=Path(path); pending=path.with_name(path.name+'.next')
    pending.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    pending.replace(path)

def read_json(path):
    try: return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError,ValueError): return {}

def heartbeat_fps(current,previous):
    """No new publication is unknown, not a zero-FPS interval."""
    elapsed=current.get('tick_ms',0)-previous.get('tick_ms',0)
    frames=current.get('presents',0)-previous.get('presents',0)
    if elapsed<=0 or frames<0: return None
    return frames*1000/elapsed

def collector_stopped(state):
    pid=state.get('collector_pid')
    if not isinstance(pid,int): return False
    try: process=ProcessMetrics(pid)
    except PermissionError as error:
        return getattr(error,'winerror',None)==87 # nonexistent PID, not denied access
    try:
        current=process.read()
        return bool(current.get('ended_filetime')) or bool(state.get('collector_created_filetime') and
            current.get('created_filetime')!=state['collector_created_filetime'])
    finally: process.close()

def prune_sessions(base,keep=3):
    """Only delete finished collector-created directories within the debug root."""
    base=Path(base).resolve()
    sessions=[]
    for path in base.glob('session-*'):
        if not path.is_dir() or path.is_symlink(): continue
        resolved=path.resolve()
        if resolved.parent!=base: continue
        state=read_json(path/'status.json')
        if state.get('collector')=='infamous-debug-v1' and (state.get('finished') or collector_stopped(state)):
            sessions.append((state.get('retain_for_debug') is True,path))
    # A selected user baseline takes one of the existing retention slots;
    # it does not add storage beyond the configured session count.
    ordered=sorted(sessions,key=lambda item:(item[0],item[1].name),reverse=True)
    for _,path in ordered[keep:]:
        # Recheck resolution immediately before recursive removal on Windows.
        if path.resolve().parent!=base: raise ValueError('Diagnostic retention path escaped root')
        shutil.rmtree(path)

class ProcessMetrics:
    class Memory(ctypes.Structure):
        _fields_=[('cb',wintypes.DWORD),('faults',wintypes.DWORD)]+[(name,ctypes.c_size_t) for name in
            ('peak_ws','ws','peak_paged','paged','peak_nonpaged','nonpaged','pagefile','peak_pagefile','private')]
    def __init__(self,pid):
        self.kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        self.psapi=ctypes.WinDLL('psapi',use_last_error=True)
        self.kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
        self.kernel.OpenProcess.restype=wintypes.HANDLE
        self.kernel.GetTickCount64.restype=ctypes.c_uint64
        self.kernel.CloseHandle.argtypes=[wintypes.HANDLE]
        self.kernel.GetProcessTimes.argtypes=[wintypes.HANDLE]+[ctypes.POINTER(wintypes.FILETIME)]*4
        self.kernel.GetProcessHandleCount.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.DWORD)]
        self.psapi.GetProcessMemoryInfo.argtypes=[wintypes.HANDLE,ctypes.POINTER(self.Memory),wintypes.DWORD]
        self.handle=self.kernel.OpenProcess(0x1000,False,pid)
        if not self.handle:
            code=ctypes.get_last_error()
            error=PermissionError(f'OpenProcess({pid}, QUERY_LIMITED_INFORMATION): Win32 {code}')
            error.winerror=code
            raise error
    def read(self):
        times=[wintypes.FILETIME() for _ in range(4)]
        data={'tick_ms':self.kernel.GetTickCount64()}
        if self.kernel.GetProcessTimes(self.handle,*(ctypes.byref(t) for t in times)):
            seconds=lambda t: ((t.dwHighDateTime<<32)|t.dwLowDateTime)/10000000
            data['cpu_seconds']=seconds(times[2])+seconds(times[3])
            ticks=lambda t: (t.dwHighDateTime<<32)|t.dwLowDateTime
            data.update(created_filetime=ticks(times[0]),ended_filetime=ticks(times[1]))
        memory=self.Memory(); memory.cb=ctypes.sizeof(memory)
        if self.psapi.GetProcessMemoryInfo(self.handle,ctypes.byref(memory),memory.cb):
            data.update(working_set_bytes=memory.ws,private_bytes=memory.private,page_faults=memory.faults)
        count=wintypes.DWORD()
        if self.kernel.GetProcessHandleCount(self.handle,ctypes.byref(count)): data['handles']=count.value
        return data
    def close(self): self.kernel.CloseHandle(self.handle)

def request_snapshot(pid):
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenEventW.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.LPCWSTR]
    kernel.OpenEventW.restype=wintypes.HANDLE
    kernel.SetEvent.argtypes=[wintypes.HANDLE]; kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    event=kernel.OpenEventW(2,False,f'Local\\bbport-dump-{pid}')
    if not event: return {'requested':False,'win32_error':ctypes.get_last_error()}
    try: return {'requested':bool(kernel.SetEvent(event))}
    finally: kernel.CloseHandle(event)

def collect(command,cwd,base,profile,env=None):
    base=Path(base).resolve(); base.mkdir(parents=True,exist_ok=True)
    prune_sessions(base,keep=2) # one new session + two completed sessions
    session=base/('session-'+datetime.now().strftime('%Y%m%d-%H%M%S-')+uuid.uuid4().hex[:6])
    session.mkdir()
    env=dict(os.environ if env is None else env)
    env.setdefault('BB_PERF_STATS','1')
    env.setdefault('BB_CAPTURE_MANUAL_ONLY','1')
    env.update(BB_DEBUG_DIR=str(session),BB_CAPTURE_FRAME=str(session/'last-frame.bmp'),
               BB_CAPTURE_INTERVAL_MS='10000',BB_CAPTURE_TRIGGER=str(session/'render-frame-request'),
               BB_CAPTURE_DIR=str(session),BB_TOGGLE_FILE=str(session/'optimizations-mask.txt'),
               BB_IMAGE_DUMP_TRIGGER=str(session/'inspect-images'),BB_IMAGE_INSPECT='1')
    # Record just the relevant non-secret configuration; never serialize all env.
    manifest={'collector':'infamous-debug-v1','started_utc':datetime.now(timezone.utc).isoformat(),
        'title_id':profile.get('title_id'),'game_sha256':profile.get('source_sha256'),
        'command':[str(v) for v in command],'python':os.sys.executable,
        'limits':{'runtime_log_bytes':8*MIB,'runtime_log_backups':1,'input_bytes':4*MIB,
                  'input_backups':1,'metrics_bytes':MIB,'metrics_backups':1,'sessions':3,
                  'gpu_operations':512,'frame_metrics_bytes':2*MIB,'frame_metrics_backups':1,
                  'performance_recording_cache_bytes':32*MIB,
                  'snapshot_max_resolution':[1920,1080],'snapshot_source_max_resolution':[4096,2160],
                  'snapshot_slots':8,'snapshot_mark_slots':4,'snapshot_followup_slots':4,'snapshot_followups':3,'snapshot_interval_ms':1000,
                  'snapshot_manual_only':env.get('BB_CAPTURE_MANUAL_ONLY')=='1'},
        'settings':{key:env.get(key) for key in ('BB_GAME_PROFILE','BB_SHADER_SOURCE','BB_SHADER_BUNDLE','BB_IMAGE_READ_MEMO','BB_FLAT_DATA_MEMO','BB_PRESENT_MODE','BB_HDR','BB_FSR1','BB_UPSCALER','BB_GPU_PROFILE','BB_PREP_PRIORITY','BB_PIPELINE_CACHE','BB_DRAW_PIPE','BB_VK_RECORD_THREAD','BB_FPS','BB_VBLANK_HZ',
            'BB_REGION','BB_LANGUAGE','BB_TIMEZONE_MINUTES','BB_ENTER_BUTTON','BB_PERF_STATS','BB_F10_DEEP')}}
    save_json(session/'manifest.json',manifest)
    executable=Path(command[0])
    if executable.is_file():
        with executable.open('rb') as binary: manifest['executable_sha256']=hashlib.file_digest(binary,'sha256').hexdigest()
        manifest['executable_bytes']=executable.stat().st_size
        save_json(session/'manifest.json',manifest)
    print(f'Debug session: {session}\nAutomatic input/GPU/process/frame recording enabled; F10 marks an issue. Use debug-control snapshot for deep capture.',flush=True)
    log=RotatingWriter(session/'runtime.log',8*MIB)
    startup=RotatingWriter(session/'startup.log',256*1024,backups=0)
    metrics_log=RotatingWriter(session/'metrics.jsonl',MIB)
    faults=[]; reader_errors=[]
    from recover_performance_recording import RecordingJournal
    recording_journal=RecordingJournal(session)
    recording_errors=[]
    try:
        child=subprocess.Popen([str(c) for c in command],cwd=cwd,env=env,
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    except BaseException as error:
        for writer in (log,startup,metrics_log): writer.close()
        save_json(session/'status.json',{'collector':'infamous-debug-v1','finished':True,'launch_error':str(error)})
        raise
    manifest['pid']=child.pid; save_json(session/'manifest.json',manifest)
    owner=ProcessMetrics(os.getpid())
    try: owner_created=owner.read().get('created_filetime')
    finally: owner.close()
    live={'collector':'infamous-debug-v1','pid':child.pid,'session':str(session),'finished':False,
        'collector_pid':os.getpid(),'collector_created_filetime':owner_created}
    save_json(base/'active.json',live); save_json(session/'status.json',live)
    def drain():
        forwarded=0; crash_lines=0; first_bytes=0
        try:
            while True:
                raw=child.stdout.readline(16384)
                if not raw: break
                line=raw.decode('utf-8',errors='replace').rstrip('\r\n')
                stamp=datetime.now(timezone.utc).isoformat(timespec='milliseconds')
                log.write(f'{stamp} {line}\n')
                recording_journal.observe(line)
                if first_bytes<240*1024:
                    startup.write(f'{stamp} {line}\n'); first_bytes+=len(raw)
                if any(token in line for token in ('Host fault','Guest fault','STOP:','DEBUG_CRASH')):
                    if len(faults)<16: faults.append(line)
                    crash_lines=100
                if forwarded<12 or crash_lines or line.startswith(('DEBUG_MARK','Thread dump','DEBUG:')):
                    print(line,flush=True); forwarded+=1
                    if crash_lines: crash_lines-=1
        except (OSError,ValueError) as error: reader_errors.append(str(error))
        finally: child.stdout.close()
    reader=threading.Thread(target=drain,name='debug-log-reader',daemon=True); reader.start()
    monitor=None; monitor_error=None; previous={}; stalled=False; last_snapshot=0
    try:
        try: monitor=ProcessMetrics(child.pid)
        except PermissionError as error:
            monitor_error=str(error); print(f'DEBUG: process monitoring unavailable: {error}',flush=True)
        while child.poll() is None:
            try: recording_journal.checkpoint()
            except (OSError,ValueError) as error:
                if not recording_errors: recording_errors.append(str(error))
            sample={'utc':datetime.now(timezone.utc).isoformat(timespec='milliseconds')}
            if monitor: sample.update(monitor.read())
            heartbeat=read_json(session/'gpu-heartbeat.json'); sample['gpu']=heartbeat
            tick=sample.get('tick_ms',0)
            elapsed=(tick-previous.get('tick_ms',tick))/1000
            if elapsed>0 and 'cpu_seconds' in sample and 'cpu_seconds' in previous:
                sample['cpu_percent_one_core']=max(0,(sample['cpu_seconds']-previous['cpu_seconds'])/elapsed*100)
                old_gpu=previous.get('gpu',{})
                if 'presents' in heartbeat and 'presents' in old_gpu:
                    # Counters are published independently of the collector's polling clock.
                    sample['present_fps']=heartbeat_fps(heartbeat,old_gpu)
            # An absent first frame is startup, not evidence of a hang.
            new_stall=bool(heartbeat.get('last_present_ms') and tick-heartbeat['last_present_ms']>15000)
            request=session/'request-snapshot'
            reason='manual' if request.exists() else 'possible-frame-stall' if new_stall and not stalled else None
            if reason and (reason=='manual' or tick-last_snapshot>60000):
                sample['snapshot']={'reason':reason,**request_snapshot(child.pid)}
                if request.exists(): request.unlink()
                last_snapshot=tick
                print(f'DEBUG: {reason}, thread snapshot {sample["snapshot"]}',flush=True)
            stalled=new_stall; sample['possible_frame_stall']=stalled
            metrics_log.write(json.dumps(sample,ensure_ascii=False)+'\n'); previous=sample
            save_json(session/'status.json',{**live,'latest':sample,'monitor_error':monitor_error})
            time.sleep(1)
    except KeyboardInterrupt:
        # A console interrupt is explicit user input; do not create a new game.
        print('DEBUG: collector interrupted; waiting for the game to exit.',flush=True)
        child.wait()
    finally:
        if monitor: monitor.close()
    status=child.wait(); reader.join(timeout=5)
    if reader.is_alive(): reader_errors.append('log reader did not finish within 5 seconds')
    if not reader.is_alive():
        for writer in (log,startup,metrics_log): writer.close()
    result={**live,'finished':True,'exit_code':status,'ended_utc':datetime.now(timezone.utc).isoformat(),
        'faults':faults[:16],'monitor_error':monitor_error,'reader_errors':reader_errors,
        'latest':previous,'abnormal_exit':status!=0}
    # Finalize outside the dead game process: works for _exit(), host/guest faults and forced
    # termination, where the in-process recording toggle/destructors cannot run.
    save_json(session/'status.json',result)
    try:
        recording_journal.checkpoint()
        from recover_performance_recording import recover, analyze
        recovered=recover(session)
        if recovered:
            result['recovered_recordings']=recovered
            for recording in recovered:
                path=session/f'performance-recording-{recording["recording_id"]}.csv'
                save_json(path.with_suffix('.summary.json'),analyze(path))
            print(f'DEBUG: automatically finalized {len(recovered)} interrupted performance recording(s).',flush=True)
    except (OSError,ValueError) as error:
        recording_errors.append(str(error))
    if recording_errors: result['recording_errors']=recording_errors
    try:
        from frame_report import summarize
        save_json(session/'performance-summary.json',summarize(session))
    except (OSError,ValueError) as error:
        result['performance_report_error']=str(error)
    save_json(session/'status.json',result); save_json(base/'active.json',result)
    print(f'Debug session ended: exit={status}; records: {session}',flush=True)
    return status

def control(base,action):
    base=Path(base).resolve(); active=read_json(base/'active.json')
    session=Path(active.get('session',base)).resolve()
    if session.parent!=base or not session.name.startswith('session-'):
        raise RuntimeError('No valid debug session')
    if action=='status': print(json.dumps(read_json(session/'status.json'),ensure_ascii=False,indent=2)); return 0
    if active.get('finished') or collector_stopped(active): raise RuntimeError('Game session collector has already ended')
    (session/'request-snapshot').write_text('manual',encoding='ascii')
    (session/'capture-next').touch()
    (session/'render-frame-request').touch()
    print('Thread snapshot requested; it will appear in runtime.log.'); return 0

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['status','snapshot'])
    parser.add_argument('--base',type=Path,default=Path(__file__).resolve().parent.parent/'out/CUSA00309/debug')
    args=parser.parse_args()
    try: raise SystemExit(control(args.base,args.action))
    except (OSError,RuntimeError) as error: parser.exit(1,f'{error}\n')

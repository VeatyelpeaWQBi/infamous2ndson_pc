"""Bounded Windows Second Son diagnosis using existing prepared images and tools.

Keeps the user's normal game-test.log and profile intact. No preparation,
downloads or source-game writes. A timeout is a diagnostic result, not success.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
from game_profiles import select_profile,native_environment
from windows_tools import require_windows,msys_root

ROOT=Path(__file__).resolve().parent.parent

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu',action='store_true')
    parser.add_argument('--continue-game',action='store_true',help='press circle once at 15 seconds using the existing pad replay')
    parser.add_argument('--seconds',type=int,default=20)
    parser.add_argument('--log',type=Path,default=ROOT/'out/infamous-open-native.log')
    args=parser.parse_args()
    require_windows()
    if not 1<=args.seconds<=120: parser.error('seconds must be between 1 and 120')
    if args.continue_game and (not args.gpu or args.seconds<=16):
        parser.error('--continue-game requires --gpu and more than 16 seconds')
    game=ROOT/'patches/CUSA00309'; out=ROOT/'out/CUSA00309'
    for path in (ROOT/'out/bb-probe.exe',out/'boot-linked.bin',out/'content.bin',out/'patches.bin'):
        if not path.is_file(): parser.error(f'Missing existing artifact: {path}')
    env=dict(os.environ); native_environment(select_profile(game),ROOT,env)
    state=ROOT/'out/infamous-diagnostic-profile'
    env.update(BB_USER_DIR=str(state/'user'),BB_GPU_USER_DIR=str(state/'gpu'),BB_CONFIG=str(state/'settings.ini'))
    if args.gpu: env['BB_CAPTURE_FRAME']=str(ROOT/'out/infamous-presented.bmp')
    env['PATH']=str(msys_root()/'clang64/bin')+os.pathsep+env.get('PATH','')
    for name in ('user','gpu'): (state/name).mkdir(parents=True,exist_ok=True)
    if args.continue_game:
        replay=state/'continue-replay.txt'; trigger=state/'continue-input.txt'
        replay.write_text('0 0 128 128 128 128 0 0\n15000 8192 128 128 128 128 0 0\n15500 0 128 128 128 128 0 0\n',encoding='ascii')
        trigger.write_text('replay',encoding='ascii')
        env.update(BB_PAD_REPLAY=str(replay),BB_PAD_FILE=str(trigger))
    command=[str(ROOT/'out/bb-probe.exe'),str(out/'boot-linked.bin'),
        '--content-profile',str(out/'content.bin'),'--patches',str(out/'patches.bin'),
        '--app0',str(game),'--user',env['BB_USER_DIR'],'--timeout','0']
    if not args.gpu: command.append('--cpu-only')
    args.log.parent.mkdir(parents=True,exist_ok=True)
    with args.log.open('wb') as output:
        try:
            result=subprocess.run(command,cwd=ROOT,env=env,stdout=output,stderr=subprocess.STDOUT,timeout=args.seconds)
            status=result.returncode
        except subprocess.TimeoutExpired:
            status=124
    print(f'Diagnostic exit: {status}; log: {args.log}')
    lines=args.log.read_text(encoding='utf-8',errors='replace').splitlines()
    print('\n'.join(lines[-55:]))
    return status

if __name__=='__main__': sys.exit(main())

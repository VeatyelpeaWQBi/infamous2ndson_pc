#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows launcher: prepare the game image, compile the patches and start
out/bb-probe.exe. Uses existing Windows Python and the MSYS2 CLANG64 toolchain.

    run.bat [--game-dir DIR] [bb-probe options...]

The game folder: --game-dir, else BB_GAME_DIR, else the last one used (out/game_dir.txt), else
game/CUSA00309 inside this project.
Unless BB_PREBUILT=1
the port is (re)built first through MSYS2 (build.sh in the CLANG64 environment)."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
from mods import remove_overlay
from windows_tools import build as windows_build, msys_root, require_windows
from game_profiles import select_profile, native_environment, project_game_dir
from settings_defaults import DEFAULTS

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / 'scripts'
PYTHON = sys.executable


def run(arguments, capture=False, check=True, env=None):
    child_env = dict(os.environ if env is None else env)
    child_env['PYTHONIOENCODING'] = 'utf-8'
    result = subprocess.run([str(a) for a in arguments], cwd=ROOT, env=child_env,
                            stdout=subprocess.PIPE if capture else None, text=True, encoding='utf-8')
    if check and result.returncode:
        sys.exit(result.returncode)
    return result.stdout.strip() if capture else result.returncode


def build():
    """build.sh in a CLANG64 login shell (its clang, cmake, ninja and pkg-config)."""
    status = windows_build()
    if status:
        sys.exit(status)


def settings_value(config, key):
    if not config.is_file():
        return DEFAULTS.get(key)
    for line in config.read_text(errors='replace').splitlines():
        name, _, value = line.partition('=')
        if name.strip() == key:
            return value.strip()
    return DEFAULTS.get(key)


def main():
    require_windows()
    arguments = sys.argv[1:]
    game = os.environ.get('BB_GAME_DIR')
    if arguments[:1] == ['--game-dir'] and len(arguments) > 1:
        game, arguments = arguments[1], arguments[2:]
    data = Path(os.environ.get('BB_DATA_DIR', ROOT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = Path(os.environ['BB_CONFIG'])
    if 'BB_FSR411_DIR' not in os.environ and not (ROOT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        os.environ['BB_FSR411_DIR'] = str(data / 'fsr4_411')
    # The last folder that worked is remembered, so run.bat alone starts the game afterwards.
    remembered = out / 'game_dir.txt'
    if not game and remembered.is_file():
        game = remembered.read_text(encoding='utf-8').strip()
    game = Path(game) if game else project_game_dir(ROOT)
    if not (game / 'eboot.bin').is_file():
        sys.exit(f'No eboot.bin in {game} (pass --game-dir or set BB_GAME_DIR).')
    original = game.resolve()
    profile = select_profile(original)
    os.environ['BB_GAME_DIR'] = str(original)
    native_environment(profile, data, os.environ)
    if profile['id'] == 'infamous':
        out = out / profile['title_id']
        out.mkdir(parents=True, exist_ok=True)
        config = Path(os.environ['BB_CONFIG'])
    print(f'Game profile: {profile["id"]} / {profile["title_id"]}', flush=True)
    if profile['id']=='infamous':
        print(f'Console locale: {os.environ["BB_REGION"]}, language={os.environ["BB_LANGUAGE"]}, '
              f'UTC offset={os.environ["BB_TIMEZONE_MINUTES"]} minutes, confirm={os.environ["BB_ENTER_BUTTON"]}',flush=True)
    remembered.write_text(str(original), encoding='utf-8')
    os.environ['BB_GAME_DIR'] = str(original)
    # The in-game menu's "Apply and restart" runs this launcher again (probe.c runtime_restart).
    os.environ['BB_RESTART_COMMAND'] = subprocess.list2cmdline([PYTHON, str(Path(__file__).resolve()), *sys.argv[1:]])

    merged = original if profile['id'] == 'infamous' else Path(run([PYTHON, SCRIPTS / 'mods.py', original, '--out', out,
                       '--mods-dir', os.environ.get('BB_MODS_DIR', data / 'mods'),
                       '--config', os.environ.get('BB_MODS_CONFIG', data / 'mods.json'),
                       '--enabled', os.environ.get('BB_MODS_ENABLED', '1')], capture=True))
    overlay = merged if merged.resolve() != original else None
    try:
        for script, extra in (('prepare.py', []), ('link_modules.py', ['--scoped-imports'] if profile['id']=='infamous' else []),
                              ('content_profile.py', ['--sku', os.environ.get('BB_CONTENT_SKU', 'full')])):
            run([PYTHON, SCRIPTS / script, merged, '--out', out, *extra])
        # Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if os.environ.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                os.environ.pop(key, None)
        fps = os.environ.get('BB_FPS', 'uncap')
        scaled_render = scaled_output = None
        if profile['address_patches'] and not os.environ.get('BB_RENDER_RES'):
            sizes = run([PYTHON, SCRIPTS / 'patches.py', '--print-scaled', '--settings', config], capture=True, check=False)
            if sizes and len(sizes.split()) == 2:
                scaled_render, scaled_output = sizes.split()
        prebuilt = os.environ.get('BB_PREBUILT') == '1'
        if not prebuilt:
            build()
        live = '0'
        if scaled_output:
            live = os.environ.get('BB_LIVE_RES') or settings_value(config, 'live_resolution')
            if live == 'auto':
                caps = ROOT / 'out/bb-gpu-capabilities.exe'
                live = run([caps, '--live-resolution'], capture=True, check=False) or '0'
            live = '1' if live == '1' else '0'
        if live == '1':
            print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
        elif scaled_output:
            os.environ.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
            os.environ.setdefault('BB_DMEM_MB', '9152')
            print(f'Output {scaled_output}: scene {scaled_render}, direct memory {os.environ["BB_DMEM_MB"]} MiB '
                  '(live_resolution=1: live changes)')
        if not profile['address_patches']:
            import struct
            (out / 'patches.bin').write_bytes(struct.pack('<8sQQ', b'BBPATCH2', 0x400000, 0))
        else:
            run([PYTHON, SCRIPTS / 'patches.py', '--out', out, '--fps', fps, '--extra', os.environ.get('BB_PATCHES', ''),
             '--settings', config, '--game-dir', merged, '--render-res', os.environ.get('BB_RENDER_RES', ''),
             '--output-res', os.environ.get('BB_OUTPUT_RES', ''),
             '--patches-dir', os.environ.get('BB_PATCHES_DIR', data / 'patches'),
             '--patches-config', os.environ.get('BB_PATCHES_CONFIG', data / 'patches.json')])
        os.environ.setdefault('BB_VBLANK_HZ', {'uncap': '0', '90': '90'}.get(fps, '60'))
        probe = ROOT / os.environ.get('BB_PROBE', ROOT / 'out/bb-probe.exe')
        command = [probe, out / 'boot-linked.bin', '--content-profile', out / 'content.bin',
                   '--patches', out / 'patches.bin', '--app0', merged,
                   '--user', os.environ.get('BB_USER_DIR', data / 'user'),
                   '--timeout', os.environ.get('BB_TIMEOUT', '0'), *arguments]
        # MSYS2's DLLs (libc++, SDL3, FFmpeg, ...). System32 is searched before PATH, so the
        # Vulkan loader stays the one installed with the GPU driver.
        os.environ['PATH'] = os.pathsep.join([str(msys_root() / 'clang64/bin'), os.environ.get('PATH', '')])
        if profile['id'] == 'infamous':
            Path(os.environ['BB_GPU_USER_DIR']).mkdir(parents=True, exist_ok=True)
            Path(os.environ['BB_USER_DIR']).mkdir(parents=True, exist_ok=True)
        print('Starting:', ' '.join(shlex.quote(str(c)) for c in command), flush=True)
        try:
            if os.environ.get('BB_DEBUG_SESSION')=='1':
                from debug_session import collect
                status=collect(command,ROOT,out/'debug',profile)
            else:
                status = subprocess.call([str(c) for c in command], cwd=ROOT)
        except KeyboardInterrupt:
            status = 130
        return status
    finally:
        if overlay:
            remove_overlay(overlay)


if __name__ == '__main__':
    sys.exit(main())

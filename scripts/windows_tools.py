"""Windows-only build/test entry points; never install a toolchain or edit global PATH."""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
NATIVE_TESTS = ('bb-probe', 'pad-test', 'runtime-test', 'file-mods-test', 'sema-test', 'content-test')
UNIT_TESTS = ('motion-history-test', 'ui-composition-test', 'upscaler-support-test', 'motion-shader-test')
GPU_TESTS = ('scene-resolution-test', 'taa-shader-test', 'camera-motion-test')


def require_windows():
    if os.name != 'nt' or struct.calcsize('P') != 8:
        raise RuntimeError('Use 64-bit Windows Python on Windows 10/11; WSL/Linux are unsupported.')
    if sys.version_info < (3, 12):
        raise RuntimeError('Use an existing Python 3.12 or newer (BB_PYTHON can select its executable).')


def msys_root():
    configured = os.environ.get('BB_MSYS2')
    if configured:
        return Path(configured)
    clang = shutil.which('clang')
    if clang:
        path = Path(clang).resolve()
        if path.parent.name.lower() == 'bin' and path.parent.parent.name.lower() == 'clang64':
            return path.parent.parent.parent
    return Path(r'C:\msys64')


def tool_environment():
    env = dict(os.environ)
    env['PATH'] = os.pathsep.join([str(msys_root() / 'clang64/bin'), env.get('PATH', '')])
    return env


def check_environment():
    """Read-only path checks: no subprocesses, downloads or build directories."""
    print(f'Windows Python: {sys.executable}')
    root = msys_root()
    print(f'MSYS2 root: {root}')
    required = [root / 'usr/bin/bash.exe']
    required += [root / 'clang64/bin' / name for name in
                 ('clang.exe', 'clang++.exe', 'ld.lld.exe', 'cmake.exe', 'ninja.exe', 'pkg-config.exe')]
    sources = [ROOT / 'gpu/third_party/fsr-vulkan/CMakeLists.txt',
               ROOT / 'gpu/third_party/imgui/imgui.h',
               ROOT / 'third_party/LibAtrac9/C/src']
    missing = []
    for path in required + sources:
        present = path.is_file() or (path.is_dir() and any(path.glob('*.c')))
        print(f'{"FOUND" if present else "MISSING"}: {path}')
        if not present:
            missing.append(path)
    print('Path checks only: package versions, linkability and GPU initialization remain unverified.')
    return 1 if missing else 0


def build(build_tests=False, allow_downloads=False):
    bash = msys_root() / 'usr/bin/bash.exe'
    if not bash.is_file():
        raise RuntimeError(f'Existing MSYS2 not found: {bash}. Set BB_MSYS2; no tools were installed.')
    env = tool_environment()
    env.update(MSYSTEM='CLANG64', CHERE_INVOKING='1', MSYS2_PATH_TYPE='inherit', BB_PROJECT_ROOT=str(ROOT),
               BB_ALLOW_DOWNLOADS='1' if allow_downloads else '0')
    if allow_downloads:
        print('Downloads enabled: missing submodules and CMake sources may be fetched into this checkout.', flush=True)
    command = 'cd -- "$(cygpath -u "$BB_PROJECT_ROOT")" && bash build.sh'
    if build_tests:
        command += ' --build-tests'
    return subprocess.call([str(bash), '-lc', command], cwd=ROOT, env=env)


def test(python_only=False, gpu=False):
    if python_only and gpu:
        raise RuntimeError('--python-only and --gpu cannot be combined.')
    if not python_only:
        required = [ROOT / 'out' / (name + '.exe') for name in NATIVE_TESTS]
        required += [ROOT / 'out/gpu' / (name + '.exe') for name in UNIT_TESTS + (GPU_TESTS if gpu else ())]
        required += [ROOT / 'out/gpu/CTestTestfile.cmake']
        missing = [str(path) for path in required if not path.is_file()]
        if missing:
            raise RuntimeError('Missing test artifacts; run build.bat --build-tests first:\n' + '\n'.join(missing))
    env = tool_environment()
    command = [sys.executable, str(ROOT / 'scripts/windows_tests.py')]
    if python_only:
        command.append('--python-only')
    status = subprocess.call(command, cwd=ROOT, env=env)
    if status or python_only:
        return status
    # Detailed runtime/error-path cases are exercised by the Python suite above.
    for name in ('pad-test', 'file-mods-test'):
        status = subprocess.call([str(ROOT / 'out' / (name + '.exe'))], cwd=ROOT, env=env)
        if status:
            return status
    ctest = msys_root() / 'clang64/bin/ctest.exe'
    if not ctest.is_file():
        raise RuntimeError(f'Existing CTest not found: {ctest}')
    for label in ('unit', 'gpu') if gpu else ('unit',):
        status = subprocess.call([str(ctest), '--test-dir', str(ROOT / 'out/gpu'),
                                  '-L', label, '--output-on-failure', '--no-tests=error'], cwd=ROOT, env=env)
        if status:
            return status
    if not gpu:
        print('GPU integration tests were not requested; use test.bat --gpu to run them.')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    build_parser = commands.add_parser('build')
    build_parser.add_argument('--check', action='store_true', help='read-only tool/source path check')
    build_parser.add_argument('--build-tests', action='store_true')
    build_parser.add_argument('--allow-downloads', action='store_true', help='explicitly permit missing source downloads')
    test_parser = commands.add_parser('test')
    test_parser.add_argument('--python-only', action='store_true')
    test_parser.add_argument('--gpu', action='store_true', help='also execute Vulkan integration tests on a real device')
    args = parser.parse_args()
    require_windows()
    if args.command == 'build':
        if args.check:
            if args.build_tests or args.allow_downloads:
                parser.error('--check cannot be combined with build/download options')
            return check_environment()
        return build(args.build_tests, args.allow_downloads)
    return test(args.python_only, args.gpu)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        sys.exit(1)

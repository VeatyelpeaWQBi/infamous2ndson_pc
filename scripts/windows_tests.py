"""Run Windows Python tests with explicit omissions and no silent native-test skips."""
import argparse
import os
from pathlib import Path
import sys
import unittest
from windows_tools import require_windows, tool_environment

ROOT = Path(__file__).resolve().parent.parent
NATIVE_CLASSES = ('test_probe.LoaderTests.', 'test_runtime.RuntimeTests.',
                  'test_cpu_profile.NativeCpuProfilerTests.',
                  'test_debug_session.NativeDiagnosticsTests.',
                  'test_infamous.InfamousNativeTests.',
                  'test_sema.SemaphoreTests.', 'test_content.ContentTests.')


def cases(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from cases(item)
        else:
            yield item


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python-only', action='store_true')
    args = parser.parse_args()
    require_windows()
    # Keep native test children on the same MSYS2 CLANG64 runtime path even
    # when this module is started directly from VS Code or Explorer.  The
    # normal test.bat wrapper already supplies this environment; doing it here
    # prevents direct invocations from producing Windows "missing DLL" dialogs.
    os.environ.update(tool_environment())
    sys.path.insert(0, str(ROOT / 'tests'))
    suite = unittest.TestSuite()
    omitted = 0
    print('Excluded module: test_packaged_vulkan (legacy Linux/AppImage packaging).', flush=True)
    for path in sorted((ROOT / 'tests').glob('test_*.py')):
        if path.stem == 'test_packaged_vulkan':
            continue
        discovered = unittest.defaultTestLoader.loadTestsFromName(path.stem)
        for case in cases(discovered):
            if args.python_only and case.id().startswith(NATIVE_CLASSES):
                omitted += 1
            else:
                suite.addTest(case)
    if args.python_only:
        print(f'Explicitly omitted {omitted} native executable cases (--python-only).', flush=True)
    if not suite.countTestCases():
        print('ERROR: No tests selected.', file=sys.stderr)
        return 1
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(f'Python tests: ran={result.testsRun}, failures={len(result.failures)}, '
          f'errors={len(result.errors)}, skipped={len(result.skipped)}, omitted={omitted}')
    if result.skipped:
        print('ERROR: Unexpected skipped tests; this is not a complete pass.', file=sys.stderr)
    return 0 if result.wasSuccessful() and not result.skipped else 1


if __name__ == '__main__':
    sys.exit(main())

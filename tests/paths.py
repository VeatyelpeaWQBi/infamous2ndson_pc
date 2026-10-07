"""Repository paths and Windows executable names for the test suite."""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))

# Tests are often started directly by a VS Code test adapter.  Native targets
# use MSYS2's CLANG64 runtime DLLs, so make their search path deterministic
# before any subprocess is created.  This is process-local and does not edit
# the user's Windows PATH.
if os.name == 'nt':
    from windows_tools import tool_environment
    os.environ.update(tool_environment())


def native_executable(name):
    return ROOT / 'out' / (name + '.exe')

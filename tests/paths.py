"""Repository paths and Windows executable names for the test suite."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))


def native_executable(name):
    return ROOT / 'out' / (name + '.exe')

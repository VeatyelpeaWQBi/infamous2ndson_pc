"""Windows launcher defaults from the same table used by the engine/setup."""
from pathlib import Path
import re

def read_defaults(path=None):
    path=Path(path) if path is not None else Path(__file__).with_name('bbport_settings_table.inc')
    result={}
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line.startswith('BB_SETTING'): continue
        entry=re.fullmatch(r'BB_SETTING\("(\w+)", "([^"\r\n]*)"\)',line)
        if entry is None: raise ValueError(f'Invalid setting in {path}')
        key,value=entry.groups()
        if key in result: raise ValueError(f'Duplicate setting {key} in {path}')
        result[key]=value
    return result

DEFAULTS=read_defaults()

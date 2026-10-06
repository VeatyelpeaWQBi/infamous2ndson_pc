"""Read-only PS4 dump inventory and static import triage; never launch or patch a game."""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
import re

from link_modules import module, FS_LOAD
from prepare import inspect_libc, nid, sfo

ROOT = Path(__file__).resolve().parent.parent


def code_text(path):
    text = path.read_text(encoding='utf-8', errors='replace')
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)


def runtime_inventory():
    """Candidates only: source registration is not proof of implemented behavior."""
    explicit, lookup_names, hints = {}, set(), {}
    known = (ROOT / 'src/import_names.inc').read_text(encoding='utf-8')
    for scoped, name in re.findall(r'\{"([^"]+)","([^"]+)"\}', known):
        hints[scoped.split('#')[0]] = name
    # Name hints verified against the official shadPS4 Videodec/Pad/PlayGo
    # registrations. They do not add implementations or change coverage.
    external_hints = ('sceVideodecCreateDecoder', 'sceVideodecDecode',
        'sceVideodecDeleteDecoder', 'sceVideodecFlush', 'sceVideodecQueryResourceInfo',
        'sceVideodecReset', 'scePadSetLightBar', 'scePlayGoClose', 'scePlayGoGetEta',
        'scePlayGoGetProgress', 'scePlayGoGetToDoList', 'scePlayGoSetLanguageMask',
        'scePlayGoSetToDoList', 'scePlayGoTerminate', 'sceNpRegisterGamePresenceCallbackA')
    for name in external_hints:
        hints.setdefault(nid(name), name)
    for path in sorted((ROOT / 'src').glob('runtime*.c')):
        text = code_text(path)
        for scoped in re.findall(r'"([A-Za-z0-9+\-]{11}#[^"\s]+)"', text):
            explicit[scoped] = str(path.relative_to(ROOT)).replace('\\', '/')
        for block in re.findall(r'RuntimeExport\s+\w+\[\]\s*=\s*\{(.*?)\};', text, re.S):
            for name in re.findall(r'\{\s*"([^"]+)"', block):
                lookup_names.add(name)
                hints.setdefault(nid(name), name)
    legacy = dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', known))
    gpu = {}
    registered = ['gnmdriver/gnmdriver.cpp', 'videoout/video_out.cpp',
                  'kernel/equeue.cpp', 'avplayer/avplayer.cpp']
    for relative in registered:
        path = ROOT / 'gpu/shadps4/core/libraries' / relative
        for value, library, version, owner, function in re.findall(
                r'LIB_FUNCTION\(\s*"([^"]+)",\s*"([^"]+)",\s*(\d+),\s*"([^"]+)",\s*(\w+)\s*\)',
                code_text(path)):
            gpu[value] = dict(library=library, version=int(version), module=owner,
                              function=function, source=str(path.relative_to(ROOT)).replace('\\', '/'))
            hints.setdefault(value, function)
    return explicit, lookup_names, legacy, gpu, hints


def referenced_imports(parsed):
    indices = sorted({sym for _, kind, sym, _ in parsed['relocs']
                      if kind in (1, 6, 7) and not parsed['symbols'][sym]['section']})
    return [parsed['symbols'][index] for index in indices]


def audit(game):
    explicit, lookup_names, legacy, gpu, hints = runtime_inventory()
    host_prefixes = {name.split('#')[0] for name in explicit}
    paths = [game / 'eboot.bin', *sorted((game / 'sce_module').glob('*.prx'))]
    parsed = {str(path.relative_to(game)).replace('\\', '/'): module(path) for path in paths}
    main = parsed['eboot.bin']
    main_libs = {value: key for key, value in main['libraries'].items()}
    main_mods = {value: key for key, value in main['modules'].items()}
    exports, libc_nids = {}, collections.defaultdict(list)
    for filename, data in parsed.items():
        if filename == 'eboot.bin':
            continue
        for symbol in data['symbols']:
            if symbol['section'] and symbol['binding'] in (1, 2) and symbol['identity']:
                exports[symbol['identity']] = filename
                if symbol['identity'][1][0] == 'libc':
                    libc_nids[symbol['identity'][0]].append(filename)
    imports, modules = {}, []
    for filename, data in parsed.items():
        used = referenced_imports(data)
        for symbol in used:
            identity = symbol['identity']
            if identity is None:
                continue
            value, library, owner = identity
            canonical = (f'{value}#{main_libs[library]}#{main_mods[owner]}'
                         if library in main_libs and owner in main_mods else f'{value}#{library[0]}')
            if identity in imports:
                imports[identity]['referenced_by'].append(filename)
                continue
            native = exports.get(identity)
            if not native and library[0] == 'libSceLibcInternal' and len(libc_nids[value]) == 1:
                native = libc_nids[value][0]
            c_candidate = canonical in explicit or legacy.get(canonical) in lookup_names
            gpu_candidate = value in gpu
            scope_candidate = value in host_prefixes or hints.get(value) in lookup_names
            kind = ('host_registration_candidate' if c_candidate else
                    'gpu_registration_candidate' if gpu_candidate else
                    'native_export_candidate' if native else
                    'host_scope_adaptation_candidate' if scope_candidate else
                    'unresolved_in_static_inventory')
            imports[identity] = dict(nid=value, name_hint=hints.get(value),
                original_name=symbol['name'], canonical_name=canonical,
                library=library[0], library_version=library[1],
                module=owner[0], module_version=owner[1], symbol_type=symbol['type'],
                classification=kind, native_export=native, referenced_by=[filename],
                scoped_name_mismatch_candidate=(not c_candidate and scope_candidate))
        loads = [p for p in data['ph'] if p['type'] in (1, 0x61000010)]
        tls = next((p for p in data['ph'] if p['type'] == 7), None)
        fs_loads = sum(data['elf'][p['offset']:p['offset']+p['filesz']].count(FS_LOAD)
                       for p in loads if p['flags'] & 1)
        modules.append(dict(file=filename, sha256=data['sha256'], file_bytes=(game / filename).stat().st_size,
            entry=data['header'][4], memory_bytes=max(p['vaddr']+p['memsz'] for p in loads),
            load_segments=loads, tls=tls, fs_thread_pointer_loads=fs_loads,
            referenced_import_symbols=len(used),
            libraries={key:list(value) for key,value in data['libraries'].items()},
            relocation_counts=dict(collections.Counter(str(kind) for _,kind,_,_ in data['relocs'])),
            missing_segments=data['missing']))
    records = sorted(imports.values(), key=lambda row:(row['library'], row['nid']))
    by_library = {}
    for row in records:
        counts = by_library.setdefault(row['library'], collections.Counter())
        counts[row['classification']] += 1
    assets, total, examples = collections.Counter(), 0, []
    for path in sorted((game / 'art').rglob('*')):
        if not path.is_file():
            continue
        assets[path.suffix.lower() or '<none>'] += 1
        total += path.stat().st_size
        if len(examples) < 4:
            with path.open('rb') as stream:
                prefix = stream.read(16).hex()
            examples.append(dict(file=str(path.relative_to(game)).replace('\\', '/'),
                                 bytes=path.stat().st_size, first_16_bytes_hex=prefix))
    metadata = sfo((game / 'sce_sys/param.sfo').read_bytes())
    return dict(schema_version=1, game_directory=str(game),
        sfo={key:metadata.get(key) for key in ('TITLE','TITLE_ID','APP_VER','VERSION','CONTENT_ID','SYSTEM_VER','CATEGORY')},
        modules=modules, libc_proof=inspect_libc(game / 'sce_module/libc.prx'),
        unique_import_identities=len(records),
        classification_counts=dict(collections.Counter(row['classification'] for row in records)),
        imports_by_library=by_library, imports=records,
        assets=dict(root='art', file_count=sum(assets.values()), bytes=total,
                    extensions=dict(assets), samples=examples),
        limitations=['Static candidates are not runtime resolution or gameplay compatibility proof.',
                     'Only asset names, sizes and four short headers were read; resource integrity was not verified.',
                     'No game launch, patch application, full memory-image output or save writes performed.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--gaps-csv', type=Path)
    args = parser.parse_args()
    report = audit(args.game.resolve())
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    if args.gaps_csv:
        args.gaps_csv.parent.mkdir(parents=True, exist_ok=True)
        fields = ('library', 'module', 'nid', 'name_hint', 'symbol_type', 'referenced_by')
        with args.gaps_csv.open('w', encoding='utf-8-sig', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for row in report['imports']:
                if row['classification'] == 'unresolved_in_static_inventory':
                    output = {key:row[key] for key in fields}
                    output['referenced_by'] = ';'.join(row['referenced_by'])
                    writer.writerow(output)
    summary = {key:report[key] for key in ('sfo','unique_import_identities','classification_counts','imports_by_library','assets')}
    summary['modules'] = [{key:row[key] for key in ('file','sha256','file_bytes','memory_bytes','referenced_import_symbols','fs_thread_pointer_loads')}
                          for row in report['modules']]
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()

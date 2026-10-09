"""Read-only comparison of original GCN shaders and runtime SPIR-V for measured hotspots."""
import argparse
import ctypes
from collections import Counter
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess


def decode_runtime_bundle(data, source, checksum):
    """Validate the native BBVKPK01 codec; never resurrect loose-file entries."""
    limit = 64 * 1024 * 1024
    if len(data) < 24 or len(data) > limit or data[:8] != b'BBVKPK01':
        raise ValueError('Invalid runtime shader bundle header or size')
    stored, count, version = struct.unpack_from('<QII', data, 8)
    if stored != source or version != 1 or count > 32768:
        raise ValueError('Runtime shader bundle source/version mismatch')
    at, blobs = 24, {}
    for _ in range(count):
        if at + 16 > len(data):
            raise ValueError('Truncated runtime shader bundle entry')
        namesize, size, digest = struct.unpack_from('<IIQ', data, at)
        at += 16
        if not 1 <= namesize <= 128 or at + namesize + size > len(data):
            raise ValueError('Invalid runtime shader bundle entry bounds')
        name = data[at:at + namesize].decode('ascii')
        at += namesize
        blob = data[at:at + size]
        at += size
        if '..' in name or not re.fullmatch(r'[a-z0-9_.]+', name) or name in blobs:
            raise ValueError('Invalid or duplicate runtime shader bundle name')
        if checksum(blob) != digest:
            raise ValueError('Runtime shader bundle checksum mismatch')
        blobs[name] = blob
    if at != len(data):
        raise ValueError('Trailing runtime shader bundle bytes')
    return blobs


def existing_xxh3(tools):
    library = ctypes.CDLL(str((tools / 'libxxhash.dll').resolve()))
    function = library.XXH3_64bits
    function.argtypes = (ctypes.c_char_p, ctypes.c_size_t)
    function.restype = ctypes.c_uint64
    return lambda data: function(data, len(data))


def extract_codes(data):
    """Validate the runtime's BinaryInfo signature and its code-start backreference."""
    signatures = {}
    cursor = 0
    while (cursor := data.find(b'OrbShdr', cursor)) >= 0:
        if cursor + 28 <= len(data):
            flags, = struct.unpack_from('<I', data, cursor + 8)
            shader_hash, = struct.unpack_from('<Q', data, cursor + 16)
            signatures[cursor] = (shader_hash, flags >> 8)
        cursor += 7
    result = {}
    cursor = 0
    while (cursor := data.find(b'\xff\x03\xeb\xbe', cursor)) >= 0:
        if cursor + 8 <= len(data):
            offset, = struct.unpack_from('<I', data, cursor + 4)
            info = signatures.get(cursor + (offset + 1) * 8)
            if info:
                shader_hash, length = info
                if length >= 8 and length % 4 == 0 and cursor + length <= len(data):
                    result[shader_hash] = {'offset': cursor, 'bytes': data[cursor:cursor + length]}
        cursor += 4
    return result


def spirv_counts(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError('Truncated SPIR-V')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    if words[0] != 0x07230203:
        raise ValueError('Invalid SPIR-V magic')
    at, counts = 5, Counter()
    while at < len(words):
        length, opcode = words[at] >> 16, words[at] & 0xffff
        if not length or at + length > len(words):
            raise ValueError('Invalid SPIR-V instruction bounds')
        counts[opcode] += 1
        at += length
    return {'instructions': sum(counts.values()), 'loads': counts[61], 'stores': counts[62],
            'control_barriers': counts[224], 'memory_barriers': counts[225],
            'atomic_instructions': sum(counts[i] for i in range(227, 243)),
            'loops': counts[246]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--runtime-dumps', type=Path, help='Original GCN captured by the engine, including cached programs')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--tools', type=Path, default=Path('C:/msys64/clang64/bin'))
    parser.add_argument('hashes', nargs='+', type=lambda text: int(text, 16))
    args = parser.parse_args()
    # Never export into the input directories, including their parents.
    output = args.out.resolve()
    for input_path in (args.source.resolve().parent, args.cache.resolve()):
        if output.is_relative_to(input_path) or input_path.is_relative_to(output):
            raise ValueError('Output must be separate from source and cache')
    for name in ('spirv-dis.exe', 'spirv-val.exe'):
        if not (args.tools / name).is_file():
            raise ValueError(f'Missing existing tool: {args.tools / name}')
    data = args.source.read_bytes()
    codes = extract_codes(data)
    args.out.mkdir(parents=True, exist_ok=True)
    report = {'source': str(args.source.resolve()), 'source_sha256': hashlib.sha256(data).hexdigest(),
              'original_shader_count': len(codes), 'shaders': [],
              'note': 'Static instruction counts are not runtime GPU costs. Raw GCN is exported for the engine decoder; LLVM does not support gfx700 disassembly.'}
    bundle_path = args.cache / 'all_shaders.vkpack'
    bundle = None
    if bundle_path.is_file():
        checksum = existing_xxh3(args.tools)
        bundle = decode_runtime_bundle(bundle_path.read_bytes(), checksum(data), checksum)
        report['runtime_bundle'] = {'path': str(bundle_path.resolve()), 'entries': len(bundle),
                                    'authoritative': True}
    for shader_hash in args.hashes:
        entry = {'hash': f'{shader_hash:016x}', 'original_found': shader_hash in codes, 'variants': []}
        if shader_hash not in codes and args.runtime_dumps:
            matches = sorted(args.runtime_dumps.glob(f'*_0x{shader_hash:016x}_0.bin'))
            if len(matches) == 1:
                raw = matches[0].read_bytes()
                if len(raw) < 8 or len(raw) % 4 or raw[:4] != b'\xff\x03\xeb\xbe':
                    raise ValueError(f'Invalid original GCN dump: {matches[0]}')
                codes[shader_hash] = {'offset': None, 'bytes': raw}
                entry.update(original_found=True, original_capture=str(matches[0].resolve()))
        if shader_hash in codes:
            code = codes[shader_hash]['bytes']
            entry.update(original_offset=codes[shader_hash]['offset'], original_bytes=len(code),
                         original_sha256=hashlib.sha256(code).hexdigest())
            (args.out / f'{shader_hash:016x}.gcn').write_bytes(code)
            entry['gcn_decoder'] = 'Use image-compat-test --gcn-disassemble; raw bytes are not decompiled pseudocode.'
        if bundle is not None:
            # The engine does not fall back to stale loose files on a bundle miss.
            modules = [(name, blob, 'bundle') for name, blob in bundle.items()
                       if re.fullmatch(rf'0x{shader_hash:016x}_[0-9]+\.spv', name)]
        else:
            modules = [(path.name, path.read_bytes(), 'loose')
                       for path in args.cache.glob(f'0x{shader_hash:016x}_*.spv')]
        for name, spv, origin in sorted(modules):
            path = args.cache / name if origin == 'loose' else args.out / name
            if origin == 'bundle':
                path.write_bytes(spv)  # Selected diagnostic exports only, never cache inputs.
            validation = subprocess.run([str(args.tools / 'spirv-val.exe'), '--target-env', 'vulkan1.3', str(path)],
                                        text=True, capture_output=True, check=False)
            subprocess.run([str(args.tools / 'spirv-dis.exe'), str(path), '-o', str(args.out / (path.stem + '.spvasm'))], check=True)
            entry['variants'].append({'file': path.name, 'origin': origin, 'sha256': hashlib.sha256(spv).hexdigest(),
                                      'bytes': len(spv), 'validation_exit': validation.returncode,
                                      'validation_error': validation.stderr, **spirv_counts(spv)})
        report['shaders'].append(entry)
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

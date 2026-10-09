import struct
import unittest
from analyze_particle_shaders import extract_codes, spirv_counts, decode_runtime_bundle


class ParticleAnalysisTests(unittest.TestCase):
    @staticmethod
    def bundle(entries, source=42, version=1):
        output = b'BBVKPK01' + struct.pack('<QII',source,len(entries),version)
        for name, blob in entries:
            raw = name.encode()
            output += struct.pack('<IIQ',len(raw),len(blob),sum(blob)) + raw + blob
        return output

    def test_bundle_native_layout_and_multiple_entries(self):
        entries = [('0x0000000075453dfa_0.spv',b'\x00\xff'), ('other.meta',b'\x03')]
        self.assertEqual(decode_runtime_bundle(self.bundle(entries),42,sum),dict(entries))
        self.assertEqual(decode_runtime_bundle(self.bundle([]),42,sum),{})

    def test_bundle_rejects_stale_source_corruption_and_unsafe_entries(self):
        good = self.bundle([('a.spv',b'\x07')])
        invalid = [good[:-1],good+b'\0',good[:-1]+b'\x08',
                   self.bundle([('../a.spv',b'x')]),self.bundle([('a/b.spv',b'x')]),
                   self.bundle([('a.spv',b'x'),('a.spv',b'y')]),
                   self.bundle([('a.spv',b'x')],version=2),self.bundle([],source=43)]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ValueError):
                decode_runtime_bundle(value,42,sum)
        for end in range(24):
            with self.assertRaises(ValueError): decode_runtime_bundle(good[:end],42,sum)

    def test_original_shader_requires_matching_header_backreference(self):
        data = bytearray(64)
        struct.pack_into('<II', data, 0, 0xbeeb03ff, 3)
        data[32:39] = b'OrbShdr'
        struct.pack_into('<I', data, 40, 16 << 8)
        struct.pack_into('<Q', data, 48, 0x12345678)
        self.assertEqual(extract_codes(data)[0x12345678]['bytes'], data[:16])
        struct.pack_into('<I', data, 4, 7)  # No matching metadata at the indicated address.
        self.assertEqual(extract_codes(data), {})

    def test_truncated_shader_and_spirv_are_rejected(self):
        self.assertEqual(extract_codes(b'OrbShdr'), {})
        for words in ([0x07230203, 0, 0, 0, 0, 61],
                      [0x07230203, 0, 0, 0, 0, (8 << 16) | 61]):
            with self.assertRaises(ValueError):
                spirv_counts(struct.pack('<' + 'I' * len(words), *words))

    def test_spirv_opcode_counts_skip_operands(self):
        words = [0x07230203, 0x10600, 0, 8, 0, (4 << 16) | 61, 224, 225, 246,
                 (1 << 16) | 224, (1 << 16) | 246]
        counts = spirv_counts(struct.pack('<' + 'I' * len(words), *words))
        self.assertEqual(counts['loads'], 1)
        self.assertEqual(counts['control_barriers'], 1)
        self.assertEqual(counts['memory_barriers'], 0)
        self.assertEqual(counts['loops'], 1)


if __name__ == '__main__':
    unittest.main()

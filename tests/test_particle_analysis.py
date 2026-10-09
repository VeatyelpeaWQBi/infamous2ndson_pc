import struct
import unittest
from analyze_particle_shaders import extract_codes, spirv_counts


class ParticleAnalysisTests(unittest.TestCase):
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

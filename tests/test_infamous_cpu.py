"""Title-specific CPU patch guards must fail before changing prepared images."""
import struct
import unittest
from infamous_cpu import protect_text_leaf,protect_animation_leaf,jump,EBOOT_SHA256,END,ANIMATION_END

class CpuPatchGuards(unittest.TestCase):
    def test_other_executable_is_untouched(self):
        image=bytearray(b'unchanged'); segments=[(0,9,5)]
        self.assertEqual(protect_text_leaf(image,segments,'another executable'),0)
        self.assertEqual(protect_animation_leaf(image,segments,'another executable'),0)
        self.assertEqual(image,b'unchanged'); self.assertEqual(segments,[(0,9,5)])

    def test_changed_original_function_is_rejected_without_writes(self):
        image=bytearray(END); segments=[(0,END,5)]
        before=bytes(image)
        with self.assertRaisesRegex(ValueError,'function bytes'):
            protect_text_leaf(image,segments,EBOOT_SHA256)
        self.assertEqual(image,before); self.assertEqual(segments,[(0,END,5)])

    def test_changed_animation_function_is_rejected_without_writes(self):
        image=bytearray(ANIMATION_END); segments=[(0,ANIMATION_END,5)]
        before=bytes(image)
        with self.assertRaisesRegex(ValueError,'function bytes'):
            protect_animation_leaf(image,segments,EBOOT_SHA256)
        self.assertEqual(image,before); self.assertEqual(segments,[(0,ANIMATION_END,5)])

    def test_trampoline_branch_addresses_in_both_directions(self):
        for source,target in ((0x1cf4e0,0x8300000),(0x8300050,0x1cfb4e)):
            encoded=jump(source,target)
            self.assertEqual(encoded[0],0xe9)
            self.assertEqual(source+5+struct.unpack('<i',encoded[1:])[0],target)

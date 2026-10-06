"""Cross-file local IDs, library versions and function/data kind separation."""
from paths import ROOT
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import link_modules
from test_probe import package


class ScopedModuleTests(unittest.TestCase):
    def fixture(self, export_type=2, version=1):
        identity=('188x57JYp0g',('libkernel',1),('libkernel',257))
        main=dict(identity=lambda name:identity,libraries={'b':('libkernel',1)},
            modules={'T':('libkernel',257)},ph=[dict(type=0x61000001,vaddr=256)])
        exported=('188x57JYp0g',('libkernel',version),('libkernel',257))
        native=dict(elf=b'\x31\xc0\xc3'+bytes(13)+b'\xc3',
            ph=[dict(type=1,vaddr=0,memsz=4096,filesz=17,offset=0,flags=5)],tags={12:0},relocs=[],
            symbols=[dict(name='188x57JYp0g#X#Y',type=export_type,binding=1,section=1,
                          value=16,size=1,identity=exported)],sha256='fixture')
        return main,native

    def link(self, export_type=2, version=1):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp)
            (out/'boot.bin').write_bytes(package(b'\xc3',names=['188x57JYp0g#b#T'],
                                                relocs=[(8,1,0,0)],capabilities=1))
            with patch.object(link_modules,'module',side_effect=self.fixture(export_type,version)):
                link_modules.link(Path('fixture'),out,['libc.prx'],scoped_imports=True)
            return json.loads((out/'link.json').read_text()), (out/'boot-linked.bin').read_bytes()

    def test_local_ids_are_replaced_by_verified_identity(self):
        report,image=self.link()
        self.assertEqual(report['bindings'],1)
        self.assertIn(b'188x57JYp0g#libkernel:1#libkernel:257#F\0',image)
        self.assertEqual(report['import_identities'][0]['symbol_type'],2)

    def test_same_nid_data_export_cannot_satisfy_function_import(self):
        report,_=self.link(export_type=1)
        self.assertEqual(report['bindings'],0)

    def test_wrong_library_version_cannot_satisfy_import(self):
        report,_=self.link(version=2)
        self.assertEqual(report['bindings'],0)

    def test_unsupported_symbol_kind_rejected(self):
        with self.assertRaisesRegex(ValueError,'identity/type'):
            link_modules.scoped_name(('188x57JYp0g',('libkernel',1),('libkernel',257)),6)

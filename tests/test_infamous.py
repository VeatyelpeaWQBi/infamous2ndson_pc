from paths import ROOT, native_executable
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

EXE=native_executable('infamous-test')

def chunk_fixture(count=71):
    attrs=256; ids=attrs+count*32; mattrs=ids+count*2
    data=bytearray(mattrs+count*16)
    struct.pack_into('<I',data,0,0x6f676c70)
    struct.pack_into('<HH',data,10,count,count)
    struct.pack_into('<I',data,16,len(data))
    struct.pack_into('<4I',data,192,attrs,count*32,ids,count*2)
    struct.pack_into('<2I',data,216,mattrs,count*16)
    for i in range(count):
        struct.pack_into('<H',data,attrs+i*32+14,1)
        struct.pack_into('<I',data,attrs+i*32+24,i*2)
        struct.pack_into('<H',data,ids+i*2,i)
        struct.pack_into('<Q',data,mattrs+i*16+8,i+1)
    return data

@unittest.skipUnless(EXE.exists(),'run build.bat --build-tests first')
class InfamousNativeTests(unittest.TestCase):
    def run_case(self,*args):
        result=subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_scoped_resolver_rejects_wrong_library_versions_and_symbol_kinds(self):
        self.run_case('--identities')

    def test_event_flags_wake_cancel_and_delete_blocked_threads(self):
        self.run_case('--eventflags')

    def test_playgo_71_chunks_uses_real_sizes_and_handles(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp); (path/'sce_sys').mkdir()
            (path/'sce_sys/playgo-chunk.dat').write_bytes(chunk_fixture())
            self.run_case('--playgo',str(path))

    def test_playgo_rejects_out_of_bounds_chunk_tables(self):
        data=chunk_fixture()
        struct.pack_into('<I',data,192,len(data)+4)
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp); (path/'sce_sys').mkdir()
            (path/'sce_sys/playgo-chunk.dat').write_bytes(data)
            self.run_case('--bad-playgo',str(path))

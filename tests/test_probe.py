"""Boundary tests for the native loader; uses tiny synthetic x86-64 images."""
from paths import ROOT, native_executable
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

EXE = native_executable('bb-probe')


def package(code, relocs=(), names=(), capabilities=None):
    image = code.ljust(4096, b'\0')
    header = struct.pack('<8sQQQQQ', b'BBPROBE1' if capabilities is None else b'BBPROBE2', len(image), 0, 1, len(relocs), len(names))
    if capabilities is not None:
        header += struct.pack('<Q', capabilities)
    segment = struct.pack('<QQQ', 0, len(image), 5)
    return (header + segment + b''.join(n.encode().ljust(128, b'\0') for n in names)
            + b''.join(struct.pack('<QQqq', *r) for r in relocs) + image)


def native_package(name='fixture-native', binding_address=4112, binding_kind=1,
                   lib_flags=5, init=b'\x31\xc0\xc3', native=b'\xc3', metadata=None):
    # Main calls an import then tail-jumps to the terminal diagnostic import.
    code = b'\x48\x83\xec\x08\xff\x15\x16\0\0\0\x48\x83\xc4\x08\xff\x25\x14\0\0\0'
    image = code.ljust(4096,b'\0') + init.ljust(16,b'\0') + native
    image = image.ljust(12288,b'\0')
    header=struct.pack('<8s6Q',b'BBPROBE3',len(image),0,3,2,2,1)
    meta=metadata or (4096,8192,4096,8192,32,4,1,256)
    return (header + struct.pack('<8Q',*meta) + struct.pack('<3Q',0,binding_address,binding_kind)
            + struct.pack('<9Q',0,4096,5,4096,4096,lib_flags,8192,4096,6)
            + name.encode().ljust(128,b'\0') + b'after-native'.ljust(128,b'\0')
            + struct.pack('<8Q',32,1,0,0,40,1,1,0) + image)


@unittest.skipUnless(EXE.exists(), 'run build.bat --build-tests first')
class LoaderTests(unittest.TestCase):
    def test_reserved_red_zone_preserves_locals_and_incoming_argument_across_extrq(self):
        import hashlib
        from infamous_cpu import _reserve_leaf_red_zone
        # An actual native leaf keeps a sentinel below RSP and in an ordinary
        # local, executes EXTRQ, then checks both values and the caller's argument.
        # Deterministic writes below the *current* RSP model asynchronous host
        # stack use: actual exception delivery need not overwrite it every time.
        sentinel=0x123456789abcdef0
        frame=0x28
        code=bytearray(); allocations=[]; locals_=[]; arguments=[]
        def emit(value,sites=None):
            value=bytes.fromhex(value)
            if sites is not None: sites.append((len(code),value))
            code.extend(value)
        emit('4881ec28000000',allocations)
        code.extend(b'\x48\xb9'+struct.pack('<Q',sentinel))
        emit('48894c24f8',locals_)
        emit('48894c2408')
        emit('66480f6ec1')
        for displacement in ('e0','d0','c0','b0'):
            emit('c5f8294424'+displacement,locals_)
        emit('660f78c0080c')
        emit('c5f157c9') # vxorpd xmm1,xmm1,xmm1
        for displacement in ('e0','d0','c0','b0'):
            emit('c5f8294c24'+displacement) # host-clobber model, not a guest local
        emit('488b4424f8',locals_)
        emit('4839c8 0f95c2 488b442408 4839c8 0f95c0 08c2')
        for displacement in ('e0','d0','c0','b0'):
            emit('c5f8284424'+displacement,locals_)
            emit('66480f7ec0 4839c8 0f95c0 08c2')
        emit('488b842430000000',arguments)
        emit('4839c8 0f95c0 08d0 0fb6c0')
        emit('4881c428000000',allocations)
        emit('c3')
        wrapper=bytes.fromhex('4883ec08')+b'\x48\xb8'+struct.pack('<Q',sentinel)+bytes.fromhex('48890424')
        wrapper+=b'\xe8'+struct.pack('<i',128-len(wrapper)-5)
        wrapper+=bytes.fromhex('4883c408 85c0 7506')
        wrapper+=b'\xff\x25'+struct.pack('<i',64-len(wrapper)-6)+b'\x0f\x0b'
        for protected in (False,True):
            with self.subTest(protected=protected):
                image=bytearray(wrapper.ljust(128,b'\0')+code)
                if protected:
                    _reserve_leaf_red_zone(image,[],128,len(image),hashlib.sha256(code).hexdigest(),
                                           frame,allocations,locals_,arguments)
                result=self.run_image(package(bytes(image),[(64,1,0,0)],['stack-locals-preserved']))
                self.assertEqual(result.returncode,20 if protected else 139,result.stdout+result.stderr)
                if protected:
                    self.assertIn('first unsupported PS4 import: stack-locals-preserved',result.stdout)

    def test_sse4a_extract_register_and_immediate_forms(self):
        def movq_to_xmm(reg, value):
            return (b'\x48\xb8'+struct.pack('<Q',value)+
                    bytes([0x66,0x48|(4 if reg>=8 else 0),0x0f,0x6e,0xc0|((reg&7)<<3)]))
        for dst in range(16):
            for immediate in (False,True):
                for length,index in ((8,12),(0,0),(1,63),(63,1),(32,48),(0,8)):
                    with self.subTest(dst=dst,immediate=immediate,length=length,index=index):
                        src=(dst+1)%16
                        value=0xfedcba9876543210
                        expected=(value>>index)&((1<<(length or 64))-1)
                        code=movq_to_xmm(dst,value)
                        if not immediate: code+=movq_to_xmm(src,length|(index<<8))
                        code+=b'\xf9' # stc: extraction must preserve flags
                        rex=0x40|((1 if dst>=8 else 0) if immediate else
                                  (4 if dst>=8 else 0)|(1 if src>=8 else 0))
                        code+=b'\x66'+(bytes([rex]) if rex!=0x40 else b'')+b'\x0f'
                        if immediate: code+=bytes([0x78,0xc0|(dst&7),length,index])
                        else: code+=bytes([0x79,0xc0|((dst&7)<<3)|(src&7)])
                        # Capture carry before any flag-changing comparisons.
                        code+=b'\x0f\x93\xc1' # setnc cl
                        code+=bytes([0x66,0x48|(4 if dst>=8 else 0),0x0f,0x7e,0xc0|((dst&7)<<3)])
                        code+=b'\x48\xba'+struct.pack('<Q',expected)
                        code+=bytes.fromhex('4839d00f95c008c80fb6c0c3')
                        r=self.run_image(native_package(init=code,binding_address=4096+len(code)))
                        self.assertEqual(r.returncode,20,r.stdout+r.stderr)
                        self.assertIn('Module 0 initializer returned 0',r.stdout)

    def test_guest_thread_pointer_reads_preserve_each_destination_register(self):
        for reg in range(16):
            with self.subTest(register=reg):
                # Preserve SysV nonvolatile registers, and save/restore RSP when
                # deliberately using it as the TLS load's destination.
                pushes=bytes.fromhex('53554154415541564157')
                pops=bytes.fromhex('415f415e415d415c5d5b')
                code=pushes
                if reg==4: code+=bytes.fromhex('4989e3') # mov r11,rsp
                code+=bytes([0x65,0x48|(4 if reg>=8 else 0),0x8b,4|((reg&7)<<3),0x25,0,0,0,0])
                code+=bytes([0x48|(4 if reg>=8 else 0),0x89,0xc0|((reg&7)<<3)]) # mov rax,destination
                if reg==4: code+=bytes.fromhex('4c89dc') # mov rsp,r11
                code+=bytes.fromhex('483b000f95c00fb6c0')+pops+b'\xc3' # TCB self equals pointer -> zero
                result=self.run_image(native_package(init=code,binding_address=4096+max(16,len(code))))
                self.assertEqual(result.returncode,20,result.stdout+result.stderr)
                self.assertIn('Module 0 initializer returned 0',result.stdout)

    def test_invalid_content_profile_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'content.bin'
            for data in (b'BBCONT01',struct.pack('<8s5I',b'BBCONT01',2,0,0,0,0),
                         struct.pack('<8s5I',b'BBCONT01',3,0,0,0,0)+b'extra'):
                with self.subTest(data=data):
                    path.write_bytes(data)
                    r=self.run_image(package(b'\xc3'),'--content-profile',str(path))
                    self.assertEqual(r.returncode,1,r.stdout+r.stderr)
                    self.assertIn('content profile',r.stderr)

    def run_image(self, data, *options):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'boot.bin'
            path.write_bytes(data)
            return subprocess.run([str(EXE.resolve()), str(path), '--cpu-only', *options],
                                  capture_output=True, text=True, timeout=5)

    def test_original_instruction_reaches_named_import(self):
        # jmp [rip+2]; two padding bytes; relocated function pointer at +8
        r = self.run_image(package(b'\xff\x25\x02\0\0\0\x90\x90', [(8, 1, 0, 0)], ['fixture-import']))
        self.assertEqual(r.returncode, 20, r.stderr)
        self.assertIn('first unsupported PS4 import: fixture-import', r.stdout)

    def test_native_initializer_and_export_return(self):
        r=self.run_image(native_package())
        self.assertEqual(r.returncode,20,r.stdout+r.stderr)
        self.assertIn('Module 0 initializer returned 0',r.stdout)
        self.assertIn('first unsupported PS4 import: after-native',r.stdout)

    def test_host_contract_takes_priority_over_native_export(self):
        r=self.run_image(native_package(name='bzQExy189ZI#q#q',native=b'\x0f\x0b'))
        self.assertEqual(r.returncode,20,r.stdout+r.stderr)
        self.assertIn('_init_env returned',r.stdout)
        self.assertIn('first unsupported PS4 import: after-native',r.stdout)

    def test_strict_mode_skips_native_initialization_and_binding(self):
        r=self.run_image(native_package(init=b'\x0f\x0b'), '--strict-imports')
        self.assertEqual(r.returncode,20,r.stdout+r.stderr)
        self.assertIn('first unsupported PS4 import: fixture-native',r.stdout)
        self.assertNotIn('Starting native libc',r.stdout)

    def test_native_metadata_and_bindings_are_validated(self):
        for kwargs,message in [
            ({'binding_address':12288},'invalid native binding'),
            ({'binding_kind':2},'native export kind/range mismatch'),
            ({'lib_flags':4},'native function is not executable'),
            ({'metadata':(4096,8192,4096,8192,32,33,1,256)},'invalid linked module metadata'),
            ({'metadata':(4096,8192,4096,8192,32,4,1,12280)},'unmapped procparam'),
        ]:
            with self.subTest(kwargs=kwargs):
                r=self.run_image(native_package(**kwargs))
                self.assertEqual(r.returncode,1,r.stdout+r.stderr)
                self.assertIn(message,r.stderr)

    def test_failed_native_initializer_does_not_enter_game(self):
        r=self.run_image(native_package(init=b'\xb8\x01\0\0\0\xc3'))
        self.assertEqual(r.returncode,1,r.stdout+r.stderr)
        self.assertIn('module initializer failed',r.stderr)
        self.assertNotIn('Entering original',r.stdout)

    def test_base_relative_address_is_relocated(self):
        # jump via a relative relocation to code at +16, then to an import at +24
        code = b'\xff\x25\x02\0\0\0\x90\x90' + b'\0'*8 + b'\xff\x25\x02\0\0\0\x90\x90'
        r = self.run_image(package(code, [(8, 0, 16, 0), (24, 1, 0, 0)], ['after-relative-jump']))
        self.assertEqual(r.returncode, 20, r.stderr)
        self.assertIn('after-relative-jump', r.stdout)

    def test_out_of_bounds_relocation_is_rejected(self):
        r = self.run_image(package(b'\xc3', [(4092, 0, 0, 0)]))
        self.assertEqual(r.returncode, 1)
        self.assertIn('bad relocation', r.stderr)

    def test_truncated_image_is_rejected(self):
        r = self.run_image(package(b'\xc3')[:-1])
        self.assertEqual(r.returncode, 1)
        self.assertIn('incorrect memory image size', r.stderr)

    def test_illegal_instruction_reports_guest_offset(self):
        r = self.run_image(package(b'\x0f\x0b'))
        # The Windows exception handler reports the native exception code and
        # terminates fatal faults with 139 (the POSIX SIGILL status is not used).
        self.assertEqual(r.returncode, 139, r.stderr)
        self.assertIn('Guest fault 0xc000001d at guest+0x0', r.stderr)

    def test_runtime_returns_from_verified_init_env(self):
        # Align stack, call _init_env through +32, then tail-jump to unknown +40.
        code = b'\x48\x83\xec\x08\xff\x15\x16\0\0\0\x48\x83\xc4\x08\xff\x25\x14\0\0\0'
        data = package(code, [(32,1,0,0),(40,1,1,0)], ['bzQExy189ZI#q#q','after-init'], 1)
        r = self.run_image(data)
        self.assertEqual(r.returncode,20,r.stderr)
        self.assertIn('first unsupported PS4 import: after-init',r.stdout)
        self.assertIn('_init_env=1',r.stdout)
        strict = self.run_image(data,'--strict-imports')
        self.assertEqual(strict.returncode,20,strict.stderr)
        self.assertIn('first unsupported PS4 import: bzQExy189ZI#q#q',strict.stdout)

    def test_unverified_runtime_stays_disabled(self):
        r = self.run_image(package(b'\xff\x25\x02\0\0\0\x90\x90',[(8,1,0,0)],['bzQExy189ZI#q#q'],0))
        self.assertEqual(r.returncode,20,r.stderr)
        self.assertIn('_init_env=0',r.stdout)

    def test_unknown_capabilities_rejected(self):
        r = self.run_image(package(b'\xc3',capabilities=2))
        self.assertEqual(r.returncode,1)
        self.assertIn('unknown runtime capabilities',r.stderr)


if __name__ == '__main__':
    unittest.main()

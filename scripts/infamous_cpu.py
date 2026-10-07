"""Audited Windows stack reservations for Second Son's leaf functions.

The text vertex leaf and animation decoder use data below RSP. Windows exception
delivery can overwrite that System V red zone, including during EXTRQ emulation.
Reserve the data explicitly, preserving stack arguments and SIMD alignment.
Original control flow and RIP-relative references retain their virtual addresses.
Only the exact executable AND complete original function bytes are accepted.
"""
import hashlib
import struct

EBOOT_SHA256='2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab'
START,END=0x1cf4e0,0x1cfb4e
FUNCTION_SHA256='c7fb78d36b8e74b004fd2a350f625ce4c399cbb0483c87ca53b2a89eb74fbd54'

def jump(source,target):
    return b'\xe9'+struct.pack('<i',target-source-5)


ANIMATION_START,ANIMATION_END=0x892a80,0x894886
ANIMATION_SHA256='9d47555a97b39e7cfd6cc8107712720cb5b514069779d623cb6b7c6b51927b75'


def _reserve_leaf_red_zone(image,segments,start,end,digest,frame,allocations,locals_,arguments):
    """Expand an audited leaf frame without moving its ordinary local operands.

    Negative RSP operands move into the new 128-byte tail. Incoming arguments
    move by 128; internal positive operands keep their offsets. Each expanded
    disp8 instruction runs in a rel32 trampoline, preserving flags/registers.
    Callers supply decoded instruction boundaries, guarded by a complete digest.
    """
    original=bytes(image[start:end])
    if hashlib.sha256(original).hexdigest()!=digest:
        raise ValueError('Second Son red-zone function bytes do not match audited executable')
    thunk_base=(len(image)+4095)&~4095
    thunks=bytearray()
    changes=[]
    def expect(offset,old):
        if original[offset:offset+len(old)]!=old:
            raise ValueError('red-zone instruction mismatch')
    for offset,old in allocations:
        expect(offset,old)
        if old[:3] not in (b'\x48\x81\xec',b'\x48\x81\xc4') or struct.unpack('<I',old[3:])[0]!=frame:
            raise ValueError('invalid leaf frame allocation')
        changes.append((offset,old[:3]+struct.pack('<I',frame+128)))
    for offset,old in locals_:
        expect(offset,old)
        if len(old)<5 or old[-2]!=0x24 or old[-3]&0xc7!=0x44 or old[-1]<0x80:
            raise ValueError('invalid negative RSP operand')
        displacement=struct.unpack('b',old[-1:])[0]
        # SIMD locals keep their 16-byte alignment. The frame size includes
        # the return-address alignment skew and need not be a multiple of 16.
        relocated=((frame+128)&~15)+displacement
        if relocated<frame:
            raise ValueError('red-zone locals exceed aligned reserved space')
        replacement=old[:-3]+bytes([old[-3]+0x40,0x24])+struct.pack('<i',relocated)
        target=thunk_base+len(thunks)
        thunks.extend(replacement)
        thunks.extend(jump(thunk_base+len(thunks),start+offset+len(old)))
        changes.append((offset,jump(start+offset,target)+b'\x90'*(len(old)-5)))
    for offset,old in arguments:
        expect(offset,old)
        displacement=struct.unpack('<I',old[-4:])[0]
        if old[-5]!=0x24 or old[-6]&0xc7!=0x84 or displacement<frame:
            raise ValueError('invalid incoming stack argument')
        changes.append((offset,old[:-4]+struct.pack('<I',displacement+128)))
    covered=set()
    for offset,data in changes:
        positions=set(range(offset,offset+len(data)))
        if covered&positions or offset<0 or offset+len(data)>len(original):
            raise ValueError('Overlapping or out-of-range stack fixes')
        covered|=positions
    if len(thunks)>4096:
        raise ValueError('red-zone trampolines exceed reserved page')
    image.extend(bytes(thunk_base-len(image)))
    image.extend(thunks.ljust(4096,b'\x90'))
    segments.append((thunk_base,4096,5))
    for offset,data in changes:
        image[start+offset:start+offset+len(data)]=data
    return len(locals_)+len(arguments)


def protect_animation_leaf(image,segments,eboot_sha256):
    """Protect the EXTRQ animation decoder's constants from Windows exceptions."""
    if eboot_sha256!=EBOOT_SHA256:
        return 0
    def sites(rows):
        return [(int(address,16)-ANIMATION_START,bytes.fromhex(code))
                for address,code in (row.split() for row in rows.strip().splitlines())]
    return _reserve_leaf_red_zone(image,segments,ANIMATION_START,ANIMATION_END,ANIMATION_SHA256,0x3d8,
        sites('''892a8a 4881ecd8030000
                 894871 4881c4d8030000'''),
        sites('''892a9a 48894c24f8
                 892b02 c5f8294424e0
                 892b0d c5f97f4424d0
                 892b18 c5f97f4424c0
                 892b2a c5f97f4424b0
                 892bbd c5c1fe7c24d0
                 89326f c5e1fe6424c0
                 893485 488b6c24f8
                 89423d c5f9fa4424b0
                 894269 c5f8284424e0'''),
        sites('''892aad 8b8c2438040000
                 892ab4 448b842430040000
                 892ada 488b8c2420040000
                 892b1e 8b9c2418040000
                 892bf5 488b842420040000
                 892c5c 488bb42410040000
                 892d1c 488b842428040000
                 89480a 488b8c2440040000'''))

def protect_text_leaf(image,segments,eboot_sha256):
    if eboot_sha256!=EBOOT_SHA256:
        return 0
    original=bytes(image[START:END])
    if hashlib.sha256(original).hexdigest()!=FUNCTION_SHA256:
        raise ValueError('Second Son red-zone function bytes do not match audited executable')
    thunk_base=(len(image)+4095)&~4095
    thunks=bytearray()
    changes=[]
    def redirect(offset,old,replacement,returns=True):
        if original[offset:offset+len(old)]!=old or len(old)<5:
            raise ValueError('red-zone instruction mismatch')
        dest=thunk_base+len(thunks)
        thunks.extend(replacement)
        if returns: thunks.extend(jump(thunk_base+len(thunks),START+offset+len(old)))
        changes.append((offset,jump(START+offset,dest)+b'\x90'*(len(old)-5)))
    prologue=bytes.fromhex('55415741564155415453')
    redirect(0,prologue,prologue+bytes.fromhex('488d6424b0')) # lea rsp,[rsp-80]
    epilogue=bytes.fromhex('5b415c415d415e415f5dc3')
    redirect(len(original)-len(epilogue),epilogue,bytes.fromhex('488d642450')+epilogue,False)
    # Complete set of decoded negative RSP-relative operand encodings. The
    # whole-function digest guards instruction boundaries and occurrence counts.
    patterns='''48897424e0 48897c24e8 c5fa114c24fc 488b4424e8 48897c24d0
        48894c24c8 48895c24b0 48894424d8 c5fa114c24c4 48897c24b8
        44897c24c0 48894424f0 4c8b6c24e8 480f487424d8 488b7424c8
        480f487424d0 488b7c24f0 c5fa107424fc 488b7424e0 448b7c24c0
        488b7c24b8 488b5c24b0 c5fa104c24fc c5fa104c24c4 c5fa104424fc'''
    operands=0
    for pattern in patterns.split():
        old=bytes.fromhex(pattern); at=0
        while (at:=original.find(old,at))>=0:
            changes.append((at,old[:-1]+bytes([(old[-1]+80)&255])))
            operands+=1; at+=len(old)
    if operands!=28:
        raise ValueError(f'Unexpected red-zone operand count: {operands}')
    for old,new in [('448b742438','448bb42488000000'),('488b6c2440','488bac2490000000')]:
        old=bytes.fromhex(old)
        if original.count(old)!=1: raise ValueError('Unexpected stack argument access')
        redirect(original.index(old),old,bytes.fromhex(new))
    # Validate the complete patch before mutating anything.
    covered=set()
    for offset,data in changes:
        positions=set(range(offset,offset+len(data)))
        if covered&positions: raise ValueError('Overlapping stack fixes')
        covered|=positions
    image.extend(bytes(thunk_base-len(image)))
    image.extend(thunks.ljust(4096,b'\x90'))
    segments.append((thunk_base,4096,5))
    for offset,data in changes: image[START+offset:START+offset+len(data)]=data
    return operands+2

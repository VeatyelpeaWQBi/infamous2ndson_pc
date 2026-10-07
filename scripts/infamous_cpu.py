"""Audited Windows stack reservation for Second Son's text vertex leaf function.

The original leaf uses 80 bytes below RSP, including its output pointer at -32.
Windows exception delivery can overwrite that System V red zone. Reserve those
bytes explicitly, adjust every stack operand, and release them on the sole exit.
Control-flow offsets and RIP-relative data references stay at their original VAs.
Only the exact executable AND original function bytes are accepted.
"""
import hashlib
import struct

EBOOT_SHA256='2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab'
START,END=0x1cf4e0,0x1cfb4e
FUNCTION_SHA256='c7fb78d36b8e74b004fd2a350f625ce4c399cbb0483c87ca53b2a89eb74fbd54'

def jump(source,target):
    return b'\xe9'+struct.pack('<i',target-source-5)

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

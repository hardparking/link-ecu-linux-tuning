# Work around Rosetta occasionally leaving the CPU in 32-bit mode after the
# 32->64 far jump into wow64cpu's syscall_32to64 / unix_call_32to64.
# Each thunk now targets a 32/64-bit polyglot stub that re-issues the mode
# switch (far return to cs 0x2b) if it finds itself still in 32-bit mode.
#
# Usage: python3 patch_wow64cpu.py <Wine.app>/Contents/Resources/wine/lib/wine/x86_64-windows/wow64cpu.dll
#
# Binary patch for Gcenx's macOS Wine builds (tested: wine-devel 11.18; the
# byte checks also pass on 11.8). It asserts the expected bytes before
# writing anything, so on a different build it fails instead of corrupting
# the DLL. Patch a copy of the Wine app; a Wine update replaces the DLL.
import struct, sys
path = sys.argv[1]
d = bytearray(open(path, 'rb').read())
TEXT_RVA, TEXT_RAW = 0x1000, 0x1000
def foff(rva): return rva - TEXT_RVA + TEXT_RAW

def stub(at, target):
    s  = bytes.fromhex('31c9')          # xor ecx,ecx
    s += bytes.fromhex('4189d2')        # 32: inc ecx; mov edx,edx | 64: mov r10d,edx
    s += bytes.fromhex('85c9')          # test ecx,ecx
    s += bytes.fromhex('740d')          # jz L64 (64-bit mode)
    s += bytes.fromhex('e800000000')    # 32: call next
    s += bytes.fromhex('59')            #     pop ecx         (ecx = at+14)
    s += bytes.fromhex('8d4908')        #     lea ecx,[ecx+8] (ecx = L64)
    s += bytes.fromhex('6a2b')          #     push 0x2b
    s += bytes.fromhex('51')            #     push ecx
    s += bytes.fromhex('cb')            #     retf -> 0x2b:L64
    assert len(s) == 22
    s += b'\xe9' + struct.pack('<i', target - (at + 22 + 5))   # L64: jmp target
    return s

SYS_ENTRY, UNIX_ENTRY = 0x110c, 0x1210
SYS_STUB, UNIX_STUB = 0x1d90, 0x1db0
assert d[foff(SYS_ENTRY):foff(SYS_ENTRY)+3] == b'\x49\x87\xe6'
assert d[foff(UNIX_ENTRY):foff(UNIX_ENTRY)+3] == b'\x49\x87\xe6'
assert set(d[foff(0x1d86):foff(0x1e00)]) == {0xcc}, "cave not int3 padding"
d[foff(SYS_STUB):foff(SYS_STUB)+27] = stub(SYS_STUB, SYS_ENTRY)
d[foff(UNIX_STUB):foff(UNIX_STUB)+27] = stub(UNIX_STUB, UNIX_ENTRY)

def retarget_lea(at, old, new):   # lea reg, [rip+disp32] (7 bytes)
    disp = struct.unpack_from('<i', d, foff(at) + 3)[0]
    assert at + 7 + disp == old, hex(at + 7 + disp)
    struct.pack_into('<i', d, foff(at) + 3, new - (at + 7))
retarget_lea(0x140e, SYS_ENTRY, SYS_STUB)    # thunk->syscall_thunk.addr
retarget_lea(0x1430, UNIX_ENTRY, UNIX_STUB)  # thunk->unix_thunk.addr

TEXT_HDR = 0x180  # .text section header: Name[8], VirtualSize, VirtualAddress, ...
assert d[TEXT_HDR:TEXT_HDR + 8] == b'.text\0\0\0', "unexpected section header layout"
assert struct.unpack_from('<II', d, TEXT_HDR + 8) == (0xd86, TEXT_RVA), "unexpected .text size/address"
struct.pack_into('<I', d, TEXT_HDR + 8, 0xe00)  # .text VirtualSize covers the stubs
open(path, 'wb').write(d)
print("patched", path)

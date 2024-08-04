
import ctypes

asm = """
55                      push   %rbp
48 89 e5                mov    %rsp,%rbp
48 81 ec 00 00 00 00    sub    $0x0,%rsp
48 89 4d 10             mov    %rcx,0x10(%rbp)
8b 45 10                mov    0x10(%rbp),%eax
83 c0 01                add    $0x1,%eax
f3 0f 2a c0             cvtsi2ss %eax,%xmm0
c9                      leave
c3                      ret
""".split('\n')

code = []
for line in asm:
    code.extend(int(x, 16) for x in line[:20].split())

byte_array = bytes(code)

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

MEM_COMMIT = 0x00001000
MEM_RESERVE = 0x00002000
PAGE_EXECUTE_READWRITE = 0x40

VirtualAlloc = kernel32.VirtualAlloc
VirtualAlloc.restype = ctypes.c_void_p

buf = VirtualAlloc(
        None,
        len(byte_array),
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE)

ctypes.memmove(buf, byte_array, len(byte_array))

functype = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int)

increment_by_one = functype(buf)
result = increment_by_one(42)
print(result)

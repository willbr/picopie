import ctypes
import ctypes.util

libc = ctypes.CDLL(ctypes.util.find_library('c'))

PROT_READ = 1
PROT_WRITE = 2
PROT_EXEC = 4
MAP_PRIVATE = 2
MAP_ANON = 0x1000

mmap = libc.mmap
mmap.restype = ctypes.c_void_p
mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int64]

mprotect = libc.mprotect
mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]

asm = """
00 04 00 11                add    w0, w0, #1
00 00 22 1e                fcvts  s0, w0
c0 03 5f d6                ret
""".split('\n')

code = []
for line in asm:
    code.extend(int(x, 16) for x in line[:20].split())

byte_array = bytes(code)

buf = mmap(0, len(byte_array), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)

if buf == (1 << 64) - 1:
    raise Exception("mmap failed")

ctypes.memmove(buf, byte_array, len(byte_array))

mprotect(buf, len(byte_array), PROT_READ | PROT_EXEC)

invalidate = libc.sys_icache_invalidate
invalidate.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
invalidate(buf, len(byte_array))

functype = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int)

increment_by_one = functype(buf)
result = increment_by_one(42)
print(result)

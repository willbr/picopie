"""JIT execution: allocate executable memory, write code, call functions in it."""

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
mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                 ctypes.c_int, ctypes.c_int, ctypes.c_int64]

mprotect = libc.mprotect
mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]

invalidate = libc.sys_icache_invalidate
invalidate.argtypes = [ctypes.c_void_p, ctypes.c_size_t]


def jit_load(code_bytes):
    """Allocate executable memory, copy code in, mark executable. Returns buffer pointer."""
    size = len(code_bytes)
    buf = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
    if buf == (1 << 64) - 1:
        raise RuntimeError("mmap failed")
    ctypes.memmove(buf, code_bytes, size)
    mprotect(buf, size, PROT_READ | PROT_EXEC)
    invalidate(buf, size)
    return buf


def jit_call(buf, offset, args, ret_type=ctypes.c_int64):
    """Call the function at buf+offset with int64 args."""
    arg_types = [ctypes.c_int64] * len(args)
    functype = ctypes.CFUNCTYPE(ret_type, *arg_types)
    func = functype(buf + offset)
    return func(*args)

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Picopie is an experimental Python-based compiler/transpiler that parses Python source files using the `unwind` library and targets native machine code. The project explores JIT-style code generation via `ctypes` for both x86-64 (Windows) and AArch64 (macOS/ARM).

## Build & Run

```bash
# Run the compiler on an example
python -m picopie examples/00.py

# Run with file watch (requires watchexec)
make wall

# Install in dev mode
make install
```

## Architecture

- **`picopie/__main__.py`** — Entry point. Uses `unwind` to parse a Python source file into an AST, then iterates over top-level expressions. Invoked as `python -m picopie <file>`.
- **`x86/code.py`** — JIT code execution proof-of-concept for x86-64 on Windows. Allocates executable memory via `VirtualAlloc`, writes raw machine code bytes, and calls the resulting function through `ctypes.CFUNCTYPE`.
- **`aarch64/code.py`** — Same concept for AArch64 on macOS. Uses `mmap`/`mprotect`/`sys_icache_invalidate` via libc for executable memory allocation.
- **`x86/00-hello/`** — Standalone NASM x86-64 assembly example targeting Windows (uses `GetStdHandle`/`WriteFile`).
- **`aarch64/test/`** — Standalone ARM64 assembly example for macOS, built with `as`/`ld` via `build.sh`.
- **`examples/`** — Sample Python input files for the compiler.

## Key Dependencies

- `unwind` — Python AST parsing library (external, not in this repo)
- `ctypes` — Used for JIT: allocating executable memory and calling generated machine code
- Platform-specific: x86 path uses Windows APIs (`kernel32`), AArch64 path uses macOS/Darwin syscalls

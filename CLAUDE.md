# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Workflow

Commit early and often. Make small, focused commits as you complete each logical unit of work rather than batching many changes into one large commit. Keep commit messages concise and lowercase to match the existing history (e.g. `add nanopie interpreter`, `support python-style cli flags`).

## Project Overview

Picopie is an experimental Python-based compiler/transpiler that parses Python source files using the `unwind` library and targets native machine code. The project explores JIT-style code generation via `ctypes` for both x86-64 (Windows) and AArch64 (macOS/ARM).

## Build & Run

```bash
# Run the compiler on an example
python -m picopie examples/00.py

# Build the standalone interpreter (nanopie/nanopie.c, no deps)
make nanopie

# Run a file and exit
./nanopie/nanopie examples/00.py

# Run a file then drop into the REPL (like `python -i`)
./nanopie/nanopie -i examples/fib.py

# Run a command string (like `python -c`)
./nanopie/nanopie -c 'print(1 + 2)'

# Run a module by dotted name (like `python -m`)
./nanopie/nanopie -m examples.fib

# Read a program from stdin
echo 'print("hi")' | ./nanopie/nanopie -

# Dump the AST as s-expressions (parse only, no execution)
./nanopie/nanopie --print-ast examples/fib.py

# Dump the AST plus all imported modules' ASTs
./nanopie/nanopie --print-ast-and-imports examples/fib.py

# Start the REPL
make repl   # or: ./nanopie/nanopie

# Run with file watch (requires watchexec)
make wall

# Install in dev mode
make install
```

## Architecture

- **`nanopie/nanopie.h`** — Standalone tree-walking interpreter as a single-header library (no external deps). Holds the lexer, parser, and evaluator; public declarations are always visible, and the implementation is compiled where a single translation unit does `#define NANOPIE_IMPLEMENTATION` before including it. Reusable API: `parse_source`/`parse_expr_str` (parse), `eval`/`run_source` (evaluate), `env_init`/`env_set` (environment), plus `load_module`, `print_program`, and `repl`. Supports ints/strings/f-strings, `def`/`if`/`while`/`for-in-range`, arithmetic, comparisons, `print()`, `;`-separated simple statements, and `import dotted.name` to load `.py` files.
- **`nanopie/main.c`** — Thin CLI frontend over `nanopie.h` (the lone `NANOPIE_IMPLEMENTATION` translation unit). Handles argument parsing and `--help`/`--version`. CLI matches Python semantics: `./nanopie/nanopie file.py` runs and exits; `./nanopie/nanopie -i file.py` runs then drops into a REPL; `-c <cmd>` runs a string; `-m <mod>` runs a module; `-` reads from stdin; `-V`/`-h` for version/help; `--print-ast`/`--print-ast-and-imports` dump the AST as s-expressions (parse only). Built with `make nanopie`.
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

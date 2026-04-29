# picopie

A toy compiler that turns a small subset of Python into AArch64 machine code
and runs it in-process via JIT.

Parsing is delegated to [`unwind`](https://github.com/) for the AST. Code
generation is hand-rolled: integer registers only, no stack frame, no spills,
no calls. The result is written into an `mmap`'d page, marked executable, and
invoked through `ctypes.CFUNCTYPE`.

Currently macOS / Apple Silicon (AArch64) only. The `x86/` directory holds
older Windows x86-64 sketches.

## Supported language

One or more `def`s per file. Inside each:

- 64-bit signed `int` parameters and locals (up to 8 params per function)
- `+`, `-`, `*`, `//`, `%`, unary `-`
- `==`, `!=`, `<`, `<=`, `>`, `>=`
- `if` / `elif` / `else`
- `while`
- `return`
- function calls, including recursion and cross-function calls

Locals live in stack slots; expression intermediates use a small pool of
callee-saved scratch registers (x19..x24), so values survive `BL`. No
strings, no floats, no lists, no closures, no GC.

## Run

```bash
python -m picopie examples/fib.py 10
# fib(10) = 55

python -m picopie -d examples/abs.py -- -7
# disassembles the generated code via objdump, then runs it
```

The CLI takes positional ints after the filename and passes them to the
function. `--` separates flags from negative-number args.

## Examples

```
examples/add.py     a + b
examples/abs.py     branch + unary minus
examples/fib.py     while loop, multiple locals
examples/sum_to.py  accumulator loop
examples/fact.py    recursion
examples/call.py    nested cross-function calls
```

## Layout

```
picopie/
  __main__.py      CLI: parse, compile, link, JIT, call
  compile.py       AST -> AArch64; locals on the stack, scratch in callee-saved regs
  emit_aarch64.py  instruction encoder + intra-function branch patching
  link.py          concatenate compiled functions, patch BL displacements
  jit.py           mmap + mprotect + icache invalidate + CFUNCTYPE
  debug.py         wraps code in a minimal ELF so objdump can disassemble it
```

## Build / test

```bash
make          # compile and run examples/00.py
make test     # run the example suite, check return values
make test-d   # same, with disassembly
make wall     # watchexec loop
```

`PYTHONPATH` in the Makefile points at `~/src/unwind`; adjust if yours
lives elsewhere.

## Status

Experimental. The point is to see how little code it takes to go from
Python source to native instructions executing in the same process —
not to be a real compiler.

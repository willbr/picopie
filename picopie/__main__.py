import sys
import unwind

from .compile import compile_function
from .link import link
from .jit import jit_load, jit_call
from .debug import objdump

debug = '-d' in sys.argv
argv = [a for a in sys.argv if a not in ('-d', '--')]

ast = unwind.unwind_file(argv[1])

head, *expressions = ast
assert head == 'module'

funcs = []
for expr in expressions:
    if isinstance(expr, list) and expr[0] == 'def':
        funcs.append(compile_function(expr))

if not funcs:
    sys.exit(0)

blob, offsets = link(funcs)

if debug:
    for name, _n_params, code, _calls in funcs:
        start = offsets[name]
        objdump(name, blob[start:start + len(code)])

buf = jit_load(blob)

raw_args = argv[2:]
for name, n_params, _code, _calls in funcs:
    args = [int(a) for a in raw_args[:n_params]]
    if not args:
        args = [10] * n_params
    result = jit_call(buf, offsets[name], args)
    print(f'{name}({", ".join(str(a) for a in args)}) = {result}')

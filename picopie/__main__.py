import sys
import unwind

from .compile import compile_function
from .jit import jit_call
from .debug import objdump

debug = '-d' in sys.argv
argv = [a for a in sys.argv if a not in ('-d', '--')]

ast = unwind.unwind_file(argv[1])

head, *expressions = ast
assert head == 'module'

for expr in expressions:
    if isinstance(expr, list) and expr[0] == 'def':
        name, n_params, code_bytes = compile_function(expr)

        if debug:
            objdump(name, code_bytes)

        raw_args = argv[2:]
        args = [int(a) for a in raw_args[:n_params]]
        if not args:
            args = [10] * n_params

        result = jit_call(code_bytes, args)
        print(f'{name}({", ".join(str(a) for a in args)}) = {result}')

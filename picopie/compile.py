"""Compile unwind AST to AArch64 machine code."""

from .emit_aarch64 import Emitter


class Compiler:
    """Compiles a single function definition from unwind AST to AArch64."""

    def __init__(self):
        self.emit = Emitter()
        self.vars = {}       # name -> register number
        self.next_reg = 0    # next free register (w0-w15, skip w16+ reserved)
        self.label_count = 0

    def fresh_label(self, prefix='L'):
        n = self.label_count
        self.label_count += 1
        return f'{prefix}{n}'

    def alloc_reg(self, name=None):
        r = self.next_reg
        if r > 15:
            raise RuntimeError("out of registers")
        self.next_reg += 1
        if name:
            self.vars[name] = r
        return r

    def reg(self, name):
        return self.vars[name]

    def compile_function(self, node):
        """Compile ['def', name, args, returns, body]."""
        _, name, args, _returns, body = node

        # Allocate registers for parameters (already in w0, w1, ... per AArch64 CC)
        param_names = []
        for arg_group in args:
            if arg_group[0] == 'args':
                param_names = arg_group[1:]

        for i, pname in enumerate(param_names):
            self.vars[pname] = i
            self.next_reg = max(self.next_reg, i + 1)

        for stmt in body:
            self.compile_stmt(stmt)

        # safety: if control falls off the end, return 0
        self.emit.mov_imm(0, 0)
        self.emit.ret()

        return name, len(param_names), self.emit.to_bytes()

    def compile_stmt(self, stmt):
        if not isinstance(stmt, list):
            raise RuntimeError(f"unexpected statement: {stmt}")

        tag = stmt[0]

        if tag == 'return':
            _, value = stmt
            r = self.compile_expr(value)
            if r != 0:
                self.emit.mov_reg(0, r)
            self.emit.ret()

        elif tag == 'assign':
            _, target, value = stmt
            r = self.compile_expr(value)
            if target in self.vars:
                dst = self.vars[target]
                if dst != r:
                    self.emit.mov_reg(dst, r)
            else:
                dst = self.alloc_reg(target)
                if dst != r:
                    self.emit.mov_reg(dst, r)

        elif tag == 'while':
            _, test, body = stmt
            loop_top = self.fresh_label('while_top')
            loop_end = self.fresh_label('while_end')

            self.emit.label(loop_top)
            self.compile_condition(test, loop_end, jump_if_false=True)

            for s in body:
                self.compile_stmt(s)

            self.emit.b_back(loop_top)
            self.emit.label(loop_end)
            self.emit.patch()  # resolve forward branches to loop_end

        elif tag == 'cond':
            # ['cond', [test, body], ...maybe ['else', body]]
            clauses = stmt[1:]
            end_label = self.fresh_label('cond_end')

            for clause in clauses:
                if clause[0] == 'else':
                    for s in clause[1]:
                        self.compile_stmt(s)
                else:
                    test, body = clause
                    skip_label = self.fresh_label('cond_skip')
                    self.compile_condition(test, skip_label, jump_if_false=True)
                    for s in body:
                        self.compile_stmt(s)
                    self.emit.b(end_label)
                    self.emit.label(skip_label)
                    self.emit.patch()

            self.emit.label(end_label)
            self.emit.patch()

        else:
            raise RuntimeError(f"unknown statement: {tag}")

    def compile_condition(self, node, target_label, jump_if_false=True):
        """Compile a condition and emit a branch to target_label."""
        if isinstance(node, list) and node[0] == 'compare':
            # ['compare', left, [op], [right]]
            _, left, ops, comparators = node
            l = self.compile_expr(left)
            r = self.compile_expr(comparators[0])
            op = ops[0]
            self.emit.cmp_reg(l, r)
            cond = Emitter.COND_MAP[op]
            if jump_if_false:
                # invert the condition
                cond = cond ^ 1
            self.emit.b_cond(cond, target_label)
        else:
            # Treat as truthy: compile expr, compare to 0
            r = self.compile_expr(node)
            self.emit.cmp_reg(r, 31)  # compare to wzr
            if jump_if_false:
                self.emit.b_cond(Emitter.EQ, target_label)
            else:
                self.emit.b_cond(Emitter.NE, target_label)

    def compile_expr(self, node):
        """Compile an expression, return register number holding the result."""
        if isinstance(node, int):
            r = self.alloc_reg()
            self.emit.mov_imm(r, node)
            return r

        if isinstance(node, str):
            # variable reference
            return self.reg(node)

        if not isinstance(node, list):
            raise RuntimeError(f"unexpected expr: {node}")

        tag = node[0]

        if tag == '+':
            _, left, right = node
            l = self.compile_expr(left)
            r = self.compile_expr(right)
            dst = self.alloc_reg()
            self.emit.add_reg(dst, l, r)
            return dst

        elif tag == '-':
            _, left, right = node
            l = self.compile_expr(left)
            if isinstance(right, int) and 0 < right < 4096:
                dst = self.alloc_reg()
                self.emit.sub_imm(dst, l, right)
                return dst
            r = self.compile_expr(right)
            dst = self.alloc_reg()
            self.emit.sub_reg(dst, l, r)
            return dst

        elif tag == '*':
            _, left, right = node
            l = self.compile_expr(left)
            r = self.compile_expr(right)
            dst = self.alloc_reg()
            self.emit.mul(dst, l, r)
            return dst

        elif tag == 'floor_div':
            _, left, right = node
            l = self.compile_expr(left)
            r = self.compile_expr(right)
            dst = self.alloc_reg()
            self.emit.sdiv(dst, l, r)
            return dst

        elif tag == '%':
            _, left, right = node
            l = self.compile_expr(left)
            r = self.compile_expr(right)
            q = self.alloc_reg()
            self.emit.sdiv(q, l, r)
            dst = self.alloc_reg()
            self.emit.msub(dst, q, r, l)
            return dst

        elif tag == 'USub':
            _, operand = node
            r = self.compile_expr(operand)
            dst = self.alloc_reg()
            self.emit.neg(dst, r)
            return dst

        elif tag == 'compare':
            # produce 0 or 1
            _, left, ops, comparators = node
            l = self.compile_expr(left)
            r = self.compile_expr(comparators[0])
            self.emit.cmp_reg(l, r)
            dst = self.alloc_reg()
            cond = Emitter.COND_MAP[ops[0]]
            self.emit.cset(dst, cond)
            return dst

        else:
            raise RuntimeError(f"unknown expr: {tag}")


def compile_function(node):
    """Compile a function AST node, return (name, n_params, code_bytes)."""
    compiler = Compiler()
    return compiler.compile_function(node)

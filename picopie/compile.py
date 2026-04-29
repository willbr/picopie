"""Compile unwind AST to AArch64 machine code.

Strategy: every named variable gets a stack slot. Expression intermediates
use a small pool of callee-saved scratch registers (x19..x24) so values
survive function calls. No register pressure cliff, no spilling logic —
the stack does the work.
"""

from .emit_aarch64 import Emitter


# Callee-saved scratch registers. Preserved across BL by the callee's prologue.
SCRATCH = [19, 20, 21, 22, 23, 24]

# Frame layout (offsets from SP, after the SUB SP):
#   0  : saved x29
#   8  : saved x30 (LR)
#  16  : saved x19
#  24  : saved x20
#  32  : saved x21
#  40  : saved x22
#  48  : saved x23
#  56  : saved x24
#  64+ : params, then locals (8 bytes each)
SAVED_AREA = 64


def collect_locals(stmts, names):
    for stmt in stmts:
        if not isinstance(stmt, list):
            continue
        tag = stmt[0]
        if tag == 'assign':
            names.add(stmt[1])
        elif tag == 'while':
            collect_locals(stmt[2], names)
        elif tag == 'cond':
            for clause in stmt[1:]:
                collect_locals(clause[1], names)


# Tags that aren't function names. Anything else with a string head in
# expression position is treated as a call: [fname, arg1, arg2, ...].
KNOWN_EXPR_TAGS = frozenset({
    '+', '-', '*', '%', 'floor_div', 'USub', 'compare',
})


class Compiler:
    def __init__(self):
        self.emit = Emitter()
        self.slots = {}        # var name -> SP offset (bytes)
        self.frame_size = 0
        self.scratch_top = 0   # next free index into SCRATCH
        self.label_count = 0

    def fresh_label(self, prefix='L'):
        n = self.label_count
        self.label_count += 1
        return f'{prefix}{n}'

    def push_scratch(self):
        if self.scratch_top >= len(SCRATCH):
            raise RuntimeError("expression too deep — scratch register pool exhausted")
        r = SCRATCH[self.scratch_top]
        self.scratch_top += 1
        return r

    def pop_scratch(self):
        self.scratch_top -= 1
        assert self.scratch_top >= 0

    def slot(self, name):
        return self.slots[name]

    # ---- function entry ----

    def compile_function(self, node):
        _, name, args, _returns, body = node

        params = []
        for arg_group in args:
            if isinstance(arg_group, list) and arg_group[0] == 'args':
                params = list(arg_group[1:])

        if len(params) > 8:
            raise RuntimeError("more than 8 parameters not supported")

        all_names = set(params)
        collect_locals(body, all_names)
        other_locals = sorted(all_names - set(params))

        n_slots = len(params) + len(other_locals)
        frame_size = SAVED_AREA + n_slots * 8
        frame_size = (frame_size + 15) & ~15
        self.frame_size = frame_size

        offset = SAVED_AREA
        for p in params:
            self.slots[p] = offset
            offset += 8
        for n in other_locals:
            self.slots[n] = offset
            offset += 8

        # Prologue
        self.emit.sub_imm(31, 31, frame_size)         # SUB SP, SP, #frame
        self.emit.stp_x_sp(29, 30, 0)
        self.emit.stp_x_sp(19, 20, 16)
        self.emit.stp_x_sp(21, 22, 32)
        self.emit.stp_x_sp(23, 24, 48)

        # Move incoming params from x0..xN into their slots
        for i, p in enumerate(params):
            self.emit.str_x_sp(i, self.slots[p])

        for stmt in body:
            self.compile_stmt(stmt)

        # Fall-through return: x0 = 0
        self.emit.mov_imm(0, 0)
        self.emit_epilogue()

        return name, len(params), self.emit.to_bytes(), self.emit.calls

    def emit_epilogue(self):
        self.emit.ldp_x_sp(29, 30, 0)
        self.emit.ldp_x_sp(19, 20, 16)
        self.emit.ldp_x_sp(21, 22, 32)
        self.emit.ldp_x_sp(23, 24, 48)
        self.emit.add_imm(31, 31, self.frame_size)
        self.emit.ret()

    # ---- statements ----

    def compile_stmt(self, stmt):
        if not isinstance(stmt, list):
            raise RuntimeError(f"unexpected statement: {stmt}")

        tag = stmt[0]

        if tag == 'return':
            _, value = stmt
            r = self.compile_expr(value)
            if r != 0:
                self.emit.mov_reg(0, r)
            self.pop_scratch()
            self.emit_epilogue()

        elif tag == 'assign':
            _, target, value = stmt
            r = self.compile_expr(value)
            self.emit.str_x_sp(r, self.slot(target))
            self.pop_scratch()

        elif tag == 'while':
            _, test, body = stmt
            top = self.fresh_label('w_top')
            end = self.fresh_label('w_end')
            self.emit.label(top)
            self.compile_condition(test, end, jump_if_false=True)
            for s in body:
                self.compile_stmt(s)
            self.emit.b_back(top)
            self.emit.label(end)
            self.emit.patch()

        elif tag == 'cond':
            clauses = stmt[1:]
            end = self.fresh_label('cond_end')
            for clause in clauses:
                if clause[0] == 'else':
                    for s in clause[1]:
                        self.compile_stmt(s)
                else:
                    test, body = clause
                    skip = self.fresh_label('cond_skip')
                    self.compile_condition(test, skip, jump_if_false=True)
                    for s in body:
                        self.compile_stmt(s)
                    self.emit.b(end)
                    self.emit.label(skip)
                    self.emit.patch()
            self.emit.label(end)
            self.emit.patch()

        else:
            # Bare expression statement (e.g. a function call) — compile and discard.
            r = self.compile_expr(stmt)
            self.pop_scratch()

    # ---- conditions ----

    def compile_condition(self, node, target_label, jump_if_false=True):
        if isinstance(node, list) and node[0] == 'compare':
            _, left, ops, comparators = node
            l = self.compile_expr(left)
            r = self.compile_expr(comparators[0])
            self.emit.cmp_reg(l, r)
            self.pop_scratch()
            self.pop_scratch()
            cond = Emitter.COND_MAP[ops[0]]
            if jump_if_false:
                cond ^= 1
            self.emit.b_cond(cond, target_label)
        else:
            r = self.compile_expr(node)
            self.emit.cmp_imm(r, 0)
            self.pop_scratch()
            if jump_if_false:
                self.emit.b_cond(Emitter.EQ, target_label)
            else:
                self.emit.b_cond(Emitter.NE, target_label)

    # ---- expressions ----

    def compile_expr(self, node):
        """Compile an expression. Result is left in a fresh scratch register
        (pushed onto the scratch stack — caller must pop_scratch when done)."""
        dst = self.push_scratch()
        self._compile_into(node, dst)
        return dst

    def _compile_into(self, node, dst):
        """Compile expression, leaving result in `dst`. dst must be the
        most recently pushed scratch slot."""
        if isinstance(node, int):
            self.emit.mov_imm(dst, node)
            return

        if isinstance(node, str):
            self.emit.ldr_x_sp(dst, self.slot(node))
            return

        if not isinstance(node, list):
            raise RuntimeError(f"unexpected expr: {node}")

        tag = node[0]

        if tag in ('+', '-', '*', 'floor_div'):
            _, l, r = node
            self._compile_into(l, dst)
            rr = self.compile_expr(r)
            if tag == '+':
                self.emit.add_reg(dst, dst, rr)
            elif tag == '-':
                self.emit.sub_reg(dst, dst, rr)
            elif tag == '*':
                self.emit.mul(dst, dst, rr)
            else:  # floor_div
                self.emit.sdiv(dst, dst, rr)
            self.pop_scratch()
            return

        if tag == '%':
            _, l, r = node
            self._compile_into(l, dst)
            rr = self.compile_expr(r)
            q = self.push_scratch()
            self.emit.sdiv(q, dst, rr)
            self.emit.msub(dst, q, rr, dst)
            self.pop_scratch()  # q
            self.pop_scratch()  # rr
            return

        if tag == 'USub':
            _, operand = node
            self._compile_into(operand, dst)
            self.emit.neg(dst, dst)
            return

        if tag == 'compare':
            _, left, ops, comparators = node
            self._compile_into(left, dst)
            rr = self.compile_expr(comparators[0])
            self.emit.cmp_reg(dst, rr)
            self.pop_scratch()
            self.emit.cset(dst, Emitter.COND_MAP[ops[0]])
            return

        if isinstance(tag, str) and tag not in KNOWN_EXPR_TAGS:
            # Function call: [fname, arg1, arg2, ...]
            fname = tag
            args_list = node[1:]

            if len(args_list) > 8:
                raise RuntimeError("more than 8 arguments not supported")

            # Evaluate each argument into a scratch register. Scratches are
            # callee-saved (x19..x24), so they survive the upcoming BL.
            arg_regs = [self.compile_expr(a) for a in args_list]

            # Move args into x0..xN. SCRATCH is disjoint from x0..x7, so the
            # moves don't clobber each other.
            for i, sr in enumerate(arg_regs):
                self.emit.mov_reg(i, sr)

            for _ in arg_regs:
                self.pop_scratch()

            self.emit.bl(fname)

            if dst != 0:
                self.emit.mov_reg(dst, 0)
            return

        raise RuntimeError(f"unknown expr: {tag}")


def compile_function(node):
    """Compile a function AST node. Returns (name, n_params, code_bytes, calls)."""
    return Compiler().compile_function(node)

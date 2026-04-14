"""AArch64 machine code emitter for picopie."""

import struct


class Emitter:
    """Emits AArch64 machine code as a byte buffer."""

    def __init__(self):
        self.code = bytearray()
        self._patches = []  # (offset, kind, target_label)
        self._labels = {}   # label -> offset

    def _emit(self, word):
        self.code += struct.pack('<I', word)

    def pos(self):
        return len(self.code)

    def label(self, name):
        self._labels[name] = self.pos()

    def patch(self):
        """Resolve forward branch references whose targets are now defined."""
        remaining = []
        for offset, kind, target in self._patches:
            if target not in self._labels:
                remaining.append((offset, kind, target))
                continue

            target_pos = self._labels[target]
            disp = (target_pos - offset) >> 2  # in instructions

            old = struct.unpack_from('<I', self.code, offset)[0]

            if kind == 'b':
                # B imm26 — bits [25:0]
                old = (old & 0xfc000000) | (disp & 0x03ffffff)
            elif kind == 'bcond':
                # B.cond imm19 — bits [23:5]
                old = (old & 0xff00001f) | ((disp & 0x7ffff) << 5)

            struct.pack_into('<I', self.code, offset, old)
        self._patches = remaining

    # -- Arithmetic (32-bit) --

    def add_reg(self, rd, rn, rm):
        self._emit(0x0b000000 | (rm << 16) | (rn << 5) | rd)

    def sub_reg(self, rd, rn, rm):
        self._emit(0x4b000000 | (rm << 16) | (rn << 5) | rd)

    def mul(self, rd, rn, rm):
        self._emit(0x1b007c00 | (rm << 16) | (rn << 5) | rd)

    def sdiv(self, rd, rn, rm):
        self._emit(0x1ac00c00 | (rm << 16) | (rn << 5) | rd)

    def msub(self, rd, rn, rm, ra):
        """rd = ra - rn * rm  (used for modulo: a % b = a - (a/b)*b)"""
        self._emit(0x1b008000 | (rm << 16) | (ra << 10) | (rn << 5) | rd)

    def neg(self, rd, rm):
        # sub rd, wzr, rm
        self.sub_reg(rd, 31, rm)

    # -- Immediates --

    def mov_imm(self, rd, imm):
        if imm < 0:
            # MOVN rd, #(~imm)
            nimm = (~imm) & 0xffff
            self._emit(0x12800000 | (nimm << 5) | rd)
        else:
            # MOVZ rd, #imm
            self._emit(0x52800000 | ((imm & 0xffff) << 5) | rd)

    def add_imm(self, rd, rn, imm):
        self._emit(0x11000000 | ((imm & 0xfff) << 10) | (rn << 5) | rd)

    def sub_imm(self, rd, rn, imm):
        self._emit(0x51000000 | ((imm & 0xfff) << 10) | (rn << 5) | rd)

    # -- Comparison --

    def cmp_reg(self, rn, rm):
        # subs wzr, rn, rm
        self._emit(0x6b000000 | (rm << 16) | (rn << 5) | 31)

    # Condition codes
    EQ = 0x0
    NE = 0x1
    LT = 0xb  # signed less than
    GE = 0xa  # signed greater or equal
    LE = 0xd  # signed less or equal
    GT = 0xc  # signed greater than

    COND_MAP = {
        '==': EQ, '!=': NE,
        '<': LT, '>=': GE,
        '<=': LE, '>': GT,
    }

    def cset(self, rd, cond):
        # CSET is CSINC rd, wzr, wzr, invert(cond)
        inv = cond ^ 1
        self._emit(0x1a9f0400 | (inv << 12) | (31 << 5) | rd)

    # -- Branches --

    def b(self, target_label):
        self._patches.append((self.pos(), 'b', target_label))
        self._emit(0x14000000)  # placeholder

    def b_cond(self, cond, target_label):
        self._patches.append((self.pos(), 'bcond', target_label))
        self._emit(0x54000000 | cond)  # placeholder

    def b_back(self, target_label):
        """Unconditional backward branch to an already-defined label."""
        target_pos = self._labels[target_label]
        disp = (target_pos - self.pos()) >> 2
        self._emit(0x14000000 | (disp & 0x03ffffff))

    def b_cond_back(self, cond, target_label):
        """Conditional backward branch to an already-defined label."""
        target_pos = self._labels[target_label]
        disp = (target_pos - self.pos()) >> 2
        self._emit(0x54000000 | ((disp & 0x7ffff) << 5) | cond)

    # -- Move between registers --

    def mov_reg(self, rd, rm):
        # ORR rd, wzr, rm
        self._emit(0x2a000000 | (rm << 16) | (31 << 5) | rd)

    # -- Return --

    def ret(self):
        self._emit(0xd65f03c0)

    def to_bytes(self):
        self.patch()
        return bytes(self.code)

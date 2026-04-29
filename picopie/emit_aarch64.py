"""AArch64 machine code emitter for picopie. 64-bit (X-registers) throughout."""

import struct


class Emitter:
    """Emits AArch64 machine code as a byte buffer."""

    def __init__(self):
        self.code = bytearray()
        self._patches = []   # (offset, kind, target_label) — intra-function
        self._labels = {}    # label -> offset within this function
        self.calls = []      # (offset, target_function_name) — resolved by linker

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
            disp = (target_pos - offset) >> 2

            old = struct.unpack_from('<I', self.code, offset)[0]

            if kind == 'b':
                old = (old & 0xfc000000) | (disp & 0x03ffffff)
            elif kind == 'bcond':
                old = (old & 0xff00001f) | ((disp & 0x7ffff) << 5)

            struct.pack_into('<I', self.code, offset, old)
        self._patches = remaining

    # -- Arithmetic, register operands (64-bit) --

    def add_reg(self, rd, rn, rm):
        self._emit(0x8b000000 | (rm << 16) | (rn << 5) | rd)

    def sub_reg(self, rd, rn, rm):
        self._emit(0xcb000000 | (rm << 16) | (rn << 5) | rd)

    def mul(self, rd, rn, rm):
        # MADD rd, rn, rm, xzr
        self._emit(0x9b007c00 | (rm << 16) | (rn << 5) | rd)

    def sdiv(self, rd, rn, rm):
        self._emit(0x9ac00c00 | (rm << 16) | (rn << 5) | rd)

    def msub(self, rd, rn, rm, ra):
        """rd = ra - rn * rm"""
        self._emit(0x9b008000 | (rm << 16) | (ra << 10) | (rn << 5) | rd)

    def neg(self, rd, rm):
        self.sub_reg(rd, 31, rm)

    # -- Immediates --

    def add_imm(self, rd, rn, imm):
        """ADD rd, rn, #imm (also "ADD SP, SP, #imm" when rd=rn=31)."""
        assert 0 <= imm < 4096
        self._emit(0x91000000 | ((imm & 0xfff) << 10) | (rn << 5) | rd)

    def sub_imm(self, rd, rn, imm):
        """SUB rd, rn, #imm (also "SUB SP, SP, #imm" when rd=rn=31)."""
        assert 0 <= imm < 4096
        self._emit(0xd1000000 | ((imm & 0xfff) << 10) | (rn << 5) | rd)

    def mov_imm(self, rd, imm):
        """64-bit immediate via MOVZ/MOVN + MOVK chain."""
        val = imm & ((1 << 64) - 1)

        if val == 0:
            self._emit(0xd2800000 | rd)
            return

        chunks = [(val >> (16 * i)) & 0xffff for i in range(4)]
        n_zero = sum(1 for c in chunks if c == 0)
        n_ones = sum(1 for c in chunks if c == 0xffff)

        if n_ones > n_zero:
            # MOVN with first chunk, then MOVK for any non-0xffff chunks above it.
            inv = chunks[0] ^ 0xffff
            self._emit(0x92800000 | (inv << 5) | rd)
            for i in range(1, 4):
                if chunks[i] != 0xffff:
                    self._emit(0xf2800000 | (i << 21) | (chunks[i] << 5) | rd)
        else:
            first = next(i for i, c in enumerate(chunks) if c != 0)
            self._emit(0xd2800000 | (first << 21) | (chunks[first] << 5) | rd)
            for i in range(first + 1, 4):
                if chunks[i] != 0:
                    self._emit(0xf2800000 | (i << 21) | (chunks[i] << 5) | rd)

    # -- Comparison --

    def cmp_reg(self, rn, rm):
        # SUBS xzr, rn, rm
        self._emit(0xeb000000 | (rm << 16) | (rn << 5) | 31)

    def cmp_imm(self, rn, imm):
        # SUBS xzr, rn, #imm
        assert 0 <= imm < 4096
        self._emit(0xf1000000 | ((imm & 0xfff) << 10) | (rn << 5) | 31)

    EQ = 0x0
    NE = 0x1
    LT = 0xb  # signed
    GE = 0xa
    LE = 0xd
    GT = 0xc

    COND_MAP = {
        '==': EQ, '!=': NE,
        '<': LT, '>=': GE,
        '<=': LE, '>': GT,
    }

    def cset(self, rd, cond):
        # CSINC rd, xzr, xzr, invert(cond)
        inv = cond ^ 1
        self._emit(0x9a9f07e0 | (inv << 12) | rd)

    # -- Move register --

    def mov_reg(self, rd, rm):
        # ORR rd, xzr, rm
        self._emit(0xaa0003e0 | (rm << 16) | rd)

    # -- Memory: SP-relative load/store --

    def str_x_sp(self, rt, offset):
        """STR Xrt, [SP, #offset]"""
        assert offset % 8 == 0 and 0 <= offset < 32768
        self._emit(0xf90003e0 | ((offset // 8) << 10) | rt)

    def ldr_x_sp(self, rt, offset):
        """LDR Xrt, [SP, #offset]"""
        assert offset % 8 == 0 and 0 <= offset < 32768
        self._emit(0xf94003e0 | ((offset // 8) << 10) | rt)

    def stp_x_sp(self, rt1, rt2, offset):
        """STP Xrt1, Xrt2, [SP, #offset]  (signed offset, 8-byte scaled)"""
        assert offset % 8 == 0 and -512 <= offset < 512
        imm7 = (offset // 8) & 0x7f
        self._emit(0xa9000000 | (imm7 << 15) | (rt2 << 10) | (31 << 5) | rt1)

    def ldp_x_sp(self, rt1, rt2, offset):
        """LDP Xrt1, Xrt2, [SP, #offset]"""
        assert offset % 8 == 0 and -512 <= offset < 512
        imm7 = (offset // 8) & 0x7f
        self._emit(0xa9400000 | (imm7 << 15) | (rt2 << 10) | (31 << 5) | rt1)

    # -- Branches --

    def b(self, target_label):
        self._patches.append((self.pos(), 'b', target_label))
        self._emit(0x14000000)

    def b_cond(self, cond, target_label):
        self._patches.append((self.pos(), 'bcond', target_label))
        self._emit(0x54000000 | cond)

    def b_back(self, target_label):
        target_pos = self._labels[target_label]
        disp = (target_pos - self.pos()) >> 2
        self._emit(0x14000000 | (disp & 0x03ffffff))

    def bl(self, target_func_name):
        """Function call. Displacement filled in by the linker."""
        self.calls.append((self.pos(), target_func_name))
        self._emit(0x94000000)

    # -- Return --

    def ret(self):
        self._emit(0xd65f03c0)

    def to_bytes(self):
        self.patch()
        return bytes(self.code)

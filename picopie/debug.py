"""Debug utilities: disassemble generated machine code."""

import struct
import subprocess
import tempfile
import os


def make_elf(code_bytes):
    """Wrap raw aarch64 code in a minimal ELF64 so objdump can read it."""
    # ELF header (64 bytes) + section header table (2 entries * 64 bytes = 128)
    # + code + section name strings
    shstrtab = b'\x00.text\x00.shstrtab\x00'
    code_offset = 64  # right after ELF header
    shstrtab_offset = code_offset + len(code_bytes)
    shdr_offset = (shstrtab_offset + len(shstrtab) + 7) & ~7  # align to 8

    # ELF64 header
    ehdr = struct.pack('<16sHHIQQQIHHHHHH',
        b'\x7fELF\x02\x01\x01' + b'\x00' * 9,  # e_ident (ELF64, little-endian)
        1,             # e_type: ET_REL
        0xb7,          # e_machine: EM_AARCH64
        1,             # e_version
        0,             # e_entry
        0,             # e_phoff
        shdr_offset,   # e_shoff
        0,             # e_flags
        64,            # e_ehsize
        0,             # e_phentsize
        0,             # e_phnum
        64,            # e_shentsize
        3,             # e_shnum (null + .text + .shstrtab)
        2,             # e_shstrndx
    )

    # Section headers: null, .text, .shstrtab
    shdrs = struct.pack('<IIQQQQIIQQ',  # null section
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    shdrs += struct.pack('<IIQQQQIIQQ',  # .text
        1,             # sh_name (offset in shstrtab)
        1,             # sh_type: SHT_PROGBITS
        6,             # sh_flags: SHF_ALLOC | SHF_EXECINSTR
        0,             # sh_addr
        code_offset,   # sh_offset
        len(code_bytes),  # sh_size
        0, 0,          # sh_link, sh_info
        4,             # sh_addralign
        0,             # sh_entsize
    )
    shdrs += struct.pack('<IIQQQQIIQQ',  # .shstrtab
        7,             # sh_name
        3,             # sh_type: SHT_STRTAB
        0,             # sh_flags
        0,             # sh_addr
        shstrtab_offset,  # sh_offset
        len(shstrtab),    # sh_size
        0, 0,          # sh_link, sh_info
        1,             # sh_addralign
        0,             # sh_entsize
    )

    # Pad between shstrtab and section headers
    pad = b'\x00' * (shdr_offset - shstrtab_offset - len(shstrtab))

    return ehdr + code_bytes + shstrtab + pad + shdrs


def objdump(name, code_bytes):
    """Disassemble code_bytes using objdump."""
    elf = make_elf(code_bytes)

    with tempfile.NamedTemporaryFile(suffix='.o', delete=False) as f:
        f.write(elf)
        path = f.name

    try:
        print(f'--- {name} ({len(code_bytes)} bytes) ---')
        result = subprocess.run(
            ['objdump', '-d', '--disassembler-color=on', path],
            capture_output=True, text=True)
        for line in result.stdout.splitlines():
            line = line.strip()
            if ':' in line and line[0] in '0123456789abcdef':
                print(f'  {line}')
        print()
    finally:
        os.unlink(path)

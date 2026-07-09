"""Raw ELF64 image parsing/serialization with plain struct.

This is the load-bearing half of the converter: everything a loader needs
(ehdr fields, program headers, segment bytes) comes straight from the file
bytes, independent of LIEF. self2elf and the C loader implement the exact
inverse of `parse_image`.
"""

from dataclasses import dataclass
import struct

EHDR_FMT = "<16sHHIQQQIHHHHHH"
EHDR_SIZE = 64
PHDR_FMT = "<IIQQQQQQ"
PHDR_SIZE = 56

PT_NAMES = {
    1: "load",
    2: "dynamic",
    3: "interp",
    4: "note",
    6: "phdr",
    7: "tls",
    0x6474E550: "eh_frame",
    0x6474E551: "stack",
    0x6474E552: "relro",
    0x6474E553: "property",
}
PT_LOAD = 1
PT_INTERP = 3

PF_X, PF_W, PF_R = 1, 2, 4

EM_NAMES = {3: "i386", 40: "arm", 62: "x86_64", 183: "aarch64", 243: "riscv64"}
ET_NAMES = {2: "EXEC", 3: "DYN"}


@dataclass
class Ehdr:
    et: int
    em: int
    entry: int
    phoff: int
    eflags: int
    phentsize: int
    phnum: int
    osabi: int


@dataclass
class Phdr:
    ptype: int
    flags: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    align: int
    content: bytes | None = None  # filesz bytes, PT_LOAD only

    @property
    def type_name(self) -> str:
        return PT_NAMES.get(self.ptype, "other")


def parse_image(data: bytes) -> tuple[Ehdr, list[Phdr]]:
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise ValueError("only ELF64 little-endian is supported")
    (_ident, et, em, _ver, entry, phoff, _shoff, eflags,
     _ehsize, phentsize, phnum, *_rest) = struct.unpack_from(EHDR_FMT, data)
    ehdr = Ehdr(et, em, entry, phoff, eflags, phentsize, phnum, data[7])
    phdrs = []
    for i in range(phnum):
        (ptype, flags, offset, vaddr, _paddr, filesz, memsz,
         align) = struct.unpack_from(PHDR_FMT, data, phoff + i * PHDR_SIZE)
        content = bytes(data[offset:offset + filesz]) if ptype == PT_LOAD else None
        phdrs.append(Phdr(ptype, flags, offset, vaddr, filesz, memsz, align, content))
    return ehdr, phdrs


def serialize_image(ehdr: Ehdr, phdrs: list[Phdr]) -> bytes:
    """Inverse of parse_image: rebuild the executable image from rows.

    Segment contents keep their original file offsets, so offset/vaddr
    congruence holds and the phdr table lands inside the first load segment
    (making the kernel's AT_PHDR point at valid mapped memory). The section
    header table is intentionally gone: e_shoff = 0.
    """
    size = max(p.offset + p.filesz for p in phdrs)
    size = max(size, EHDR_SIZE, ehdr.phoff + len(phdrs) * PHDR_SIZE)
    img = bytearray(size)
    for p in phdrs:
        if p.content is not None:
            img[p.offset:p.offset + p.filesz] = p.content
    ident = b"\x7fELF" + bytes([2, 1, 1, ehdr.osabi]) + bytes(8)
    struct.pack_into(EHDR_FMT, img, 0, ident, ehdr.et, ehdr.em, 1,
                     ehdr.entry, ehdr.phoff, 0, ehdr.eflags,
                     EHDR_SIZE, PHDR_SIZE, len(phdrs), 0, 0, 0)
    for i, p in enumerate(phdrs):
        struct.pack_into(PHDR_FMT, img, ehdr.phoff + i * PHDR_SIZE,
                         p.ptype, p.flags, p.offset, p.vaddr, p.vaddr,
                         p.filesz, p.memsz, p.align)
    return bytes(img)

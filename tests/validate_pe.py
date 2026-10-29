"""Check ARM64 kernel image layout and exercise its actual base relocations."""
import struct
import sys
from pathlib import Path


def validate(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe:pe + 4] == b'PE\0\0'
    machine, count, _, _, _, optional_size, flags = struct.unpack_from('<HHIIIHH', data, pe + 4)
    optional = pe + 24
    assert machine == 0xaa64, 'Not an ARM64 image'
    assert flags & 0x2000, 'Not linked as a native DLL driver image'
    assert not flags & 1, 'Relocations are stripped'
    assert struct.unpack_from('<H', data, optional)[0] == 0x20b
    entry = struct.unpack_from('<I', data, optional + 16)[0]
    image_base = struct.unpack_from('<Q', data, optional + 24)[0]
    alignment = struct.unpack_from('<I', data, optional + 32)[0]
    image_size = struct.unpack_from('<I', data, optional + 56)[0]
    header_size = struct.unpack_from('<I', data, optional + 60)[0]
    subsystem, dll_flags = struct.unpack_from('<HH', data, optional + 68)
    assert subsystem == 1 and dll_flags & 0x40, 'Native subsystem / ASLR required'
    assert alignment >= 4096 and alignment % 4096 == 0
    reloc_rva, reloc_size = struct.unpack_from('<II', data, optional + 112 + 5 * 8)
    assert reloc_rva and reloc_size >= 12, 'Missing base relocation table'
    image = bytearray(image_size)
    image[:header_size] = data[:header_size]
    entry_valid = False
    for i in range(count):
        section = struct.unpack_from('<8sIIIIIIHHI', data, optional + optional_size + i * 40)
        _, virtual_size, rva, raw_size, raw_offset, *_, characteristics = section
        assert characteristics & 0xa0000000 != 0xa0000000, 'Writable executable section'
        assert rva % 4096 == 0
        assert rva + max(virtual_size, raw_size) <= image_size
        image[rva:rva + raw_size] = data[raw_offset:raw_offset + raw_size]
        if rva <= entry < rva + virtual_size and characteristics & 0x20000000:
            entry_valid = True
    assert entry_valid, 'Entry point is outside executable code'
    assert reloc_rva + reloc_size <= image_size
    new_base = 0xfffff80660000000
    delta = new_base - image_base
    position, changed = reloc_rva, 0
    while position < reloc_rva + reloc_size:
        page, size = struct.unpack_from('<II', image, position)
        assert size >= 8 and size % 4 == 0 and position + size <= reloc_rva + reloc_size
        for offset in range(position + 8, position + size, 2):
            record = struct.unpack_from('<H', image, offset)[0]
            kind, within_page = record >> 12, record & 0xfff
            if kind == 0:
                continue
            assert kind == 10, f'Unexpected ARM64 base relocation type {kind}'
            target = page + within_page
            assert target + 8 <= image_size and within_page + 8 <= 4096
            value = struct.unpack_from('<Q', image, target)[0]
            assert image_base <= value < image_base + image_size
            relocated = (value + delta) & 0xffffffffffffffff
            struct.pack_into('<Q', image, target, relocated)
            assert new_base <= relocated < new_base + image_size
            changed += 1
        position += size
    assert changed, 'No usable absolute relocation entries'
    return changed


if __name__ == '__main__':
    count = validate(Path(sys.argv[1]).read_bytes())
    print(f'PASS: ARM64 native DLL driver, NX sections, page alignment, {count} DIR64 relocation(s) rebased to kernel address range')

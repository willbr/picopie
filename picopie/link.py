"""Link compiled functions into a single executable blob.

Each function comes in as (name, n_params, code, calls), where `calls` is
a list of (offset_within_code, target_function_name) for every BL placeholder.
We concatenate the functions, then patch every BL displacement.
"""

import struct


def link(functions):
    """Returns (blob_bytes, name_to_offset)."""
    blob = bytearray()
    offsets = {}

    for name, _n_params, code, _calls in functions:
        offsets[name] = len(blob)
        blob.extend(code)

    for name, _n_params, _code, calls in functions:
        base = offsets[name]
        for site_offset, target in calls:
            if target not in offsets:
                raise RuntimeError(f"unresolved call to '{target}'")
            site = base + site_offset
            disp = (offsets[target] - site) >> 2
            old = struct.unpack_from('<I', blob, site)[0]
            new = (old & 0xfc000000) | (disp & 0x03ffffff)
            struct.pack_into('<I', blob, site, new)

    return bytes(blob), offsets

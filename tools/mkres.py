#!/usr/bin/env python3
"""mkres.py - build a Windows COFF resource object without windres.

Embeds the app icon (RT_ICON + RT_GROUP_ICON), the application manifest
(RT_MANIFEST) and a VERSIONINFO resource (RT_VERSION) into an x86-64 COFF
object with a `.rsrc` section, in the same shape `windres -O coff` emits
(data-entry RVAs are fixed up with IMAGE_REL_AMD64_ADDR32NB relocations,
so any PE linker - lld, MSVC link, MinGW ld - resolves them).

Usage:
    python3 tools/mkres.py <icon.ico> <app.manifest> <out.o> \
        [--version 1.2.0.0] [--product "SwitchProXInput"]

Pure Python 3, no third-party dependencies.
"""
import struct
import sys

RT_ICON = 3
RT_GROUP_ICON = 14
RT_VERSION = 16
RT_MANIFEST = 24

LANG_EN_US = 0x0409
IMAGE_REL_AMD64_ADDR32NB = 3


# --------------------------------------------------------------------------
#  VERSIONINFO blob (VS_VERSION_INFO tree)
# --------------------------------------------------------------------------

def _align4(b: bytes) -> bytes:
    return b + b"\x00" * ((4 - len(b) % 4) % 4)


def _block(key: str, value: bytes, children: bytes, value_is_text: bool) -> bytes:
    """One IMAGE resource string block (VS_* structures all share the layout:
    wLength, wValueLength, wType, szKey, padding, Value, Children)."""
    key_b = key.encode("utf-16-le") + b"\x00\x00"
    w_value_len = len(value) // 2 if value_is_text else len(value)
    body = struct.pack("<HHH", 0, w_value_len, 1 if value_is_text else 0) + key_b
    body = _align4(body) + value
    body = _align4(body) + children
    return struct.pack("<H", len(body)) + body[2:]


def build_version_info(version: str, product: str) -> bytes:
    parts = [int(x) for x in version.split(".")]
    while len(parts) < 4:
        parts.append(0)
    ms = (parts[0] << 16) | parts[1]
    ls = (parts[2] << 16) | parts[3]
    # VS_FIXEDFILEINFO (52 bytes)
    fixed = struct.pack("<IIIIIIIIIIIII",
                        0xFEEF04BD, 0x0000,      # dwSignature, dwStrucVersion
                        ms, ls,                  # dwFileVersion
                        ms, ls,                  # dwProductVersion
                        0x3F, 0,                 # dwFileFlagsMask, dwFileFlags
                        0x00040004,              # VOS_NT_WINDOWS32
                        1, 0,                    # VFT_APP, subtype
                        0, 0)                    # dates

    strings = [
        ("CompanyName", "SwitchProXInput"),
        ("FileDescription", "Switch Pro Controller to XInput bridge"),
        ("FileVersion", version),
        ("InternalName", "SwitchProXInput"),
        ("LegalCopyright", "MIT License"),
        ("OriginalFilename", "SwitchProXInput.exe"),
        ("ProductName", product),
        ("ProductVersion", version),
    ]
    table_children = b""
    for k, v in strings:
        val = v.encode("utf-16-le") + b"\x00\x00"
        table_children += _block(k, val, b"", True)
    table = _block("040904B0", b"", table_children, False)
    sfi = _block("StringFileInfo", b"", table, False)

    var = _block("Translation", struct.pack("<HH", 0x0409, 1200), b"", False)
    vfi = _block("VarFileInfo", b"", var, False)

    return _block("VS_VERSION_INFO", fixed, sfi + vfi, False)


# --------------------------------------------------------------------------
#  Icon parsing
# --------------------------------------------------------------------------

def parse_ico(path: str):
    data = open(path, "rb").read()
    _, typ, n = struct.unpack_from("<HHH", data, 0)
    if typ != 1:
        raise SystemExit("not an .ico file")
    images, entries = [], []
    off = 6
    for _ in range(n):
        w, h, c, _r, planes, bpp, size, o = struct.unpack_from("<BBBBHHII", data, off)
        images.append(data[o:o + size])
        entries.append((w, h, c, planes, bpp, size))
        off += 16
    return entries, images


def build_group_icon(entries) -> bytes:
    out = struct.pack("<HHH", 0, 1, len(entries))
    for i, (w, h, c, planes, bpp, size) in enumerate(entries):
        out += struct.pack("<BBBBHHIH", w, h, c, 0, planes, bpp, size, i + 1)
    return out


# --------------------------------------------------------------------------
#  Resource tree + COFF emission
# --------------------------------------------------------------------------

class ResObject:
    def __init__(self):
        self.items = []       # (rtype, rid, blob)
    def add(self, rtype: int, rid: int, blob: bytes):
        self.items.append((rtype, rid, blob))

    def build(self) -> bytes:
        # Tree: root -> type nodes -> id nodes -> one language leaf each.
        types = {}
        for rtype, rid, blob in self.items:
            types.setdefault(rtype, []).append((rid, blob))
        for t in types:
            types[t].sort(key=lambda p: p[0])
        sorted_types = sorted(types)
        nleaf = sum(len(ids) for ids in types.values())

        root_sz = 16 + 8 * len(sorted_types)
        type_node_off, lang_node_off, cursor = {}, {}, root_sz
        for t in sorted_types:
            type_node_off[t] = cursor
            cursor += 16 + 8 * len(types[t])
        for t in sorted_types:
            for rid, _ in types[t]:
                lang_node_off[(t, rid)] = cursor
                cursor += 16 + 8                       # language node + its entry
        de_off = cursor                                # IMAGE_RESOURCE_DATA_ENTRYs
        blobs_off = de_off + 16 * nleaf

        # Lay out the data blobs (4-byte aligned), remembering offsets.
        blobs, blob_off = bytearray(), {}
        for i, (_, _, blob) in enumerate(self._ordered(types, sorted_types)):
            blobs += b"\x00" * ((4 - len(blobs) % 4) % 4)
            blob_off[i] = blobs_off + len(blobs)
            blobs += blob
        section_size = blobs_off + len(blobs)

        sec = bytearray(section_size)
        relocs = []

        # Root directory
        struct.pack_into("<IIHHHH", sec, 0, 0, 0, 0, 0, 0, len(sorted_types))
        e = 16
        for t in sorted_types:
            struct.pack_into("<II", sec, e, t, type_node_off[t] | 0x80000000)
            e += 8

        # Type nodes
        for t in sorted_types:
            o = type_node_off[t]
            ids = types[t]
            struct.pack_into("<IIHHHH", sec, o, 0, 0, 0, 0, 0, len(ids))
            e = o + 16
            for rid, _ in ids:
                struct.pack_into("<II", sec, e, rid,
                                 lang_node_off[(t, rid)] | 0x80000000)
                e += 8

        # Language nodes + data entries
        de = de_off
        for i, (t, rid, blob) in enumerate(self._ordered(types, sorted_types)):
            o = lang_node_off[(t, rid)]
            struct.pack_into("<IIHHHH", sec, o, 0, 0, 0, 0, 0, 1)
            struct.pack_into("<II", sec, o + 16, LANG_EN_US, de)
            struct.pack_into("<IIII", sec, de, blob_off[i], len(blob), 0, 0)
            relocs.append(de)                          # OffsetToData needs a fixup
            de += 16

        sec[blobs_off:blobs_off + len(blobs)] = blobs

        # ---- COFF wrapper (x86-64, one section) ----
        nreloc = len(relocs)
        nsyms = 2                                      # section symbol + aux
        raw_size = (section_size + 3) & ~3
        hdr_off, sec_off = 0, 20
        raw_off = sec_off + 40
        reloc_off = raw_off + raw_size
        sym_off = reloc_off + 10 * nreloc

        out = bytearray()
        out += struct.pack("<HHIIIHH", 0x8664, 1, 0, sym_off, nsyms, 0, 0)
        name = b".rsrc\x00\x00\x00"
        chars = 0x40000040 | (3 << 20)   # CNT_INITIALIZED_DATA | MEM_READ | ALIGN_4
        out += name + struct.pack("<IIIIIIHHI", 0, 0, raw_size, raw_off,
                                  reloc_off, 0, nreloc, 0, chars)
        out += bytes(sec) + b"\x00" * (raw_size - section_size)
        for r in relocs:
            out += struct.pack("<IIH", r, 0, IMAGE_REL_AMD64_ADDR32NB)
        # Section symbol ".rsrc" (class 102) + section-definition aux record.
        out += name + struct.pack("<IhHBB", 0, 1, 0, 102, 1)
        out += struct.pack("<IHHIHBxxx", raw_size, nreloc, 0, 0, 1, 0)
        out += struct.pack("<I", 4)                    # empty string table
        return bytes(out)

    @staticmethod
    def _ordered(types, sorted_types):
        for t in sorted_types:
            for rid, blob in types[t]:
                yield t, rid, blob


def main():
    args = sys.argv[1:]
    if len(args) < 3:
        raise SystemExit(__doc__)
    ico_path, manifest_path, out_path = args[0], args[1], args[2]
    version, product = "1.0.0.0", "SwitchProXInput"
    i = 3
    while i < len(args):
        if args[i] == "--version":
            version = args[i + 1]; i += 2
        elif args[i] == "--product":
            product = args[i + 1]; i += 2
        else:
            raise SystemExit(f"unknown arg {args[i]}")

    entries, images = parse_ico(ico_path)
    manifest = open(manifest_path, "rb").read()

    robj = ResObject()
    for idx, img in enumerate(images):
        robj.add(RT_ICON, idx + 1, img)
    robj.add(RT_GROUP_ICON, 101, build_group_icon(entries))
    robj.add(RT_MANIFEST, 1, manifest)
    robj.add(RT_VERSION, 1, build_version_info(version, product))

    with open(out_path, "wb") as f:
        f.write(robj.build())
    print(f"wrote {out_path}: {len(images)} icon image(s), manifest "
          f"{len(manifest)} bytes, version {version}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Wrap raw Xenos microcode dumps into synthetic 2008-layout shader containers.

Usage: python -I tools/wrap_unknown_shaders.py <unknown_shaders folder> <output folder>

Input  (written by airborne_dump_unknown_shaders, native_shaders.cpp DumpUnknownShader):
  "VSMC"/"PSMC", u32 BE word count, then the microcode words big-endian.
Output: <sha256>.bin containers (signature 0x102A1100 PS / 0x102A1101 VS) that
  tools/shaders_native (XenosRecomp fork) translates and the native renderer's Leer()
  (rex/src/native/native_shaders.cpp) parses. A manifest.txt maps source -> container.

What is synthesised (the dumps carry no D3D declaration):
  * CTAB: one Float4 array constant "g_AllFloatConstants" covering c0..c223 (PS) / c0..c255 (VS),
    so every constant read (also relative) becomes a constant-buffer read by register index and the
    renderer uploads the whole range. No samplers: texture fetches on undeclared samplers are
    declared by the translator and bound by Leer().
  * VS vertex elements: one per vertex fetch (opcode 0); POSITION for the fetch feeding the position
    export (else the first), TEXCOORD0.. for the rest. VS interpolators: TEXCOORDi / reg i for
    i = 0..highest export dest 0..15 (the translator keys them by export index).
  * PS interpolators: TEXCOORDi / reg i for the registers read before being written (max 16).
    PS outputs: colour exports 0..3 and depth (61).
  * No literal definitions; physical part = the microcode, padded to a multiple of 12 bytes.
"""
import hashlib
import os
import struct
import sys

# Source operand counts of the vector opcodes (shader_code.h AluVectorOpcode).
VEC_SRCS = {0: 2, 1: 2, 2: 2, 3: 2, 4: 2, 5: 2, 6: 2, 7: 2, 8: 1, 9: 1, 10: 1, 11: 3, 12: 3, 13: 3,
            14: 3, 15: 2, 16: 2, 17: 3, 18: 2, 19: 1, 20: 2, 21: 2, 22: 2, 23: 2, 24: 2, 25: 2,
            26: 2, 27: 2, 28: 2, 29: 2}
SCALAR_NO_DEST_OPS = set(range(27, 40))  # predicate setters and kills act without a write mask
TEX_FETCH_OPS = {1, 16, 17, 18, 19}


def parse_dump(data):
    tag = data[:4]
    if tag not in (b"VSMC", b"PSMC"):
        raise ValueError("bad tag %r" % tag)
    n = struct.unpack(">I", data[4:8])[0]
    if len(data) != 8 + n * 4:
        raise ValueError("size mismatch: %d words, %d bytes" % (n, len(data)))
    return tag == b"VSMC", list(struct.unpack(">%dI" % n, data[8:]))


def walk(words):
    """Yield ('alu'|'fetch', index) in control-flow order. Returns also the lowest exec address."""
    blocks = len(words) // 3
    limit = blocks
    execs = []
    i = 0
    while i < limit:
        a, b, c = words[i * 3:i * 3 + 3]
        for cf in (a | (b & 0xFFFF) << 32, b >> 16 | c << 16):
            op = (cf >> 44) & 0xF
            if (1 <= op <= 6) or op in (13, 14):
                addr, count, seq = cf & 0xFFF, (cf >> 12) & 7, (cf >> 16) & 0xFFF
                if addr:
                    limit = min(limit, addr)
                execs.append((addr, count, seq))
        i += 1
    out = []
    for addr, count, seq in execs:
        for j in range(count):
            k = addr + j
            if (k + 1) * 3 > len(words):
                continue
            out.append(("fetch" if (seq >> (j * 2)) & 1 else "alu", k))
    if not execs:
        raise ValueError("no exec clause")
    return out


def alu_sources(w):
    """Temporary registers an ALU instruction reads."""
    w0, w1, w2 = w
    regs = []
    vop = (w2 >> 24) & 0x1F
    sel = [(w2 >> 31) & 1, (w2 >> 30) & 1, (w2 >> 29) & 1]  # src1, src2, src3
    reg = [(w2 >> 16) & 0xFF, (w2 >> 8) & 0xFF, w2 & 0xFF]
    nvec = VEC_SRCS.get(vop, 3)
    for s in range(nvec):
        if sel[s]:
            regs.append(reg[s] & 0x3F)
    sop = (w0 >> 26) & 0x3F
    smask = (w0 >> 20) & 0xF
    if sop != 50 and (smask or sop in SCALAR_NO_DEST_OPS) and sel[2]:
        regs.append(reg[2] & 0x3F)
    return regs


def analyse(words, vertex):
    order = walk(words)
    exports = {"vec": set(), "pos_srcs": set()}
    vfetches = []  # (index, dst register)
    written = set()
    read_first = set()
    ps_outputs = 0
    for kind, k in order:
        w = words[k * 3:k * 3 + 3]
        if kind == "fetch":
            op = w[0] & 0x1F
            pred = (w[1] >> 31) & 1
            if op == 0:
                if vertex:
                    vfetches.append((k, (w[0] >> 12) & 0x3F))
            elif op in TEX_FETCH_OPS:
                src = (w[0] >> 5) & 0x3F
                if src not in written:
                    read_first.add(src)
            dst = (w[0] >> 12) & 0x3F
            if not pred:
                written.add(dst)
            continue
        w0 = w[0]
        export = (w0 >> 15) & 1
        vdest = w0 & 0x3F
        sdest = (w0 >> 8) & 0x3F
        vmask = (w0 >> 16) & 0xF
        smask = (w0 >> 20) & 0xF
        pred = (w[1] >> 28) & 1
        srcs = alu_sources(w)
        for r in srcs:
            if r not in written:
                read_first.add(r)
        if export:
            # The export register is the vector dest field for both halves (the translator does the same).
            for dest, mask in ((vdest, vmask | smask),):
                if not mask:
                    continue
                if vertex:
                    if dest < 16:
                        exports["vec"].add(dest)
                    elif dest == 62:
                        exports["pos_srcs"].update(srcs)
                else:
                    if dest < 4:
                        ps_outputs |= 1 << dest
                    elif dest == 61:
                        ps_outputs |= 0x10
        else:
            if not pred and vmask == 0xF:
                written.add(vdest)
            if not pred and smask == 0xF:
                written.add(sdest)
    return {"vfetches": vfetches, "exports": sorted(exports["vec"]), "pos_srcs": exports["pos_srcs"],
            "ps_outputs": ps_outputs, "reads_first": sorted(read_first)}


def ctab(vertex):
    """D3DXSHADER_CONSTANTTABLE with one Float4 array covering the whole register range."""
    count = 256 if vertex else 224
    name = b"g_AllFloatConstants\0"
    creator = b"AirborneNX wrap_unknown_shaders\0"
    target = b"vs_3_0\0" if vertex else b"ps_3_0\0"
    info_off = 28
    name_off = info_off + 20
    type_off = (name_off + len(name) + 3) & ~3
    creator_off = type_off + 16
    target_off = creator_off + len(creator)
    size = (target_off + len(target) + 3) & ~3
    body = bytearray(size)
    struct.pack_into(">7I", body, 0, 28, creator_off, 0xFFFE0300 if vertex else 0xFFFF0300, 1, info_off, 0,
                     target_off)
    # ConstantInfo: name, set (2 = Float4), index, count, reserved, typeInfo, defaultValue
    struct.pack_into(">IHHHHII", body, info_off, name_off, 2, 0, count, 0, type_off, 0)
    body[name_off:name_off + len(name)] = name
    # TypeInfo: class Vector(1), type Float(3), rows 1, columns 4, elements count
    struct.pack_into(">HHHHHHI", body, type_off, 1, 3, 1, 4, count, 0, 0)
    body[creator_off:creator_off + len(creator)] = creator
    body[target_off:target_off + len(target)] = target
    return struct.pack(">I", len(body)) + bytes(body)


def build(vertex, words):
    info = analyse(words, vertex)
    words = list(words)
    while len(words) % 3:
        words.append(0)
    micro = b"".join(struct.pack(">I", x) for x in words)
    table = ctab(vertex)
    shader_off = 36 + len(table)
    if vertex:
        fetches = info["vfetches"]
        pos = None
        for k, dst in fetches:
            if dst in info["pos_srcs"]:
                pos = k
                break
        if pos is None and fetches:
            pos = fetches[0][0]
        elements = []
        tex = 0
        for k, _ in fetches:
            if k == pos:
                elements.append(k | 0 << 12 | 0 << 16)
            else:
                elements.append(k | 5 << 12 | min(tex, 15) << 16)
                tex += 1
        interps = (max(info["exports"]) + 1) if info["exports"] else 0
        interp_words = [i | 5 << 4 | i << 8 for i in range(min(interps, 16))]
        header = struct.pack(">6I", 0, len(micro), 0, 0, 0, len(interp_words) << 5)
        header += struct.pack(">3I", 0, len(elements), 0)
        header += b"".join(struct.pack(">I", e) for e in elements)
        header += b"".join(struct.pack(">I", e) for e in interp_words)
        header += struct.pack(">I", 0)  # tail pad as in real containers
        meta = {"elements": len(elements), "pos_fetch": pos, "interpolators": interps}
    else:
        n = min(max(info["reads_first"]) + 1, 16) if info["reads_first"] else 0
        interp_words = [i | 5 << 4 | i << 8 for i in range(n)]
        # svPos register 0xFF: never matches r0..r31, so r0 stays a plain interpolator/temp.
        header = struct.pack(">6I", 0, len(micro), 0, 0xFF00, 0, n << 5)
        header += struct.pack(">2I", 0, info["ps_outputs"])
        header += b"".join(struct.pack(">I", e) for e in interp_words)
        meta = {"outputs": info["ps_outputs"], "interpolators": n}
    virtual = shader_off + len(header)
    head = struct.pack(">9I", 0x102A1100 | (1 if vertex else 0), virtual, len(micro), 0, 36, 0, shader_off, 0, 0)
    blob = head + table + header + micro
    assert len(head) + len(table) == shader_off and len(head + table + header) == virtual
    return blob, meta


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    manifest = []
    for name in sorted(os.listdir(src)):
        if not name.lower().endswith(".bin"):
            continue
        with open(os.path.join(src, name), "rb") as f:
            vertex, words = parse_dump(f.read())
        blob, meta = build(vertex, words)
        digest = hashlib.sha256(blob).hexdigest()
        with open(os.path.join(dst, digest + ".bin"), "wb") as f:
            f.write(blob)
        manifest.append("%s %s %s %s" % (digest, name, "VS" if vertex else "PS", meta))
        print(manifest[-1])
    with open(os.path.join(dst, "manifest.txt"), "w") as f:
        f.write("\n".join(manifest) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Extract bounded XDK 0x102A11 containers from the user's Airborne files.

Does not treat a signature as proof of a shader, nor claim compressed UE3
packages have been decoded. Derived shader data stays under generated/.
"""
from pathlib import Path
import argparse
import hashlib
import json
import mmap
import struct

ROOT = Path(__file__).resolve().parents[1]


def inspect_container(data, start=0):
    def bounded(at, size, limit):
        if at < 0 or size < 0 or at > limit or size > limit - at:
            raise ValueError('container range outside its owner')

    remaining = len(data) - start
    bounded(0, 36, remaining)
    u32 = lambda at: struct.unpack_from('>I', data, start + at)[0]
    flags, virtual, physical, _, ct, definitions, shader, _, _ = struct.unpack_from('>9I', data, start)
    if flags not in (0x102a1100, 0x102a1101):
        raise ValueError('unsupported XDK container version/stage')
    if virtual < 36 or not physical or virtual % 4 or physical % 4:
        raise ValueError('invalid container size/alignment')
    total = virtual + physical
    bounded(0, total, remaining)
    bounded(shader, 32 if flags == 0x102a1100 else 36, virtual)
    code_offset, code_size = u32(shader), u32(shader + 4)
    bounded(code_offset, code_size, physical)
    if not code_size or code_size % 12:
        raise ValueError('invalid microcode instruction length')

    # Packed 48-bit CF pairs refer to 96-bit instructions, in BE dwords.
    instructions = code_size // 12
    cf_limit = instructions
    block = 0
    exec_count = 0
    ended = False
    opcodes = set()
    while block < cf_limit:
        a, b, c = (u32(virtual + code_offset + block * 12 + j * 4) for j in range(3))
        for cf in (a | ((b & 0xffff) << 32), (b >> 16) | (c << 16)):
            opcode = cf >> 44
            opcodes.add(opcode)
            if opcode in (1, 2, 3, 4, 5, 6, 13, 14):
                address, count = cf & 0xfff, (cf >> 12) & 7
                if address <= block or count > 6 or address + count > instructions:
                    raise ValueError('EXEC range outside shader instructions')
                cf_limit = min(cf_limit, address)
                exec_count += count
                ended |= opcode in (2, 4, 6, 14)
        block += 1
    if not ended or not exec_count:
        raise ValueError('no executable terminating control flow')

    constants = []
    if ct:
        bounded(ct, 32, virtual)
        ct_size = u32(ct)
        bounded(ct, ct_size, virtual)
        base = ct + 4
        count, entries = u32(base + 12), u32(base + 16)
        bounded(base + entries, count * 20, ct + ct_size)
        for i in range(count):
            at = base + entries + i * 20
            name, register_set, index, registers, reserved, type_info, default = struct.unpack_from('>I4H2I', data, start + at)
            bounded(base + name, 1, ct + ct_size)
            end = data.find(b'\0', start + base + name, start + ct + ct_size)
            if end < 0:
                raise ValueError('unterminated reflection name')
            bounded(base + type_info, 16, ct + ct_size)
            if register_set > 3 or not registers:
                raise ValueError('unknown/empty reflection register range')
            constants.append(dict(name=bytes(data[start + base + name:end]).decode('ascii'),
                                  register_set=register_set, index=index, count=registers))
    # Validate tables traversed by XenosRecomp before passing it any bytes.
    if definitions:
        bounded(definitions, 20, virtual)
        at = definitions + 20
        while True:
            bounded(at, 4, virtual)
            if not u32(at):
                at += 4
                break
            bounded(at, 8, virtual)
            count = u32(at) & 0xffff
            bounded(u32(at + 4), ((count + 3) // 4) * 16, physical)
            at += 8
        while True:
            bounded(at, 4, virtual)
            if not u32(at):
                break
            count = u32(at) & 0xffff
            bounded(at, 8 + count * 4, virtual)
            at += 8 + count * 4
    interpolators = (u32(shader + 20) >> 5) & 31
    if flags & 1:
        skip, elements = u32(shader + 24), u32(shader + 28)
        bounded(shader + 36, (skip + elements + interpolators) * 4, virtual)
    else:
        bounded(shader + 32, interpolators * 4, virtual)
    code = bytes(data[start + virtual + code_offset:start + virtual + code_offset + code_size])
    return dict(stage='vertex' if flags & 1 else 'pixel', bytes=total,
                virtual_bytes=virtual, physical_bytes=physical, shader_offset=shader,
                code_offset=code_offset, code_bytes=code_size,
                code_sha256=hashlib.sha256(code).hexdigest(), cf_blocks=cf_limit,
                cf_opcodes=sorted(opcodes), constants=constants,
                reflection_present=bool(ct))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image-only', action='store_true')
    options = parser.parse_args()
    paths = [ROOT / 'analysis/image/image_loader_normalized.bin']
    if not options.image_only:
        paths += sorted((ROOT / 'extracted/base/MOHAGame/CookedXenon').glob('*.xxx'))
    out = ROOT / 'generated/shaders/containers'
    out.mkdir(parents=True, exist_ok=True)
    found, rejected, scanned = {}, [], []
    for path in paths:
        if not path.stat().st_size:
            continue
        with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            position, candidates, accepted = 0, 0, 0
            while (position := data.find(b'\x10\x2a\x11', position)) >= 0:
                start = position
                position += 1
                candidates += 1
                try:
                    record = inspect_container(data, start)
                except (ValueError, struct.error, UnicodeError) as error:
                    rejected.append(dict(path=str(path.relative_to(ROOT)), offset=start, reason=str(error)))
                    continue
                accepted += 1
                raw = bytes(data[start:start + record['bytes']])
                digest = hashlib.sha256(raw).hexdigest()
                origin = dict(path=str(path.relative_to(ROOT)), offset=start)
                if digest in found:
                    found[digest]['origins'].append(origin)
                else:
                    target = out / (digest + '.bin')
                    target.write_bytes(raw)
                    found[digest] = dict(record, sha256=digest, path=str(target.relative_to(ROOT)), origins=[origin])
            scanned.append(dict(path=str(path.relative_to(ROOT)), bytes=len(data),
                                sha256=hashlib.sha256(data).hexdigest(), candidates=candidates, accepted=accepted))
    report = dict(scope='Validated uncompressed XDK containers only; no claim of complete UE3 shader coverage',
                  image_only=options.image_only, files=scanned, unique_shaders=len(found),
                  shaders=sorted(found.values(), key=lambda r:r['sha256']), rejected=rejected,
                  tool_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    destination = ROOT / 'analysis/game_shader_inventory.json'
    destination.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'{len(found)} unique shader containers; {len(rejected)} rejected candidates; {len(scanned)} files scanned')


if __name__ == '__main__':
    main()

"""Medal of Honor: Airborne (Xbox 360) Coalesced files: decrypt, list, patch, encrypt.

The Xbox build keeps its configuration in MOHAGame/Config/Xenon/Cooked/Coalesced.ini and the text of each
language in MOHAGame/Localization/Coalesced.<lang>. Both are encrypted with a byte-wise XOR stream found in
the game's loader (sub_8232BA20 at 0x8232BC94):

    state = 0x0BADF00D
    for each byte: state = state * 0x0BB38435 + 0x3619636B (mod 2^32); byte ^= state & 0xFF

The same function encrypts and decrypts. Decrypted layout, big-endian int32 throughout:
    string_count (two per file: name and content)
    per file: name_len, name (ASCII, NUL included in name_len),
              content_len, content (content_len > 0: 8-bit text with NUL;
                                    content_len < 0: UTF-16LE, -content_len code units with NUL)

Usage:
    python tools/coalesced.py list  <Coalesced.x>
    python tools/coalesced.py dump  <Coalesced.x> <out_dir>
    python tools/coalesced.py set   <Coalesced.x> <out> "<file suffix>" "[Section]" Key=Value [Key=Value ...]

"set" replaces the value of every Key in [Section] of the inner file whose name ends with <file suffix>
(case-insensitive), e.g. "Cooked\\MOHAEngine.ini". The output is encrypted like the original.
"""
import os
import struct
import sys


def crypt(data):
    state = 0x0BADF00D
    out = bytearray(data)
    for i in range(len(out)):
        state = (state * 0x0BB38435 + 0x3619636B) & 0xFFFFFFFF
        out[i] ^= state & 0xFF
    return bytes(out)


def parse(plain):
    count, = struct.unpack_from(">i", plain, 0)
    pos = 4
    files = []
    for _ in range(count // 2):
        n, = struct.unpack_from(">i", plain, pos)
        pos += 4
        name = plain[pos:pos + n].rstrip(b"\0").decode("latin-1")
        pos += n
        c, = struct.unpack_from(">i", plain, pos)
        pos += 4
        if c >= 0:
            text = plain[pos:pos + c].rstrip(b"\0").decode("latin-1")
            pos += c
            wide = False
        else:
            text = plain[pos:pos - c * 2].decode("utf-16-le").rstrip("\0")
            pos += -c * 2
            wide = True
        files.append([name, text, wide])
    # Coalesced.int ends with bytes no entry covers; they are kept as they are.
    return files, plain[pos:]


def serialize(files, trailer=b""):
    out = bytearray(struct.pack(">i", len(files) * 2))
    for name, text, wide in files:
        raw = name.encode("latin-1") + b"\0"
        out += struct.pack(">i", len(raw)) + raw
        if wide:
            body = (text + "\0").encode("utf-16-le")
            out += struct.pack(">i", -(len(body) // 2)) + body
        else:
            body = text.encode("latin-1") + b"\0"
            out += struct.pack(">i", len(body)) + body
    return bytes(out) + trailer


def set_values(text, section, pairs):
    lines = text.split("\r\n") if "\r\n" in text else text.split("\n")
    sep = "\r\n" if "\r\n" in text else "\n"
    current = None
    done = set()
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            current = stripped
            continue
        if current and current.lower() == section.lower() and "=" in line:
            key = line.split("=", 1)[0].strip()
            if key in pairs:
                lines[i] = "%s=%s" % (key, pairs[key])
                done.add(key)
    return sep.join(lines), done


def main():
    cmd, src = sys.argv[1], sys.argv[2]
    files, trailer = parse(crypt(open(src, "rb").read()))
    if cmd == "list":
        for name, text, wide in files:
            print("%-70s %8d chars %s" % (name, len(text), "utf16" if wide else "8bit"))
        print("trailer: %d bytes" % len(trailer))
    elif cmd == "dump":
        out_dir = sys.argv[3]
        for name, text, wide in files:
            rel = name.replace("..\\", "").replace("\\", os.sep)
            path = os.path.join(out_dir, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w", encoding="utf-8", newline="").write(text)
        print("dumped %d files to %s" % (len(files), out_dir))
    elif cmd == "set":
        out, suffix, section = sys.argv[3], sys.argv[4].lower(), sys.argv[5]
        pairs = dict(a.split("=", 1) for a in sys.argv[6:])
        matched = False
        for f in files:
            if f[0].lower().endswith(suffix):
                f[1], done = set_values(f[1], section, pairs)
                matched = True
                print("%s %s: set %s, missing %s" % (f[0], section, sorted(done), sorted(set(pairs) - done)))
        if not matched:
            raise SystemExit("no inner file ends with " + suffix)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        open(out, "wb").write(crypt(serialize(files, trailer)))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()

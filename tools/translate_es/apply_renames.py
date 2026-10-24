#!/usr/bin/env python3
"""Rename identifiers in C++ sources from a reviewed Spanish -> English map.

Usage:
  python -I apply_renames.py MAP DIR --dry-run
  python -I apply_renames.py MAP DIR --apply

MAP  tsv with a header; columns: spanish, english, ... (rename_map_reviewed.tsv).
DIR  searched recursively for *.cpp, *.h, *.hpp, *.cc, *.inl (other files are
     never touched).

Only whole C++ identifier tokens are replaced. The scanner skips, byte for byte:
  * // comments (including backslash-newline continuations) and /* */ comments,
  * string literals with any encoding prefix (L, u, U, u8) and raw strings
    R"delim( ... )delim" (also LR, uR, UR, u8R),
  * character literals (digit separators such as 1'000 are not mistaken for
    them), and numeric literals such as 0x7e3 or 1e-5f,
  * the whole of a #include line (tokens that match the map there are counted
    and reported, never changed).
Line endings and the BOM are preserved. --dry-run writes nothing; --apply
rewrites only the files that change. Exit code 0 on success.

Not handled (none occur in rex/src/native; the dry run reports them if they do):
identifiers built by token pasting (##) or stringizing (#x) inside macros.
"""
import os
import re
import sys
from collections import Counter

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
PPNUM = re.compile(r"\.?[0-9](?:[eEpP][+-]|[A-Za-z0-9_.']|'(?=[A-Za-z0-9_]))*")
EXTS = (".cpp", ".h", ".hpp", ".cc", ".inl")
STR_PREFIX = {"L", "u", "U", "u8"}
RAW_PREFIX = {"R", "LR", "uR", "UR", "u8R"}
INCLUDE = re.compile(r"#[ \t]*(?:include|include_next|import)\b")


def load_map(path):
    m = {}
    with open(path, encoding="utf-8") as fh:
        next(fh)
        for line in fh:
            c = line.rstrip("\r\n").split("\t")
            if len(c) < 2 or not c[0]:
                continue
            if c[0] == c[1]:
                continue
            if c[0] in m and m[c[0]] != c[1]:
                sys.exit("map has two targets for %s" % c[0])
            if not IDENT.fullmatch(c[1]):
                sys.exit("invalid target %r for %s" % (c[1], c[0]))
            m[c[0]] = c[1]
    targets = Counter(m.values())
    dup = [t for t, n in targets.items() if n > 1]
    if dup:
        sys.exit("map targets are not unique: %s" % ", ".join(sorted(dup)[:10]))
    return m


def rename_source(src, mapping, hits, include_hits):
    """Returns the new text. hits: Counter of renamed identifiers;
    include_hits: list of (line_no, identifier) found in #include lines."""
    out = []
    n = len(src)
    i = 0
    line = 1
    at_line_start = True  # only whitespace seen since the last newline
    while i < n:
        c = src[i]
        if c == "\n":
            out.append(c)
            i += 1
            line += 1
            at_line_start = True
            continue
        if at_line_start and c in " \t":
            out.append(c)
            i += 1
            continue
        if at_line_start and c == "#":
            m = INCLUDE.match(src, i)
            if m:
                j = src.find("\n", i)
                j = n if j < 0 else j
                seg = src[i:j]
                for t in IDENT.finditer(seg):
                    if t.group(0) in mapping:
                        include_hits.append((line, t.group(0)))
                out.append(seg)
                i = j
                at_line_start = False
                continue
        at_line_start = False
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = i + 2
            while True:
                k = src.find("\n", j)
                if k < 0:
                    j = n
                    break
                # a backslash (optionally followed by \r) before the newline continues the comment
                e = k - 1
                if e >= 0 and src[e] == "\r":
                    e -= 1
                if e >= 0 and src[e] == "\\":
                    j = k + 1
                    line += 1
                    continue
                j = k
                break
            out.append(src[i:j])
            i = j
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            seg = src[i:j]
            line += seg.count("\n")
            out.append(seg)
            i = j
        elif c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == "\\":
                    j += 1
                if j < n and src[j] == "\n":
                    break
                j += 1
            j = min(j + 1, n)
            out.append(src[i:j])
            i = j
        elif c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                if src[j] == "\\":
                    j += 1
                if j < n and src[j] == "\n":
                    break
                j += 1
            j = min(j + 1, n)
            out.append(src[i:j])
            i = j
        elif c.isdigit() or (c == "." and nxt.isdigit()):
            m = PPNUM.match(src, i)
            j = m.end() if m else i + 1
            out.append(src[i:j])
            i = j
        elif c.isalpha() or c == "_":
            m = IDENT.match(src, i)
            word = m.group(0)
            k = m.end()
            if k < n and src[k] == '"' and word in RAW_PREFIX:
                p = src.find("(", k + 1)
                delim = src[k + 1:p] if p > 0 else ""
                end = src.find(")" + delim + '"', p) if p > 0 else -1
                j = n if end < 0 else end + len(delim) + 2
                seg = src[i:j]
                line += seg.count("\n")
                out.append(seg)
                i = j
            elif k < n and src[k] in "\"'" and word in STR_PREFIX:
                q = src[k]
                j = k + 1
                while j < n and src[j] != q:
                    if src[j] == "\\":
                        j += 1
                    if j < n and src[j] == "\n":
                        break
                    j += 1
                j = min(j + 1, n)
                out.append(src[i:j])
                i = j
            else:
                new = mapping.get(word)
                if new is not None:
                    hits[word] += 1
                    out.append(new)
                else:
                    out.append(word)
                i = k
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main(argv):
    if len(argv) != 4 or argv[3] not in ("--dry-run", "--apply"):
        print(__doc__)
        return 2
    mapping = load_map(argv[1])
    root = argv[2]
    apply_ = argv[3] == "--apply"
    totals = Counter()
    per_file = []
    all_include_hits = []
    files = 0
    for dp, _, fns in os.walk(root):
        for fn in sorted(fns):
            if not fn.endswith(EXTS):
                continue
            path = os.path.join(dp, fn)
            with open(path, "rb") as fh:
                raw = fh.read()
            text = raw.decode("utf-8", errors="surrogateescape")
            hits = Counter()
            inc = []
            new = rename_source(text, mapping, hits, inc)
            files += 1
            totals.update(hits)
            for ln, name in inc:
                all_include_hits.append((path, ln, name))
            per_file.append((path, sum(hits.values()), len(hits)))
            if apply_ and new != text:
                with open(path, "wb") as fh:
                    fh.write(new.encode("utf-8", errors="surrogateescape"))
    mode = "APPLIED" if apply_ else "DRY RUN (nothing written)"
    print("%s: map %d renames, %d files scanned" % (mode, len(mapping), files))
    for path, n, d in per_file:
        print("  %6d tokens, %4d distinct  %s" % (n, d, path))
    print("total: %d tokens replaced, %d of %d map identifiers used"
          % (sum(totals.values()), len(totals), len(mapping)))
    unused = sorted(set(mapping) - set(totals))
    print("map identifiers not found as code tokens: %d%s" % (
        len(unused), (" (e.g. %s)" % ", ".join(unused[:8])) if unused else ""))
    print("tokens inside #include lines (skipped): %d" % len(all_include_hits))
    for path, ln, name in all_include_hits:
        print("  %s:%d %s" % (path, ln, name))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

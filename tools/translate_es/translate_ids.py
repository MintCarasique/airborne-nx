#!/usr/bin/env python3
"""Build a Spanish -> English identifier rename map. Nothing is renamed.

Reads:  identifiers.tsv and words.tsv (this folder), every file under
        rex\\src\\native (scanned with inventory.py's own scanner).
Writes: rename_map.tsv, collisions.md (this folder).

Run:    python -I D:\\Games\\AirborneNX\\analysis\\native_es\\translate_ids.py

Word splitting is the same as inventory.py (underscore parts, then
CamelCase pieces). Each piece is translated through words.tsv, the original
capitalization style is kept per piece, and separators are preserved.
A word whose translation is empty (articles, "de", "del", ...) is dropped.
"""
import importlib.util
import os
import re
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_ROOT = r"D:\Games\AirborneNX\rex\src\native"
IDENTS = os.path.join(HERE, "identifiers.tsv")
WORDS = os.path.join(HERE, "words.tsv")
INVENTORY = os.path.join(HERE, "inventory.py")
OUT_MAP = os.path.join(HERE, "rename_map.tsv")
OUT_MD = os.path.join(HERE, "collisions.md")
SUFFIX = "_es"

CAMEL = re.compile(r"[A-Z]{2,}(?![a-z])|[A-Z]?[a-z]+|[A-Z]|[0-9]+")
SEPARATORS = re.compile(r"(_+)")

# (c) keywords are matched exactly (they are lowercase).
CPP_KEYWORDS = frozenset("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char
char8_t char16_t char32_t class compl concept const consteval constexpr
constinit const_cast continue co_await co_return co_yield decltype default
delete do double dynamic_cast else enum explicit export extern false float
for friend goto if inline int long mutable namespace new noexcept not not_eq
nullptr operator or or_eq private protected public register reinterpret_cast
requires return short signed sizeof static static_assert static_cast struct
switch template this thread_local throw true try typedef typeid typename
union unsigned using virtual void volatile wchar_t while xor xor_eq
""".split())

# (c) common std / rex / C-library names, matched case-insensitively.
COMMON_NAMES = frozenset("""
data size begin end string map min max log vector list set array pair tuple
swap find count sort move copy free malloc memcpy memset memcmp memcmp assert
printf abs exp main std rex cvar size_t fill clear reset make_shared
unique_ptr shared_ptr optional function
""".split())


def load_inventory():
    spec = importlib.util.spec_from_file_location("inventory", INVENTORY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def codebase_identifiers(inv):
    """Every identifier token in every file under SRC_ROOT (comments and
    string literals excluded by the inventory scanner)."""
    counts = Counter()
    nfiles = 0
    for dp, _, fns in os.walk(SRC_ROOT):
        for fn in fns:
            with open(os.path.join(dp, fn), "rb") as fh:
                raw = fh.read().decode("utf-8", errors="replace")
            raw = raw.lstrip("\ufeff").replace("\r\n", "\n")
            code, _, _ = inv.scan(raw)
            counts.update(m.group(0) for m in inv.IDENT.finditer(code))
            nfiles += 1
    return counts, nfiles


def load_words():
    table = {}
    with open(WORDS, encoding="utf-8") as fh:
        for i, line in enumerate(fh):
            if i == 0:
                continue
            cols = line.rstrip("\r\n").split("\t")
            cols += [""] * (3 - len(cols))
            table[cols[0]] = (cols[1], cols[2])
    return table


def case_like(tok, eng):
    """Give the English word the capitalization style of the Spanish piece."""
    if len(tok) > 1 and tok.isupper():
        return eng.upper()
    if tok[0].isupper():
        return eng[:1].upper() + eng[1:]
    return eng


class Translator:
    def __init__(self, table):
        self.table = table
        self.missing = set()
        self.seen = set()
        self.dropped = Counter()

    def piece(self, w):
        """Translate one run of letters/digits (no underscores)."""
        toks = CAMEL.findall(w)
        if "".join(toks) != w:
            raise ValueError("cannot split %r" % w)
        out = []
        for t in toks:
            if t.isdigit():
                out.append(t)
                continue
            key = t.lower()
            self.seen.add(key)
            if key not in self.table:
                self.missing.add(key)
                out.append(t)
                continue
            eng = self.table[key][0]
            if eng == "":
                self.dropped[key] += 1
                continue
            out.append(case_like(t, eng))
        return "".join(out)

    def name(self, ident):
        """Returns the English identifier, or None if everything was dropped."""
        lead = len(ident) - len(ident.lstrip("_"))
        trail = len(ident) - len(ident.rstrip("_"))
        core = ident[lead:len(ident) - trail] if trail else ident[lead:]
        if core == "":
            return None
        pieces = SEPARATORS.split(core)
        words = pieces[0::2]
        seps = [""] + pieces[1::2]
        out = ""
        started = False
        for w, sep in zip((self.piece(x) for x in words), seps):
            if w == "":
                continue
            if started:
                out += sep
            out += w
            started = True
        if out == "":
            return None
        return "_" * lead + out + "_" * trail


def check_map(path):
    """--map FILE: re-run the collision checks on a reviewed map.

    FILE has columns spanish, english, kind, ... Checks (strict):
      (a) two different Spanish identifiers share one English name;
      (b) an English name equals another identifier that exists in
          rex/src/native (identity rows, english == spanish, are exempt);
      (c) an English name is a C++ keyword (exact) or a std/rex name
          (case-insensitive; identity rows exempt).
    Also reports empty names and names that are not valid identifiers.
    Returns the number of problems (0 = clean)."""
    inv = load_inventory()
    codebase, nfiles = codebase_identifiers(inv)
    rows = []
    with open(path, encoding="utf-8") as fh:
        next(fh)
        for line in fh:
            c = line.rstrip("\r\n").split("\t")
            if len(c) >= 2:
                rows.append((c[0], c[1]))
    by_en = defaultdict(list)
    for sp, en in rows:
        by_en[en].append(sp)
    problems = []
    seen_sp = Counter(sp for sp, _ in rows)
    for sp, n in seen_sp.items():
        if n > 1:
            problems.append("duplicate spanish row: %s" % sp)
    for sp, en in rows:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", en):
            problems.append("(invalid) %s -> %r" % (sp, en))
            continue
        if sp == en:
            continue
        if len(by_en[en]) > 1:
            problems.append("(a) %s -> %s shared with %s" % (
                sp, en, ", ".join(x for x in by_en[en] if x != sp)))
        if en in codebase:
            problems.append("(b) %s -> %s exists in rex/src/native (%d)"
                            % (sp, en, codebase[en]))
        if en in CPP_KEYWORDS:
            problems.append("(c) %s -> %s is a C++ keyword" % (sp, en))
        elif en.lower() in COMMON_NAMES:
            problems.append("(c) %s -> %s is a std/rex name" % (sp, en))
    print("map: %s, rows: %d, source files scanned: %d" % (path, len(rows), nfiles))
    for p in problems:
        print("  " + p)
    print("collisions/problems: %d" % len(problems))
    return len(problems)


def main():
    import sys
    if "--map" in sys.argv:
        i = sys.argv.index("--map")
        sys.exit(1 if check_map(sys.argv[i + 1]) else 0)
    inv = load_inventory()
    table = load_words()
    codebase, nfiles = codebase_identifiers(inv)

    rows = []
    with open(IDENTS, encoding="utf-8") as fh:
        next(fh)
        for line in fh:
            cols = line.rstrip("\r\n").split("\t")
            if len(cols) < 4:
                continue
            rows.append({"name": cols[0], "occ": int(cols[1]),
                         "kind": cols[3], "note": []})
    rows.sort(key=lambda r: (-r["occ"], r["name"]))

    tr = Translator(table)
    for r in rows:
        dst = tr.name(r["name"])
        if dst is None:
            dst = r["name"]
            r["note"].append("empty translation, kept")
        r["raw"] = dst
        r["identity"] = dst == r["name"]

    # Group every row (identity rows included) by its target name.
    groups = defaultdict(list)
    for r in rows:
        groups[r["raw"]].append(r)

    for dst, members in groups.items():
        ident = [m for m in members if m["identity"]]
        keeper = ident[0] if ident else members[0]
        for m in members:
            if m is keeper:
                if len(members) > 1:
                    others = [x["name"] for x in members if x is not m]
                    m["note"].append("kept; also target of " + ", ".join(others))
                if m["identity"]:
                    continue
            if m["identity"]:
                continue
            # (b) target already exists in the codebase as another identifier
            if dst in codebase:
                m["note"].append(
                    "(b) '%s' already exists in rex/src/native (%d occurrences)"
                    % (dst, codebase[dst]))
            # (c) C++ keyword or common std/rex name
            if dst in CPP_KEYWORDS:
                m["note"].append("(c) '%s' is a C++ keyword" % dst)
            elif dst.lower() in COMMON_NAMES:
                m["note"].append("(c) '%s' is a common std/rex name" % dst)
            # (a) several Spanish identifiers map to the same target
            if m is not keeper:
                m["note"].append(
                    "(a) same target '%s' as '%s' (kept the name)"
                    % (dst, keeper["name"]))

    taken = set(codebase)
    for r in rows:
        if r["identity"]:
            r["final"] = r["name"]
            continue
        flagged = any(n[:3] in ("(a)", "(b)", "(c)") for n in r["note"])
        if not flagged:
            r["final"] = r["raw"]
            taken.add(r["raw"])
            continue
        cand = r["raw"] + SUFFIX
        n = 2
        while cand in taken:
            cand = "%s%s%d" % (r["raw"], SUFFIX, n)
            n += 1
        taken.add(cand)
        r["final"] = cand
        r["note"].append("raw '%s' -> '%s'" % (r["raw"], cand))

    with open(OUT_MAP, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("spanish\tenglish\tkind\toccurrences\tcollision_note\n")
        for r in rows:
            fh.write("%s\t%s\t%s\t%d\t%s\n" % (
                r["name"], r["final"], r["kind"], r["occ"],
                "; ".join(r["note"])))

    changed = [r for r in rows if r["final"] != r["name"]]
    kinds = Counter(r["kind"] for r in changed)
    type_counts = Counter()
    for r in rows:
        for tag in ("(a)", "(b)", "(c)"):
            if any(n.startswith(tag) for n in r["note"]):
                type_counts[tag] += 1
    renamed_for_collision = [r for r in rows if r["final"] != r["raw"]]

    with open(OUT_MD, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Collisions in the Spanish -> English rename map\n\n")
        fh.write("Map: `rename_map.tsv`. Nothing has been renamed in the sources.\n\n")
        fh.write("Suffix rule: a colliding identifier gets `%s` appended (then "
                 "`%s2`, `%s3`, ... if that also exists). The most frequent "
                 "identifier of a group keeps the plain English name, and an "
                 "identifier that is already English stays as it is.\n\n"
                 % (SUFFIX, SUFFIX, SUFFIX))
        fh.write("Checks: (a) two or more Spanish identifiers share a target; "
                 "(b) the target already exists in `rex/src/native` as another "
                 "identifier (all identifiers of %d files are scanned); (c) the "
                 "target is a C++ keyword (exact match) or a common std/rex name "
                 "(case-insensitive).\n\n" % nfiles)
        fh.write("Rows flagged (a): %d, (b): %d, (c): %d. Renamed for a "
                 "collision: %d.\n\n" % (type_counts["(a)"], type_counts["(b)"],
                                        type_counts["(c)"],
                                        len(renamed_for_collision)))
        fh.write("| spanish | raw English | final | resolution / reasons |\n")
        fh.write("|---|---|---|---|\n")
        for r in renamed_for_collision:
            reasons = "; ".join(n for n in r["note"] if not n.startswith("raw "))
            fh.write("| `%s` | `%s` | `%s` | %s |\n" % (
                r["name"], r["raw"], r["final"], reasons))

    print("source files scanned: %d, codebase identifiers: %d distinct"
          % (nfiles, len(codebase)))
    print("Spanish identifiers: %d" % len(rows))
    print("distinct words used: %d (words.tsv rows: %d)"
          % (len(tr.seen), len(table)))
    print("words not in words.tsv: %s" % (sorted(tr.missing) or "none"))
    unused = sorted(set(table) - tr.seen)
    print("words.tsv rows not used: %d %s" % (len(unused), unused[:40]))
    print("identifiers changed (final != spanish): %d" % len(changed))
    print("changed by kind: %s" % dict(kinds))
    print("collision rows: (a)=%d (b)=%d (c)=%d; renamed for collision=%d"
          % (type_counts["(a)"], type_counts["(b)"], type_counts["(c)"],
             len(renamed_for_collision)))
    print("words dropped as empty translation: %s" % dict(tr.dropped))
    print("outputs: %s, %s" % (OUT_MAP, OUT_MD))


if __name__ == "__main__":
    main()

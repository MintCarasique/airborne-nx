# Spanish to English renaming of rex/src/native

The native renderer came from the NFS: Most Wanted port with Spanish identifiers.
These scripts translated them (stdlib Python, run with `python -I`):

- `inventory.py` lists the Spanish identifiers in a source tree (word lists: `words.tsv`, `words_extra.tsv`).
- `translate_ids.py` builds a first word-by-word map and, with `--map FILE`, checks a map for collisions
  (duplicate targets, targets that already exist, C++ keywords and std/rex names).
- `review_ids.py` rebuilds the map in English word order and applies `overrides.tsv` last:
  `rename_map_reviewed.tsv`.
- `rename_map_apply.tsv` is that map without names that belong to the SDK (`rex::` paths, `VK_*`,
  `RexSwitch*` externs, SDK struct fields and macros).
- `apply_renames.py MAP DIR --dry-run|--apply` renames whole C++ tokens outside string/char literals,
  comments and `#include` lines.
- `cvars_es.tsv`: proposed English names for the cvars, which are a user-visible interface and are renamed
  in a separate step. Log strings and comments are still Spanish.

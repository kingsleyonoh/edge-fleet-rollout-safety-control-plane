# Patterns — Index

> **One file per pattern.** This index is a human-readable catalog, rewritten by the AI whenever a sibling file is added, renamed, or removed. Never append to a single growing file — write a new sibling instead. See `.agent/rules/CODING_STANDARDS.md` — "Append-Only Knowledge Files Banned."

## Catalog

| File | Summary |
|------|---------|
| `EXAMPLE.md` | Template showing the expected shape — delete once a real pattern exists. |

## How to add a new pattern

1. Pick a short kebab-case slug describing the pattern (e.g. `row-lock-allocator`, `two-phase-finalize`, `hash-chain-audit`).
2. Prefix with a zero-padded sequence number so files sort by discovery order (e.g. `001-row-lock-allocator.md`).
3. Write the file using `EXAMPLE.md` as the shape.
4. Add one row to the `## Catalog` table above with the filename and a one-line summary.
5. Cross-references from other rules files use the full path: `.agent/knowledge/patterns/NNN-slug.md`.

## Why directory-per-kind

Single append-only files grow forever and eventually hit the 12K auto-load truncation limit. Splitting them is firefighting — the fire is the append-only model. New content = new file is the only layout that scales. See `MAINTAINING.md` — "Append-Only Knowledge Files Banned."

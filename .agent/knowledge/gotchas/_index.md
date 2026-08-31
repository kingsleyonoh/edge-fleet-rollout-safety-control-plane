# Gotchas — Index

> **One file per gotcha.** This index is a human-readable catalog, rewritten by the AI whenever a sibling file is added, renamed, or removed. Never append to a single growing table — write a new sibling instead. See `.agent/rules/CODING_STANDARDS.md` — "Append-Only Knowledge Files Banned."

## Catalog

| File | Summary |
|------|---------|
| `EXAMPLE.md` | Template showing the expected shape — delete once a real gotcha exists. |

## How to add a new gotcha

1. Filename pattern: `YYYY-MM-DD-short-slug.md` (date of discovery + kebab-case slug).
2. Copy the shape from `EXAMPLE.md`. Record the observed symptom, verified cause, tested solution, discovery context, affected paths or versions, and the evidence that supports the entry.
3. Add one row to the `## Catalog` table above.
4. If the gotcha could affect other projects on the same stack, state that scope in the sibling file. Promotion outside this repository requires a separate review of the recorded evidence.

## Why directory-per-kind

A single `## Gotchas & Lessons Learned` table grows monotonically as every batch appends a row. The table hits 50 rows, then 200, then a size-limit platform truncates the file silently. New file per gotcha eliminates the problem — and git history per gotcha becomes atomic. See `MAINTAINING.md` — "Append-Only Knowledge Files Banned."

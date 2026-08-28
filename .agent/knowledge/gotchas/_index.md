# Gotchas — Index

> **One file per gotcha.** This index is a human-readable catalog, rewritten by the AI whenever a sibling file is added, renamed, or removed. Never append to a single growing table — write a new sibling instead. See `.agent/rules/CODING_STANDARDS.md` — "Append-Only Knowledge Files Banned."

## Catalog

| File | Summary |
|------|---------|
| `EXAMPLE.md` | Template showing the expected shape — delete once a real gotcha exists. |

## How to add a new gotcha

1. Filename pattern: `YYYY-MM-DD-short-slug.md` (date of discovery + kebab-case slug).
2. Use the Symptom / Cause / Solution / Discovered in / Affects shape from `EXAMPLE.md` — matches `knowledge/gotchas-by-stack/` format so entries promote cleanly via `/harvest-gotchas`.
3. Add one row to the `## Catalog` table above.
4. If the gotcha is cross-project (would bite other projects on the same stack), queue it for harvest.

## Why directory-per-kind

A single `## Gotchas & Lessons Learned` table grows monotonically as every batch appends a row. The table hits 50 rows, then 200, then a size-limit platform truncates the file silently. New file per gotcha eliminates the problem — and git history per gotcha becomes atomic. See `MAINTAINING.md` — "Append-Only Knowledge Files Banned."

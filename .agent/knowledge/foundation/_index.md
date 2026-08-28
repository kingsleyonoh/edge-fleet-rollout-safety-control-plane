# Shared Foundation — Index

> **One file per foundation primitive.** This index is a human-readable catalog, rewritten by the AI whenever a sibling file is added, renamed, or removed. Never append to a single growing table — write a new sibling instead. See `.agent/rules/CODING_STANDARDS.md` — "Append-Only Knowledge Files Banned."

## Catalog

| File | Summary |
|------|---------|
| `EXAMPLE.md` | Template showing the expected shape — delete once a real foundation primitive exists. |

## What belongs here

Primitives imported by 3+ modules or that establish a project-wide contract. Examples: config loading, DB pool bootstrap, HTTP server bootstrap, auth middleware, shared error types, logging, feature flags, i18n.

## Mandatory reading rule

`CODING_STANDARDS.md` requires these files to be read **in full** before writing any new code that touches the surface they establish. The individual files in this directory replace the old flat `## Shared Foundation` table in `CODEBASE_CONTEXT.md`.

## How to add a new foundation primitive

1. Filename pattern: `category-slug.md` (e.g. `core-config-loading.md`, `db-pool-singleton.md`, `plugin-auth.md`).
2. Use the What it establishes / Files / When to read shape from `EXAMPLE.md`.
3. Add one row to the `## Catalog` table above.

## Why directory-per-kind

Shared Foundation grows every time a new cross-cutting primitive lands. One row per primitive in a flat table becomes impossible to maintain once the project has 10+ primitives. Directory-per-kind scales — and each file is the right size to read "in full" without triggering context pressure.

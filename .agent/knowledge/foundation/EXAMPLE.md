# Example Foundation Primitive — Delete Me

> Template shape for a foundation file. Delete this file once a real foundation primitive is documented.

## What it establishes

One sentence. The contract / invariant / convention this primitive enforces across the codebase.

## Files

- `src/<path>/<file>` — the primitive's source
- Related test files

## When to read this

Before writing any code that:
- Imports from this primitive
- Creates a new instance of what this primitive bootstraps (DB client, server, auth middleware, etc.)
- Extends the contract this primitive owns

## Contract

The specific rules a consumer must follow. E.g.:
- "Always use the exported `pool` singleton — never `new Pool()` directly."
- "All handlers must be wrapped in `withTenantScope()`."
- "Config is frozen at startup — never mutate at runtime."

## Cross-references

- Related modules: `.agent/knowledge/modules/*.md`
- Related patterns: `.agent/knowledge/patterns/*.md`

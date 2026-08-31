# Edge Fleet Rollout Safety Control Plane — Coding Standards

> Core rules. Route testing, production, API, database, auth, job, and frontend work through the matching file in `.agent/rules/_index.md`.

## Workflow discipline

- Read `.agent/workflows/PIPELINE.md` after a workflow and suggest the appropriate next workflow.
- Handle every approval gate in conversation. Ask `Approve? [yes / no / edit]` and wait.
- Do not use plan-mode tools or `.claude/plans/` as a substitute for approval.
- Keep at most 30 workflow files. Update the workflow index and pipeline when adding or removing one.

## Project architecture

Dependency direction is:

```text
shared -> domain -> application -> infrastructure/web/cli -> bootstrap
```

- `domain` contains no Drogon, SQL-dialect, filesystem, HTTP, or template headers.
- Application services own use cases and depend on domain ports.
- Infrastructure implements storage, crypto, artifacts, configuration, and external adapters.
- Web handlers stay thin: validate, authorize, call an application service, map the result.
- Bootstrap composes long-lived pools and workers and owns graceful shutdown.
- Business modules use `Storage`, not embedded SQL strings. SQLite and PostgreSQL must pass one logical contract suite.
- Lookups use immutable UUIDs inside `tenant_id`. Names and versions are display fields, never fallback identifiers.
- New shared primitives require a record under `.agent/knowledge/foundation/` once source exists.

## C++ conventions

- C++23, RAII, value types, explicit ownership, `std::unique_ptr` by default, and `std::shared_ptr` only for proven shared lifetime.
- Headers expose the smallest interface. Implementations live in `.cpp` files.
- Namespaces follow module paths. Types use `PascalCase`; functions and variables use `camelCase`; constants use `kPascalCase`.
- Prefer `std::expected` or the project `Result` type for recoverable errors. Exceptions do not cross HTTP, job, or storage boundaries.
- Keep canonical JSON, digest, clock, and state-transition logic independent of Drogon and databases.
- Never compare report sequence, desired generation, and stage ordinal as if they were one counter.

## Git convention

Use `type(scope): descriptive message`, imperative mood, 72 characters or fewer in the subject.

Allowed types are `feat`, `fix`, `refactor`, `test`, `docs`, `chore`, `style`, and `hotfix`. Useful scopes include `auth`, `fleets`, `devices`, `artifacts`, `cohorts`, `releases`, `health`, `simulation`, `replay`, `evidence`, `storage`, `integrations`, `web`, `cli`, and `build`.

All implementation work happens on `dev`. `main` is production-only. One commit covers one completed item or cohesive approved batch.

## AI discipline

### No scope creep

Implement only the selected `docs/progress.md` item or explicit user request. Ask before adding untracked features, helpers, or abstractions.

### Search before creating

Search names, paths, module exports, foundation records, and related code before adding a file, type, function, route, middleware, job, or utility. Extend an existing implementation instead of creating a parallel one.

### Skills before memory

When an applicable skill exists, read its `SKILL.md` and follow it. Project rules and verified package/library documentation override remembered patterns.

### No phantom dependencies or APIs

Add dependencies to `vcpkg.json`, CMake, or the dev-only `package.json` before use. Verify APIs against the pinned version.

### No placeholders

Final code cannot contain `TODO`, `FIXME`, empty bodies, `NotImplementedError`, or fake return values. Setup documents may name future paths only when they clearly say planned.

### No silent failures

Every error path returns, logs, or propagates a typed failure. Empty catches and broad error swallowing are banned.

### No silent workarounds

Do not hardcode a value that belongs in schema, config, an API response, or a frozen snapshot. Missing tenant identity, token, manifest, policy, or evidence data is a schema/spec decision. Stop and surface it.

Never shrink, truncate, or silently drop catalog rows to satisfy a size limit. Split by responsibility.

### Determinism and evidence

- Frozen release membership, policy, manifests, rollback manifest, cohort salt, and evidence decide safety transitions.
- Wall clock, unordered query results, thread scheduling, and adapters cannot affect simulator or replay output.
- Every accepted mutation and its tenant hash-chained evidence event commit together.
- Delivery and acknowledgement never imply convergence.
- Unknown enums, strategies, gates, adapters, event versions, and replay versions fail closed.

### No over-engineering

Version 1 has fixed deterministic waves, one pull protocol, local artifact storage, two storage dialects, three optional adapters, one simulator schema, and one replay family. New protocols, providers, strategies, or remediation actions require a PRD decision.

### Verify before claiming

Run the exact checks and preserve output before claiming completion. Cite the PRD section and path-backed evidence.

## Append-only knowledge files are banned

Use one file per item:

- `.agent/knowledge/patterns/NNN-slug.md`
- `.agent/knowledge/gotchas/YYYY-MM-DD-slug.md`
- `.agent/knowledge/modules/<source-path>.md`
- `.agent/knowledge/foundation/category-slug.md`
- `.agent/knowledge/checks/failure-type-slug.md`
- `docs/build-journal/NNN-batch.md`

Update the matching `_index.md` only for membership or summary changes. Never create a flat `docs/build-journal.md`.

## Secrets and ignore safety

- Never write real keys, tokens, passwords, cookies, authorization headers, signed URLs, or device secrets to tracked files or mission evidence.
- Runtime secrets come from environment or host secret storage. `.env.example` contains safe blanks.
- Never run `git add -f`. Stage with `git add .` only.
- Keep `tools/klevar-pi-package/`, `.pi/browser/`, and `.pi/agents/` ignored and untracked.
- Keep proprietary workflow, guide, progress, journal, and PRD ignore lines commented during private development. `/prepare-public` handles an explicitly authorized public-history rewrite later.

## Lean reading and Mesh ownership

Main agents read the smallest governance context needed to orchestrate. Focused Mesh workers own source/test/config discovery, edits, tests, and evidence. Full-file reads belong in a worker when a file will be substantially changed or uncertainty remains. Direct main-agent source access is limited to a named firewall exception.

Before touching a shared primitive, read its foundation record. Before implementation, read the selected progress item, invariants, exclusions, mapped PRD section, and relevant rule files.

## Wiring requirement

A new route, middleware, job, adapter, storage method, service, or utility must be imported, registered, and reachable from the real entry point in the same batch. If it has no caller, it is dead code.

## Size limits

- Source file: 800 lines maximum; reassess at 700.
- Function or method: 50 lines maximum.
- Class: 200 lines maximum.
- Rule file: 10,000 characters maximum.
- HTMX fragment: 100 KiB maximum.

Split by responsibility. Do not compress meaning to meet the limit.

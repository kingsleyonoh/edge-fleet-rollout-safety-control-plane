# Edge Fleet Rollout Safety Control Plane — Codebase Context

> **This is the TEMPLATE version of `CODEBASE_CONTEXT.md`** — a blueprint with empty tables and `{{PLACEHOLDER}}` tokens.
>
> When a new project is created via `/bootstrap` or `/retrofit`, this file is copied and populated with real project data (tech stack, modules, schema, etc.). Once populated, it becomes the AI's primary source of truth for understanding that project. Updated by `/sync-context`.
>
> **Do NOT fill in the tables here.** They are intentionally empty — workflows fill them per-project.
>
> Last updated: {{DATE}}
> Template synced: {{DATE}}

<template_manager_warning>
⚠️ **TEMPLATE MANAGER — MANDATORY PROCESS FOR EVERY CHANGE:**
1. **BEFORE modifying any file** in this template, open `MAINTAINING.md` and find the matching checklist.
2. **AFTER modifying the file**, walk through every item in that checklist and apply each one.
3. **AFTER all checklist items are done**, check the "After ANY Template Change" section at the bottom.
4. Do NOT commit until all propagation steps are complete.

This is not optional. Skipping this causes sync failures across all downstream projects.
(Note: bootstrap/retrofit workflows will delete this block when creating a new project.)
</template_manager_warning>

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Language | |
| Framework | |
| Database | |
| Hosting | |
| Package Manager | |
| Test Runner | |
| Build Tool | |

## Project Structure

```
project-name/
├── [populated by bootstrap/retrofit from PRD Project Structure section (default §9) or codebase scan]
```

## Key Modules

> **Modules live in `.agent/knowledge/modules/` — one file per module.** See `modules/_index.md` for the catalog. Do NOT add a flat table here — it's a banned append-only pattern. See `CODING_STANDARDS.md` — "Append-Only Knowledge Files Banned."

## Database Schema

| Table | Purpose | Key Fields |
|-------|---------|-----------|
| | | |

## External Integrations

| Service | Purpose | Auth Method |
|---------|---------|------------|
| | | |

## Environment Variables

| Variable | Purpose | Source |
|----------|---------|--------|
| | | |

## Commands

| Action | Command |
|--------|---------|
| Dev server | |
| Run tests | |
| Run tests (unit only) | |
| Run tests (integration only) | |
| Lint/check | |
| Build | |
| Migrate DB | |
| E2E tests | |
| Start infra | |
| Stop infra | |
| Check infra | |

> **Run tests / Run tests (unit only) / Run tests (integration only):** focused Pi/Mesh workers use all three for efficient RED/GREEN/REGRESSION evidence:
> - **Run tests** — the FULL suite (unit + integration). Always runs for final regression evidence. Populate with the project's canonical full-test command (e.g. `npm test`, `pytest`, `go test ./...`).
> - **Run tests (unit only)** — a fast-feedback tier that skips DB / cache / queue / browser setup. Populate if the project splits tests by directory or marker (e.g. `npm run test:unit`, `pytest tests/unit`, `go test ./pkg/...`). If no such split exists, set to `N/A`; workers use the full command and report that no focused tier exists.
> - **Run tests (integration only)** — unit + in-process integration but NOT E2E over HTTP. Populate if the project splits (e.g. `npm run test:integration`, `pytest tests/integration`). If there is no separate tier, duplicate the "Run tests" value. If there is no test suite at all, set to `N/A`.
>
> **Start infra / Stop infra / Check infra:** Whatever command this project uses to start/stop/check its local services (Postgres, Redis, NATS, etc.). May be `docker compose up -d`, `brew services start postgresql@16 redis`, `foreman start -f Procfile.dev`, `make dev-up`, `npm run services:up`, or `N/A` if the project has no external services. Focused integration/E2E lanes use these commands only when the requested evidence requires local infrastructure.

## Key Patterns & Conventions

> **Patterns live in `.agent/knowledge/patterns/` — one file per pattern.** See `patterns/_index.md` for the catalog. Do NOT add a flat bullet list here — it's a banned append-only pattern. See `CODING_STANDARDS.md` — "Append-Only Knowledge Files Banned."

## Gotchas & Lessons Learned

> **Gotchas live in `.agent/knowledge/gotchas/` — one file per gotcha.** See `gotchas/_index.md` for the catalog. Focused implementation/review workers write evidence-backed gotcha files; never append to a flat table here. See `CODING_STANDARDS.md` — "Append-Only Knowledge Files Banned."

## Shared Foundation (MUST READ before any implementation)

> **Foundation primitives live in `.agent/knowledge/foundation/` — one file per primitive.** See `foundation/_index.md` for the catalog. The AI MUST read the relevant files **in full** before writing any new code that touches the surface they establish. Do NOT add a flat table here — it's a banned append-only pattern. See `CODING_STANDARDS.md` — "Append-Only Knowledge Files Banned."

## Deep References

> For detailed implementation patterns, have a focused Mesh discovery/implementation lane read the relevant source — don't embed full source detail here. Keeps this file lean. When `/deep-study` or `/sync-context` runs, it populates this table and **trims** the corresponding embedded sections above to one-line summaries.

| Topic | Where to look |
|-------|--------------|
| [module name] | `src/[module]/` |
| Test patterns | `tests/` |

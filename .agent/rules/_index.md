# Rules — Index

> Routing map for this project. Read only the rules that govern the touched area, unless the work is broad, cross-cutting, security-sensitive, or changes coordination files.

## Core rules

| File | Purpose |
|------|---------|
| `CODEBASE_CONTEXT.md` | Stack, planned structure, schema, env vars, commands, tenant model, references |
| `CODING_STANDARDS.md` | Core architecture, AI discipline, Git, evidence, file limits |
| `CODING_STANDARDS_META.md` | Skills, Mesh ownership, shell, branches, approval |
| `CODING_STANDARDS_TESTING.md` | TDD, anti-cheat, unhappy paths, test quality |
| `CODING_STANDARDS_TESTING_LOGIC.md` | Business correctness, two-tenant fixtures, edge cases |
| `CODING_STANDARDS_TESTING_LIVE.md` | Real SQLite/artifact integration, Drogon, CSP/HTMX, adapter fixtures |
| `CODING_STANDARDS_TESTING_E2E.md` | Real-server HTTP and Playwright journeys |
| `CODING_STANDARDS_DOMAIN.md` | Security, release/device/artifact safety, production, observability |
| `COLLABORATION_RULES.md` | Branch, claim, and contributor coordination |

## Domain-specific rules

| File | Read when working on |
|------|----------------------|
| `auth_rules.md` | Operator/device authentication, sessions, roles, tenant resolution |
| `db_rules.md` | Storage contracts, migrations, tenant keys, transactions, evidence append |
| `api_rules.md` | REST, device protocol, HTMX handlers, pagination, errors, OpenAPI |
| `jobs_rules.md` | Schedulers, leases, simulation/replay/export/outbox workers |
| `FRONTEND_IMPECCABLE_RULES.md` | CSP templates, HTMX, CSS, accessibility, responsive UI |

## Knowledge routing

Use each directory's `_index.md`, then read only matching sibling files:

- `.agent/knowledge/patterns/`
- `.agent/knowledge/gotchas/`
- `.agent/knowledge/modules/`
- `.agent/knowledge/foundation/`
- `.agent/knowledge/checks/`

Update this index when a rule file is added, removed, or split. Root pointer files reference this index and never duplicate its tables.

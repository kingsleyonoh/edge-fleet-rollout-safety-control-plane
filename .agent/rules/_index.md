# Rules — Index

> **Single source of truth for which rules apply to this project.** Pointer files at the project root (`CLAUDE.md`, `AGENTS.md`, `.cursorrules`) reference this file. When a rule is added, removed, or split, this index is the only file that needs editing — root pointers stay stable.
>
> **LEAN LOADING:** This index is a routing map, not a preload list. Agents should triage the task first, then read only the listed rule files that govern the touched surface before editing. Read the full core set only for broad/cross-cutting/safety-sensitive work, coordination-file changes, or when triage leaves uncertainty.

## Core rules (load selectively after triage)

| File | Purpose |
|------|---------|
| `CODEBASE_CONTEXT.md` | Tech stack, schema, env vars, commands, project structure |
| `CODING_STANDARDS.md` | Core AI discipline, git, file size limits, append-only files banned |
| `CODING_STANDARDS_META.md` | Skill orchestration, PowerShell environment, git branching |
| `CODING_STANDARDS_TESTING.md` | Core TDD workflow (RED/GREEN/REGRESSION), anti-cheat |
| `CODING_STANDARDS_TESTING_LOGIC.md` | Business logic correctness, multi-tenant fixtures, edge cases, test modularity |
| `CODING_STANDARDS_TESTING_LIVE.md` | Mock policy, component testing, in-process backend integration |
| `CODING_STANDARDS_TESTING_E2E.md` | E2E testing over real HTTP |
| `CODING_STANDARDS_DOMAIN.md` | Deployment, security, naming conventions |
| `COLLABORATION_RULES.md` | Branch, claim, and AI-assisted contributor coordination rules |

> **Split-file note:** If `CODEBASE_CONTEXT.md` exceeds 10K characters, it auto-splits into `CODEBASE_CONTEXT_SCHEMA.md` and/or `CODEBASE_CONTEXT_MODULES.md`. Read every `CODEBASE_CONTEXT*.md` file present in this directory.

## Domain-specific (read only if your task touches the area)

| File | Read when working on… |
|------|----------------------|
| `auth_rules.md` | Authentication, sessions, permissions |
| `db_rules.md` | Database, migrations, queries |
| `api_rules.md` | API endpoints, serializers, validation |
| `jobs_rules.md` | Background jobs, queues, scheduling |
| `FRONTEND_IMPECCABLE_RULES.md` | UI, UX, frontend routes, templates, CSS, page copy, design tokens |

> Domain rules are created by `/bootstrap` only when a domain has 5+ concentrated conventions in this project. If a file isn't listed in the directory listing, the corresponding rules live in `CODING_STANDARDS.md`.

## Template-management rules (template workspace only)

| File | Purpose |
|------|---------|
| `TEMPLATE_RULES.md` | Rules for editing the template itself — not copied into bootstrapped projects |

## How to update this index

- **New rules file added:** append a row to the relevant table above. Update by reading the file's purpose, not by guessing.
- **Rules file split** (e.g., `CODING_STANDARDS.md` exceeded 10K chars and split into `CODING_STANDARDS_FOO.md`): append the new file to "Core rules" so agents can route to it selectively.
- **Domain rule added** (e.g., `bootstrap-guide.md` Step 5b detected 5+ auth conventions and emitted `auth_rules.md`): append to "Domain-specific" — never to core.
- **Rules file removed:** delete its row.

The pointer files (`CLAUDE.md`, `AGENTS.md`) reference this file by path. They never list individual rules. If you find yourself editing a rules table in `CLAUDE.md` or `AGENTS.md`, stop — that change belongs here instead.

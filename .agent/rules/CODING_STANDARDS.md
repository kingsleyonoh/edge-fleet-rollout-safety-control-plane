# Edge Fleet Rollout Safety Control Plane — Coding Standards

> Part 1 of 5. Related rule files exist for meta, testing, live integration/E2E, and domain/security concerns. Load those files only when the task touches that surface.

These rules are ALWAYS ACTIVE. Follow them on every response without being asked.

## Workflow Pipeline Awareness
- After completing ANY workflow, **read `.agent/workflows/PIPELINE.md`** and suggest the NEXT logical workflow based on the current context.
- **PIPELINE.md is the single source of truth** for "what comes next." Individual workflows do NOT hardcode their next step — they defer to PIPELINE.md.
- Never leave the user guessing what to do next. Always end with a clear next step.
- **When creating a NEW workflow file**, ALWAYS add it to `PIPELINE.md` with its "When Done, Suggest" message.
- **When deleting a workflow file**, ALWAYS remove it from `PIPELINE.md`.
- `PIPELINE.md` must ALWAYS match the actual files in `.agent/workflows/`. If they're out of sync, fix `PIPELINE.md` immediately.

## Workflow Approval Gates (CRITICAL — Prevents Plan Mode Errors)
When a workflow step says "present to user", "wait for approval", or "approve before proceeding":
1. Present the content directly as **formatted text in the conversation**.
2. End with a clear question: `Approve? [yes / no / edit]`
3. Wait for the user's response before proceeding to the next step.
4. **NEVER call `ExitPlanMode` or `EnterPlanMode`** during workflow execution. These are Claude Code built-in tools for a separate system (toggled via `Shift+Tab`). Workflow approval gates are handled through direct conversation.
5. **NEVER write to `.claude/plans/`** during workflow execution — that directory is reserved for Claude Code's built-in plan mode.

This applies to ALL approval gates: batch selection, implementation plans, RED/GREEN/REGRESSION evidence, commit approval, refactor plans, and any other "present and wait" step in any workflow.

## Domain-Specific Rules

If your task touches any of the domains below, **also read the corresponding rules file before starting**. These files contain deeper conventions than fit here.

| When working on... | Also read |
|--------------------|-----------|
| Authentication / sessions / permissions | `.agent/rules/auth_rules.md` (if exists) |
| Database / migrations / queries | `.agent/rules/db_rules.md` (if exists) |
| Background jobs / queues / scheduling | `.agent/rules/jobs_rules.md` (if exists) |
| API endpoints / serializers / validation | `.agent/rules/api_rules.md` (if exists) |
| UI / UX / frontend routes / templates / CSS / page copy / design tokens | `.agent/rules/FRONTEND_IMPECCABLE_RULES.md` |

> These files are created by `/bootstrap` when a domain has 5+ concentrated conventions. If a file doesn't exist for a domain, the relevant rules are here in CODING_STANDARDS.md.
>
> **When you create a new domain rules file or split an existing rules file:** update `.agent/rules/_index.md` with a new row. Pointer files at the project root (`CLAUDE.md`, `AGENTS.md`) reference the index — they do NOT list individual rules files. Adding a row to the index makes the new rule discoverable by every CLI without editing the pointer files.

## Git Commit Convention

**Format:** `type(scope): descriptive message`

| Type | When to use |
|------|------------|
| `feat` | New feature or functionality |
| `fix` | Bug fix |
| `refactor` | Code restructuring without behavior change |
| `test` | Adding or updating tests |
| `docs` | Documentation changes |
| `chore` | Tooling, workflows, config, dependencies |
| `style` | Formatting, whitespace, no logic change |

**Scope** = the module, app, or area affected (e.g., `pricing`, `auth`, `db`, `workflows`).

**Rules:**
- Subject line max 72 characters.
- Use imperative mood: "add filter" not "added filter".
- Reference the `[BUG]`/`[FIX]`/`[FEATURE]` from `progress.md` when applicable.
- One commit per completed item. Don't bundle unrelated changes.

**Examples:**
```
feat(pricing): implement UndercutBracket model with tenant FK
fix(sending): guard against None accounts on sending page
refactor(db): extract monitoring queries into dedicated mixin
test(replies): add 11 tests for intent classification edge cases
docs(context): update CODEBASE_CONTEXT.md with new schema tables
chore(workflows): add sprint velocity to resume workflow
```

## AI Discipline Rules (Prevent Common AI Failures)

### No Scope Creep
- **ONLY implement what's asked or what's next in `docs/progress.md`.** Do not add features, helpers, utilities, or "nice-to-haves" that aren't in the spec.
- If you think something SHOULD be added, ASK the user first. Never add it silently.

### No Phantom Dependencies
- **NEVER import a package that isn't in the dependency file** (requirements.txt / package.json / etc). Add it FIRST, then use it.
- Before using any library method, **verify it exists** in that version. Don't hallucinate API methods.

### No Placeholder Code
- **NEVER write `# TODO`, `pass`, `...`, or `NotImplementedError`** as final code. Every function must be fully implemented before marking the task done.

### No Hallucinated APIs
- Before calling any external library method, **verify the method exists** by checking docs or the installed package.
- If unsure, say so and check rather than assuming.

### No Silent Failures
- **NEVER write code that swallows errors silently.** Every `except`/`catch` block must either re-raise, log, or return a meaningful error.
- `except: pass` / `catch {}` is permanently BANNED.

### No Silent Workarounds (CRITICAL — Prevents Hidden Schema/Spec Gaps)
- **NEVER hardcode a value that should come from config, schema, or API response** to "make the test pass."
- Template token fails to resolve? Schema field missing? Context API missing data? **STOP.** Either (1) extend the schema in this batch if PRD already specifies the field, or (2) escalate as `SILENT_WORKAROUND` and wait for user decision.
- Banned pattern: literal tenant/customer identity strings (names, addresses, emails, registrations, legal boilerplate, wordmarks) written into templates, emails, invoices, legal docs, or any config-driven surface. Single-tenant fixtures mask this — it leaks in production.
- **Also banned:** silently compacting/shrinking/truncating a file to hit a size limit (dropping bullets from a rules file, collapsing entries in a catalog, removing rows from a table to fit under 10K). The fix for an oversized knowledge file is the directory-per-kind pattern below, not truncation.
- Applies to manual work, `/implement-next`, focused Mesh workers, and review lanes. Stop and surface the missing schema/spec decision instead of routing around it.

### Append-Only Knowledge Files Banned (CRITICAL — Prevents Recurring Splits)
- **NEVER create or grow a single file that accumulates entries of unbounded cardinality.** New gotcha → new file. New pattern → new file. New module → new file. New foundation primitive → new file. New build-journal batch → new file.
- Canonical directory-per-kind locations:
  - `.agent/knowledge/patterns/` — one file per pattern (filename `NNN-slug.md`)
  - `.agent/knowledge/gotchas/` — one file per gotcha (filename `YYYY-MM-DD-slug.md`)
  - `.agent/knowledge/modules/` — one file per module (filename mirrors source path)
  - `.agent/knowledge/foundation/` — one file per foundation primitive (filename `category-slug.md`)
  - `.agent/knowledge/checks/` — one file per project-local enforcement check (filename `{failure_type}-{slug}.md`, lowercase hyphenated). Written by focused evidence-backed workers after recurring failures; read during implementation/review planning; retire only after evidence proves the pattern is gone.
  - `docs/build-journal/` — one file per batch (filename `NNN-batch.md`)
- Each directory has an `_index.md` catalog that the AI rewrites when siblings are added, renamed, or removed. The index is a catalog, not a growing file — it tracks directory membership, nothing more.
- **Exempt from this rule** (bounded cardinality, safe as single files): `CODING_STANDARDS*.md` (fixed taxonomy of rule categories), workflow stubs, `CODEBASE_CONTEXT.md` tech-stack / commands / env-vars tables, PRD sections.
- **When unsure whether cardinality is bounded:** assume unbounded and use a directory. A project with 3 patterns today has 30 in a year; one file per pattern scales, one row per pattern in a table does not.
- Violations show up as recurring "split this file" work every few batches. The split is firefighting — the fire is the append-only architecture. See `MAINTAINING.md` — "Append-Only Knowledge Files Banned" for migration guidance.

### No Over-Engineering
- Match the spec's complexity level. No abstractions without 2+ concrete implementations.
- Build for the scale defined in the spec, not 100x that.

### Verify Before Claiming
- **NEVER say "done" or "all tests pass" without actually running the tests** and showing the output.
- **NEVER say "this follows the spec" without having read the relevant section** in this session.
- If you haven't read a file in this conversation, you don't know what's in it. Read it first.

### Hardcoded Secrets Banned (CRITICAL)
NEVER write real API keys, tokens, passwords, JWTs, or signing secrets as string literals in any tracked file or mission evidence. Read from env or vault. Pre-commit scanner: `scripts/scan-secrets.ps1`. Full rule + pattern catalogue + recovery flow → `CODING_STANDARDS_DOMAIN.md` § Secrets Management.

### Respect .gitignore (CRITICAL — Prevents Accidental Exposure)
- **NEVER run `git add -f` on ANY file.** If a file is gitignored, it is gitignored ON PURPOSE.
- `docs/progress.md`, `docs/build-journal/`, `docs/architect_journal.md`, `.agent/workflows/`, `.agent/guides/`, `.agent/.last-sync`, `.claude/`, and PRD files are LOCAL working files (tracked during dev via the `⚠️ TRACKED DURING DEV` pattern, stripped by `/prepare-public`). They must NEVER end up in a public release.
- **Proprietary files are tracked during development** so all platforms can reference them. `.gitignore` has commented-out entries marked `⚠️ TRACKED DURING DEV` — this is the default. Run `/prepare-public` before making the repo public.
- If `git status` doesn't show a file as staged after `git add .`, that means `.gitignore` is working correctly. **Do not "fix" it.**
- The ONLY acceptable staging command is `git add .` (which respects `.gitignore`).

### Lean Read Rule (CRITICAL — Prevents Context Bloat)
- **Do not treat workflow read instructions as automatic full-file reads.** Main agents read the smallest sufficient governance/orchestration context first: headings, matching task entry, referenced PRD section, Mesh artifacts, or the specific rule section that applies.
- Source/test/config discovery, source-changing edits, tests, and evidence-sensitive validation default to delegated Mesh workers, even for small or single-file fixes. Direct main-agent source/test/config reads or edits are limited to explicit firewall exceptions: Mesh unavailable/broken, user refuses Mesh and confirms context-risk, emergency safety intervention with minimum direct inspection, or governance/Mesh artifact reads needed to orchestrate.
- Read an entire file only when it is small, you will substantially edit/rewrite it, the task is broad/cross-cutting/safety-sensitive, or targeted reading leaves uncertainty; for source/test/config files, this instruction applies inside the delegated worker lane by default.
- For PRDs and `progress.md`, prefer heading/section search plus the selected item context over full-document reads during ordinary feature implementation.
- For source files, workers read the whole file when modifying it heavily; otherwise they read the relevant function/module and surrounding context.

### Read Shared Foundation When Touching Shared Primitives (CRITICAL — Prevents Duplication)
- Before writing a new utility, helper, middleware, handler, component, or shared pattern, search `CODEBASE_CONTEXT.md` and `.agent/knowledge/foundation/_index.md` for the relevant primitive.
- Read only the matching foundation file(s) unless the change spans the shared architecture broadly.
- If a pattern, function, or module already exists there — **USE IT.** Do not recreate it.

### Workflow Discipline
- **Max 30 workflow files** in `.agent/workflows/`. If approaching 30, retire rarely-used workflows or convert procedural knowledge to reusable global skills.
- Before creating a new workflow, check if an existing one can be extended.

### Search Before Creating (CRITICAL — Prevents Duplicate Code)
- **Before creating ANY new file, function, class, or utility**, search the codebase first:
  1. Search file contents for the function/class name
  2. Search for the file by name
  3. Check relevant module exports / `__init__` files
- If it already exists, **USE IT**. Do not recreate it.
- If a similar function exists, **extend it** — don't create a parallel version.
- When in doubt, **ASK the user**: "I can't find X — does it exist, or should I create it?"

## File Size Limits
- **Max 800 lines** per source file. If approaching 700, plan to split.
- **Max 50 lines** per function/method.
- **Max 200 lines** per class.

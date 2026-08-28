# Collaboration Rules

These rules apply when a Klevar project has external contributors, feature branches, or `docs/claims/*.json`. Current orchestration uses Pi parent/worker roles, Delegation Mesh missions, and optional Mesh Loops.

## Operator Model

A contributor may be a human using Claude Code, Codex, Cursor, Pi, Mesh, or manual tools. Use ordinary contributor identities such as:

- `operator:pi-mesh:*`
- `contributor:<name>` using Claude Code, Codex, Cursor, Pi, or manual edits
- `human-manual:<name>`

## Branch Protocol

- `main` is production.
- `dev` is the integration branch.
- `feature/<slug>` is contributor work.
- `mesh/<slug>` is an optional operator-owned branch for explicitly approved isolated work.
- `hotfix/<slug>` is emergency production repair.

Do not work directly on another operator's branch without explicit approval. An authorized AI or user chooses ordinary local Git mechanics under current project policy.

## Claims Protocol

When multiple operators may work at once, claim work before editing. Claims live in `docs/claims/*.json`:

```json
{
  "schemaVersion": 1,
  "task": "[API] Add survey export — PRD §8b",
  "operator": "contributor:alice",
  "tool": "claude-code",
  "branch": "feature/survey-export",
  "status": "active",
  "startedAt": "2026-05-18T15:30:00Z",
  "expectedFiles": ["src/api/survey-export.ts"]
}
```

Rules:

- Do not claim a task already actively claimed by another operator.
- Do not edit files listed in another active claim's `expectedFiles`.
- Keep `expectedFiles` honest and update the claim if scope changes.
- Mark claims `done` or `released` when finished or abandoned.
- If no claims exist, solo Klevar flow remains unchanged.

These collaboration claims coordinate humans and agents; they are not acceptance packets, per-file read permits, or restrictions on unclaimed normal project access.

## AI Contributor Workflow

1. Read `docs/progress.md`, this file, and relevant project rules.
2. Create or use the project-approved branch.
3. Add a collaboration claim when parallel contributors require one.
4. Use TDD and run the project regression command.
5. Run the secret scan before PR/commit when project policy requires it.
6. Open a PR into `dev` with evidence when that is the selected collaboration flow.

Use current Git status, collaboration claims, Mesh/Agency evidence, and explicit user instructions for coordination decisions.
## Pi / Mesh Behavior

Pi and Mesh workers must skip externally claimed tasks and avoid expected-file conflicts. Parallel lanes are allowed only when independence can be proven. If there is doubt, fall back to serial work or stop for operator decision.

## Main-Agent Safety While Mesh or Agency Work Is Active

When mission, loop, or Agency status reports active isolated work, the chat/main agent remains the conductor and avoids overlapping edits by default.

Allowed without takeover:

- Explain status, evidence, logs, claims, changed files, and likely next actions.
- Use Mesh/Loop/Agency status and report artifacts before raw source or logs.
- Inspect current claims and worktree identity without mutating another operator's work.

Blocked unless the user explicitly asks to take over/intervene:

- Editing files inside another active operator worktree.
- Editing files listed in active `docs/claims/*.json` claims.
- Editing root-project files that overlap the active worker's changed/claimed files.
- Cleaning/removing worktrees by hand instead of using the owning orchestration surface.

If the user wants manual intervention, preserve evidence first and confirm whether edits should target the isolated worktree or the root project.

# Edge Fleet Rollout Safety Control Plane — Meta Rules

> Skill selection, shell usage, Mesh evidence, and Git branching.

## Skill selection

Before implementation, inspect available skills. Read the most specific matching `SKILL.md` and follow it. Project rules and pinned-version documentation override remembered conventions. Use security, C++, CMake, database, API, frontend, testing, and deployment guidance when those areas change.

## Mesh ownership

The parent agent orchestrates. Focused workers own source/test/config discovery, edits, command execution, and path-backed reports. Start with mission reports and indexes. Read raw source or logs in the parent only under a named exception.

Keep active handoff nuance in `.pi/agents/conductor/current.md` when the package provides that facility. Do not duplicate `docs/progress.md` or worker reports there. Mesh artifacts grant no commit, push, release, deploy, or secret authority.

## Windows and shell

The supported development hosts include Windows and Linux. Use the current shell's real syntax. Prefer CMake presets and checked-in scripts over hand-built command variants. Quote paths that contain spaces. Do not install system-wide dependencies when vcpkg or the pinned dev-only package manifest owns them.

Run the canonical commands from `CODEBASE_CONTEXT.md`:

- configure: `cmake --preset dev`
- build: `cmake --build --preset dev -j 2`
- full regression: `ctest --preset dev --output-on-failure`
- unit: `build/dev/tests/edgefleet_tests "[unit]"`
- component: `build/dev/tests/edgefleet_tests "[component]"`
- UI E2E, Phase 3 dev tooling only: `npm ci && npx playwright test`

Node exists only for Playwright development tooling. It is not part of the C++ product build or runtime.

## Git branching

- `main` is production-only.
- `dev` is active integration and implementation.
- `feature/<slug>` is optional contributor work.
- `hotfix/<slug>` starts from `main` and merges back to both `main` and `dev` only with explicit approval.

Do not work directly on another operator's branch or claimed files. Never force-push, rewrite history, change visibility, or delete remotes unless a separate explicit request authorizes that exact action.

## Evidence and approval

Show exact RED, GREEN, regression, E2E, wiring, secret, and criterion evidence required by the selected workflow. Ask in conversation at approval gates and wait. A deterministic validator checks shape only; semantic acceptance remains an AI/human judgment backed by evidence.

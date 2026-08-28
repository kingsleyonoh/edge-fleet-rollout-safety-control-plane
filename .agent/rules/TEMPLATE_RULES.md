# Template Management Rules

> ⛔ This project IS the source template. Every change here propagates to ALL future projects via `/bootstrap`, `/retrofit`, and CHANGELOG-driven sync.

## Before Making ANY Change

1. Read `MAINTAINING.md` in full (it is NOT in `.agent/rules/`, it is in the project root)
2. Identify which checklist(s) apply to your change
3. Complete ALL checklist items — do not skip propagation steps

## After Making ANY Change

1. Update `CHANGELOG.md` with a dated, file-specific entry (this is the sync manifest)
2. Re-read `MAINTAINING.md` to verify no propagation step was missed
3. If the change affects existing projects → run the setup skill to sync them

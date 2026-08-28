# Example Check — Delete Me

> Template shape for a project-local check file. Delete this file once a focused evidence-backed worker adds a real check.
>
> Filename convention: `{failure_type}-{slug}.md` (lowercase, hyphenated). The `{failure_type}` uses the project failure vocabulary (`tests-wont-green`, `silent-workaround`, `regression-failure`, etc.). The `{slug}` is a 2-4 word descriptor of the specific pattern.

**Trigger pattern:** A precise description of when this check fires. Be specific enough that a focused implementation/review worker can pattern-match against it before editing. Examples:
- "Plan touches `tests/integration/**` AND uses any of: `vi.mock('pg')`, `jest.mock('postgres')`, `MockDB`."
- "Plan creates a route handler under `src/api/payments/` AND does NOT import from `src/payments/registry.ts`."
- "Plan modifies a Drizzle migration AND adds a column to `tenants` table without a backfill."

**Verdict:** REJECT. The focused implementation worker must NOT proceed with the planned approach.

**Recovery procedure:** What the worker should do instead. Be concrete — name the file / function / pattern that should be used.
- Example: "Use the real Postgres test container per `.agent/knowledge/foundation/db-test-container.md`. Mocks in integration tests are banned by check-induced rule (see provenance below)."
- Example: "Register the new payment processor in `src/payments/registry.ts` instead of importing it directly. See `.agent/knowledge/foundation/feature-payments.md` for the registry pattern."

**Provenance:**
- **Failure type:** `{failure_type}`
- **First seen:** evidence artifact + date
- **Reinforced after:** {N} artifact-backed recurrences
- **Source result files:** cite the relevant `.pi/agents/runs/<run-id>/workers/<worker>/report.json` or project test artifacts.
- **Stack-class candidate:** [yes / no]. Stack tags detected: [list, e.g. Postgres, Drizzle].

**Retirement criteria** (verified by a focused read-only review):
- Has not fired in last 10 batches AND
- The trigger pattern's referenced files / code patterns no longer exist in the codebase (e.g., the module was refactored away, the dependency removed)

If both conditions hold, the review proposes retirement; user confirms; then the check file and `_index.md` row are removed together.

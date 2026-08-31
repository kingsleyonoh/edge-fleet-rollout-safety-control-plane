# Edge Fleet Rollout Safety Control Plane — E2E Testing

> E2E hits `build/dev/edgefleet serve` over real HTTP. In-process Drogon integration belongs in `CODING_STANDARDS_TESTING_LIVE.md`.

## Commands

1. Configure and build with the canonical CMake presets.
2. Run `build/dev/edgefleet setup` against an isolated ignored runtime root.
3. Start `build/dev/edgefleet serve` and wait for `/health/ready` on `http://localhost:8080`.
4. Run `npm ci && npx playwright test` with the pinned Phase 3 dev-only toolchain.
5. Stop the server and verify no process, database, artifact, or test credential leaked.

Node and Playwright are development tooling only. They do not enter the product executable or runtime image.

## Required coverage

Run E2E for every changed API endpoint, browser page, HTMX fragment with interaction, device protocol path, or full journey. Pure domain utilities or configuration may skip with `SKIPPED_NO_ENDPOINTS`; missing E2E setup is `E2E_NOT_CONFIGURED` and remains a warning, not fabricated coverage.

Backend E2E asserts status, body, headers, middleware order, CORS/CSP/CSRF, persistence, evidence, and restart behavior. Browser E2E asserts visible state, keyboard flow, forms, conflicts, role permissions, responsive critical controls, focus after HTMX swaps, and no console/network errors.

## Primary journeys

- First run: setup, one-time API key, login, tenant review, first fleet.
- Prepare release: devices, signed target and rollback artifacts, policy, frozen cohort, approval.
- Run release: approve, schedule/start, canary, gate evidence, staged promotion.
- Handle failure: notice, failed segment, pause/rollback approval, rollback convergence.
- Prove decision: evidence verify, replay, divergence check, immutable export.
- Test before exposure: scenario, all three strategies, benchmark metrics.

Run role-sensitive journeys for admin, release manager, approver, and viewer with two tenants. Device protocol E2E uses a fake device against the signed real routes with delayed, duplicate, reversed, and conflicting reports.

## Environment parity

E2E uses production migrations in order, the real executable, real SQLite/artifact paths for standalone checks, and the same environment loader used by production. Later PostgreSQL/container E2E uses the production image and matching PostgreSQL 16 profile. Do not substitute an older database, simplified schema, bundled fake binary, or alternate env loader.

## Cleanup

Use isolated test roots and generated non-production credentials. Delete only owned test data. Ensure the server and worker loops stop within the configured shutdown deadline. Screenshots and traces go to ignored `.pi/browser/` or mission artifacts.

## Evidence

Record server command and readiness, endpoint/page list, tenant/role matrix, Playwright summary, screenshots for failures or requested visual proof, console/network findings, cleanup result, and exact skip reason when inapplicable.

Valid endpoint skip reasons are only `SKIPPED_NO_ENDPOINTS`, `SKIPPED_NO_SERVER`, and `E2E_NOT_CONFIGURED`. Integration coverage is not a substitute for real HTTP.

# Edge Fleet Rollout Safety Control Plane — Coding Standards: E2E Testing (Real Endpoints)

> Part 5 of 5. Related core/meta/testing/live/domain rules exist; load them only when the task touches those surfaces.
> This file covers end-to-end testing that hits a running server via real HTTP. In-process testing (supertest, inject, test clients) lives in `CODING_STANDARDS_TESTING_LIVE.md`.

## E2E Testing (Real Endpoints)

> E2E tests hit a RUNNING server over HTTP — not in-process test clients like `inject()` or `supertest`.
> The point is testing the deployed stack: server startup, middleware chain, database, cache, and response serialization.
> These catch issues that unit/integration tests miss: port binding, CORS headers, middleware ordering, connection pool behavior under load.

### When E2E is Required
- **Any batch that creates or modifies an API endpoint** → E2E MUST hit the running server
- **Any batch that creates or modifies a page/component with user interaction** → E2E MUST include a browser test
- **Pure utility/library/config batches with no endpoints** → E2E not required (skip with note)
- **`[SETUP]` items** → E2E not required unless the setup itself starts a server

### E2E Test Architecture

**Backend E2E (API projects):**
1. Start the actual server: `npm run dev` or equivalent (NOT a test-mode in-process server)
2. Wait for ready signal (health check passes)
3. Hit real endpoints via HTTP (fetch, axios, or curl)
4. Assert on status codes, response bodies, headers
5. Stop the server after tests complete

**Frontend E2E (projects with UI):**
1. Start the dev server (backend + frontend)
2. Use Playwright to navigate pages and interact with the UI
3. Assert on visible elements, form submissions, navigation, error states
4. Capture screenshots on failure for debugging

**Both require local services running** (Docker PostgreSQL, Redis, etc.) — this aligns with the mock policy in `CODING_STANDARDS_TESTING_LIVE.md` ("Don't Mock What You Own").

### E2E Test File Structure
```
tests/e2e/
  api/                        ← Backend E2E tests
    auth.e2e.test.ts          ← Auth endpoint tests
    simulations.e2e.test.ts   ← Simulation endpoint tests
  ui/                         ← Frontend E2E tests (Playwright)
    login.spec.ts
    dashboard.spec.ts
  helpers/
    server.ts                 ← Start/stop server utilities
    seed.ts                   ← Test data seeding
```

### E2E vs Integration Tests
| Aspect | Integration (inject/supertest) | E2E (running server) |
|--------|-------------------------------|---------------------|
| Server | In-process, no real HTTP | Real HTTP, real port |
| Speed | Fast (~1ms per test) | Slower (~100ms+ per test) |
| What it catches | Handler logic, validation, DB | Middleware ordering, CORS, startup, ports |
| When to use | Every endpoint (RED/GREEN phase) | After REGRESSION passes (Step 7d) |
| Run command | `{{TEST_COMMAND}}` | `{{E2E_COMMAND}}` |

**Both are required.** Integration tests are your fast feedback loop (TDD). E2E tests are your deployment confidence check.

### E2E Test Cleanup
- Each E2E test must clean up its own data (delete created records, reset state)
- Use a dedicated test database or schema to avoid polluting dev data
- Kill the server process reliably in the `afterAll` hook — leaked processes block ports

### Test-Env Approximates Prod-Env (CRITICAL — Prevents "Works In Test, Fails In Prod")

E2E tests should run in an environment whose behavior-shaping characteristics match production. The exact mechanism is project-specific (Docker image reuse, Kubernetes locally, system-package install scripts) — the principle is universal: variables that change behavior must match.

**Match these between E2E and prod (the high-leverage variables):**
- **OS-level binaries** invoked by application code (browsers, headless renderers, image converters, signing tools, system utilities) — same binary version, same install path, same configuration. A test using a node-bundled Chromium and prod using a system Chromium IS a parity violation.
- **Env-var loading order** — production loads `.env` via a specific mechanism (boot loader, systemd unit, Docker runtime, hosting platform); tests should exercise the same loader, not bypass it with hardcoded test config.
- **Migration state** — E2E runs against a DB that has had ALL production migrations applied in order. Tests do not run against a "test schema" that's a simplified subset.
- **External service shapes** — when production talks to Postgres 16 + Redis 7 + Resend, E2E talks to Postgres 16 + Redis 7 + Resend (or sandbox variants thereof). Not Postgres 12 + memory cache + console-log.
- **File system / volume mounts** — if production reads templates from `/app/templates/` mounted from a volume, tests use the same path resolution.

**Rejected justifications** (why this rule is mandatory, not advisory):
- "Tests run faster on the slim image" — production deploys the prod image. Test what you ship.
- "The slim image is good enough — same Node version" — Node version is one variable; system libraries (libssl, fontconfig, glibc), available binaries (gs, wkhtmltopdf, ffmpeg), and OS package state are equally behavior-shaping.
- "Migration order doesn't matter — we use the latest schema in tests" — order matters when a later migration depends on data shape from an earlier one. Skipping the order lets data-migration bugs hide.
- "We mock the external service to avoid flakiness" — covered by the mock policy in `CODING_STANDARDS_TESTING_LIVE.md`. Mock only services you don't control; for services you do (your DB, your cache, your queue), use the real one.

**Implementation specifics are project-shaped:**
- Dockerized projects can reuse the production `Dockerfile` for the test container (`docker compose -f docker-compose.test.yml up`).
- Non-Dockerized projects can install the same system-package set via a `scripts/setup-test-env.*` script that mirrors the production deploy script.
- Cloud-only projects can run E2E against a dedicated staging tenant of the same hosting platform.
- The TEMPLATE does NOT mandate any specific implementation — it mandates the parity outcome. `.agent/knowledge/checks/` is where project-specific implementation rules live (e.g. `test-env-uses-prod-dockerfile.md` written by a focused worker after a "works locally, fails in deploy" recurrence).

### Bootstrap Setup for E2E
During `/bootstrap` Phase 0, a `[SETUP]` item should configure the E2E framework:
- Create `tests/e2e/` directory structure
- Install E2E dependencies (Playwright for frontend, or just fetch/node:test for API-only)
- Add `test:e2e` script to `package.json` (or equivalent)
- Add E2E config file if needed (`playwright.config.ts`)
- Verify the E2E command runs and exits cleanly (even with 0 tests)

### Honesty Check for E2E Skips
**E2E skip reasons are a high-fabrication surface** — workers must not claim "E2E covered by integration tests" or "E2E deferred" to shortcut the running-server requirement. When a change touches endpoints, the ONLY valid skip reasons are:
- `SKIPPED_NO_ENDPOINTS` — the batch genuinely touched no endpoints (verify against `## Items Completed`)
- `SKIPPED_NO_SERVER` — the project has no server (pure library / CLI / static site)
- `E2E_NOT_CONFIGURED` — framework not installed yet; warning logged, not blocking

Any other skip reason (including "infrastructure required", "covered by integration tests", or `DEFERRED`) is rejected by focused review as `E2E_DISHONEST_SKIP`.

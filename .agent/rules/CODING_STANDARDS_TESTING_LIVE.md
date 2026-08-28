# Edge Fleet Rollout Safety Control Plane — Coding Standards: Live & Integration Testing

> Part 4 of 5. Related core/meta/testing/E2E/domain rules exist; load them only when the task touches those surfaces.
> This file covers the mock policy, component testing, and in-process backend integration testing. E2E testing lives in `CODING_STANDARDS_TESTING_E2E.md`.

## Live Integration Testing (Mock Policy)

### The Rule: Don't Mock What You Own
If you control the service and can run it locally → test against the real thing.

### Service Fallback Hierarchy
When deciding how to test a service, follow this order:
1. **Local instance** (best) — Docker, CLI, emulator on your machine
2. **Cloud dev instance** (good) — dedicated test project / staging environment
3. **Mock** (last resort) — only when options 1 and 2 are impossible

### Test LIVE (Never Mock)
- Your database ({{LOCAL_SERVICES}}) — validates schema, column names, constraints, query behavior
- Your own API endpoints — call the actual route, not a stub
- Your own server actions / business logic — test the real function
- File storage you control (local filesystem, local object storage)

### Mock ONLY These
- Third-party payment APIs (Stripe charges money)
- Email/SMS delivery (SendGrid/Twilio sends messages)
- Rate-limited external APIs you don't control
- Services with irreversible side effects
- Cloud-only services with no local emulator AND no dev tier

### No Services? No Problem
If the project has no external services (CLI tool, library, static site), this policy doesn't apply — just write standard unit tests.

### Why This Matters
A mock that returns `{ user_id: 1 }` will pass even when the real column is `userId`. A mock that returns success will pass even when the real constraint rejects your data. Mocks test your ASSUMPTIONS about the service. Live tests test REALITY.

### Common Mock Violations (DO NOT DO THESE)
- ❌ Mocking your database client to return fake rows — hit the real database
- ❌ Mocking your own API routes with `nock`/`msw` — call the real endpoint via test client
- ❌ Using an in-memory SQLite when production uses PostgreSQL — use the real PostgreSQL
- ❌ Mocking Redis/cache when it's running in Docker — connect to the real instance
- ✅ Mocking Stripe's charge API — you don't want to charge real money in tests
- ✅ Mocking SendGrid — you don't want to send real emails in tests
- ✅ Mocking an external API with rate limits — you don't control their uptime

### Test Cleanup
- Each test MUST clean up after itself (delete rows, reset state)
- Use transactions with rollback when possible for speed

<!-- CONDITIONAL: FRONTEND_FRAMEWORK=React — Bootstrap Step 5c: KEEP this section for React frontend projects, REMOVE for backend-only -->
## Component Testing (React Testing Library)

> This section applies to projects with a React frontend. If the project has no UI, skip this section entirely.

### When to Write Component Tests
- Every **interactive component**: forms, dialogs, accordions, dropdowns, buttons with click handlers
- Every component with **conditional rendering** (show/hide logic, loading states, error states)
- Any component where a bug would **block user interaction** (can't type, can't click, can't submit)
- **Not required for**: pure display components with no interactivity (static text, icons, layout wrappers)

### What to Test
| Priority | Test This | Example |
|----------|-----------|---------|
| 1 | User interactions | Click button → dialog opens; type in input → value updates |
| 2 | Conditional rendering | Error state shows message; loading state shows spinner |
| 3 | Form validation feedback | Submit empty form → validation errors appear |
| 4 | Accessible roles & labels | Button has correct label; form inputs are labeled |
| 5 | Callback invocation | onSubmit called with correct data; onCancel fires |

### What NOT to Test
- **Styling** — don't assert on classNames, colors, or CSS
- **Internal state** — don't reach into `useState` values; test what the USER sees
- **Snapshot tests** — they create noise and break on every minor change. Test behavior instead.
- **Implementation details** — don't test that a specific hook was called; test the outcome

### RTL Query Priority (follow this order)
1. `getByRole` — accessible role (button, textbox, dialog) — **always prefer this**
2. `getByLabelText` — form inputs with labels
3. `getByText` — visible text content
4. `getByPlaceholderText` — placeholder fallback
5. `getByTestId` — **last resort only** — used when no semantic query works

### RTL Best Practices
- Use `userEvent` over `fireEvent` — it simulates real browser behavior (focus, blur, keyboard)
- Use `screen` for queries — not destructured render result
- Use `waitFor` for async operations — never `setTimeout`
- Use `within` to scope queries inside a container (e.g., within a specific dialog)
- Wrap state updates in `act()` only if React warns you — RTL handles this automatically in most cases

### File Naming & Location
- Name: `ComponentName.test.tsx` — co-located next to the component file
- Example: `src/components/ProductFormDialog.test.tsx`
- Group test utilities in `src/test/helpers.ts` if shared across component tests

### Minimum Coverage Rule
Every interactive React component MUST have at least:
- **1 happy-path interaction test** (user performs the primary action successfully)
- **1 error/edge-case test** (empty submission, missing data, disabled state)
- If a component has 0 tests and it has click/type/submit handlers → it's a bug waiting to happen

### Setup (Vitest + jsdom)
Component tests run in Node.js with a simulated DOM — no browser needed. Typical setup:
- `vitest` as test runner (or `jest` if the project already uses it)
- `@testing-library/react` for component rendering and queries
- `@testing-library/user-event` for simulating user interactions
- `jsdom` or `happy-dom` as the test environment
- Configure in `vitest.config.ts`: `environment: 'jsdom'`
<!-- END CONDITIONAL: FRONTEND_FRAMEWORK=React -->

<!-- CONDITIONAL: BACKEND_ONLY — Bootstrap Step 5c: KEEP this section for backend-only projects (API, CLI, worker), REMOVE for React frontend -->
## Backend API & Integration Testing

> This section applies to backend-only projects (APIs, workers, CLI tools). If the project has a React frontend, use the Component Testing section above instead.
> **Note:** This is in-process integration testing (test client like `inject()` or `supertest`). For real-HTTP testing over the network, see `CODING_STANDARDS_TESTING_E2E.md`.

### When to Write API Integration Tests
- Every **API endpoint**: test request → response cycle with real HTTP semantics
- Every **message consumer/handler**: test event processing with real or local message broker
- Every **background job/worker**: test job execution with actual service dependencies
- Every **middleware**: test request interception, auth guards, validation layers

### What to Test
| Priority | Test This | Example |
|----------|-----------|---------|
| 1 | Request/response cycle | POST /api/users → 201, returns created user |
| 2 | Input validation | Missing required field → 400 with specific error |
| 3 | Auth & authorization | No token → 401; wrong role → 403 |
| 4 | Error handling | Invalid ID → 404; DB constraint → 409 |
| 5 | Edge cases | Empty body, oversized payload, duplicate submission |

### API Testing Patterns
- Use the framework's built-in test client (e.g., Fastify `inject()`, Express `supertest`, FastAPI `TestClient`)
- Test full request lifecycle — serialization, middleware, handler, response
- Assert on status codes, response body structure, AND headers where relevant
- Test pagination, filtering, and sorting with real DB rows

### Message/Event Consumer Testing
- Publish test events to a local broker (Kafka/Redpanda, RabbitMQ, Redis Streams)
- Assert the consumer processes them correctly (DB writes, side effects)
- Test error handling: malformed events, duplicate events, consumer restart

### File Naming & Location
- Name: `module-name.test.ts` or `test_module_name.py` — co-located or in `tests/` mirror
- Group shared test helpers in `tests/helpers/` or `tests/factories/`
<!-- END CONDITIONAL: BACKEND_ONLY -->

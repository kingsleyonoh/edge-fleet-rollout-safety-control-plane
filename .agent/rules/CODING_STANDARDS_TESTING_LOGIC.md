# Edge Fleet Rollout Safety Control Plane — Coding Standards: Testing Logic & Correctness

> Part 4 of 7. Related core/meta/testing/live/E2E/domain rules exist; load them only when the task touches those surfaces.
> This file covers business logic correctness, edge cases, modularity, multi-tenant fixtures, and business-context testing.

## Performance Awareness
- Correctness tests alone don't catch latency regressions — a page can pass all tests while making 10x the necessary network calls.
- When a single page/endpoint triggers 3+ backend operations, consider asserting call count or response time.
- After every batch of 5+ features, do a compound load check: load real pages and verify total I/O matches expectations.

## Multi-Tenant Fixtures Mandatory (CRITICAL — Catches Cross-Tenant Leakage)

If the project is multi-tenant (PRD §2 Architecture Principles mandates `tenant_id`), every test suite that touches tenant-scoped data MUST load **at least TWO distinct tenants** with different literal values for every tenant-identity column (legal_name, full_legal_name, display_name, address, registration, contact, wordmark).

**Why:** A template that hardcodes "Acme Corp LLC" passes every test when the fixture only loads Acme. It fails the moment Globex is onboarded. Two-tenant fixtures expose this at RED phase, not in production.

**Rules:**

1. **Fixtures file (`tests/fixtures/tenants.*` or equivalent) MUST define >=2 tenants** with intentionally-different identity values. Include edge cases: non-ASCII characters, longer addresses, different jurisdictions.
2. **Template / email / invoice / PDF tests MUST parametrize over both tenants** (pytest parametrize, table-driven tests, etc.) and assert that rendering Tenant A's snapshot does NOT include any Tenant B literal value and vice versa.
3. **Cross-tenant leakage grep (runs in suite):** Add a test that reads the generated artifact and greps for EVERY literal identity value of the OTHER tenant. Any match fails the test with message `TENANT_IDENTITY_LEAK: field=X expected=A actual_included=B`.
4. **Tenant isolation test per module:** Category 6 in the Test Quality Checklist becomes MANDATORY. Every query, every API response, every job run must be asserted to respect `tenant_id` scoping.

**This rule is non-optional for config-driven surfaces.** Skipping it means the template-hardcoding bug class will re-occur project-by-project until tests catch it at RED.

## Edge Case Coverage Guide

### Models
- Every field from the spec → at least 1 test per constraint.
- Every FK → test CASCADE behavior.
- Every choice field → test all valid values + 1 invalid value.

### Services (when applicable)
- Boundary values (min, max, zero, negative).
- Invalid input types.
- Idempotency (running twice = same result).
- Mock external API failures.

### Views/Pages (when applicable)
- Authenticated vs unauthenticated access.
- Correct HTTP methods (GET/POST/PUT/DELETE).
- Response format validation.
- Tenant scoping (if multi-tenant).

## Test Modularity Rules
1. **One test class per model/service** — never mix models in one class.
2. **Max 800 lines per test file** — split if larger.
3. **`setUp` creates only what that class needs** — no global fixtures.
4. **Tests are independent** — no shared state, no ordering dependency.
5. **Any single test can run in isolation** — `python -m pytest tests/test_x.py::TestClass::test_method`.
6. **Test names describe business behavior** — not technical actions.
7. **No test helpers longer than 10 lines** — extract to a `tests/factories.py` if needed.

## Business-Context Testing
- Tests must reflect the BUSINESS PURPOSE described in the spec.
- Every test must answer: Does this protect data? Apply rules correctly? Handle failure? Match the spec?
- Test names must describe business behavior, not technical actions.

## Business Logic Correctness Testing

Unit, integration, and E2E tests are not sufficient if they only prove that code executes. Every feature must also test whether the business outcome is logically correct.

A business logic correctness test is required when a feature:
- creates or displays a client-facing identifier
- calculates totals, balances, taxes, discounts, deadlines, or limits
- changes status or lifecycle state
- authorizes or denies an action
- emits events, webhooks, notifications, exports, reports, or generated artifacts
- writes data later consumed by a job, cache, search index, report, or reconciliation flow
- renders the same business entity through multiple paths
- stores both internal values and external/public values

The test must:
1. Create or mutate real data through the production path.
2. Assert the canonical business rule, not only the implementation detail.
3. Observe every relevant path where the rule is externally visible or consumed.
4. Assert internal-only values do not leak.
5. Assert derived values use one source of truth or one shared calculation path.
6. Include at least one failure-mode test showing the rule would catch a wrong-but-running system.

Example: an invoice-like document has one public identity. Its raw numeric sequence may exist for allocation, but it is internal-only. PDF/HTML, public page, API response, email payload, webhook/event payload, XML/e-invoice payload, payment metadata, and reconciliation/export rows must all use the public identity, and the test must fail if one path emits the raw sequence instead.

# Edge Fleet Rollout Safety Control Plane — Live and Integration Testing

> Covers real local dependencies, Drogon in-process integration, server-rendered UI components, and adapter fixtures. Real-network E2E is in `CODING_STANDARDS_TESTING_E2E.md`.

## Do not mock what the project owns

Standalone tests use real SQLite in WAL mode, real filesystem artifact storage, production migrations, production parsers, and production application services. Do not replace them with fake repositories, alternate schemas, test-only seeders, or simplified binaries.

The standalone core has no external service. Optional adapters default off. Recorded HTTP fixtures are allowed for IoT, Notification Hub, and Workflow Engine because those services are outside this repository. Opt-in live contract checks require explicit test flags and non-production credentials.

## Test tiers

- Unit: `build/dev/tests/edgefleet_tests "[unit]"`
- Component: `build/dev/tests/edgefleet_tests "[component]"`
- Full unit, component, contract, and integration suite: `ctest --preset dev --output-on-failure`
- Real-network E2E: `npm ci && npx playwright test`

Tests derive fixtures from versioned migrations, schemas, scenario manifests, and production setup paths. A test must not define a parallel contract.

## Drogon API integration

Every endpoint, middleware, controller, application service, job handler, and device protocol route needs production-path integration coverage.

Assert:

1. request method, route, body, headers, and closed-schema validation;
2. authentication and exact role permission;
3. tenant filter and cross-tenant 404 behavior;
4. status, response schema, security headers, and safe error format;
5. database, artifact, command, outbox, notice, and evidence effects in one transaction;
6. idempotency, optimistic conflict, duplicate, reorder, and failure paths;
7. no secret, SQL, raw adapter body, or filesystem path leaks.

Use Drogon's test facilities against the real handler stack for fast integration. Real HTTP remains a separate E2E requirement.

## Server-rendered UI integration

Drogon CSP and HTMX tests render through production controllers and typed template contexts. Test full pages and fragments for:

- loading, empty, error, conflict, stale evidence, adapter unavailable, denied, paused, rollback, and completed states;
- role-visible controls based on the shared backend `PolicyDecision`;
- two-tenant literal exclusion;
- strict missing-token failure;
- table headers, labels, status text, focus targets, CSRF, CSP, and no inline script;
- fragment size under 100 KiB and safe polling behavior.

Do not snapshot large HTML trees. Assert semantic roles, labels, text, links, forms, hidden sensitive values, and canonical state.

## Adapter contract fixtures

Each supported adapter has success, timeout, 401, 403, 429 where applicable, malformed JSON, stale data, redirect, TLS, disabled, and exhaustion fixtures under `tests/fixtures/`.

Contract tests prove:

- `iot_rest_v1` reads only and cannot mutate release state;
- `notification_hub_v1` emits frozen idempotent events and retains local notices;
- `workflow_manual_v1` starts and observes playbooks but has no release authority;
- unknown adapter values fail before an adapter exists;
- disabled adapters leave the standalone acceptance suite green.

## Cleanup and isolation

Each test owns a temporary database and artifact root, or uses a transaction/fixture that leaves no shared state. Tests can run alone and in shuffled order. Two-tenant fixtures are mandatory whenever tenant-owned data appears. Do not use production data or credentials.

## Mock policy

Allowed mocks and fixtures cover only external systems not controlled by this repository, wall-clock injection through `TenantClock`, and deterministic failure injection at declared ports. Do not mock storage, evidence writing, artifact bytes, state machines, cohort planning, simulator/replay algorithms, or the real HTTP route under test.

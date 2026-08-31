# Edge Fleet Rollout Safety Control Plane — Codebase Context

> Greenfield implementation context derived from the locked PRD. Planned paths are aspirational until their Phase item lands.
>
> Last updated: 2026-08-30
> Template synced: 2026-08-28

## Tech Stack

| Layer | Technology |
|-------|------------|
| Language | C++23 |
| HTTP and UI | Drogon 1.9.x, CSP templates, vendored HTMX 2.x, project CSS |
| Standalone data | SQLite 3 WAL through `Storage`; local artifact filesystem |
| Later production data | PostgreSQL 16 through the same `Storage` contract |
| Replay analytics | Embedded DuckDB 1.x, not operational truth |
| Build and dependencies | CMake 3.30+, CMake presets, pinned vcpkg manifest |
| Crypto and JSON | OpenSSL 3, Argon2id, nlohmann/json |
| Logging and metrics | spdlog JSON, Prometheus text, W3C trace context |
| Tests | Catch2 3; real SQLite components; later PostgreSQL contracts; dev-only Playwright |
| Hosting | Standalone executable first; later non-root Linux container behind Traefik |

## Project Structure

```text
src/{shared,domain,application,infrastructure,web,cli,bootstrap}/
web/{templates,fragments,static,vendor}/
config/schemas/
migrations/{sqlite,postgres}/
fixtures/{scenarios,benchmarks,tenants,devices}/
tests/{unit,component,contract,integration,e2e,fixtures}/
scripts/
docs/{architecture,build-journal}/
```

Dependency order is `shared -> domain -> application -> infrastructure/web/cli -> bootstrap`. Domain code cannot include Drogon, SQL-dialect, filesystem, HTTP, or template headers.

## Key Modules

Module records live one-per-file under `.agent/knowledge/modules/`. The PRD plans auth, fleets, artifacts, cohorts, releases, devices, health, simulation, replay, evidence, storage, integrations, web, CLI, and bootstrap modules.

## Database Schema

| Group | Tables |
|-------|--------|
| Tenant and auth | `tenants`, `operator_credentials` |
| Fleet and device | `fleets`, `devices`, `device_credentials` |
| Artifact | `artifact_signing_keys`, `artifacts` |
| Rollout | `rollout_policies`, `releases`, `release_memberships`, `release_stages`, `release_assignments` |
| Immutable protocol | `rollout_commands`, `device_reports`, `health_samples`, `health_gate_evaluations`, `approval_requests` |
| Simulation and replay | `simulation_runs`, `replay_runs`, `benchmark_runs`, `benchmark_results` |
| Evidence and delivery | `evidence_events`, `evidence_checkpoints`, `evidence_exports`, `operator_notices`, `integration_configs`, `outbox_deliveries` |
| Reliability | `idempotency_records`, `job_leases` |

All data-bearing tables except `tenants` carry `tenant_id`. Tenant-owned relationships use composite tenant foreign keys. Commands, reports, gate evaluations, approvals, and evidence events are immutable. SQLite stores validated UUID/timestamp text; PostgreSQL uses UUID, TIMESTAMPTZ, and JSONB while preserving the same logical schema and `schema_version`.

## External Integrations

| Service | Purpose | Auth | Default |
|---------|---------|------|---------|
| IoT Sensor Data Aggregator | Read optional active-stage health evidence | `X-API-Key` | Disabled |
| Event-Driven Notification Hub | Publish frozen release and safety events | `X-API-Key` | Disabled; local notices remain |
| Workflow Automation Engine | Start and observe non-authoritative playbooks | `X-API-Key` | Disabled |
| Sentry-compatible OTLP endpoint | Optional error export | Endpoint configuration | Disabled |

External adapters never mutate release state. Unknown adapter types fail validation. Required IoT evidence fails closed when stale or unavailable.

## Environment Variables

| Group | Variables |
|-------|-----------|
| Runtime | `EDGEFLEET_ENV`, `EDGEFLEET_HOST`, `EDGEFLEET_PORT`, `EDGEFLEET_PUBLIC_URL`, `EDGEFLEET_LOG_LEVEL`, `EDGEFLEET_LOG_FORMAT`, `EDGEFLEET_WORKER_ENABLED`, `EDGEFLEET_WORKER_CONCURRENCY` |
| Tenant | `SELF_REGISTRATION_ENABLED`, `DEFAULT_TENANT_NAME`, `DEFAULT_TENANT_LEGAL_NAME`, `DEFAULT_TENANT_TIMEZONE` |
| Security | `SESSION_ENCRYPTION_KEY`, `CREDENTIAL_ENCRYPTION_KEY`, `CURSOR_HMAC_KEY`, `TRUSTED_PROXY_CIDRS`, `PRIVATE_ADAPTER_CIDRS` |
| Storage | `STORAGE_BACKEND`, `SQLITE_PATH`, `SQLITE_BUSY_TIMEOUT_MS`, `DATABASE_URL`, `DATABASE_POOL_SIZE`, `POSTGRES_DB`, `POSTGRES_USER`, `POSTGRES_PASSWORD` |
| Artifact/evidence | `ARTIFACT_STORE_PATH`, `ARTIFACT_TEMP_PATH`, `ARTIFACT_MAX_BYTES`, `ARTIFACT_MIN_FREE_BYTES`, `TRACE_STORE_PATH`, `EXPORT_STORE_PATH` |
| Release | `DEFAULT_STAGE_PERCENTAGES`, `DEFAULT_MIN_OBSERVATION_SECONDS`, `DEFAULT_TELEMETRY_FRESHNESS_SECONDS`, `DEFAULT_CONVERGENCE_TARGET`, `DEFAULT_MAX_OFFLINE_FRACTION`, `COMMAND_EXPIRY_SECONDS` |
| Simulation | `SIMULATOR_MAX_DEVICES`, `SIMULATOR_MAX_EVENTS`, `SIMULATOR_WORKER_CONCURRENCY`, `BENCHMARK_CORPUS_PATH`, `REPLAY_CHECKPOINT_INTERVAL` |
| IoT | `IOT_AGGREGATOR_ENABLED`, `IOT_AGGREGATOR_URL`, `IOT_AGGREGATOR_API_KEY`, `IOT_AGGREGATOR_REQUIRED_FOR_PROMOTION`, `IOT_AGGREGATOR_FIXTURE_MODE` |
| Notification | `NOTIFICATION_HUB_ENABLED`, `NOTIFICATION_HUB_URL`, `NOTIFICATION_HUB_API_KEY`, `NOTIFICATION_HUB_FIXTURE_MODE` |
| Workflow | `WORKFLOW_ENGINE_ENABLED`, `WORKFLOW_ENGINE_URL`, `WORKFLOW_ENGINE_API_KEY`, `WORKFLOW_ENGINE_WORKFLOW_ID`, `WORKFLOW_ENGINE_FIXTURE_MODE` |
| Observability | `METRICS_BIND_HOST`, `METRICS_PORT`, `OTEL_EXPORTER_OTLP_ENDPOINT`, `SENTRY_DSN` |

Secret values remain blank in `.env.example`. Development setup generates missing runtime keys once into ignored `local-secrets/runtime.env`. Production rejects blanks and development-generated secrets.

## Commands

| Action | Command |
|--------|---------|
| Configure standalone | `cmake --preset dev` |
| Build | `cmake --build --preset dev -j 2` |
| Dev server | `build/dev/edgefleet serve` |
| Run tests | `ctest --preset dev --output-on-failure` |
| Run tests, unit only | `build/dev/edgefleet_tests.exe "[unit]"` |
| Run tests, component/integration | `build/dev/edgefleet_tests.exe "[component]"` |
| Static/API/secret checks | `npm audit --package-lock-only --audit-level=high`; `scripts/scan-secrets.ps1`; parse `openapi.yaml` |
| Migrate DB | `build/dev/edgefleet migrate` |
| E2E tests | `npm ci && npm run test:e2e -- --workers=1 --project=chromium --project=mobile` |
| Run setup | `build/dev/edgefleet setup` |
| Run simulator | `build/dev/edgefleet simulate --scenario fixtures/scenarios/healthy-10k.json --seed 42` |
| Run benchmark | `build/dev/edgefleet benchmark --corpus fixtures/benchmarks/v1/manifest.json` |
| Verify evidence | `build/dev/edgefleet evidence verify --tenant <tenant-id>` |
| Start infra | `N/A for standalone core`; later `docker compose -f docker-compose.prod.yml --profile production up -d` |
| Stop infra | `N/A for standalone core`; later `docker compose -f docker-compose.prod.yml --profile production down` |
| Check infra | `N/A for standalone core`; Docker packaging verification is recorded only after the production profile is restored |

## Tenant Model

Operator API-key middleware resolves one tenant, actor, and role. Device HMAC middleware resolves one tenant and device. `TenantContext` is mandatory at every protected API, HTMX, job, and adapter entry. Storage methods take `tenant_id` first. The bootstrap tenant key is admin; additional credentials are `admin`, `release_manager`, `approver`, or `viewer`. Device credentials never enumerate tenant resources.

## Key Patterns and Conventions

Pattern records live one-per-file under `.agent/knowledge/patterns/`. Binding rules are frozen-input determinism, monotonic generations and report projections, immutable evidence, UUID lookups within tenant scope, strict enum rejection, explicit state transitions, outbox side effects after local safety commits, and no runtime dependency on optional adapters.

## Gotchas and Lessons Learned

Gotchas live one-per-file under `.agent/knowledge/gotchas/`. No implementation gotcha has evidence yet.

## Shared Foundation

Foundation primitives live one-per-file under `.agent/knowledge/foundation/`. Planned primitives are `CanonicalJson`, `DigestService`, `TenantClock`, `SecretResolver`, `HttpClientPool`, `DatabasePool`, `JobLeaseStore`, shared errors, authorization, and configuration. Create a foundation record only when its source exists.

## Deep References

| Topic | Planned path |
|-------|--------------|
| Domain modules | `src/domain/` |
| Application commands, queries, and jobs | `src/application/` |
| Storage and external adapters | `src/infrastructure/` |
| REST, device protocol, pages, and middleware | `src/web/` |
| CLI commands | `src/cli/` |
| Runtime composition | `src/bootstrap/` |
| Templates and HTMX fragments | `web/` |
| Tests | `tests/` |
| Database migrations | `migrations/` |
| Scenario and benchmark corpus | `fixtures/` |

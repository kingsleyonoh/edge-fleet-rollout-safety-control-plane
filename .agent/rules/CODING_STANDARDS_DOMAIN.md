# Edge Fleet Rollout Safety Control Plane — Domain and Production Rules

> Load for security, deployment, operations, data-boundary, or performance work. Auth, database, API, and job details live in their domain rule files.

## Branch and deployment boundary

Develop and test on `dev` against standalone SQLite and local artifacts. Merge to `main`, migrate production data, push, release, or deploy only with separate explicit authorization. A normal implementation commit or bootstrap does not authorize deployment.

Docker and PostgreSQL are later production concerns. The core configure, build, test, setup, serve, simulation, benchmark, evidence, and replay paths must remain Docker-free.

## Secrets management

- Never hardcode real API keys, operator/device keys, HMAC material, session/cursor/credential encryption keys, database passwords, cookies, authorization headers, signed URLs, or adapter secrets.
- Tracked code, tests, fixtures, docs, compose files, and journal files receive env-var references or safe placeholders only.
- Local `.env`, `local-secrets/`, runtime data, and Mesh evidence stay ignored. Mission evidence must not copy secrets either.
- `docker-compose.prod.yml` uses `${VAR}` references. Production gets values from host-managed secret storage.
- Logs redact API and device headers, cookies, auth/device request bodies, signatures, secret references, and adapter bodies that may contain sensitive values.
- Run the project secret scanner before commit/push once Phase 0 creates it. A finding blocks the action and requires human-driven credential rotation if a real value leaked.

## Tenant and config-driven surfaces

Every API, UI, job, storage, cache, simulation, replay, artifact, evidence, and adapter operation resolves one tenant. Cross-tenant identifiers return 404 and write no business row.

Tenant identity comes from Section 4.T fields. UI, local notices, external event payloads, and evidence exports use a typed immutable snapshot. Missing required fields fail validation. Re-export and outbox retry reuse the stored snapshot.

Tests use at least two tenants with different identity, credentials, labels, artifacts, and wordmarks. A response or rendered payload must exclude every other tenant's literal values.

## Release safety

- Only `ReleaseStateMachine` changes release state.
- Every control carries expected release version and idempotency key.
- Release readiness freezes membership, policy, manifests, rollback manifest, and cohort salt.
- A stage promotes only on an immutable passing evaluation over fresh evidence from frozen membership.
- Decision precedence is rollback, abort, pause, insufficient evidence, pass.
- Gate override is evaluation-bound, version-bound, two-person, expires in 30 minutes, starts a fresh observation window, and never directly promotes.
- Rollback sends a higher desired generation to every device that received or observed the target generation.
- Adapter failure never reverses a safe local state change. Required IoT evidence may block promotion only through insufficient evidence.

## Device protocol safety

- Devices poll over HTTPS. No inbound device access, remote shell, or arbitrary command execution.
- HMAC covers method, path, sequence, and raw-body SHA-256. Use constant-time comparison.
- Insert immutable reports before projection updates.
- Duplicate identifiers with the same digest return the stored result. A changed payload returns 409 and a security event.
- Lower report sequences may remain evidence but cannot move current projections backward.
- Matching desired generation and digest proves convergence. Acknowledgement alone is delivery evidence.

## Artifact safety

Stream uploads with size limits and digest calculation. Normalize filenames. Store immutable content-addressed bytes with no execute permission. Validate compatibility, manifest schema, size, SHA-256, Ed25519 signature, signing-key status, and free-space threshold before `ready`.

Assigned devices and authorized operators only may download. Do not reveal filesystem paths. A compromised key blocks affected artifacts and unsafe rollback. Referenced bytes remain preserved.

## API and browser security

- Validate at API/form boundaries and reject unknown fields where the schema is closed.
- Use one backend authorization function for REST and HTMX controls.
- Browser sessions are encrypted, `HttpOnly`, `Secure` in production, and `SameSite=Strict`; mutations require CSRF.
- Apply CSP without inline script, clickjacking denial, MIME-sniffing denial, and `same-origin` referrer policy.
- Use rate limits from PRD §8b and body/upload limits from the module contracts.
- Error responses contain code, safe message, details, and trace ID. Never expose SQL, stack traces, secrets, raw adapter bodies, or filesystem paths.
- Adapter SSRF controls require HTTPS in production, approved hosts and ports, resolved-IP validation, no cross-host redirects, mandatory TLS verification, and explicit private-network allowlists.

## Production packaging

The production image is multi-stage and non-root, UID/GID 10001. Keep the root filesystem read-only and mount writable volumes only for artifacts, traces, exports, and temporary uploads. Migrations run as a one-shot command before app startup. The app refuses unsupported schema versions.

Traefik terminates TLS. Trust forwarded headers only from configured proxies. TLS 1.2 or newer is required. Add HSTS only after a verified deployment.

Production Compose starts this app and PostgreSQL. Prometheus is optional. It must not start portfolio services.

## Logging and observability

Use spdlog JSON on stdout in production. Include tenant-safe context such as `trace_id`, actor ID, module, release/device public ID, job name, timestamps, result, and error code. Do not log secrets, artifact bytes, or PII-heavy request bodies.

Expose Prometheus metrics and W3C trace IDs. `/health` proves the event loop; `/health/db`, `/health/artifacts`, and `/health/evidence` prove their dependencies; `/health/ready` requires database/schema, artifact storage, secret resolver, and job leases. Optional adapters appear in details but do not make the core unready.

Evidence append or chain verification failure blocks mutations and moves the affected tenant to safe read-only behavior.

## Performance rules

- Reuse startup-owned `DatabasePool`, `HttpClientPool`, `SecretResolver`, `EvidenceWriter`, and clocks. Do not create them per request or job.
- Run independent I/O concurrently. Keep dependent transitions serial inside one transaction.
- Prefer joins or bounded batched queries over N+1 access.
- Authenticated API and HTML use `no-store`. Cache immutable static assets and authorized artifact bytes by digest only.
- Credential cache is five minutes and invalidates on revoke. Artifact metadata and active policies cache for 30 seconds. Never cache release-control decisions.
- Audit compound I/O after five or more operations share one endpoint or page.
- Record CPU, cores, RAM, OS, compiler, build type, storage profile, and commit SHA with benchmark claims.

## Backup and recovery

SQLite backup includes a consistent database backup and artifact snapshot. Production backs up PostgreSQL and artifacts from one epoch. Recovery restores both in isolation, verifies the evidence chain, and replays the latest completed release. No recovery tool rewrites immutable evidence.

## Production-readiness gate

Before merge to `main`:

1. Full Catch2/ctest, contract, integration, and applicable real-HTTP/Playwright suites pass.
2. Both storage dialects reach the same schema version when PostgreSQL applies.
3. No debug output, placeholder code, stale TODO/FIXME/HACK, undocumented env var, or unhandled user path remains.
4. Migrations, OpenAPI, MCP, and context match reachable implementation.
5. Dependency, container, secret, API-schema, security, accessibility, and required performance checks have no high or critical finding.
6. Evidence and command tables retain their immutability controls.

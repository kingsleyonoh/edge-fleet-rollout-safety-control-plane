# API and web rules

- JSON endpoints use `/api`, device protocol uses `/api/agent/v1`, and HTML uses `/app`.
- Mutation endpoints require `Idempotency-Key`, except tenant registration and signed report envelopes with `report_id`.
- Handlers validate closed schemas, authorize through shared `PolicyDecision`, call an application service, and map typed results. Keep SQL and domain transitions out of controllers.
- Storage calls carry `tenant_id`. Cross-tenant IDs return 404.
- Use opaque UUID resource IDs and opaque HMAC-bound pagination cursors. Changed cursor filters return `CURSOR_FILTER_MISMATCH`.
- Error JSON is `{ error: { code, message, details, trace_id } }`. Use 401, 403, 404, 409, 422, 429, and 503 as defined in PRD §8b. Never expose secrets, SQL, stack traces, raw adapter bodies, or paths.
- Apply PRD rate limits, size limits, CSRF, CSP, secure session cookies, and `no-store` on authenticated responses.
- Device download routes require an active assignment for that exact digest. Support ranges without disclosing storage paths.
- Register `requireRole(role, resource, permission)` metadata so the role matrix audit can grep reachability and exercise every cell.
- OpenAPI paths must match actual registered routes. Every endpoint needs in-process production-stack tests and real-HTTP E2E when implemented.

# Authentication and authorization rules

- Every protected REST, HTMX, device, job, and adapter path receives a non-null `TenantContext` or device context before business logic.
- Operator keys use globally unique lookup prefixes and Argon2id hashes. Return a raw key once. Never store or log it.
- Browser sessions carry opaque IDs only, expire after eight hours, use encrypted `HttpOnly` cookies, `Secure` in production, `SameSite=Strict`, and CSRF on mutations.
- Device HMAC covers method, path, sequence, and raw-body SHA-256. Compare in constant time. Rotation acknowledgement must use the successor key and atomically revoke the prior key.
- Use the one `PolicyDecision` implementation for API controllers, HTMX rendering, jobs, and callbacks.
- Enforce the complete PRD §2b role matrix. A `release_manager` cannot self-approve when two-person approval applies. Approver and viewer roles do not gain mutation through hidden routes.
- Cross-tenant resource IDs return 404. Authentication failure is 401; authenticated lack of permission is 403; disabled tenants are 403.
- Seed two tenants and all five credential roles. Exercise every allowed and denied matrix cell through real HTTP and assert denied paths write no business row.

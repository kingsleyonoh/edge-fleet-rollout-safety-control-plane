# Database and storage rules

- Domain and application code depend on `Storage`; SQL stays inside dialect implementations.
- SQLite and PostgreSQL migrations are numbered, reach the same `schema_version`, and implement one logical schema.
- Every data-bearing table except `tenants` has `tenant_id`; every tenant-owned parent relation uses a composite tenant foreign key and index starting with `tenant_id`.
- Storage methods take `tenant_id` first. Only globally unique credential-prefix lookup may query before tenant resolution.
- Commands, reports, gate evaluations, approvals, and evidence events are insert-only facts. No repair path rewrites them.
- Business projection changes and the next hash-chained evidence event commit atomically under the tenant chain lock.
- Use optimistic release versions and idempotency records. Same key plus same digest returns the stored response; changed digest returns 409.
- SQLite uses one writer and a bounded read pool. PostgreSQL uses a bounded async pool and tenant advisory lock for evidence append.
- Tests run production migrations and the shared storage contract against two tenants. No fake repository or alternate test schema.

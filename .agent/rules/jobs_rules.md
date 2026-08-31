# Background job rules

- Queue rows persist complete frozen input at origin. A retry or reclaimed lease must not reread mutable release, policy, tenant identity, adapter payload, corpus, or source bounds.
- Claim work with expiring `JobLeaseStore` leases. Standalone SQLite runs one worker process. Jobs resume after abandoned leases without duplicate side effects.
- Jobs receive tenant context, `TenantClock`, storage, evidence writer, and shared pools through composition. They do not create pools or call wall clock directly in simulation or replay.
- `stage_gate_evaluator` writes an immutable evaluation and hands it to the state machine. It never changes release state itself.
- Outbox workers use frozen payloads and stable idempotency keys. Safe local transitions remain committed when external delivery fails.
- Workflow timeout after possible delivery becomes `AMBIGUOUS_DELIVERY`; do not trigger again automatically.
- Simulation, replay, benchmark, export, and adapter jobs preserve input/version/digest and deterministic order. Cancellation retains reproducible input.
- Log job name, tenant, trace, start, finish, result, row counts, and safe error code. Never log secrets or artifact bytes.
- Restart tests prove due jobs resume, leases release, commands do not duplicate, and evidence has no gap.

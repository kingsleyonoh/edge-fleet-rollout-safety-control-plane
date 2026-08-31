# Product contract

Edge Fleet is an operations console for decisions that must remain safe when devices are slow, offline, duplicated, or out of order.

The primary operator loop is: inspect fleet health, register or quarantine devices, validate a signed artifact, draft and activate a fixed-wave policy, freeze a release, request or grant four-eyes approval, start a stage, watch observed convergence, evaluate fresh health evidence, and pause or roll back when a hard gate fails.

Every page must expose source-of-truth state, last update time, tenant identity, and the evidence link for mutations. Delivery, command receipt, and acknowledgement are not convergence. All destructive controls require an explicit reason and show the expected release version.

The console supports loading, empty, stale, denied, conflict, paused, rollback, completed, adapter-down, and error states. It uses plain language, semantic status text in addition to color, keyboard-visible focus, reduced-motion support, and compact responsive layouts for field operators.

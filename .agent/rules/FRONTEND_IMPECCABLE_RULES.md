# Frontend Impeccable Rules

Use these rules whenever a task touches UI, UX, frontend routes, templates, static assets, CSS, design tokens, page copy, or visual states.

## Purpose

Codex-style agents tend to ship generic, technically-correct frontend work. Klevar frontend work must instead carry explicit product/design intent, avoid AI-slop defaults, and pass measurable UI quality checks.

This rule is based on the Impeccable design workflow documented at `https://impeccable.style/docs/`.

## Required Context Files

Frontend work must use these project-root context files when present:

- `PRODUCT.md` — product/register/users/personality/anti-references/design principles.
- `DESIGN.md` — colors, typography, spacing/elevation, components, do/don't rules.

If a frontend task starts and either file is missing, create a minimal starter from the PRD and existing UI before implementing the UI. Ask the user only when the PRD lacks enough information to make a safe starter.

## Frontend Build Flow

For new UI features, follow the Impeccable `craft` shape:

1. Shape the design intent before coding: purpose, user, content, constraints, emotional tone, anti-reference.
2. Load/use `PRODUCT.md` and `DESIGN.md`.
3. Implement structure first, then spacing/hierarchy, typography/color, states, motion, and responsive behavior.
4. Run browser/E2E validation for reachable UI.
5. Polish after functional completion.

## Frontend Audit Gate

Every frontend batch must run the Impeccable detector through a focused Pi/Mesh validation lane when the package is available:

```bash
npx --yes impeccable@latest detect --fast --json <changed-frontend-paths>
```

Save detector output under the worker or browser artifact directory and cite it from `report.json`. Detector findings are blocking unless the project explicitly documents and evidences a narrower frontend policy.

Every frontend batch must also produce explicit Impeccable-style evidence in the result flags or report:

- `FRONTEND_IMPECCABLE_AUDIT_PASS` — checked accessibility, performance, theming, responsive behavior, and anti-patterns.
- `FRONTEND_IMPECCABLE_POLISH_PASS` — checked spacing, typography, color/contrast, interaction states, motion/reduced-motion, copy, and design-system token drift.

Equivalent flags with `_P0` or `_P1` mean blocking issues and must be fixed before merge. P2/P3 findings may be journaled as follow-up work if functionality is otherwise correct.

## Blocking Frontend Findings

Treat these as hard failures for frontend batches:

- P0/P1 accessibility failures: missing labels, broken keyboard flow, unusable focus state, contrast that blocks reading.
- Broken responsive behavior at mobile/tablet/desktop widths.
- Hard-coded visual values where design tokens or shared components exist.
- Generic AI-slop defaults: unexplained purple gradients, card grids on card grids, glassmorphism without product rationale, random emojis/illustrations, placeholder copy.
- Missing hover/focus/active/disabled/loading/error states for interactive controls.
- Motion that causes layout jank or ignores `prefers-reduced-motion`.

## Product Quality Gate Evidence

A focused frontend review lane also runs a PRD-driven quality check. It reads the PRD and only requires evidence for concerns the PRD actually names:

- If the PRD says mobile-first, mobile, touch, responsive, or small-screen: provide `MOBILE_VIEWPORT_PASS` or equivalent Playwright/component evidence for the critical mobile viewport flow.
- If the PRD says offline, PWA, local-first, service worker, or Workbox: provide `OFFLINE_PWA_PASS` or equivalent evidence for the offline/PWA flow.
- If the PRD says privacy, consent, raw coordinates, client details, tenant data, or data policy: provide `PRIVACY_MATRIX_PASS` or equivalent evidence that sensitive frontend flows respect consent/privacy boundaries.
- If the PRD says bundle, first-load, dynamic import, lazy load, or heavy libraries: provide `BUNDLE_DYNAMIC_IMPORT_AUDIT_PASS` or equivalent bundle/dynamic-import evidence.

Backend-only batches are not burdened by this frontend quality gate; they remain governed by TDD/E2E/wiring/business/secrets gates.

## Result Contract Expectations

For any frontend-touching implementation batch:

- Include `PRODUCT.md` and/or `DESIGN.md` in `filesChanged` if the batch had to create or update them.
- Ensure `DESIGN.md` is product-specific, not generic. It must cover mobile/responsive behavior when relevant, accessibility/focus/contrast, loading/empty/error/offline/warning states, spacing/typography/density/hierarchy, component patterns, and anti-patterns.
- Include frontend route/component entrypoints in `wiring.entrypoints`.
- Include browser or component/E2E evidence in `tests.e2e` when a reachable page/interaction changed.
- Include `FRONTEND_IMPECCABLE_AUDIT_PASS` and `FRONTEND_IMPECCABLE_POLISH_PASS` in `flags` when no blocking issues remain.
- Include PRD-driven product quality flags such as `MOBILE_VIEWPORT_PASS`, `OFFLINE_PWA_PASS`, `PRIVACY_MATRIX_PASS`, and `BUNDLE_DYNAMIC_IMPORT_AUDIT_PASS` when the PRD names those concerns and the batch touches the frontend surface.
- If blocking P0/P1 findings remain, return `status: FAILURE` with `failureType: FRONTEND_IMPECCABLE_FINDINGS`.

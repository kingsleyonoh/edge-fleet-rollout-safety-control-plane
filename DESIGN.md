# Operations console design

## Visual language

Use a dark graphite canvas, high-contrast white text, a restrained mint accent, and status colors only as secondary cues. Cards are reserved for bounded operational facts; tables are preferred for fleet, device, assignment, and evidence lists. The page title states the decision context, not a marketing slogan.

## Interaction rules

- Keep the tenant name, current release state, stage, and evidence link visible near every control.
- Disable a control only when the API would reject it, and explain why in text.
- Confirm pause, cancel, abort, rollback, key compromise, retirement, and adapter disablement with a reason and expected version.
- Render stale evidence with its timestamp and a refresh action. Never silently treat missing data as healthy.
- Use progressive disclosure for long JSON and evidence payloads; preserve copyable digests.

## Accessibility and responsive contract

Pages use landmarks, one `h1`, associated labels, logical tab order, a visible focus ring, 4.5:1 normal-text contrast, and text labels for severity. Controls remain usable at a 320px viewport, tables can scroll without trapping focus, and `prefers-reduced-motion` disables transitions. HTMX fragments must remain valid standalone HTML and stay below 100 KiB.

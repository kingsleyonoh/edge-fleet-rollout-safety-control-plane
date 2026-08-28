# Project-Local Checks (catalog)

> **One file per check.** Each check is a project-local enforcement rule created from evidence of a recurring failure pattern. Focused implementation/review lanes read matching checks before editing and **reject the plan** if it triggers one — preventing recurrence at plan time, not test time.
>
> Checks are project-local by design. Stack-class candidates may be reviewed for promotion to template knowledge using ordinary evidence-backed maintenance.
>
> Checks may be retired after an evidence-backed review proves their target pattern no longer exists. Template-class coding standards remain human-maintained; checks here have a lifecycle tied to the project's evolution.
>
> Filename convention: `{failure_type}-{slug}.md` (lowercase, hyphenated). Example: `tests-wont-green-mock-database-in-integration.md`.

## Catalog

| Filename | Failure type | Slug | Created (batch / date) | Last fired (batch) | Times fired | Status |
|----------|--------------|------|------------------------|---------------------|-------------|--------|
| EXAMPLE.md | (template) | (template) | (template) | — | — | template — delete me |

> Add one row per check file. The focused worker that lands a check updates both the file and this row. Evidence-backed review updates `Last fired` and `Times fired`, and may propose retirement (remove the row and delete the file) when the pattern is demonstrably dead.

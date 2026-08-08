# Agent Instructions for Aerogram

## Hard Rules

1.  **Read `docs/ARCHITECTURE.md` in full before making any change.** It
    defines the design patterns this project depends on. Changes that
    violate these patterns will be rejected even if they compile and
    run.

2.  **Do not introduce new architectural patterns.** If an existing
    pattern in the doc doesn't fit your task, stop and ask.

3.  **Do not bypass the architecture layers.** Specifically:
    - UI components must not call backends directly.
    - IPC must not touch UI components.
    - Backends must not know about the UI or IPC.

4.  **Do not hand-write IPC dispatch code.** The IPC layer discovers
    controller methods reflectively. Adding a controller slot is
    enough; the IPC surface extends automatically.

5.  **Do not commit without explicit approval.** Only commit, push,
    amend, or create PRs when the user has explicitly asked for it.

## Further Reading

- **`docs/ARCHITECTURE.md`** — design patterns and rationale. Read
  first.
- **`docs/DEVELOPMENT.md`** — build, run, and test commands. Read
  before running the app.

## Questions

If a design decision is unclear, resolve it by reading
`docs/ARCHITECTURE.md`. If still unclear, ask the user before making
the change. Do not guess at architectural intent.

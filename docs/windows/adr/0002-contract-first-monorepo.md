# ADR 0002: Keep Windows contract work in the product repository

- Status: Accepted for M0
- Date: 2026-07-31
- Owner: Engineering
- Applies to: M0

## Decision

Keep project, effect, and MCP contracts beside the macOS source that currently
defines their behavior. Store compatibility fixtures and drift checks in this
repository so macOS and future Windows CI consume the same inputs.

The initial layout is:

```text
contracts/
fixtures/contracts/
docs/windows/adr/
tools/validate_contracts.py
```

## Consequences

- Contract changes and their source changes are reviewed together.
- Snapshot updates must be explicit and cannot be rewritten by CI.
- M0 does not add an empty CMake target or claim a Windows build exists.
- A shared C++ core and Swift binding remain a post-MVP proposal requiring a
  separate ADR.

## Revisit when

- The first compilable Windows contract probe is ready.
- Build ownership or repository size demonstrates a concrete monorepo problem.


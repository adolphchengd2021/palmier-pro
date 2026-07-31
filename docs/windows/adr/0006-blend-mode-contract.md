# ADR 0006: Lock the Windows blend-mode contract to 16 modes

- Status: Accepted
- Date: 2026-07-31
- Owner: Media and rendering
- Applies to: M0 contract and later

## Decision

Windows implements the 16 values in
`contracts/effects/v1/blend-modes.json`. The count includes `normal`, whose
baseline compositing behavior is source-over. The value order and identifiers
match the current Swift `BlendMode` enum.

This locks the compatibility surface, not the prototype implementation scope.
The Technical MVP implements and verifies one representative blend path plus
the normal baseline. P0-release implements all 16 modes before claiming parity
with the current macOS product.

The earlier requirement for 17 modes was a counting error, not a request for a
new blend operation. A seventeenth mode may be added only through a separate
product and compatibility change that defines its stable identifier, math,
alpha behavior, preview/export parity, persistence behavior, and tests on both
platforms.

## Consequences

- Windows must not invent a placeholder or alias to reach a count of 17.
- Preview, export, UI, project persistence, and MCP use the same 16 identifiers.
- Contract audit fails if the Swift enum, JSON snapshot, or requirements count
  diverges.

## Revisit when

- Product explicitly approves a new blend operation.
- A compatibility migration is required for an existing identifier.

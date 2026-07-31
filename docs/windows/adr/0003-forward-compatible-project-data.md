# ADR 0003: Preserve project data across platform round trips

- Status: Accepted invariant; implementation blocked
- Date: 2026-07-31
- Owner: Project core
- Applies to: P0

## Decision

P0-release requires semantic round trips in both directions:

```text
macOS → Windows → macOS
Windows → macOS → Windows
```

Known values, stable IDs, integer-frame timing, and ordering must retain their
meaning. Unknown values must retain their JSON type and value at the owning
entity. Byte-for-byte JSON formatting is not required.

Unknown data is matched to stable entity identity, never only to an array
position. An implementation must not use a generic recursive merge that can
reattach unknown data to a different track, clip, effect, or media entry after
reordering or deletion.

If a client cannot preserve a newer format safely, it opens the project
read-only and reports why. It must not silently write a downgraded project.

Project-relative media paths serialize with `/`. An unavailable macOS absolute
path stays unchanged while Windows reports the asset offline.

## Current gap

The current Swift `Codable` models use closed coding keys and rebuild JSON on
save. They do not preserve arbitrary unknown fields. The M0 fixtures and audit
make that gap observable but do not fix it.

## Required proof

A client must:

1. Load the unknown-field fixture.
2. Modify one known field through the normal domain operation.
3. Save the project package.
4. Reopen the raw JSON.
5. Confirm every canary JSON Pointer still has the same type and value.

Schema acceptance or parser-only round trips do not satisfy this proof.

## Revisit when

- The opaque JSON representation and migration API are designed.
- The macOS and Windows serializers both pass the canary round trip.


# ADR 0003: Preserve project data across platform round trips

- Status: Accepted invariant; macOS canary enforced, Windows pending
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

## Current status

The audited Swift project and manifest paths preserve opaque fields at the
root, timeline, track, clip, effect, parameter, keyframe, word-timing,
manifest, entry, generation, import, and media-source layers. The declared
canaries pass a production `NSDocument` Save As and reopen test in macOS CI.
The verified reference is commit `29d98f8`, CI run `30644474474`.

The Windows writer does not exist yet. It must implement the same identity and
replacement rules and pass differential round trips before this invariant is
complete across platforms.

## Required proof

A client must:

1. Load the unknown-field fixture.
2. Modify one known field through the normal domain operation.
3. Save the project package.
4. Reopen the raw JSON.
5. Confirm every canary JSON Pointer still has the same type and value.

Schema acceptance or parser-only round trips do not satisfy this proof.

## Revisit when

- The macOS and Windows serializers both pass the canary round trip.

# Project contract v1

This contract documents the v0.6.16 disk format and forward-compatibility
requirements. It is not an on-disk schema version.

## Accepted roots

- Current `ProjectFile` root containing a non-empty `timelines` array.
- Legacy bare `Timeline` root.

## Time and identity

- Integer frames are authoritative.
- Timeline, track, clip, media, effect, link, caption, and multicam identities
  are stable across compatible saves.
- Validation precedes integer arithmetic, indexing, and conversion.

## Unknown fields

Every project object is extensible. Accepting an unknown field is insufficient:
the client must preserve its type and value through a normal load-edit-save.
Unknown data belongs to its stable entity identity.

The canary list in `canaries.json` covers every JSON value kind plus project
root, timeline, track, clip, transform, effect, effect parameter, keyframe,
media root, media entry, generation input, import input, and media-source
payload.

Runtime round-trip status: **Enforced**. The production project reader, editor
snapshot, shared writer, package writer, and reopen path retain every declared
canary while known fields are edited. Stable-ID arrays keep opaque fields with
their entity across reordering; deleted or replaced owners do not donate opaque
fields to new IDs.

Optional nested objects use the owning stable entity plus property slot as
their identity. Entity arrays use stable IDs. Keyframes and word timings carry
their opaque fields with the value so moves and timing rescaling retain them.
Other unidentifiable arrays retain opaque element data only while their known
content is unchanged; an edited unidentifiable array uses the current value
rather than risking stale-field attachment.

The project JSON Schema remains a provisional structural check and must not be
used alone to implement the Windows reader. The media JSON Schema enumerates
all current known Swift fields and stable enums. `ContractFixtureTests` freezes
current Swift decoder defaults and representative writer output;
`UnknownFieldRoundTripTests` independently enforces the declared opaque-field
canaries through load-edit-save and reopen.

## Paths

- Project media paths use `/`.
- External absolute paths retain their original platform spelling.
- An offline foreign-platform path is not rewritten until the user relinks it.

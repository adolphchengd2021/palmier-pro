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

The canary list in `canaries.json` covers project root, timeline, track, clip,
transform, effect, effect parameter, media root, media entry, generation input,
import input, and media-source payload.

Runtime round-trip status: **Blocked**. The current Swift serializer is known to
drop these canaries.

The project JSON Schema remains a provisional structural check and must not be
used alone to implement the Windows reader. The media JSON Schema enumerates
all current known Swift fields and stable enums, but client defaults and
writer/decode behavior still require Swift golden tests.

## Paths

- Project media paths use `/`.
- External absolute paths retain their original platform spelling.
- An offline foreign-platform path is not rewritten until the user relinks it.

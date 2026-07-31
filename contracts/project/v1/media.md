# Media manifest contract v1

`media.schema.json` enumerates every persisted field in the current
`MediaManifest`, `MediaManifestEntry`, `MediaImportInput`, `GenerationInput`,
`MediaFolder`, and `UpscaleSettings` Swift models.

## Decoder behavior

- A missing manifest `version` decodes as `1`; a new writer emits `2`.
- Missing manifest `entries` and `folders` decode as empty arrays.
- Media entries require `id`, `name`, `type`, `source`, and `duration`.
- Generation input requires `prompt`, `model`, `duration`, and `aspectRatio`.
- Import input may be an empty object.
- Upscale settings require the `selections`, `numbers`, and `toggles` maps when
  the object is present.
- Optional values accept either a missing key or explicit JSON `null`.
- Foundation `Date` values use the default numeric representation. The Swift
  writer golden fixture freezes the exact encoding Windows must reproduce.

The schema's `version >= 1` and `duration >= 0` rules are normative validity
requirements. The current Swift decoder does not enforce those numeric bounds,
so decoder acceptance alone is not evidence that a manifest is valid.

If present, manifest `version`, `entries`, and `folders` must be non-null in the
canonical contract. The current Swift decoder treats explicit `null` like a
missing key, but compatible writers must omit an absent value or write its
concrete value instead of serializing `null` at the manifest root.

## Source and path behavior

`MediaSource` is exactly one of:

- `external.absolutePath`, retaining its platform-native spelling;
- `project.relativePath`, serialized with `/` separators.

An object with both cases, no known case, or an invalid payload is rejected by
the current Swift decoder. Forward-compatible preservation of a future source
case remains blocked until the model can retain opaque enum data.

## Verification boundary

The Python audit compares schema properties, Swift field types, optionality,
required decode fields, stable enums, associated-value payloads, and focused
type-body digests with `media-model.json`, then validates positive and negative
fixtures for their expected reasons. It does not run `JSONEncoder` or
`JSONDecoder`. `ContractFixtureTests` exercises the production Swift decoder,
shared writer, and package exporter in macOS CI. Unknown-field load-edit-save
preservation remains a separate blocked requirement.

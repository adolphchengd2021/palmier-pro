from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from decimal import Decimal
from pathlib import Path
from typing import Any


CLIP_TYPES = {"video", "audio", "image", "text", "lottie", "sequence"}
BLEND_MODES = {
    "normal",
    "darken",
    "multiply",
    "colorBurn",
    "lighten",
    "screen",
    "colorDodge",
    "overlay",
    "softLight",
    "hardLight",
    "difference",
    "exclusion",
    "hue",
    "saturation",
    "color",
    "luminosity",
}


class OracleError(RuntimeError):
    pass


def reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise OracleError(f"duplicate key: {key}")
        result[key] = value
    return result


def reject_constant(value: str) -> Any:
    raise OracleError(f"non-finite number: {value}")


def load(path: Path) -> Any:
    return json.loads(
        path.read_text(encoding="utf-8"),
        object_pairs_hook=reject_duplicate_pairs,
        parse_constant=reject_constant,
        parse_float=Decimal,
    )


def is_integer(value: Any) -> bool:
    return type(value) is int and -(2**63) <= value <= 2**63 - 1


class Oracle:
    def __init__(self) -> None:
        self.diagnostics: list[dict[str, str]] = []
        self.next_id = 0

    def diagnose(self, code: str, pointer: str) -> None:
        self.diagnostics.append({"code": code, "jsonPointer": pointer})

    def diagnose_duplicates(
        self,
        values: list[dict[str, Any]],
        pointer: str,
    ) -> None:
        seen: set[str] = set()
        for index, value in enumerate(values):
            identifier = value["id"]["value"]
            if identifier in seen:
                self.diagnose("duplicateStableId", f"{pointer}/{index}/id")
            seen.add(identifier)

    def identifier(self, value: dict[str, Any], pointer: str) -> dict[str, str]:
        persisted = value.get("id")
        if isinstance(persisted, str):
            return {"origin": "persisted", "value": persisted}
        self.next_id += 1
        self.diagnose("synthesizedId", f"{pointer}/id")
        return {"origin": "synthesized", "value": f"synthesized-{self.next_id}"}

    def required_integer(self, value: dict[str, Any], key: str, pointer: str) -> int:
        if key not in value:
            raise OracleError(f"missingRequiredField {pointer}/{key}")
        if type(value[key]) is not int:
            raise OracleError(f"wrongRequiredType {pointer}/{key}")
        if not is_integer(value[key]):
            raise OracleError(f"integerOutOfRange {pointer}/{key}")
        return value[key]

    def required_positive_integer(
        self,
        value: dict[str, Any],
        key: str,
        pointer: str,
    ) -> int:
        field = self.required_integer(value, key, pointer)
        if field <= 0:
            raise OracleError(f"invalidRequiredValue {pointer}/{key}")
        return field

    def required_string(self, value: dict[str, Any], key: str, pointer: str) -> str:
        if key not in value:
            raise OracleError(f"missingRequiredField {pointer}/{key}")
        if not isinstance(value[key], str):
            raise OracleError(f"wrongRequiredType {pointer}/{key}")
        return value[key]

    def loose_string(self, value: dict[str, Any], key: str, pointer: str) -> str | None:
        field = value.get(key)
        if field is None:
            return None
        if isinstance(field, str):
            return field
        self.diagnose("invalidOptionalDefaulted", f"{pointer}/{key}")
        return None

    def default_string(
        self,
        value: dict[str, Any],
        key: str,
        pointer: str,
        fallback: str,
    ) -> str:
        field = value.get(key)
        if isinstance(field, str):
            return field
        if key in value:
            self.diagnose("invalidOptionalDefaulted", f"{pointer}/{key}")
        return fallback

    def default_bool(
        self,
        value: dict[str, Any],
        key: str,
        pointer: str,
        fallback: bool,
    ) -> bool:
        field = value.get(key)
        if type(field) is bool:
            return field
        if key in value:
            self.diagnose("invalidOptionalDefaulted", f"{pointer}/{key}")
        return fallback

    def default_integer(
        self,
        value: dict[str, Any],
        key: str,
        pointer: str,
        fallback: int,
    ) -> int:
        field = value.get(key)
        if is_integer(field):
            return field
        if key in value:
            self.diagnose("invalidOptionalDefaulted", f"{pointer}/{key}")
        return fallback

    def default_number(
        self,
        value: dict[str, Any],
        key: str,
        pointer: str,
        fallback: float,
    ) -> float:
        field = value.get(key)
        if type(field) in {int, float, Decimal}:
            try:
                converted = float(field)
            except OverflowError:
                converted = math.inf
            if math.isfinite(converted) and not (field != 0 and converted == 0):
                return converted
        if key in value:
            self.diagnose("invalidOptionalDefaulted", f"{pointer}/{key}")
        return fallback

    def clip_type(self, value: dict[str, Any], key: str, pointer: str) -> str:
        field = value.get(key)
        if isinstance(field, str) and field in CLIP_TYPES:
            return field
        if key in value:
            self.diagnose("invalidOptionalDefaulted", f"{pointer}/{key}")
        return "video"

    def blend_mode(self, value: dict[str, Any], pointer: str) -> str | None:
        field = value.get("blendMode")
        if field is None:
            return None
        if isinstance(field, str) and field in BLEND_MODES:
            return field
        self.diagnose("invalidOptionalDefaulted", f"{pointer}/blendMode")
        return None

    def clip(self, value: Any, pointer: str) -> dict[str, Any]:
        if not isinstance(value, dict):
            raise OracleError(f"wrongRequiredType {pointer}")
        clip_id = self.identifier(value, pointer)
        media_ref = self.required_string(value, "mediaRef", pointer)
        media_type = self.clip_type(value, "mediaType", pointer)
        source_clip_type = self.clip_type(value, "sourceClipType", pointer)
        start = self.required_integer(value, "startFrame", pointer)
        duration = self.required_integer(value, "durationFrames", pointer)
        trim_start = self.default_integer(value, "trimStartFrame", pointer, 0)
        trim_end = self.default_integer(value, "trimEndFrame", pointer, 0)
        speed = self.default_number(value, "speed", pointer, 1.0)
        volume = self.default_number(value, "volume", pointer, 1.0)
        opacity = self.default_number(value, "opacity", pointer, 1.0)
        link_group = self.loose_string(value, "linkGroupId", pointer)
        caption_group = self.loose_string(value, "captionGroupId", pointer)
        multicam_group = self.loose_string(value, "multicamGroupId", pointer)
        blend_mode = self.blend_mode(value, pointer)
        result = {
            "blendMode": blend_mode,
            "captionGroupId": caption_group,
            "durationFrames": duration,
            "id": clip_id,
            "linkGroupId": link_group,
            "mediaRef": media_ref,
            "mediaType": media_type,
            "multicamGroupId": multicam_group,
            "opacity": opacity,
            "sourceClipType": source_clip_type,
            "speed": speed,
            "startFrame": start,
            "trimEndFrame": trim_end,
            "trimStartFrame": trim_start,
            "volume": volume,
        }
        if start < 0 or duration <= 0 or start + duration > 2**63 - 1:
            self.diagnose("unsafeFrameRange", pointer)
        return result

    def track(self, value: Any, pointer: str) -> dict[str, Any]:
        if not isinstance(value, dict):
            raise OracleError(f"wrongRequiredType {pointer}")
        track_id = self.identifier(value, pointer)
        track_type = self.required_string(value, "type", pointer)
        if track_type not in CLIP_TYPES:
            raise OracleError(f"unsupportedRequiredEnum {pointer}/type")
        muted = self.default_bool(value, "muted", pointer, False)
        hidden = self.default_bool(value, "hidden", pointer, False)
        sync_locked = self.default_bool(value, "syncLocked", pointer, True)
        clips: list[dict[str, Any]] = []
        if "clips" in value:
            checkpoint = len(self.diagnostics)
            try:
                if not isinstance(value["clips"], list):
                    raise OracleError(f"wrongRequiredType {pointer}/clips")
                clips = [
                    self.clip(item, f"{pointer}/clips/{index}")
                    for index, item in enumerate(value["clips"])
                ]
                self.diagnose_duplicates(clips, f"{pointer}/clips")
            except OracleError:
                del self.diagnostics[checkpoint:]
                clips = []
                self.diagnose("invalidOptionalDefaulted", f"{pointer}/clips")
        display_height = min(
            max(self.default_number(value, "displayHeight", pointer, 50.0), 32.0),
            200.0,
        )
        return {
            "clips": clips,
            "displayHeight": display_height,
            "hidden": hidden,
            "id": track_id,
            "muted": muted,
            "syncLocked": sync_locked,
            "type": track_type,
        }

    def timeline(self, value: Any, pointer: str) -> dict[str, Any]:
        if not isinstance(value, dict):
            raise OracleError(f"wrongRequiredType {pointer}")
        timeline_id = self.identifier(value, pointer)
        name = self.default_string(value, "name", pointer, "Timeline 1")
        fps = self.required_positive_integer(value, "fps", pointer)
        width = self.required_positive_integer(value, "width", pointer)
        height = self.required_positive_integer(value, "height", pointer)
        settings = self.default_bool(value, "settingsConfigured", pointer, False)
        folder_id = self.loose_string(value, "folderId", pointer)
        if "tracks" not in value:
            raise OracleError(f"missingRequiredField {pointer}/tracks")
        tracks = value["tracks"]
        if not isinstance(tracks, list):
            raise OracleError(f"wrongRequiredType {pointer}/tracks")
        decoded_tracks = [
            self.track(track, f"{pointer}/tracks/{index}")
            for index, track in enumerate(tracks)
        ]
        self.diagnose_duplicates(decoded_tracks, f"{pointer}/tracks")
        return {
            "folderId": folder_id,
            "fps": fps,
            "height": height,
            "id": timeline_id,
            "name": name,
            "settingsConfigured": settings,
            "tracks": decoded_tracks,
            "width": width,
        }

    def normalize(self, source: Any) -> dict[str, Any]:
        if not isinstance(source, dict):
            raise OracleError("wrongRequiredType root")
        if "timelines" in source:
            root_kind = "current"
            if not isinstance(source["timelines"], list):
                raise OracleError("wrongRequiredType /timelines")
            if not source["timelines"]:
                raise OracleError("emptyTimelines /timelines")
            timelines = [
                self.timeline(value, f"/timelines/{index}")
                for index, value in enumerate(source["timelines"])
            ]
            self.diagnose_duplicates(timelines, "/timelines")
            ids = {timeline["id"]["value"] for timeline in timelines}
            active = source.get("activeTimelineId")
            if active is not None and not isinstance(active, str):
                raise OracleError("wrongRequiredType /activeTimelineId")
            if active not in ids:
                if active is not None:
                    self.diagnose("invalidActiveTimelineId", "/activeTimelineId")
                active = timelines[0]["id"]["value"]
            open_ids: list[str] = []
            raw_open = source.get("openTimelineIds")
            if raw_open is not None:
                if not isinstance(raw_open, list) or any(
                    not isinstance(item, str) for item in raw_open
                ):
                    raise OracleError("wrongRequiredType /openTimelineIds")
                for index, item in enumerate(raw_open):
                    if item in ids:
                        open_ids.append(item)
                    else:
                        self.diagnose(
                            "invalidOpenTimelineId", f"/openTimelineIds/{index}"
                        )
            if active not in open_ids:
                open_ids.append(active)
        else:
            root_kind = "legacy"
            timeline = self.timeline(source, "")
            timelines = [timeline]
            active = timeline["id"]["value"]
            open_ids = [active]
        return {
            "contractVersion": 1,
            "diagnostics": self.diagnostics,
            "disposition": "readOnly",
            "project": {
                "activeTimelineId": active,
                "openTimelineIds": open_ids,
                "timelines": timelines,
            },
            "rootKind": root_kind,
        }


def compare_types(left: Any, right: Any, pointer: str = "") -> None:
    if type(left) is not type(right):
        raise OracleError(
            f"type drift at {pointer}: {type(left).__name__} != {type(right).__name__}"
        )
    if isinstance(left, dict):
        if set(left) != set(right):
            raise OracleError(f"key drift at {pointer}")
        for key in left:
            compare_types(left[key], right[key], f"{pointer}/{key}")
    elif isinstance(left, list):
        if len(left) != len(right):
            raise OracleError(f"length drift at {pointer}")
        for index, (left_item, right_item) in enumerate(zip(left, right)):
            compare_types(left_item, right_item, f"{pointer}/{index}")
    elif left != right:
        raise OracleError(f"value drift at {pointer}: {left!r} != {right!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--expected-error")
    args = parser.parse_args()

    source = load(args.input)
    if args.expected_error:
        try:
            Oracle().normalize(source)
        except OracleError as error:
            actual_error = str(error).split(maxsplit=1)[0]
            if actual_error != args.expected_error:
                raise OracleError(
                    f"oracle error drift: {actual_error} != {args.expected_error}"
                ) from error
        else:
            raise OracleError("oracle unexpectedly accepted negative input")
    else:
        expected = Oracle().normalize(source)
    process = subprocess.run(
        [str(args.probe), "--normalize-project-file", str(args.input)],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if args.expected_error:
        if process.returncode != 6:
            raise OracleError(
                f"C++ reader exit drift: {process.returncode}; {process.stderr.strip()}"
            )
        if args.expected_error not in process.stderr:
            raise OracleError(
                f"C++ reader error drift: {process.stderr.strip()}"
            )
        print("PALMIER_PROJECT_ERROR_DIFFERENTIAL_OK")
        return 0
    if process.returncode != 0:
        raise OracleError(f"C++ reader failed: {process.stderr.strip()}")
    actual = json.loads(
        process.stdout,
        object_pairs_hook=reject_duplicate_pairs,
        parse_constant=reject_constant,
    )
    compare_types(expected, actual)
    print("PALMIER_PROJECT_DIFFERENTIAL_OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, json.JSONDecodeError, OracleError) as error:
        print(f"PALMIER_PROJECT_DIFFERENTIAL_FAILED {error}", file=sys.stderr)
        raise SystemExit(1)

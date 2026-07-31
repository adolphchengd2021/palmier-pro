import Foundation

struct JSONCompatibilitySnapshot: Sendable {
    enum DocumentKind: Sendable, Equatable {
        case project
        case media
    }

    private let kind: DocumentKind
    private let original: Data
    private let baseline: Data

    init(kind: DocumentKind, original: Data, baseline: Data) {
        self.kind = kind
        self.original = original
        self.baseline = baseline
    }

    func preservingUnknownFields(in current: Data) throws -> Data {
        let originalObject = try JSONSerialization.jsonObject(with: original)
        let baselineObject = try JSONSerialization.jsonObject(with: baseline)
        let currentObject = try JSONSerialization.jsonObject(with: current)
        let context: MergeContext = kind == .project ? .projectRoot : .mediaRoot
        let merged = try Self.merge(
            current: currentObject,
            original: originalObject,
            baseline: baselineObject,
            context: context
        )
        return try JSONSerialization.data(withJSONObject: merged)
    }

    private static func merge(
        current: Any,
        original: Any,
        baseline: Any,
        context: MergeContext
    ) throws -> Any {
        if let currentObject = current as? [String: Any],
           let originalObject = original as? [String: Any],
           let baselineObject = baseline as? [String: Any] {
            var result = currentObject
            for (key, originalValue) in originalObject {
                if let baselineValue = baselineObject[key] {
                    guard let currentValue = result[key] else { continue }
                    result[key] = try merge(
                        current: currentValue,
                        original: originalValue,
                        baseline: baselineValue,
                        context: context.child(for: key)
                    )
                } else if !context.isKnownKey(key), result[key] == nil {
                    result[key] = originalValue
                }
            }
            return result
        }

        if let currentArray = current as? [Any],
           let originalArray = original as? [Any],
           let baselineArray = baseline as? [Any] {
            let elementContext = context.arrayElement
            if let originalByID = indexedByIdentity(originalArray),
               let baselineByID = indexedByIdentity(baselineArray),
               indexedByIdentity(currentArray) != nil {
                return try currentArray.map { currentValue in
                    guard let identity = identity(of: currentValue),
                          let originalValue = originalByID[identity],
                          let baselineValue = baselineByID[identity] else {
                        return currentValue
                    }
                    return try merge(
                        current: currentValue,
                        original: originalValue,
                        baseline: baselineValue,
                        context: elementContext
                    )
                }
            }

            if try canonicalData(currentArray) == canonicalData(baselineArray) {
                return originalArray
            }
        }

        return current
    }

    private static func indexedByIdentity(_ values: [Any]) -> [String: Any]? {
        guard !values.isEmpty else { return nil }
        var result: [String: Any] = [:]
        for value in values {
            guard let key = identity(of: value), result[key] == nil else { return nil }
            result[key] = value
        }
        return result
    }

    private static func identity(of value: Any) -> String? {
        guard let object = value as? [String: Any] else { return nil }
        if let id = object["id"] {
            if let string = id as? String { return "id-string:\(string)" }
            if let number = id as? NSNumber { return "id-number:\(number.stringValue)" }
        }
        if let frame = object["frame"] as? NSNumber {
            return "frame:\(frame.stringValue)"
        }
        if let start = object["startFrame"] as? NSNumber,
           let end = object["endFrame"] as? NSNumber {
            return "range:\(start.stringValue):\(end.stringValue)"
        }
        return nil
    }

    private static func canonicalData(_ value: Any) throws -> Data {
        try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
    }
}

private indirect enum MergeContext {
    case projectRoot
    case timeline
    case track
    case clip
    case transform
    case effect
    case effectParam
    case mediaRoot
    case mediaEntry
    case generationInput
    case importInput
    case mediaSource
    case projectSource
    case externalSource
    case mediaFolder
    case array(MergeContext)
    case dictionary(MergeContext)
    case generic

    var arrayElement: MergeContext {
        if case .array(let element) = self { return element }
        return .generic
    }

    func child(for key: String) -> MergeContext {
        switch self {
        case .projectRoot:
            if key == "timelines" { return .array(.timeline) }
        case .timeline:
            if key == "tracks" { return .array(.track) }
        case .track:
            if key == "clips" { return .array(.clip) }
        case .clip:
            if key == "transform" { return .transform }
            if key == "effects" { return .array(.effect) }
        case .effect:
            if key == "params" { return .dictionary(.effectParam) }
        case .mediaRoot:
            if key == "entries" { return .array(.mediaEntry) }
            if key == "folders" { return .array(.mediaFolder) }
        case .mediaEntry:
            if key == "source" { return .mediaSource }
            if key == "generationInput" { return .generationInput }
            if key == "importInput" { return .importInput }
        case .mediaSource:
            if key == "project" { return .projectSource }
            if key == "external" { return .externalSource }
        case .array(let element), .dictionary(let element):
            return element
        default:
            break
        }
        return .generic
    }

    func isKnownKey(_ key: String) -> Bool {
        knownKeys?.contains(key) ?? false
    }

    private var knownKeys: Set<String>? {
        switch self {
        case .projectRoot:
            ["timelines", "activeTimelineId", "openTimelineIds", "viewStates", "speakers", "multicamGroups"]
        case .timeline:
            ["id", "name", "fps", "width", "height", "settingsConfigured", "folderId", "tracks"]
        case .track:
            ["id", "type", "muted", "hidden", "syncLocked", "clips", "displayHeight"]
        case .clip:
            [
                "id", "mediaRef", "mediaType", "sourceClipType", "startFrame", "durationFrames",
                "trimStartFrame", "trimEndFrame", "speed", "volume", "fadeInFrames", "fadeOutFrames",
                "fadeInInterpolation", "fadeOutInterpolation", "opacity", "transform", "crop",
                "edgeRounding", "edgeSoftness", "linkGroupId", "captionGroupId", "multicamGroupId",
                "textContent", "textStyle", "textAnimation", "wordTimings", "textFillMode",
                "opacityTrack", "positionTrack", "scaleTrack", "rotationTrack", "cropTrack",
                "volumeTrack", "effects", "blendMode",
            ]
        case .transform:
            ["centerX", "centerY", "width", "height", "rotation", "flipHorizontal", "flipVertical", "x", "y"]
        case .effect:
            ["id", "type", "enabled", "params"]
        case .effectParam:
            ["value", "string", "track"]
        case .mediaRoot:
            ["version", "entries", "folders"]
        case .mediaEntry:
            [
                "id", "name", "type", "source", "duration", "generationInput", "sourceWidth",
                "sourceHeight", "sourceFPS", "hasAudio", "folderId", "cachedRemoteURL",
                "cachedRemoteURLExpiresAt", "generationStatus", "importInput",
            ]
        case .generationInput:
            [
                "prompt", "model", "duration", "aspectRatio", "resolution", "upscaleSettings",
                "upscaleSourceWidth", "upscaleSourceHeight", "upscaleSourceFPS", "quality",
                "imageURLs", "numImages", "voice", "lyrics", "styleInstructions", "instrumental",
                "targetLanguage", "multilingual", "audioInput", "generateAudio",
                "referenceImageURLs", "referenceVideoURLs", "referenceAudioURLs",
                "imageURLAssetIds", "referenceImageAssetIds", "referenceVideoAssetIds",
                "referenceAudioAssetIds", "createdAt", "backendJobId", "outputIndex", "resultURLs",
            ]
        case .importInput:
            ["sourceURL", "sourcePath", "createdAt"]
        case .mediaSource:
            ["external", "project"]
        case .projectSource:
            ["relativePath"]
        case .externalSource:
            ["absolutePath"]
        case .mediaFolder:
            ["id", "name", "parentFolderId"]
        case .array, .dictionary, .generic:
            nil
        }
    }
}

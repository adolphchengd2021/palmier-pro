import Foundation

struct MediaManifest: Codable, Sendable, Equatable, JSONCompatibilityCarrying {
    var version: Int = 2
    var entries: [MediaManifestEntry] = []
    var folders: [MediaFolder] = []
    var compatibilitySnapshot: JSONCompatibilitySnapshot? = nil

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version = try c.decodeIfPresent(Int.self, forKey: .version) ?? 1
        entries = try c.decodeIfPresent([MediaManifestEntry].self, forKey: .entries) ?? []
        folders = try c.decodeIfPresent([MediaFolder].self, forKey: .folders) ?? []
    }

    init() {}

    private enum CodingKeys: String, CodingKey { case version, entries, folders }

    static func decode(_ data: Data) throws -> MediaManifest {
        var manifest = try JSONDecoder().decode(MediaManifest.self, from: data)
        manifest.compatibilitySnapshot = try JSONCompatibilitySnapshot(
            kind: .media,
            original: data,
            baseline: ProjectJSONCodec.encodeKnownFields(manifest)
        )
        return manifest
    }

    static func == (lhs: MediaManifest, rhs: MediaManifest) -> Bool {
        lhs.version == rhs.version
            && lhs.entries == rhs.entries
            && lhs.folders == rhs.folders
    }
}

struct MediaManifestEntry: Codable, Sendable, Equatable, Identifiable {
    let id: String
    var name: String
    var type: ClipType
    var source: MediaSource
    var duration: Double
    var generationInput: GenerationInput?
    var sourceWidth: Int?
    var sourceHeight: Int?
    var sourceFPS: Double?
    var hasAudio: Bool?
    var folderId: String?
    var cachedRemoteURL: String?
    var cachedRemoteURLExpiresAt: Date?
    var generationStatus: String?
    var importInput: MediaImportInput?
}

struct MediaImportInput: Codable, Sendable, Equatable {
    var sourceURL: String? = nil
    var sourcePath: String? = nil
    var createdAt: Date? = nil
}

struct GenerationInput: Codable, Sendable, Equatable {
    var prompt: String
    var model: String
    var duration: Int
    var aspectRatio: String
    var resolution: String?
    var upscaleSettings: UpscaleSettings? = nil
    var upscaleSourceWidth: Int? = nil
    var upscaleSourceHeight: Int? = nil
    var upscaleSourceFPS: Double? = nil
    var quality: String?
    var imageURLs: [String]?
    /// Image-only
    var numImages: Int?
    /// Audio-only
    var voice: String?
    var lyrics: String?
    var styleInstructions: String?
    var instrumental: Bool?
    var targetLanguage: String?
    var multilingual: Bool?
    var audioInput: String?
    /// Video-only
    var generateAudio: Bool?
    var referenceImageURLs: [String]?
    var referenceVideoURLs: [String]?
    var referenceAudioURLs: [String]?

    /// Asset IDs for the references.
    var imageURLAssetIds: [String]?
    var referenceImageAssetIds: [String]?
    var referenceVideoAssetIds: [String]?
    var referenceAudioAssetIds: [String]?
    var createdAt: Date?
    var backendJobId: String?
    var outputIndex: Int?
    var resultURLs: [String]?
}

enum MediaSource: Codable, Sendable, Equatable {
    case external(absolutePath: String)
    case project(relativePath: String)
}

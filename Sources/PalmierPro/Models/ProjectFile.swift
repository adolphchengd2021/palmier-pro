import Foundation

/// Root of project.json. Legacy projects stored a bare Timeline; decode falls back and wraps.
struct ProjectFile: Codable, Sendable, JSONCompatibilityCarrying {
    var timelines: [Timeline]
    var activeTimelineId: String?
    var openTimelineIds: [String]?
    var viewStates: [String: TimelineViewState]?
    var speakers: [SpeakerRegistryEntry]?
    var multicamGroups: [MulticamSource]?
    var compatibilitySnapshot: JSONCompatibilitySnapshot? = nil

    private enum CodingKeys: String, CodingKey {
        case timelines, activeTimelineId, openTimelineIds, viewStates, speakers, multicamGroups
    }

    static func decode(_ data: Data) throws -> ProjectFile {
        let decoder = JSONDecoder()
        let root = try JSONSerialization.jsonObject(with: data) as? [String: Any]
        if root?["timelines"] != nil {
            let file = try decoder.decode(ProjectFile.self, from: data)
            guard !file.timelines.isEmpty else {
                throw DecodingError.dataCorrupted(.init(codingPath: [], debugDescription: "project has no timelines"))
            }
            var result = file
            result.compatibilitySnapshot = try JSONCompatibilitySnapshot(
                kind: .project,
                original: data,
                baseline: ProjectJSONCodec.encodeKnownFields(file)
            )
            return result
        }

        let legacy = try decoder.decode(Timeline.self, from: data)
        var result = ProjectFile(
            timelines: [legacy],
            activeTimelineId: legacy.id,
            openTimelineIds: [legacy.id]
        )
        let legacyObject = try JSONSerialization.jsonObject(with: data)
        let wrappedObject: [String: Any] = [
            "timelines": [legacyObject],
            "activeTimelineId": legacy.id,
            "openTimelineIds": [legacy.id],
        ]
        let wrappedData = try JSONSerialization.data(withJSONObject: wrappedObject)
        result.compatibilitySnapshot = try JSONCompatibilitySnapshot(
            kind: .project,
            original: wrappedData,
            baseline: ProjectJSONCodec.encodeKnownFields(result)
        )
        return result
    }
}

import AppKit
import Foundation
import Testing
@testable import PalmierPro

private struct ContractCanaryError: Error, CustomStringConvertible, Sendable {
    let description: String
}

@Suite("Unknown project field preservation")
struct UnknownFieldRoundTripTests {
    @Test @MainActor
    func declaredCanariesSurviveProductionLoadEditSave() async throws {
        let fixture = Self.fixturePackage("unknown-fields.palmier")
        let loaded = try await Task.detached {
            try VideoProject.readProjectPackage(at: fixture)
        }.value
        let manifest = try #require(loaded.manifest)

        let document = VideoProject()
        document.fileURL = fixture
        document.fileType = VideoProject.typeIdentifier
        let editor = document.editorViewModel
        editor.applyProjectFile(loaded.projectFile)
        editor.projectURL = fixture
        editor.mediaManifest = manifest

        var timeline = editor.timelines[0]
        timeline.name = "Edited canary"
        var clip = timeline.tracks[0].clips[0]
        clip.startFrame = 12
        clip.mediaType = .text
        clip.transform.centerX = 0.625
        if var effects = clip.effects, var parameter = effects[0].params["ev"] {
            parameter.value = 1.25
            effects[0].params["ev"] = parameter
            clip.effects = effects
        }
        timeline.tracks[0].clips[0] = clip
        editor.timelines[0] = timeline

        let entry = editor.mediaManifest.entries[0]
        let asset = MediaAsset(
            entry: entry,
            resolvedURL: fixture.appendingPathComponent("media/canary.mp4")
        )
        asset.name = "Edited media"
        editor.updateManifestMetadata(for: [asset])
        document.updateChangeCount(.changeDone)

        let destination = FileManager.default.temporaryDirectory
            .appendingPathComponent("unknown-roundtrip-\(UUID().uuidString).palmier", isDirectory: true)
        do {
            try await Self.saveAs(document, to: destination)
            let reopened = try await Task.detached {
                try VideoProject.readProjectPackage(at: destination)
            }.value
            #expect(reopened.projectFile.timelines[0].name == "Edited canary")
            #expect(reopened.projectFile.timelines[0].tracks[0].clips[0].startFrame == 12)
            #expect(reopened.projectFile.timelines[0].tracks[0].clips[0].mediaType == .text)
            #expect(reopened.projectFile.timelines[0].tracks[0].clips[0].transform.centerX == 0.625)
            #expect(
                reopened.projectFile.timelines[0].tracks[0].clips[0]
                    .effects?[0].params["ev"]?.value == 1.25
            )
            #expect(reopened.manifest?.entries[0].name == "Edited media")
            try await Self.expectDeclaredCanaries(
                projectData: ProjectJSONCodec.encode(reopened.projectFile),
                mediaData: ProjectJSONCodec.encode(try #require(reopened.manifest))
            )
        } catch {
            try await Self.removeTemporaryPackage(at: destination)
            throw error
        }
        try await Self.removeTemporaryPackage(at: destination)
    }

    @Test
    @concurrent
    func stableIDsKeepUnknownFieldsWithReorderedEntities() async throws {
        let loaded = try VideoProject.readProjectPackage(at: Self.fixturePackage("unknown-fields.palmier"))
        var project = loaded.projectFile
        var manifest = try #require(loaded.manifest)

        var newClip = project.timelines[0].tracks[0].clips[0]
        newClip.id = "clip-new"
        newClip.effects = nil
        newClip.opacityTrack = nil
        newClip.wordTimings = nil
        var newTrack = project.timelines[0].tracks[0]
        newTrack.id = "track-new"
        newTrack.clips = [newClip]
        var originalClip = project.timelines[0].tracks[0].clips[0]
        if var opacityTrack = originalClip.opacityTrack {
            opacityTrack.move(from: 0, to: 5)
            originalClip.opacityTrack = opacityTrack
        }
        originalClip.upsertKeyframe(in: \.opacityTrack, frame: 5, value: 0.5)
        originalClip.upsertKeyframe(in: \.opacityTrack, frame: 15, value: 0.6)
        originalClip.mediaType = .text
        originalClip.durationFrames = 90
        originalClip.rescaleWordTimings(from: 60)
        if var effects = originalClip.effects {
            effects.insert(Effect(id: "effect-new", type: "color.exposure"), at: 0)
            originalClip.effects = effects
        }
        project.timelines[0].tracks[0].clips[0] = originalClip
        var siblingClip = newClip
        siblingClip.id = "clip-sibling"
        project.timelines[0].tracks[0].clips.insert(siblingClip, at: 0)
        project.timelines[0].tracks.insert(newTrack, at: 0)
        var newTimeline = project.timelines[0]
        newTimeline.id = "timeline-new"
        newTimeline.tracks = []
        project.timelines.insert(newTimeline, at: 0)

        let originalEntry = manifest.entries[0]
        let newEntry = MediaManifestEntry(
            id: "media-new",
            name: "New",
            type: originalEntry.type,
            source: originalEntry.source,
            duration: originalEntry.duration
        )
        manifest.entries.insert(newEntry, at: 0)

        let projectRoot = try Self.object(ProjectJSONCodec.encode(project))
        let timelines = try #require(Self.objectArray(projectRoot["timelines"]))
        let timeline = try #require(timelines.first { $0["id"] as? String == "timeline-canary" })
        let insertedTimeline = try #require(timelines.first { $0["id"] as? String == "timeline-new" })
        #expect(timeline["x-contract-timeline"] as? String == "timeline")
        #expect(insertedTimeline["x-contract-timeline"] == nil)
        let tracks = try #require(Self.objectArray(timeline["tracks"]))
        let originalTrack = try #require(tracks.first { $0["id"] as? String == "track-canary-v1" })
        let insertedTrack = try #require(tracks.first { $0["id"] as? String == "track-new" })
        #expect(originalTrack["x-contract-track"] as? Bool == true)
        #expect(insertedTrack["x-contract-track"] == nil)
        let originalTrackClips = try #require(Self.objectArray(originalTrack["clips"]))
        let writtenOriginalClip = try #require(originalTrackClips.first { $0["id"] as? String == "clip-canary-1" })
        let siblingWrittenClip = try #require(originalTrackClips.first { $0["id"] as? String == "clip-sibling" })
        let insertedClip = try #require(Self.objectArray(insertedTrack["clips"])?.first)
        #expect(writtenOriginalClip["x-contract-clip"] != nil)
        #expect(siblingWrittenClip["x-contract-clip"] == nil)
        #expect(insertedClip["x-contract-clip"] == nil)
        let effects = try #require(Self.objectArray(writtenOriginalClip["effects"]))
        let originalEffect = try #require(effects.first { $0["id"] as? String == "effect-canary-1" })
        let insertedEffect = try #require(effects.first { $0["id"] as? String == "effect-new" })
        #expect(originalEffect["x-contract-effect"] as? String == "effect")
        #expect(insertedEffect["x-contract-effect"] == nil)
        let opacityTrack = try #require(writtenOriginalClip["opacityTrack"] as? [String: Any])
        let keyframes = try #require(Self.objectArray(opacityTrack["keyframes"]))
        let originalKeyframe = try #require(keyframes.first { ($0["frame"] as? NSNumber)?.intValue == 5 })
        let insertedKeyframe = try #require(keyframes.first { ($0["frame"] as? NSNumber)?.intValue == 15 })
        #expect(originalKeyframe["x-contract-keyframe"] as? String == "zero")
        #expect(insertedKeyframe["x-contract-keyframe"] == nil)
        let wordTiming = try #require(Self.objectArray(writtenOriginalClip["wordTimings"])?.first)
        #expect((wordTiming["endFrame"] as? NSNumber)?.intValue == 45)
        #expect(wordTiming["x-contract-word-timing"] as? String == "timing")

        let mediaRoot = try Self.object(ProjectJSONCodec.encode(manifest))
        let entries = try #require(Self.objectArray(mediaRoot["entries"]))
        let originalWrittenEntry = try #require(entries.first { $0["id"] as? String == "media-canary-1" })
        let insertedEntry = try #require(entries.first { $0["id"] as? String == "media-new" })
        #expect((originalWrittenEntry["x-contract-media-entry"] as? NSNumber)?.intValue == 99)
        #expect(insertedEntry["x-contract-media-entry"] == nil)
    }

    @Test
    @concurrent
    func replacingOwnersDoesNotReviveTheirUnknownFields() async throws {
        let loaded = try VideoProject.readProjectPackage(at: Self.fixturePackage("unknown-fields.palmier"))
        var project = loaded.projectFile
        var manifest = try #require(loaded.manifest)

        var replacement = project.timelines[0].tracks[0].clips[0]
        replacement.id = "clip-replacement"
        replacement.effects = [Effect(id: "effect-replacement", type: "color.exposure")]
        project.timelines[0].tracks[0].clips = [replacement]
        manifest.entries[0].generationInput = nil
        manifest.entries[0].importInput = nil
        manifest.entries[0].source = .external(absolutePath: "/tmp/replacement.mp4")

        let projectRoot = try Self.object(ProjectJSONCodec.encode(project))
        let timeline = try #require(Self.objectArray(projectRoot["timelines"])?.first)
        let track = try #require(Self.objectArray(timeline["tracks"])?.first)
        let clip = try #require(Self.objectArray(track["clips"])?.first)
        #expect(clip["id"] as? String == "clip-replacement")
        #expect(clip["x-contract-clip"] == nil)
        #expect((clip["transform"] as? [String: Any])?["x-contract-transform"] == nil)
        let effect = try #require(Self.objectArray(clip["effects"])?.first)
        #expect(effect["x-contract-effect"] == nil)

        let mediaRoot = try Self.object(ProjectJSONCodec.encode(manifest))
        let entry = try #require(Self.objectArray(mediaRoot["entries"])?.first)
        #expect(entry["generationInput"] == nil)
        #expect(entry["importInput"] == nil)
        let source = try #require(entry["source"] as? [String: Any])
        #expect(source["project"] == nil)
        #expect(source["external"] != nil)
    }

    @Test
    @concurrent
    func legacyTimelineUnknownFieldsMoveWithTheNormalizedTimeline() async throws {
        let data = Data(#"{"id":"legacy-canary","name":"Legacy","fps":30,"width":1920,"height":1080,"tracks":[],"x-legacy":{"value":true}}"#.utf8)

        let project = try ProjectFile.decode(data)
        let root = try Self.object(ProjectJSONCodec.encode(project))
        let timeline = try #require(Self.objectArray(root["timelines"])?.first)

        #expect(root["x-legacy"] == nil)
        #expect((timeline["x-legacy"] as? [String: Any])?["value"] as? Bool == true)
        #expect(root["activeTimelineId"] as? String == "legacy-canary")
    }

    @Test
    @concurrent
    func malformedCurrentRootNeverFallsBackToLegacyTimeline() async {
        let data = Data(#"{"timelines":[],"id":"legacy-shaped","name":"Invalid","fps":30,"width":1920,"height":1080,"tracks":[]}"#.utf8)

        #expect(throws: DecodingError.self) {
            try ProjectFile.decode(data)
        }
    }

    private static func expectDeclaredCanaries(projectData: Data, mediaData: Data) async throws {
        try await Task.detached {
            let contract = try Self.object(Data(contentsOf: Self.contractRoot
                .appendingPathComponent("project/v1/canaries.json")))
            guard let canaries = Self.objectArray(contract["canaries"]) else {
                throw ContractCanaryError(description: "Canary contract has no canaries array")
            }
            let documents = [
                "project.json": try JSONSerialization.jsonObject(with: projectData),
                "media.json": try JSONSerialization.jsonObject(with: mediaData),
            ]

            for canary in canaries {
                guard let file = canary["file"] as? String,
                      let pointer = canary["pointer"] as? String,
                      let expected = canary["value"],
                      let document = documents[file] else {
                    throw ContractCanaryError(description: "Canary entry is malformed")
                }
                let actual = try Self.value(at: pointer, in: document)
                guard try Self.canonicalJSON(actual) == Self.canonicalJSON(expected) else {
                    throw ContractCanaryError(description: "Canary \(file)\(pointer) changed")
                }
            }
        }.value
    }

    @MainActor
    private static func saveAs(_ document: VideoProject, to url: URL) async throws {
        try await withCheckedThrowingContinuation { continuation in
            document.save(
                to: url,
                ofType: VideoProject.typeIdentifier,
                for: .saveAsOperation
            ) { error in
                if let error {
                    continuation.resume(throwing: error)
                } else {
                    continuation.resume()
                }
            }
        }
    }

    private static func removeTemporaryPackage(at url: URL) async throws {
        try await Task.detached {
            let fileManager = FileManager.default
            guard fileManager.fileExists(atPath: url.path) else { return }
            try fileManager.removeItem(at: url)
        }.value
    }

    private static func value(at pointer: String, in document: Any) throws -> Any {
        var current = document
        for component in pointer.split(separator: "/").map(String.init) {
            let key = component.replacingOccurrences(of: "~1", with: "/")
                .replacingOccurrences(of: "~0", with: "~")
            if let object = current as? [String: Any] {
                guard let next = object[key] else {
                    throw ContractCanaryError(description: "Missing canary path \(pointer)")
                }
                current = next
            } else if let array = current as? [Any], let index = Int(key), array.indices.contains(index) {
                current = array[index]
            } else {
                throw ContractCanaryError(description: "Invalid canary path \(pointer)")
            }
        }
        return current
    }

    private static func object(_ data: Data) throws -> [String: Any] {
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw ContractCanaryError(description: "Expected a JSON object")
        }
        return object
    }

    private static func objectArray(_ value: Any?) -> [[String: Any]]? {
        value as? [[String: Any]]
    }

    private static func canonicalJSON(_ value: Any) throws -> Data {
        try JSONSerialization.data(withJSONObject: [value], options: [.sortedKeys])
    }

    private static func fixturePackage(_ name: String) -> URL {
        fixtureRoot.appendingPathComponent("projects/\(name)", isDirectory: true)
    }

    private static var fixtureRoot: URL {
        repositoryRoot.appendingPathComponent("fixtures/contracts", isDirectory: true)
    }

    private static var contractRoot: URL {
        repositoryRoot.appendingPathComponent("contracts", isDirectory: true)
    }

    private static var repositoryRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
    }
}

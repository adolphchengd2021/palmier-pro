import Foundation
import Testing
@testable import PalmierPro

@Suite("Versioned project contract fixtures")
struct ContractFixtureTests {
    @Test
    @concurrent
    func currentProjectFixtureDecodesThroughProductionCodec() async throws {
        let data = try Self.fixtureData(
            package: "current-multitimeline.palmier",
            file: Project.timelineFilename
        )

        let project = try ProjectFile.decode(data)

        #expect(project.timelines.map(\.id) == ["timeline-main", "timeline-nested"])
        #expect(project.activeTimelineId == "timeline-main")
        #expect(project.openTimelineIds == ["timeline-main", "timeline-nested"])
        let clip = try #require(project.timelines.first?.tracks.first?.clips.first)
        #expect(clip.id == "clip-main-1")
        #expect(clip.startFrame == 0)
        #expect(clip.durationFrames == 150)
        #expect(clip.mediaType == .video)
        #expect(clip.blendMode == .normal)
    }

    @Test
    @concurrent
    func legacyProjectFixtureNormalizesToCurrentRoot() async throws {
        let legacyData = try Self.fixtureData(
            package: "legacy-bare-timeline.palmier",
            file: Project.timelineFilename
        )

        let project = try ProjectFile.decode(legacyData)
        let timeline = try #require(project.timelines.first)
        #expect(project.timelines.count == 1)
        #expect(project.activeTimelineId == timeline.id)
        #expect(project.openTimelineIds == [timeline.id])

        let encoded = try ProjectJSONCodec.encode(project)
        let root = try #require(
            JSONSerialization.jsonObject(with: encoded) as? [String: Any]
        )
        #expect(root["timelines"] != nil)
        #expect(root["fps"] == nil)
        #expect(root["tracks"] == nil)

        let reopened = try ProjectFile.decode(encoded)
        #expect(reopened.timelines.first?.id == timeline.id)
        #expect(reopened.timelines.first?.fps == timeline.fps)
    }

    @Test
    @concurrent
    func completeMediaFixtureDecodesAllKnownFields() async throws {
        let fixture = try Self.fixtureData(
            package: "media-complete.palmier",
            file: Project.manifestFilename
        )
        let manifest = try JSONDecoder().decode(MediaManifest.self, from: fixture)

        #expect(manifest.version == 2)
        #expect(manifest.entries.map(\.id) == [
            "media-external-complete",
            "media-project-complete",
        ])
        #expect(manifest.folders.map(\.id) == ["folder-root", "folder-generated"])
        let external = try #require(manifest.entries.first)
        if case .external(let absolutePath) = external.source {
            #expect(absolutePath == "C:\\Fixture\\external.mp4")
        } else {
            Issue.record("Expected external media source")
        }
        #expect(external.cachedRemoteURLExpiresAt == Date(
            timeIntervalSinceReferenceDate: 123_456_999.25
        ))
        #expect(external.generationInput?.createdAt == Date(
            timeIntervalSinceReferenceDate: 123_456_789.25
        ))
        #expect(external.importInput?.createdAt == Date(
            timeIntervalSinceReferenceDate: 123_456_700.5
        ))

        #expect(try JSONDecoder().decode(
            MediaManifest.self,
            from: ProjectJSONCodec.encode(manifest)
        ) == manifest)
    }

    @Test
    @concurrent
    func sharedMediaWriterMatchesExpectedFixture() async throws {
        let input = try Self.fixtureData(
            package: "media-complete.palmier",
            file: Project.manifestFilename
        )
        let manifest = try JSONDecoder().decode(MediaManifest.self, from: input)
        let encoded = try ProjectJSONCodec.encode(manifest)
        let expected = try Self.mediaFixtureData("swift-writer-complete.json")

        #expect(try Self.canonicalJSON(encoded) == Self.canonicalJSON(expected))
    }

    @Test
    @concurrent
    func productionExporterWritesReopenableContractPackage() async throws {
        let fileManager = FileManager.default
        let projectData = try Self.fixtureData(
            package: "media-complete.palmier",
            file: Project.timelineFilename
        )
        let project = try ProjectFile.decode(projectData)
        let manifestData = try Self.fixtureData(
            package: "media-complete.palmier",
            file: Project.manifestFilename
        )
        let manifest = try JSONDecoder().decode(MediaManifest.self, from: manifestData)
        let root = fileManager.temporaryDirectory
            .appendingPathComponent("contract-writer-\(UUID().uuidString)", isDirectory: true)
        let destination = root.appendingPathComponent("Writer.palmier", isDirectory: true)
        defer { try? fileManager.removeItem(at: root) }

        let report = try PalmierProjectExporter.export(
            projectFile: project,
            manifest: manifest,
            sourceProjectURL: nil,
            to: destination
        )

        #expect(report.missing.map(\.id) == manifest.entries.map(\.id))
        #expect(fileManager.fileExists(
            atPath: destination.appendingPathComponent(Project.timelineFilename).path
        ))
        #expect(fileManager.fileExists(
            atPath: destination.appendingPathComponent(Project.manifestFilename).path
        ))
        var isDirectory = ObjCBool(false)
        #expect(fileManager.fileExists(
            atPath: destination.appendingPathComponent(Project.mediaDirectoryName).path,
            isDirectory: &isDirectory
        ))
        #expect(isDirectory.boolValue)

        let writtenManifest = try Data(
            contentsOf: destination.appendingPathComponent(Project.manifestFilename)
        )
        let writtenProject = try Data(
            contentsOf: destination.appendingPathComponent(Project.timelineFilename)
        )
        #expect(
            try Self.canonicalJSON(writtenManifest)
                == Self.canonicalJSON(Self.mediaFixtureData("swift-writer-complete.json"))
        )
        #expect(
            try Self.canonicalJSON(writtenProject)
                == Self.canonicalJSON(projectData)
        )
        let reopened = try VideoProject.readProjectPackage(at: destination)
        #expect(reopened.manifestUnreadable == false)
        #expect(reopened.projectFile.timelines.map(\.id) == ["timeline-media-complete"])
        #expect(reopened.manifest == manifest)
    }

    private static func fixtureData(package: String, file: String) throws -> Data {
        try Data(contentsOf: fixturePackage(package).appendingPathComponent(file))
    }

    private static func fixturePackage(_ name: String) -> URL {
        contractFixtureRoot
            .appendingPathComponent("projects", isDirectory: true)
            .appendingPathComponent(name, isDirectory: true)
    }

    private static func mediaFixtureData(_ name: String) throws -> Data {
        try Data(contentsOf: contractFixtureRoot
            .appendingPathComponent("media/v1", isDirectory: true)
            .appendingPathComponent(name))
    }

    private static var contractFixtureRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .appendingPathComponent("fixtures/contracts", isDirectory: true)
    }

    private static func canonicalJSON(_ data: Data) throws -> String {
        let object = try JSONSerialization.jsonObject(with: data)
        let normalized = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        return String(decoding: normalized, as: UTF8.self)
    }
}

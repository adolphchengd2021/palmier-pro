import Foundation

struct WordTiming: Codable, Sendable, Equatable {
    var text: String
    var startFrame: Int
    var endFrame: Int
    var unknownFields: [String: OpaqueJSONValue] = [:]

    init(
        text: String,
        startFrame: Int,
        endFrame: Int,
        unknownFields: [String: OpaqueJSONValue] = [:]
    ) {
        self.text = text
        self.startFrame = startFrame
        self.endFrame = endFrame
        self.unknownFields = unknownFields
    }

    private enum CodingKeys: String, CodingKey, CaseIterable {
        case text, startFrame, endFrame
    }

    init(from decoder: Decoder) throws {
        let known = try decoder.container(keyedBy: CodingKeys.self)
        text = try known.decode(String.self, forKey: .text)
        startFrame = try known.decode(Int.self, forKey: .startFrame)
        endFrame = try known.decode(Int.self, forKey: .endFrame)

        let dynamic = try decoder.container(keyedBy: JSONFieldKey.self)
        let knownNames = Set(CodingKeys.allCases.map(\.rawValue))
        unknownFields = try dynamic.allKeys.reduce(into: [:]) { result, key in
            guard !knownNames.contains(key.stringValue) else { return }
            result[key.stringValue] = try dynamic.decode(OpaqueJSONValue.self, forKey: key)
        }
    }

    func encode(to encoder: Encoder) throws {
        var known = encoder.container(keyedBy: CodingKeys.self)
        try known.encode(text, forKey: .text)
        try known.encode(startFrame, forKey: .startFrame)
        try known.encode(endFrame, forKey: .endFrame)

        var dynamic = encoder.container(keyedBy: JSONFieldKey.self)
        for (key, value) in unknownFields {
            try dynamic.encode(value, forKey: JSONFieldKey(key))
        }
    }
}

struct TextAnimation: Codable, Sendable, Equatable {
    var preset: Preset = .none
    var perWordFrames: Int = 6
    var highlight: TextStyle.RGBA?

    enum Preset: String, Codable, CaseIterable, Sendable {
        case none
        // Whole-clip / per-line.
        case fadeIn, popIn, slideUp, typewriter
        // Per word.
        case wordReveal, wordSlide, wordPop, wordCycle, highlightPop, highlightBlock

        enum RenderMode { case entrance, perWord, typewriter }

        var renderMode: RenderMode {
            switch self {
            case .none, .fadeIn, .popIn, .slideUp: .entrance
            case .typewriter: .typewriter
            case .wordReveal, .wordSlide, .wordPop, .wordCycle,
                 .highlightPop, .highlightBlock: .perWord
            }
        }

        var isPerWord: Bool { renderMode == .perWord }
        var usesHighlight: Bool { isPerWord }

        var displayName: String {
            switch self {
            case .none: "Off"
            case .fadeIn: "Fade In"
            case .popIn: "Pop In"
            case .slideUp: "Slide Up"
            case .typewriter: "Typewriter"
            case .wordReveal: "Word Reveal"
            case .wordSlide: "Word Slide"
            case .wordPop: "Word Pop"
            case .wordCycle: "Word Cycle"
            case .highlightPop: "Highlight"
            case .highlightBlock: "Highlight Block"
            }
        }

        static let agentValues: [String] = ["off"] + allCases.filter { $0 != .none }.map(\.rawValue)

        static let perLine: [Preset] = [.fadeIn, .popIn, .slideUp, .typewriter]
        static let perWord: [Preset] = [.wordReveal, .wordSlide, .wordPop, .wordCycle,
                                        .highlightPop, .highlightBlock]
    }

    var isActive: Bool { preset != .none }

    static let defaultHighlight = TextStyle.RGBA(r: 1, g: 0.85, b: 0, a: 1)

    private enum CodingKeys: String, CodingKey { case preset, perWordFrames, highlight }

    init(preset: Preset = .none, perWordFrames: Int = 6, highlight: TextStyle.RGBA? = nil) {
        self.preset = preset
        self.perWordFrames = perWordFrames
        self.highlight = highlight
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.init(
            preset: (try? c.decode(Preset.self, forKey: .preset)) ?? .none,
            perWordFrames: (try? c.decode(Int.self, forKey: .perWordFrames)) ?? 6,
            highlight: try? c.decode(TextStyle.RGBA.self, forKey: .highlight)
        )
    }
}

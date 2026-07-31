import Foundation

protocol JSONCompatibilityCarrying {
    var compatibilitySnapshot: JSONCompatibilitySnapshot? { get }
}

enum ProjectJSONCodec {
    static func encode<T: Encodable>(_ value: T) throws -> Data {
        let data = try encodeKnownFields(value)
        guard let carrier = value as? any JSONCompatibilityCarrying,
              let snapshot = carrier.compatibilitySnapshot else {
            return data
        }
        return try snapshot.preservingUnknownFields(in: data)
    }

    static func encodeKnownFields<T: Encodable>(_ value: T) throws -> Data {
        try JSONEncoder().encode(value)
    }
}

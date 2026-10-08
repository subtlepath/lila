import Foundation

public struct CloudCourseAssociation: Codable, Equatable, Sendable {
    public let version: Int
    public let content: ContentID
    public let identity: Data
    public init(content: ContentID, identity: Data) throws {
        version = 1; self.content = content; self.identity = identity
        try validate()
    }
    public func validate() throws {
        guard version == 1, identity.count == 16, identity.contains(where: { $0 != 0 }) else {
            throw StoreError.invalidValue
        }
    }
}

import Foundation

public struct CloudJournalRecord: Equatable, Sendable {
    public static let recordType = "LilaJournalV1"
    public static let zoneName = "LilaJournalV1"
    public let name: String
    public let mutation: JournalMutation
    public init(_ mutation: JournalMutation) {
        self.mutation = mutation
        name = mutation.event.identity.storageKey.map { String(format: "%02x", $0) }.joined()
    }
    public init(name: String, envelope: Data, body: Data) throws {
        let mutation = try JournalMutation(event: SyncEvent(decoding: envelope), body: body)
        self.init(mutation)
        guard self.name == name else { throw StoreError.invalidValue }
    }
}

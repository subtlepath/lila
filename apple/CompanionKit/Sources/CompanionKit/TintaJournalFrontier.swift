import Foundation
#if canImport(CryptoKit)
import CryptoKit
#else
import Crypto
#endif

public enum TintaJournalFrontier {
    /// Binds the complete authoritative journal, including undone reviews.
    public static func digest(_ deliveries: [JournalMutation]) throws -> Data {
        for mutation in deliveries where mutation.event.kind.rawValue >= SyncEventKind.review.rawValue {
            _ = try TintaBody(mutation: mutation)
        }
        _ = try TintaHistory.replay(deliveries)
        let events = try SyncHistory.merged(deliveries.map(\.event)).sorted { $0.identity < $1.identity }
        var prefix = Data("TJF1".utf8)
        prefix.appendLittleEndian(UInt64(events.count), count: 8)
        var hash = SHA256()
        hash.update(data: prefix)
        for event in events {
            var length = Data()
            length.appendLittleEndian(UInt64(event.bytes.count), count: 2)
            hash.update(data: length)
            hash.update(data: event.bytes)
        }
        return Data(hash.finalize())
    }
}

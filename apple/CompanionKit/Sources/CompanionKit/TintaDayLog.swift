import Foundation

public enum TintaDayLog {
    public static func encode(_ days: [UInt16: StudyTotals]) throws -> Data {
        var validated: [(UInt16, TintaDayTotals)] = []
        validated.reserveCapacity(days.count)
        var recordCount = 0
        for day in days.keys.sorted() {
            let totals = try days[day]!.readerDayTotals()
            let largest = max(totals.reviews, totals.correct, totals.newItems, totals.seconds)
            let count = max(1, Int((UInt64(largest) + 65534) / 65535))
            let (next, overflow) = recordCount.addingReportingOverflow(count)
            guard !overflow else { throw ProtocolError.value }
            recordCount = next
            validated.append((day, totals))
        }
        let (recordBytes, overflow) = recordCount.multipliedReportingOverflow(by: 12)
        let (capacity, headerOverflow) = recordBytes.addingReportingOverflow(4)
        guard !overflow, !headerOverflow else { throw ProtocolError.value }
        var output = Data("TDL1".utf8)
        output.reserveCapacity(capacity)
        for (day, totals) in validated {
            var remaining = [totals.reviews, totals.correct, totals.newItems, totals.seconds]
            repeat {
                var record = Data()
                record.reserveCapacity(12)
                record.appendLittleEndian(UInt64(day), count: 2)
                for index in remaining.indices {
                    let part = min(remaining[index], 65535)
                    record.appendLittleEndian(UInt64(part), count: 2)
                    remaining[index] -= part
                }
                record.appendLittleEndian(UInt64(legacyCRC32(record) & 0xffff), count: 2)
                output.append(record)
            } while remaining.contains(where: { $0 != 0 })
        }
        return output
    }
}

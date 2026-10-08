import Foundation

public enum InventoryCollectorError: Error, Equatable, Sendable {
    case busy, invalidResponse, control(UInt8), remote(UInt8), requestIDsExhausted
    public var requiresReaderReopen: Bool { self == .remote(4) }
}
public actor InventoryCollector {
    private var busy = false
    private var requestID: UInt32 = 0
    public init() {}
    public func collect(session: AuthenticatedReaderSession, maximumEntries: Int) async throws -> ReaderInventory {
        try await collect(device: session.device, transport: session, maximumEntries: maximumEntries)
    }
    func collect(device: DeviceDescriptor, transport: any CompanionTransport, maximumEntries: Int) async throws -> ReaderInventory {
        guard !busy else { throw InventoryCollectorError.busy }
        busy = true
        defer { busy = false }
        var scan = try InventoryScan(reader: device.identity, generation: device.storageGeneration, maximumEntries: maximumEntries)
        while !scan.complete {
            try Task.checkCancellation()
            guard requestID < UInt32.max else { throw InventoryCollectorError.requestIDsExhausted }
            requestID += 1
            let request = try scan.nextRequest(requestID: requestID)
            let response = try await transport.exchange(request)
            guard response.response, response.requestID == request.requestID else { throw InventoryCollectorError.invalidResponse }
            if response.command == .error {
                guard response.payload.count == 1, let code = response.payload.first else { throw InventoryCollectorError.invalidResponse }
                throw InventoryCollectorError.control(code)
            }
            guard response.command == .inventory, let result = response.payload.first else { throw InventoryCollectorError.invalidResponse }
            if result != 0 {
                guard response.payload.count == 1 else { throw InventoryCollectorError.invalidResponse }
                throw InventoryCollectorError.remote(result)
            }
            try scan.append(InventoryPage(decoding: Data(response.payload.dropFirst())))
        }
        try Task.checkCancellation()
        return try scan.inventory()
    }
}

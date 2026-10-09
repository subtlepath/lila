import Foundation
import Dispatch

public enum WifiHandoffTransportError: Error, Equatable, Sendable {
    case binding, expired, invalidated, busy, invalidRequest, invalidResponse
}

// Implementations must bound response collection and honor timeout/cancellation.
public protocol WifiMessageTransport: Sendable {
    func exchange(_ message: Data, timeoutNanoseconds: UInt64) async throws -> Data
    func close() async
}

public actor WifiHandoffTransport: CompanionTransport {
    private static let idleNanoseconds: UInt64 = 30_000_000_000
    private let wire: any WifiMessageTransport
    private let cipher: WifiMessageCipher
    private let transaction: Data
    private let session: Data
    private let reader: Data
    private let installation: Data
    private let generation: Data
    private let lifetimeNanoseconds: UInt64
    private let now: @Sendable () -> UInt64
    private var lastActivity: UInt64
    private var activated = false
    private var active = true
    private var busy = false

    // receivedAt and now use the same local monotonic clock, never reader wall time.
    public init(offer: WifiHandoffOffer, reader: Data, storageGeneration: Data, installation: Data,
                transaction: Data, receivedAt: UInt64, wire: any WifiMessageTransport,
                now: @escaping @Sendable () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }) throws {
        guard offer.matches(reader: reader, storageGeneration: storageGeneration, installation: installation,
                            transaction: transaction) else { throw WifiHandoffTransportError.binding }
        cipher = try WifiMessageCipher(key: offer.key, session: offer.session, sending: .appleToReader)
        self.wire = wire; self.transaction = transaction; self.installation = installation
        session = offer.session
        self.reader = reader
        generation = storageGeneration
        lifetimeNanoseconds = UInt64(offer.lifetimeSeconds) * 1_000_000_000
        lastActivity = receivedAt
        self.now = now
    }

    public func close() async {
        active = false
        await cipher.invalidate()
        await wire.close()
    }

    public func finish(requestID: UInt32) async throws {
        let request = try WifiHandoffCommands.cancel(transaction: transaction, session: session, requestID: requestID)
        do {
            let reply = try await exchange(request)
            try WifiHandoffCommands.acknowledgement(reply, to: request)
            await close()
        } catch {
            await close()
            throw error
        }
    }

    public func matches(reader: Data, storageGeneration: Data, installation: Data, transaction: Data) -> Bool {
        active && self.reader == reader && generation == storageGeneration &&
        self.installation == installation && self.transaction == transaction
    }

    public func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        guard active else { throw WifiHandoffTransportError.invalidated }
        guard !busy else { throw WifiHandoffTransportError.busy }
        busy = true
        defer { busy = false }
        do {
            try Task.checkCancellation()
            guard let timeBudget = remaining() else { throw WifiHandoffTransportError.expired }
            try validate(request)
            let encrypted = try await cipher.seal(request.encoded())
            let reply = try await wire.exchange(encrypted, timeoutNanoseconds: min(timeBudget, Self.idleNanoseconds))
            try Task.checkCancellation()
            guard active else { throw WifiHandoffTransportError.invalidated }
            guard remaining() != nil else { throw WifiHandoffTransportError.expired }
            let decoded = try ControlFrame(decoding: await cipher.open(reply), authenticated: true)
            guard active else { throw WifiHandoffTransportError.invalidated }
            guard remaining() != nil else { throw WifiHandoffTransportError.expired }
            guard decoded.response, decoded.requestID == request.requestID,
                  decoded.command == request.command || decoded.command == .error else {
                throw WifiHandoffTransportError.invalidResponse
            }
            activated = true
            lastActivity = now()
            return decoded
        } catch {
            await close()
            throw error
        }
    }

    private func remaining() -> UInt64? {
        let instant = now()
        let budget = activated ? Self.idleNanoseconds : lifetimeNanoseconds
        guard instant >= lastActivity, instant - lastActivity < budget else { return nil }
        return budget - (instant - lastActivity)
    }

    public func readContent(_ request: ReaderContentReadRequest, requestID: UInt32) async throws -> ReaderContentReadReply {
        do {
            let frame = try ControlFrame(command: .readContent, requestID: requestID, payload: transaction + request.encoded)
            let response = try await exchange(frame)
            guard response.command == .readContent else { throw WifiHandoffTransportError.invalidResponse }
            return try ReaderContentReadReply(decoding: response.payload, request: request)
        } catch {
            await close()
            throw error
        }
    }
    public func journalHeaderVersions(requestID: UInt32) async throws -> [UInt8] {
        let request = try ControlFrame(command: .journalFormats, requestID: requestID, payload: transaction)
        return try JournalFormatStatus.decode(await exchange(request), requestID: requestID)
    }

    public func journalExportPage(cursor: JournalExportCursor = .start, requestID: UInt32) async throws -> JournalExportPage {
        let request = try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: cursor.payload)
        return try JournalExportPage.decode(await exchangeJournalExport(request), requestID: requestID, requested: cursor)
    }

    func exchangeJournalExport(_ request: ControlFrame) async throws -> ControlFrame {
        guard !request.response, request.command == .exchangeChanges, request.payload.count == 40 else {
            throw WifiHandoffTransportError.invalidRequest
        }
        let bound = try ControlFrame(command: request.command, requestID: request.requestID, payload: transaction + request.payload)
        return try await exchange(bound)
    }

    public func journalState(requestID: UInt32) async throws -> JournalMergeSnapshot {
        let request = try JournalState.request(generation: generation, requestID: requestID)
        let bound = try ControlFrame(command: request.command, requestID: requestID, payload: transaction + request.payload)
        return try JournalState.decode(await exchange(bound), request: request)
    }

    public func journalMergeReadiness(snapshot: JournalMergeSnapshot, requestID: UInt32) async throws -> JournalMergeReadiness {
        let request = try JournalMergeReadiness.request(generation: generation, snapshot: snapshot, requestID: requestID)
        let bound = try ControlFrame(command: request.command, requestID: requestID, payload: transaction + request.payload)
        return try JournalMergeReadiness.decode(await exchange(bound), request: request)
    }

    func journalReaderBinding() -> (Data, Data) { (reader, generation) }

    public func exchangeJournalMerge(_ operation: JournalMergeRequest, requestID: UInt32) async throws -> JournalMergeReply {
        let request = try operation.frame(requestID: requestID)
        let bound = try ControlFrame(command: request.command, requestID: requestID, payload: transaction + request.payload)
        return try JournalMergeReply.decode(await exchange(bound), request: request)
    }

    func journalMergeBinding() -> (Data, Data, Data, Data) { (reader, generation, installation, transaction) }

    func exchangeJournalMergeControl(_ request: ControlFrame) async throws -> ControlFrame {
        guard !request.response, request.command == .exchangeChanges else { throw WifiHandoffTransportError.invalidRequest }
        let merge = try JournalMergeRequest.decodePayload(request.payload)
        guard merge.transaction == transaction else { throw WifiHandoffTransportError.binding }
        switch merge {
        case .begin(let declaration), .commit(let declaration), .abort(let declaration):
            guard declaration.owner == installation, declaration.generation == generation else {
                throw WifiHandoffTransportError.binding
            }
        case .append: break
        }
        let bound = try ControlFrame(command: request.command, requestID: request.requestID, payload: transaction + request.payload)
        return try await exchange(bound)
    }

    func tintaMigrationBinding() -> (Data, Data, Data, Data) { (reader, generation, installation, transaction) }

    func exchangeTintaMigrationControl(_ request: ControlFrame) async throws -> ControlFrame {
        guard !request.response, request.command == .exchangeChanges else { throw WifiHandoffTransportError.invalidRequest }
        if request.payload.count == TintaMigrationAdmission.byteCount && request.payload.prefix(3) == Data([0x54, 0x4d, 0x41]) {
            let admission = try TintaMigrationAdmission(decoding: request.payload)
            guard admission.reader == reader, admission.merge.owner == installation,
                  admission.merge.generation == generation, admission.merge.transaction == transaction else {
                throw WifiHandoffTransportError.binding
            }
        } else {
            let merge = try JournalMergeRequest.decodePayload(request.payload)
            guard merge.transaction == transaction else { throw WifiHandoffTransportError.binding }
        }
        let bound = try ControlFrame(command: request.command, requestID: request.requestID, payload: transaction + request.payload)
        return try await exchange(bound)
    }

    public func admitTintaMigration(_ admission: TintaMigrationAdmission, requestID: UInt32) async throws -> TintaMigrationAdmissionReply {
        guard admission.reader == reader, admission.merge.owner == installation,
              admission.merge.generation == generation, admission.merge.transaction == transaction else {
            throw WifiHandoffTransportError.binding
        }
        let request = try admission.frame(requestID: requestID)
        let bound = try ControlFrame(command: request.command, requestID: requestID, payload: transaction + request.payload)
        return try TintaMigrationAdmissionReply.decode(await exchange(bound), request: request)
    }

    public func authorizeCourseSwitch(_ consent: CourseSwitchRequest, requestID: UInt32) async throws {
        guard consent.transaction == transaction, consent.generation == generation else {
            throw WifiHandoffTransportError.binding
        }
        try Task.checkCancellation()
        let request = try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: consent.bytes)
        let bound = try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: transaction + consent.bytes)
        try CourseSwitchReply.validate(await exchange(bound), request: request)
    }

    public func exchangeLegacyBackup(_ operation: LegacyBackupRequest, requestID: UInt32) async throws -> LegacyBackupReply {
        let request = try ControlFrame(command: .exchangeChanges, requestID: requestID, payload: operation.bytes)
        return try LegacyBackupReply.decode(await exchangeLegacyBackup(request), request: request)
    }
    func legacyBackupBinding() -> (Data, Data, Data) { (reader, generation, transaction) }
    func exchangeLegacyBackup(_ request: ControlFrame) async throws -> ControlFrame {
        guard !request.response, request.command == .exchangeChanges else { throw WifiHandoffTransportError.invalidRequest }
        let operation = try LegacyBackupRequest(decoding: request.payload)
        guard operation.transaction == transaction, operation.generation == generation else {
            throw WifiHandoffTransportError.binding
        }
        let bound = try ControlFrame(command: .exchangeChanges, requestID: request.requestID, payload: transaction + request.payload)
        let reply = try await exchange(bound)
        _ = try LegacyBackupReply.decode(reply, request: request)
        return reply
    }

    private func validate(_ request: ControlFrame) throws {
        guard !request.response else { throw WifiHandoffTransportError.invalidRequest }
        switch request.command {
        case .wifiHandoff:
            let expected = try WifiHandoffCommands.cancel(transaction: transaction, session: session, requestID: request.requestID)
            guard request.payload == expected.payload else { throw WifiHandoffTransportError.binding }
        case .beginTransfer:
            let declared = request.payload.prefix(2) == Data([1, 2])
            let size = declared ? TransferDeclaration.encodedSize : 99
            guard request.payload.count > size,
                  request.payload.count == size + 1 + Int(request.payload[size]) else {
                throw WifiHandoffTransportError.invalidRequest
            }
            let state = try declared ? TransferDeclaration(decoding: Data(request.payload.prefix(size))).state
                                     : TransferState(decoding: Data(request.payload.prefix(size)))
            guard state.transaction == transaction, state.owner == installation, state.storageGeneration == generation else {
                throw WifiHandoffTransportError.binding
            }
        case .readContent:
            guard request.payload.count == 16 + ReaderContentReadRequest.encodedSize,
                  request.payload.prefix(16) == transaction else { throw WifiHandoffTransportError.binding }
            let read = try ReaderContentReadRequest(decoding: Data(request.payload.dropFirst(16)))
            guard read.generation == generation else { throw WifiHandoffTransportError.binding }
        case .transferChunk:
            guard request.payload.count > 24, request.payload.prefix(16) == transaction else {
                throw WifiHandoffTransportError.binding
            }
        case .transferStatus, .commit, .abort, .journalFormats:
            guard request.payload.count == 16, request.payload == transaction else { throw WifiHandoffTransportError.binding }
        case .exchangeChanges:
            guard request.payload.count >= 16, request.payload.prefix(16) == transaction else { throw WifiHandoffTransportError.binding }
            if request.payload.count == 16 + CourseSwitchRequest.encodedSize {
                let consent = try CourseSwitchRequest(decoding: Data(request.payload.dropFirst(16)))
                guard consent.transaction == transaction, consent.generation == generation else {
                    throw WifiHandoffTransportError.binding
                }
            } else if request.payload.count == 16 + JournalMergeReadiness.requestSize {
                do { try JournalMergeReadiness.validateRequest(Data(request.payload.dropFirst(16)), generation: generation) }
                catch { throw WifiHandoffTransportError.binding }
            } else if request.payload.count == 16 + JournalState.requestSize {
                do { try JournalState.validateRequest(Data(request.payload.dropFirst(16)), generation: generation) }
                catch { throw WifiHandoffTransportError.binding }
            } else if request.payload.count == 80 {
                let backup = try LegacyBackupRequest(decoding: Data(request.payload.dropFirst(16)))
                guard backup.transaction == transaction, backup.generation == generation else {
                    throw WifiHandoffTransportError.binding
                }
            } else if request.payload.count == 16 + TintaMigrationAdmission.byteCount,
                      request.payload.dropFirst(16).prefix(3) == Data([0x54, 0x4d, 0x41]) {
                let admission = try TintaMigrationAdmission(decoding: Data(request.payload.dropFirst(16)))
                guard admission.reader == reader, admission.merge.transaction == transaction,
                      admission.merge.owner == installation, admission.merge.generation == generation else {
                    throw WifiHandoffTransportError.binding
                }
            } else if request.payload.count != 56 {
                let merge = try JournalMergeRequest.decodePayload(Data(request.payload.dropFirst(16)))
                guard merge.transaction == transaction else { throw WifiHandoffTransportError.binding }
                switch merge {
                case .begin(let declaration), .commit(let declaration), .abort(let declaration):
                    guard declaration.owner == installation, declaration.generation == generation else {
                        throw WifiHandoffTransportError.binding
                    }
                case .append: break
                }
            }
        default: throw WifiHandoffTransportError.invalidRequest
        }
    }
}

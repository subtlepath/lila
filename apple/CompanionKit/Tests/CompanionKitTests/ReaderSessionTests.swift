import Foundation
import XCTest
@testable import CompanionKit

private final class SessionStorage: CredentialStorage, @unchecked Sendable {
    private let lock = NSLock()
    private var records: [String: Data] = [:]
    func load(_ account: String) throws -> Data? { lock.lock(); defer { lock.unlock() }; return records[account] }
    func insert(_ account: String, data: Data) throws -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard records[account] == nil else { return false }
        records[account] = data; return true
    }
    func remove(_ account: String) throws { lock.lock(); defer { lock.unlock() }; records.removeValue(forKey: account) }
}
private actor SessionReader: SessionTransport {
    var connection: UInt64 = 1
    var reconnectOnAuthorization = false
    func sessionIdentity() async throws -> UInt64 { connection }
    func reconnect() { connection += 1 }
    func reconnectAfterAuthorization() { reconnectOnAuthorization = true }
    func exchange(_ request: ControlFrame, connection: UInt64) async throws -> ControlFrame {
        guard connection == self.connection else { throw ReaderSessionError.staleConnection }
        return try await exchange(request)
    }
    var credential: Data?
    var registrations = 0
    var loseRegistration = false
    var badReply = false
    var journalExport = false
    var exportPayload = Data()
    var exportRequests = 0
    var rejectExport = false
    init(loseRegistration: Bool = false, badReply: Bool = false, journalExport: Bool = false) {
        self.loseRegistration = loseRegistration; self.badReply = badReply
        self.journalExport = journalExport
    }
    func configureExport(_ payload: Data, reject: Bool = false) { exportPayload = payload; rejectExport = reject }
    func exportCount() -> Int { exportRequests }
    func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        var command = request.command
        var payload = Data([0])
        switch request.command {
        case .discover:
            payload = Data([1, 1]) + Data(repeating: 1, count: 16) + Data(repeating: 2, count: 16)
            payload.append(contentsOf: [1, journalExport ? 8 : 0, 0, 0, 0, 80, 1, 1]); payload.append(Data(count: 32))
        case .exchangeChanges:
            exportRequests += 1
            if request.payload.count == 64 { _ = try LegacyBackupRequest(decoding: request.payload) }
            else if request.payload.count != 40 { throw ProtocolError.length }
            payload = exportPayload
            if rejectExport { command = .error; payload = Data([1]) }
        case .inventory:
            payload = Data([0, 1]); payload.append(Data(repeating: 2, count: 16))
            payload.appendLittleEndian(1, count: 8); payload.appendLittleEndian(0, count: 8)
            payload.appendLittleEndian(0, count: 8); payload.append(contentsOf: [1, 0])
        case .authenticateInstallation:
            if credential != request.payload { command = .error; payload = Data([2]) }
        case .registerInstallation:
            registrations += 1
            guard credential == nil else { return try ControlFrame(command: .error, response: true, requestID: request.requestID, payload: Data([2])) }
            credential = request.payload
            if loseRegistration { loseRegistration = false; throw URLError(.networkConnectionLost) }
        default: throw ProtocolError.command
        }
        if reconnectOnAuthorization, command == .registerInstallation || command == .authenticateInstallation {
            reconnectOnAuthorization = false; connection += 1
        }
        return try ControlFrame(command: command, response: true, requestID: request.requestID + (badReply ? 1 : 0), payload: payload)
    }
    func count() -> Int { registrations }
}
final class ReaderSessionTests: XCTestCase, @unchecked Sendable {
    func testAuthenticatedJournalExportChecksCapabilityAndConnection() async throws {
        let (_, _, coordinator) = setup()
        let reader = SessionReader(journalExport: true)
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let fixture = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/JournalExportPage-v1.fixture"))
        await reader.configureExport(fixture)
        let session = try await coordinator.pair(transport: reader)
        let page = try await session.journalExportPage(requestID: 42)
        XCTAssertEqual(page.mutation?.event.kind, .star)
        XCTAssertEqual(page.cursor.next, 1)
        await reader.configureExport(fixture, reject: true)
        do { _ = try await session.journalExportPage(requestID: 43); XCTFail() }
        catch { XCTAssertEqual(error as? ReaderSessionError, .control(1)) }
        await reader.reconnect()
        do { _ = try await session.journalExportPage(requestID: 44); XCTFail() }
        catch { XCTAssertEqual(error as? ReaderSessionError, .staleConnection) }
        let sent = await reader.exportCount(); XCTAssertEqual(sent, 2)
        let (_, _, otherCoordinator) = setup()
        let unsupportedReader = SessionReader()
        let unsupported = try await otherCoordinator.pair(transport: unsupportedReader)
        do { _ = try await unsupported.journalExportPage(requestID: 45); XCTFail() }
        catch { XCTAssertEqual(error as? ReaderSessionError, .unsupportedProtocol) }
        let unsupportedSent = await unsupportedReader.exportCount(); XCTAssertEqual(unsupportedSent, 0)
    }
    private func setup() -> (SessionStorage, PairingVault, ReaderSession) {
        let storage = SessionStorage()
        let vault = PairingVault(storage: storage, random: { Data(repeating: 7, count: $0) })
        return (storage, vault, ReaderSession(credentials: vault))
    }
    func testAuthenticatedInventoryCannotCrossReconnect() async throws {
        let (_, _, session) = setup(); let reader = SessionReader()
        let authenticated = try await session.pair(transport: reader)
        let collector = InventoryCollector()
        let inventory = try await collector.collect(session: authenticated, maximumEntries: 0)
        XCTAssertTrue(inventory.complete); XCTAssertTrue(inventory.contents.isEmpty)
        await reader.reconnect()
        do { _ = try await collector.collect(session: authenticated, maximumEntries: 0); XCTFail("Inventory crossed reconnect") }
        catch { XCTAssertEqual(error as? ReaderSessionError, .staleConnection) }
    }
    func testPairThenAuthenticateWithoutRegistration() async throws {
        let (_, vault, session) = setup(); let reader = SessionReader()
        let paired = try await session.pair(transport: reader)
        XCTAssertEqual(paired.device.identity, Data(repeating: 1, count: 16))
        let authenticated = try await session.authenticate(transport: reader, expectedReader: paired.device.identity)
        XCTAssertEqual(authenticated.installation, paired.installation)
        let stored = try await vault.credential(for: paired.device.identity)
        XCTAssertEqual(stored?.installation, paired.installation)
        let count = await reader.count(); XCTAssertEqual(count, 1)
    }
    func testAutomaticAuthenticationDoesNotCreateCredentials() async throws {
        let (storage, _, session) = setup(); let reader = SessionReader()
        do { _ = try await session.authenticate(transport: reader); XCTFail("Missing pairing accepted") }
        catch { XCTAssertEqual(error as? ReaderSessionError, .pairingRequired) }
        XCTAssertNil(try storage.load("installation"))
        let count = await reader.count(); XCTAssertEqual(count, 0)
    }
    func testLostRegistrationResponseReusesStoredCredential() async throws {
        let (_, vault, session) = setup(); let reader = SessionReader(loseRegistration: true)
        do { _ = try await session.pair(transport: reader); XCTFail("Lost response accepted") } catch is URLError {}
        let stored = try await vault.credential(for: Data(repeating: 1, count: 16))
        XCTAssertNotNil(stored)
        let paired = try await session.pair(transport: reader)
        XCTAssertEqual(paired.installation, stored?.installation)
        let count = await reader.count(); XCTAssertEqual(count, 1)
    }
    func testForeignIdentityAndMismatchedResponseFailBeforeCredentialCreation() async throws {
        let (storage, _, session) = setup()
        do { _ = try await session.pair(transport: SessionReader(), expectedReader: Data(repeating: 9, count: 16)); XCTFail() }
        catch { XCTAssertEqual(error as? ReaderSessionError, .wrongReader) }
        do { _ = try await session.pair(transport: SessionReader(badReply: true)); XCTFail() }
        catch { XCTAssertEqual(error as? ReaderSessionError, .invalidResponse) }
        XCTAssertNil(try storage.load("installation"))
    }
    func testAuthenticatedSessionCannotSurviveReconnect() async throws {
        let (_, _, coordinator) = setup(); let reader = SessionReader()
        let session = try await coordinator.pair(transport: reader)
        await reader.reconnect()
        do { _ = try await session.exchange(ControlFrame(command: .discover, requestID: 99)); XCTFail() }
        catch { XCTAssertEqual(error as? ReaderSessionError, .staleConnection) }
        let renewed = try await coordinator.authenticate(transport: reader)
        _ = try await renewed.exchange(ControlFrame(command: .discover, requestID: 100))
    }
    func testReconnectAfterAuthorizationDoesNotReturnAuthenticatedSession() async throws {
        let (_, _, coordinator) = setup(); let reader = SessionReader()
        await reader.reconnectAfterAuthorization()
        do { _ = try await coordinator.pair(transport: reader); XCTFail() }
        catch { XCTAssertEqual(error as? ReaderSessionError, .staleConnection) }
    }

}


extension ReaderSessionTests {
    func testAuthenticatedBackupRejectsForeignGenerationBeforeSending() async throws {
        let vault = PairingVault(storage: SessionStorage(), random: { Data(repeating: 7, count: $0) })
        let sessions = ReaderSession(credentials: vault)
        let transport = SessionReader()
        let authenticated = try await sessions.pair(transport: transport)
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let payload = try Data(contentsOf: root.appendingPathComponent("protocol/fixtures/LegacyBackupFileReply-v1.fixture"))
        await transport.configureExport(payload)
        let request = try LegacyBackupRequest(operation: .file, role: .reviews, bound: true,
            course: Data(repeating: 1, count: 16), transaction: Data([2]) + Data(repeating: 0, count: 15),
            generation: authenticated.device.storageGeneration, offset: 31, count: 768)
        let reply = try await authenticated.exchangeLegacyBackup(request, requestID: 21)
        XCTAssertEqual(reply.body.count, 768)
        let foreign = try LegacyBackupRequest(operation: .file, role: .reviews, bound: true,
            course: request.course, transaction: request.transaction, generation: Data(repeating: 3, count: 16),
            offset: 31, count: 768)
        do { _ = try await authenticated.exchangeLegacyBackup(foreign, requestID: 22); XCTFail("Foreign card accepted") }
        catch { XCTAssertEqual(error as? ReaderSessionError, .wrongReader) }
        let count = await transport.exportCount()
        XCTAssertEqual(count, 1)
        await transport.reconnect()
        do { _ = try await authenticated.exchangeLegacyBackup(request, requestID: 23); XCTFail("Stale connection accepted") }
        catch { XCTAssertEqual(error as? ReaderSessionError, .staleConnection) }
    }
}

import Foundation

public enum CloudSyncStateError: Error, Equatable, Sendable {
    case invalidAccount, invalidState, accountConfirmationRequired, tooLarge
    case retryDeferred(Date)
}
public struct CloudSyncState: Codable, Equatable, Sendable {
    public let version: Int
    public let account: String?
    public let enabled: Bool
    public let serialization: Data?
    public let retryNotBefore: Date?
    public init() { version = 1; account = nil; enabled = false; serialization = nil; retryNotBefore = nil }
    public func allowsForegroundResume(now: Date = Date()) -> Bool {
        guard enabled, account != nil else { return false }
        return retryNotBefore.map { $0 <= now } ?? true
    }
    fileprivate init(account: String?, enabled: Bool, serialization: Data?, retryNotBefore: Date? = nil) {
        version = 1; self.account = account; self.enabled = enabled; self.serialization = serialization
        self.retryNotBefore = retryNotBefore
    }
}

// This file stores engine checkpoints, never journal bodies, content assets or pairing credentials.
public actor CloudSyncStateStore {
    public static let maximumSerializationBytes = 8 * 1024 * 1024
    private static let maximumFileBytes = 12 * 1024 * 1024
    private let url: URL
    private var state: CloudSyncState
    public init(url: URL) throws {
        self.url = url
        if FileManager.default.fileExists(atPath: url.path) {
            let size = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
            guard size <= Self.maximumFileBytes else { throw CloudSyncStateError.tooLarge }
            state = try JSONDecoder().decode(CloudSyncState.self, from: Data(contentsOf: url))
            try Self.validate(state)
        } else { state = CloudSyncState() }
    }
    public func snapshot() -> CloudSyncState { state }
    // The caller obtains this opaque account identity from CloudKit, not from pairing credentials.
    @discardableResult
    public func observeAccount(_ account: String?) throws -> Bool {
        if let account { try Self.validateAccount(account) }
        guard account != state.account else { return false }
        try persist(CloudSyncState(account: account, enabled: false, serialization: nil))
        return true
    }
    public func enable(confirmedAccount account: String, now: Date = Date()) throws {
        try Self.validateAccount(account)
        guard state.account == account else { throw CloudSyncStateError.accountConfirmationRequired }
        if let deadline = state.retryNotBefore, deadline > now { throw CloudSyncStateError.retryDeferred(deadline) }
        guard !state.enabled else { return }
        try persist(CloudSyncState(account: account, enabled: true, serialization: state.serialization))
    }
    public func disable() throws {
        guard state.enabled else { return }
        try persist(CloudSyncState(account: state.account, enabled: false, serialization: state.serialization,
                                   retryNotBefore: state.retryNotBefore))
    }
    public func deferRetry(until deadline: Date, account: String) throws {
        guard state.account == account else { throw CloudSyncStateError.accountConfirmationRequired }
        guard deadline.timeIntervalSinceReferenceDate.isFinite else { throw CloudSyncStateError.invalidState }
        let retained = max(state.retryNotBefore ?? deadline, deadline)
        try persist(CloudSyncState(account: account, enabled: false, serialization: state.serialization,
                                   retryNotBefore: retained))
    }
    public func saveSerialization(_ data: Data, account: String) throws {
        guard data.count <= Self.maximumSerializationBytes else { throw CloudSyncStateError.tooLarge }
        guard state.enabled, state.account == account else { throw CloudSyncStateError.accountConfirmationRequired }
        guard state.serialization != data else { return }
        try persist(CloudSyncState(account: account, enabled: true, serialization: data))
    }
    private static func validateAccount(_ account: String) throws {
        guard !account.isEmpty, account.utf8.count <= 1024, !account.utf8.contains(0) else {
            throw CloudSyncStateError.invalidAccount
        }
    }
    private static func validate(_ state: CloudSyncState) throws {
        guard state.version == 1, state.account != nil || (!state.enabled && state.serialization == nil && state.retryNotBefore == nil),
              state.retryNotBefore?.timeIntervalSinceReferenceDate.isFinite ?? true else {
            throw CloudSyncStateError.invalidState
        }
        if let account = state.account { try validateAccount(account) }
        guard (state.serialization?.count ?? 0) <= maximumSerializationBytes else { throw CloudSyncStateError.tooLarge }
    }
    private func persist(_ next: CloudSyncState) throws {
        try Self.validate(next)
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let bytes = try encoder.encode(next)
        guard bytes.count <= Self.maximumFileBytes else { throw CloudSyncStateError.tooLarge }
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try bytes.write(to: url, options: .atomic)
        state = next
    }
}

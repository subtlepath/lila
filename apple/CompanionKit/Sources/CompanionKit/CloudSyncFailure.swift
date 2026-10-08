import Foundation

public enum CloudSyncFailure: Equatable, Sendable {
    case quotaExceeded, networkUnavailable, accountUnavailable, serviceUnavailable
    case journalConflict, localPersistence, other
    case retryDeferred(Date)
    public static func local(_ error: Error) -> Self {
        if error is HistoryError || error is ProtocolError || error is VaultError || error is LibraryVisibilityError { return .journalConflict }
        if let error = error as? CloudSyncStateError {
            if case .retryDeferred(let deadline) = error { return .retryDeferred(deadline) }
            return error == .accountConfirmationRequired || error == .invalidAccount ? .accountUnavailable : .localPersistence
        }
        if let error = error as? StoreError {
            if case .database = error { return .localPersistence }
            return .journalConflict
        }
        if error is CocoaError { return .localPersistence }
        return .other
    }
}

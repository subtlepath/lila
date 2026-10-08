import Foundation

public extension ContentVault {
    /// Validates the synced private stage before atomically publishing a firmware object.
    func importFirmware(_ source: URL, compatibility: FirmwareReleaseCompatibility) throws -> StoredObject {
        let expected = try ContentID(compatibility.sha256.map { String(format: "%02x", $0) }.joined())
        let (object, _) = try importValidatedFile(source, expectedID: expected, expectedLength: compatibility.length) {
            try FirmwareImageInspector.inspect($0, compatibility: compatibility)
        }
        return object
    }
}

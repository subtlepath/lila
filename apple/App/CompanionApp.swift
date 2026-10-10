import CompanionKit
import Foundation
import Observation
import SwiftUI
import UniformTypeIdentifiers

@MainActor @Observable
final class CompanionModel {
    var contents: [LibraryContent] = []
    var removedContents: [LibraryContent] = []
    var changingLibrary = false
    var courseIdentities: [ContentID: Data] = [:]
    var verifiedCourses: Set<ContentID> = []
    var courseDetails: [ContentID: CoursePackDetails] = [:]
    private var contentTitles: [ContentID: String] = [:]
    var error: String?
    var importing = false
    var releaseCheckBusy = false
    private let releaseDiscovery = StableReleaseDiscovery()
    private var releaseCheckTask: Task<Void, Never>?
    var releaseManifest: CompanionReleaseManifest?
    var allowReleaseCandidates = false
    var stagedFirmware: [ContentID: TransferJob] = [:]
    var firmwareInstallations: [ContentID: FirmwareInstallation] = [:]
    var firmwareInstallBusy = false
    var firmwareNotice: String?
    var firmwareDownloadBusy = false
    private var firmwareDownloadTask: Task<Void, Never>?
    var readers: [BluetoothReader] = []
    var bluetoothState: BluetoothTransport.State = .unavailable
    var connectionBusy = false
    var pairingRequired = false
    var connected: DeviceDescriptor?
    var savedReaders: [SavedReader] = []
    var readerSelections: [Data: Set<ContentID>] = [:]
    var savingSelection = false
    var inventory: ReaderInventory?
    var inventoryBusy = false
    var transferBusy = false
    var wifiAssistance = false
    var wifiNetworkMode = WifiNetworkMode.hotspot
    var wifiManualJoin: WifiManualJoinRequest?
    var transferNotice: String?
    private var wifiHandoffInProgress = false
    private let wifiNegotiator = WifiHandoffNegotiator()
    var pendingTransfers: [TransferJob] = []
    var pendingRemovals: [ContentRemovalJob] = []
    var pendingReaderImports: [ReaderImportJob] = []
    var readerImportActive = false
    private var readerImports: ReaderImportRunner?
    private var importCleanupTask: Task<Void, Never>?
    private var transferTask: Task<Void, Never>?
    private var transfers: TransferRunner?
    var inventoryError: String?
    private let inventories = InventoryCollector()
    private let journalExports = JournalExportCollector()
    private let learningBackups = LegacyBackupCollector()
    private let baselineReviews = CourseBaselineReviewCollector()
    private let migrations = TintaMigrationRunner()
    private let journalSync = ReaderJournalSynchronizer()
    private let journalMerges = JournalMergeRunner()
    private var migrationTask: Task<TintaMigrationJob, Error>?
    var learningBackupProgress: LegacyBackupProgress?
    private var learningBackupTask: Task<ContentID, Error>?
    private var backupStaging: URL?
    var historySyncError: String?
    var historySyncNotice: String?
    var historySyncActive = false
    var historySyncNeedsPreferences = false
    var historySyncJob: JournalMergeJob?
    private var historyAbortTask: Task<JournalMergeJob, Error>?
    private var inventoryTask: Task<Void, Never>?
    private var bluetooth: BluetoothTransport?
    private let sessions = ReaderSession(credentials: PairingVault(storage:
        KeychainCredentialStorage(service: "dev.lila.companion.credentials")))
    private var authenticated: AuthenticatedReaderSession?
    private var connectionTask: Task<Void, Never>?
    private var connectionOperation = UUID()
    private var scanRequested = false
    private var library: LibraryStore?
    private var contentVault: ContentVault?
    private var cloud: CloudJournalSync?
    var cloudBusy = false
    var cloudAccount: String?
    var cloudStatus: CloudJournalSyncStatus = .disabled
    var cloudConfigured = false
    var cloudFailed = false
    private var importer: ContentImporter?

    init() {
        do {
            let support = try FileManager.default.url(for: .applicationSupportDirectory,
                in: .userDomainMask, appropriateFor: nil, create: true)
            let root = support.appendingPathComponent("LilaCompanion", isDirectory: true)
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            let store = try LibraryStore(url: root.appendingPathComponent("library.sqlite"))
            library = store
            let vault = try ContentVault(root: root.appendingPathComponent("content"))
            contentVault = vault
            backupStaging = root.appendingPathComponent("learning-backup-staging", isDirectory: true)
            let contentImporter = ContentImporter(vault: vault, library: store)
            importer = contentImporter
            let importStorage = try ReaderImportStorage(root: root.appendingPathComponent("reader-imports", isDirectory: true))
            readerImports = ReaderImportRunner(library: store, storage: importStorage, importer: contentImporter)
            transfers = TransferRunner(library: store, vault: vault)
            if let identifier = Bundle.main.object(forInfoDictionaryKey: "LilaCloudKitContainer") as? String,
               identifier.hasPrefix("iCloud."), !identifier.contains("$(") {
                do {
                    let state = try CloudSyncStateStore(url: root.appendingPathComponent("cloud-state.json"))
                    cloud = CloudJournalSync(containerIdentifier: identifier, library: store, vault: vault, persistence: state)
                    cloudConfigured = true
                } catch { cloudFailed = true }
            }
        } catch {
            self.error = String(localized: "Your library could not be opened.")
        }
    }

    private func bluetoothTransport() -> BluetoothTransport {
        if let bluetooth { return bluetooth }
        let transport = BluetoothTransport()
        transport.onReadersChanged = { [weak self] in self?.readers = $0 }
        transport.onStateChanged = { [weak self] state in
            guard let self else { return }
            bluetoothState = state
            switch state {
            case .idle, .unavailable:
                if wifiHandoffInProgress {
                    if !historySyncActive {
                        inventoryTask?.cancel(); inventoryTask = nil
                        inventoryBusy = false
                    }
                    inventory = nil
                    return
                }
                if connected != nil || authenticated != nil || pairingRequired {
                    connectionOperation = UUID()
                    connectionTask?.cancel(); connectionTask = nil
                    connectionBusy = false
                }
                authenticated = nil; connected = nil; pairingRequired = false
                stagedFirmware = [:]; firmwareInstallations = [:]
                clearInventory()
                if case .idle = state, scanRequested { startScan() }
            default: break
            }
        }
        bluetooth = transport
        return transport
    }
    func checkCloudAccount() async {
        guard !cloudBusy, let cloud else { return }
        cloudBusy = true; cloudFailed = false; cloudAccount = nil
        defer { cloudBusy = false }
        do { cloudAccount = try await cloud.observedAccount(); cloudStatus = await cloud.currentStatus() }
        catch { await cloud.reportFailure(error); cloudFailed = true; cloudStatus = await cloud.currentStatus() }
    }
    func synchronizeCloud(confirmedAccount: String) async {
        guard !cloudBusy, let cloud, let library else { return }
        cloudBusy = true; cloudFailed = false
        defer { cloudBusy = false }
        do {
            try await performCloudSync(cloud, library: library, account: confirmedAccount)
        } catch { await cloud.reportFailure(error); cloudFailed = true; cloudStatus = await cloud.currentStatus() }
    }
    private func performCloudSync(_ cloud: CloudJournalSync, library: LibraryStore, account: String) async throws {
            let credentials = PairingVault(storage: KeychainCredentialStorage(service: "dev.lila.companion.credentials"))
            let origin = try await credentials.installationIdentity()
            while try await library.journalLegacyLibraryRemovals(origin: origin) > 0 {
                try Task.checkCancellation()
            }
            try await cloud.start(confirmedAccount: account)
            try await cloud.synchronize()
            cloudStatus = await cloud.currentStatus()
            await reload()
    }
    func resumeCloudIfEnabled() async {
        guard !cloudBusy, let cloud, let library else { return }
        cloudBusy = true
        defer { cloudBusy = false }
        do {
            if let account = try await cloud.resumableAccount() {
                cloudFailed = false
                try await performCloudSync(cloud, library: library, account: account)
            }
            cloudStatus = await cloud.currentStatus()
        } catch { await cloud.reportFailure(error); cloudFailed = true; cloudStatus = await cloud.currentStatus() }
    }
    func resumeReaderWorkIfAvailable() async {
        guard !transferBusy, !inventoryBusy, !connectionBusy, !savingSelection, !changingLibrary,
              !historySyncActive, let library, let session = authenticated, let inventory,
              inventory.complete, inventory.reader == session.device.identity,
              inventory.generation == session.device.storageGeneration,
              let bluetooth, case .ready = bluetooth.state else { return }
        let operation = connectionOperation
        do {
            let jobs = try await library.pendingJobs()
            var resumable = false
            for job in jobs where job.reader == session.device.identity &&
                job.storageGeneration == session.device.storageGeneration && job.installation == session.installation &&
                job.phase != .failed {
                if let content = try await library.content(job.content), content.kind != .firmware {
                    resumable = true
                    break
                }
            }
            if !resumable {
                resumable = try await library.pendingRemovalJobs().contains {
                    $0.reader == session.device.identity && $0.request.owner == session.installation &&
                        $0.request.generation == session.device.storageGeneration &&
                        session.device.readerCapabilities.supportsRemoval(of: $0.request.manifest.kind)
                }
            }
            guard connectionOperation == operation, !transferBusy, !inventoryBusy, !connectionBusy,
                  !savingSelection, !changingLibrary, !historySyncActive,
                  self.inventory == inventory, case .ready = bluetooth.state else { return }
            if resumable {
                transferSelectedContent()
                return
            }
            let imports = try await library.pendingReaderImports()
            for job in imports where job.reader == session.device.identity &&
                job.generation == session.device.storageGeneration && job.installation == session.installation &&
                inventory.contents.contains(job.manifest) {
                await reloadPendingTransfers(operation: operation)
                guard connectionOperation == operation, self.inventory == inventory else { return }
                if canImportInstalledContent(job.manifest) {
                    importInstalledContent(job.manifest, resuming: job)
                    return
                }
            }
        } catch {
            if connectionOperation == operation {
                self.error = String(localized: "Pending transfers could not be loaded.")
            }
        }
    }

    func stopCloud() async {
        guard !cloudBusy, let cloud else { return }
        cloudBusy = true; cloudFailed = false
        defer { cloudBusy = false }
        do { try await cloud.stop(); cloudStatus = await cloud.currentStatus(); cloudAccount = nil }
        catch { cloudFailed = true }
    }

    func findReaders() {
        guard !connectionBusy, !transferBusy, connected == nil, !pairingRequired else { return }
        transferNotice = nil
        let transport = bluetoothTransport()
        scanRequested = true
        switch transport.state {
        case .idle, .scanning: startScan()
        default: break
        }
    }

    private func startScan() {
        scanRequested = false
        do { try bluetooth?.scan() }
        catch { error = String(localized: "Readers could not be found. Check Bluetooth and try again.") }
    }

    func connectReader(_ reader: BluetoothReader) {
        guard !connectionBusy, connected == nil, !pairingRequired else { return }
        let transport = bluetoothTransport()
        scanRequested = false
        connectionBusy = true
        let operation = UUID(); connectionOperation = operation
        connectionTask = Task {
            defer { if connectionOperation == operation { connectionBusy = false; connectionTask = nil } }
            do {
                try await transport.connect(reader.id)
                let session = try await sessions.authenticate(transport: transport)
                guard connectionOperation == operation, case .ready = transport.state else { return }
                authenticated = session; connected = session.device
                await rememberReader(session.device, operation: operation)
                await refreshFirmwareState(session: session, operation: operation)
            } catch ReaderSessionError.pairingRequired {
                if connectionOperation == operation, case .ready = transport.state { pairingRequired = true }
            } catch is CancellationError { }
            catch {
                if connectionOperation == operation {
                    transport.disconnect()
                    error = String(localized: "The reader could not be connected.")
                }
            }
        }
    }

    func pairReader() {
        guard pairingRequired, !connectionBusy, let transport = bluetooth else { return }
        connectionBusy = true
        let operation = UUID(); connectionOperation = operation
        connectionTask = Task {
            defer { if connectionOperation == operation { connectionBusy = false; connectionTask = nil } }
            do {
                let session = try await sessions.pair(transport: transport)
                guard connectionOperation == operation, case .ready = transport.state else { return }
                authenticated = session; connected = session.device; pairingRequired = false
                await rememberReader(session.device, operation: operation)
                await refreshFirmwareState(session: session, operation: operation)
            } catch is CancellationError { }
            catch {
                if connectionOperation == operation {
                    transport.disconnect()
                    error = String(localized: "The reader could not be paired.")
                }
            }
        }
    }

    func disconnectReader() {
        wifiHandoffInProgress = false
        wifiManualJoin?.cancel(); wifiManualJoin = nil
        transferNotice = nil
        connectionOperation = UUID()
        connectionTask?.cancel(); connectionTask = nil
        connectionBusy = false; scanRequested = false
        authenticated = nil; connected = nil; pairingRequired = false
        stagedFirmware = [:]; firmwareInstallations = [:]
        clearInventory()
        bluetooth?.disconnect()
    }

    private func clearInventory() {
        transferTask?.cancel(); transferTask = nil
        migrationTask?.cancel(); migrationTask = nil
        historyAbortTask?.cancel(); historyAbortTask = nil
        historySyncJob = nil
        transferBusy = false; pendingTransfers = []; pendingRemovals = []; pendingReaderImports = []; readerImportActive = false
        inventoryTask?.cancel(); inventoryTask = nil
        inventory = nil; inventoryBusy = false; inventoryError = nil; historySyncError = nil
        historySyncNotice = nil; historySyncActive = false; historySyncNeedsPreferences = false
    }

    func refreshInventory() {
        guard !connectionBusy, !inventoryBusy, !transferBusy, let session = authenticated, let transport = bluetooth,
              case .ready = transport.state else { return }
        inventory = nil; inventoryError = nil; historySyncError = nil; inventoryBusy = true
        historySyncNotice = nil; historySyncNeedsPreferences = false
        let operation = connectionOperation
        inventoryTask = Task {
            defer {
                if connectionOperation == operation {
                    inventoryBusy = false; inventoryTask = nil; historySyncActive = false
                    if wifiHandoffInProgress {
                        wifiHandoffInProgress = false
                        authenticated = nil; connected = nil; inventory = nil
                        bluetooth?.disconnect()
                        error = String(localized: "History synchronization was interrupted. Reconnect to resume the saved job.")
                    }
                }
            }
            do {
                let result = try await inventories.collect(session: session, maximumEntries: 100_000)
                guard connectionOperation == operation, case .ready = transport.state,
                      connected?.identity == result.reader, connected?.storageGeneration == result.generation,
                      result.complete else { return }
                inventory = result
                if session.device.capabilities & JournalExportPage.capability != 0, let library {
                    do {
                        historySyncActive = true
                        try await synchronizeReaderHistory(session: session, inventory: result, library: library, operation: operation)
                    } catch is CancellationError {
                        if connectionOperation == operation {
                            historySyncNotice = String(localized: "History sync paused. Refresh to resume.")
                        }
                    } catch {
                        if connectionOperation == operation {
                            historySyncNotice = nil
                            if (error as? StoreError) == .unresolvedPreferences {
                                historySyncNeedsPreferences = true
                                historySyncError = String(localized: "Resolve portable preference conflicts before syncing history.")
                            } else if case JournalMergePlanningError.missingContent = error {
                                historySyncError = String(localized: "Install the selected font or dictionary on this reader before syncing history.")
                            } else if case JournalMergePlanningError.unavailableCourse = error {
                                historySyncError = String(localized: "History includes a dependency on another course. Keep it preserved until the reader can accept that course history.")
                            } else {
                                historySyncError = String(localized: "Reader history could not be synchronized. Try refreshing again.")
                            }
                        }
                    }
                }
            } catch is CancellationError { }
            catch {
                if connectionOperation == operation, case .ready = transport.state {
                    if (error as? InventoryCollectorError)?.requiresReaderReopen == true {
                        inventoryError = String(localized: "Reopen Connect & Sync on the reader, then reconnect to rebuild its library.")
                    } else {
                        inventoryError = String(localized: "Installed content could not be read. Try refreshing again.")
                    }
                }
            }
        }
    }

    func pauseContentTransfer() {
        guard transferBusy else { return }
        transferTask?.cancel()
    }

    func cancelContentTransfer(_ job: TransferJob) {
        guard !transferBusy, !inventoryBusy, !connectionBusy, let transfers,
              let session = authenticated, let transport = bluetooth, case .ready = transport.state,
              job.reader == session.device.identity, job.storageGeneration == session.device.storageGeneration,
              job.installation == session.installation, job.phase != .committing, job.phase != .completed else { return }
        transferBusy = true
        let operation = connectionOperation
        transferTask = Task {
            defer { if connectionOperation == operation { transferBusy = false; transferTask = nil } }
            do { _ = try await transfers.abort(job.id, session: session) }
            catch {
                if connectionOperation == operation {
                    self.error = String(localized: "Cancellation is pending. Reconnect to finish cancelling this transfer.")
                }
            }
            if connectionOperation == operation { self.inventory = nil }
            await reloadPendingTransfers(operation: operation)
        }
    }

    func pendingReaderImport(for manifest: ContentManifest) -> ReaderImportJob? {
        guard let session = authenticated, let inventory else { return nil }
        return pendingReaderImports.first {
            $0.reader == session.device.identity && $0.generation == inventory.generation &&
                $0.installation == session.installation && $0.manifest == manifest &&
                $0.phase != .completed && $0.phase != .aborted
        }
    }

    func canImportInstalledContent(_ manifest: ContentManifest) -> Bool {
        guard !transferBusy, !inventoryBusy, !connectionBusy, !savingSelection, !changingLibrary,
              !historySyncActive, readerImports != nil, importCleanupTask == nil, let session = authenticated, let inventory,
              inventory.complete, inventory.reader == session.device.identity,
              inventory.generation == session.device.storageGeneration, inventory.contents.contains(manifest),
              session.device.readerCapabilities.supportsContentMetadata,
              manifest.kind != .firmware,
              !contents.contains(where: { $0.id == manifest.content }) || pendingReaderImport(for: manifest) != nil,
              !removedContents.contains(where: { $0.id == manifest.content }),
              let bluetooth, case .ready = bluetooth.state else { return false }
        return true
    }

    func importInstalledContent(_ manifest: ContentManifest, resuming expectedJob: ReaderImportJob? = nil) {
        guard canImportInstalledContent(manifest), let readerImports, let session = authenticated,
              let inventory else { return }
        let operation = connectionOperation
        transferBusy = true; readerImportActive = true; transferNotice = nil
        transferTask = Task {
            defer {
                if connectionOperation == operation {
                    transferBusy = false; readerImportActive = false; transferTask = nil
                    if wifiHandoffInProgress {
                        wifiHandoffInProgress = false
                        authenticated = nil; connected = nil; inventory = nil
                        bluetooth?.disconnect()
                    }
                    startReaderImportCleanup()
                }
            }
            var importJob: ReaderImportJob?
            do {
                let job: ReaderImportJob
                if let expectedJob {
                    guard let library, expectedJob.manifest == manifest,
                          try await library.readerImportJob(expectedJob.id) == expectedJob else {
                        throw StoreError.invalidTransition
                    }
                    if try await library.readerImportFilename(expectedJob.id) != nil {
                        job = expectedJob
                    } else {
                        job = try await readerImports.prepareImport(manifest: manifest, session: session,
                            inventory: inventory, resuming: expectedJob.id)
                    }
                } else {
                    job = try await readerImports.prepareImport(manifest: manifest, session: session, inventory: inventory)
                }
                importJob = job
                if wifiAssistance, session.device.readerCapabilities.supportsWifiContentRead,
                   job.manifest.length - job.acknowledgedOffset > ReaderContentHandoffRequest.wifiThreshold {
                    try await importThroughWifi(job, session: session, inventory: inventory, runner: readerImports)
                } else {
                    _ = try await readerImports.download(job.id, session: session, inventory: inventory)
                }
                guard connectionOperation == operation else { return }
                transferNotice = String(localized: "Reader content was added to your library.")
                await reload()
                if wifiHandoffInProgress {
                    do { _ = try await restoreBluetoothAfterWifi(session, operation: operation) }
                    catch {
                        guard connectionOperation == operation else { return }
                        transferNotice = String(localized: "Reader content was added to your library. Reconnect to continue.")
                    }
                }
            } catch {
                guard connectionOperation == operation else { return }
                let completed: Bool
                if let importJob, let library {
                    completed = (try? await library.readerImportJob(importJob.id))?.phase == .completed
                } else { completed = false }
                if wifiHandoffInProgress, !Task.isCancelled {
                    _ = try? await restoreBluetoothAfterWifi(session, operation: operation)
                    guard connectionOperation == operation else { return }
                }
                if completed {
                    await reload()
                    guard connectionOperation == operation else { return }
                    transferNotice = wifiHandoffInProgress
                        ? String(localized: "Reader content was added to your library. Reconnect to continue.")
                        : String(localized: "Reader content was added to your library.")
                } else if wifiHandoffInProgress {
                    transferNotice = String(localized: "Reader import paused. Reconnect and refresh installed content to resume.")
                } else if Task.isCancelled || error is CancellationError {
                    transferNotice = String(localized: "Reader import paused. Resume it from installed content.")
                } else {
                    self.error = String(localized: "Reader content could not be imported. Refresh installed content and try again.")
                }
            }
            await reloadPendingTransfers(operation: operation)
        }
    }

    private func importThroughWifi(_ job: ReaderImportJob, session: AuthenticatedReaderSession,
                                   inventory: ReaderInventory, runner: ReaderImportRunner) async throws {
        do {
            _ = try await runner.prepareForHandoff(job.id, session: session, inventory: inventory)
        } catch {
            if !Task.isCancelled, let bluetooth, case .ready = bluetooth.state,
               error as? ReaderImportRunnerError == .unsupportedContent {
                _ = try await runner.download(job.id, session: session, inventory: inventory)
                return
            }
            throw error
        }
        let mode = wifiNetworkMode
        let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
        wifiHandoffInProgress = true
        let negotiation: WifiHandoffNegotiation
        do {
            negotiation = try await wifiNegotiator.negotiate(session: session, transaction: transaction, mode: mode)
        } catch WifiHandoffCommandError.control(let code) {
            if (code == 1 || code == 5), let bluetooth, case .ready = bluetooth.state {
                wifiHandoffInProgress = false
                _ = try await runner.download(job.id, session: session, inventory: inventory)
                return
            }
            throw WifiHandoffCommandError.control(code)
        }
        var lease: WifiHotspotLease?
        #if os(iOS)
        if mode == .hotspot { lease = try await WifiHotspotJoiner.apply(negotiation) }
        else { try await requestManualWifiJoin(negotiation) }
        #else
        try await requestManualWifiJoin(negotiation)
        #endif
        defer { lease?.close() }
        let handoff = try await WifiHandoffConnector.prepare(offer: negotiation.network.offer,
            reader: job.reader, storageGeneration: job.generation, installation: job.installation,
            transaction: transaction, receivedAtNanoseconds: negotiation.receivedAtNanoseconds)
        do {
            _ = try await runner.download(job.id, session: session, inventory: inventory, handoff: handoff)
            try await handoff.finish(requestID: UInt32.random(in: 1...UInt32.max))
        } catch {
            await handoff.close()
            throw error
        }
    }

    var canCancelReaderImport: Bool {
        !transferBusy && !inventoryBusy && !connectionBusy && !savingSelection &&
            !changingLibrary && !historySyncActive && importCleanupTask == nil &&
            readerImports != nil && library != nil
    }

    func cancelReaderImport(_ job: ReaderImportJob) async {
        guard canCancelReaderImport, let readerImports, let library else { return }
        let operation = connectionOperation
        savingSelection = true
        defer { savingSelection = false }
        do {
            _ = try await readerImports.cancel(job.id)
            if connectionOperation == operation { transferNotice = String(localized: "Reader import cancelled.") }
        } catch {
            if connectionOperation == operation {
                let saved = try? await library.readerImportJob(job.id)
                self.error = saved?.phase == .aborted
                    ? String(localized: "Reader import cancelled. Temporary files could not be cleared.")
                    : String(localized: "Reader import could not be cancelled. Try again.")
            }
        }
        await reloadPendingTransfers(operation: operation)
    }

    func pauseReaderImport() {
        guard readerImportActive else { return }
        transferTask?.cancel()
    }

    func canRemoveInstalledContent(_ manifest: ContentManifest) -> Bool {
        guard canTransferSelectedContent, let session = authenticated, let inventory,
              inventory.complete, inventory.reader == session.device.identity,
              inventory.generation == session.device.storageGeneration,
              inventory.contents.contains(manifest), session.device.readerCapabilities.supportsRemoval(of: manifest.kind),
              let bluetooth, case .ready = bluetooth.state else { return false }
        return true
    }

    func removeInstalledContent(_ manifest: ContentManifest) async {
        guard canRemoveInstalledContent(manifest), let library, let session = authenticated,
              let inventory else { return }
        let operation = connectionOperation
        savingSelection = true
        do {
            _ = try await library.queueRemoval(manifest: manifest, inventory: inventory, installation: session.installation)
            await reload()
            await reloadPendingTransfers(operation: operation)
            savingSelection = false
            guard connectionOperation == operation else { return }
            transferSelectedContent()
        } catch {
            savingSelection = false
            if connectionOperation == operation {
                if error as? StoreError == .conflictingJob {
                    self.error = String(localized: "Finish or cancel this content’s pending transfer before removing it from the reader.")
                } else {
                    self.error = String(localized: "Reader removal could not be queued. Refresh installed content and try again.")
                }
            }
        }
    }

    var canTransferSelectedContent: Bool {
        guard !transferBusy, !inventoryBusy, !connectionBusy, !savingSelection, !changingLibrary,
              !historySyncActive, library != nil, transfers != nil,
              let session = authenticated, let inventory, inventory.complete,
              inventory.reader == session.device.identity,
              inventory.generation == session.device.storageGeneration,
              let bluetooth, case .ready = bluetooth.state else { return false }
        return true
    }

    func transferSelectedContent() {
        guard canTransferSelectedContent, let library, let transfers,
              let session = authenticated, let inventory, let transport = bluetooth,
              case .ready = transport.state else { return }
        transferBusy = true
        transferNotice = nil
        let operation = connectionOperation
        transferTask = Task {
            defer {
                if connectionOperation == operation {
                    transferBusy = false; transferTask = nil
                    if wifiHandoffInProgress {
                        wifiHandoffInProgress = false
                        authenticated = nil; connected = nil; inventory = nil
                        transport.disconnect()
                    }
                }
            }
            do {
                var currentInventory = inventory
                if session.device.readerCapabilities.supportsEpubRemoval || session.device.readerCapabilities.supportsFontRemoval {
                    let removals = try await library.pendingRemovalJobs().filter {
                        $0.reader == session.device.identity && $0.request.owner == session.installation &&
                            $0.request.generation == session.device.storageGeneration &&
                            session.device.readerCapabilities.supportsRemoval(of: $0.request.manifest.kind)
                    }
                    for job in removals {
                        try Task.checkCancellation()
                        guard connectionOperation == operation else { throw CancellationError() }
                        _ = try await transfers.removeContent(job.id, session: session)
                    }
                    if !removals.isEmpty {
                        currentInventory = try await inventories.collect(session: session, maximumEntries: 100_000)
                        guard connectionOperation == operation else { throw CancellationError() }
                    }
                }
                let work = try await library.prepareContentWork(reader: session.device.identity,
                    generation: session.device.storageGeneration, installation: session.installation, inventory: currentInventory)
                await reloadPendingTransfers(operation: operation)
                var unsupported = false
                var usedWifi = false
                var currentSession = session
                for item in work {
                    try Task.checkCancellation()
                    switch item {
                    case .transfer(let job):
                        let kind = try await library.content(job.content)?.kind
                        if kind == .epub || kind == .font || kind == .course && currentSession.device.readerCapabilities.supportsCourseTransfer ||
                            kind == .dictionary && currentSession.device.readerCapabilities.supportsDictionaryTransfer {
                            if kind == .course {
                                guard let contentVault else { throw StoreError.missingContent }
                                _ = try await library.admitCourseTransfer(job.content, inventory: currentInventory, vault: contentVault, confirmedSwitchJob: job.id)
                            }
                            if wifiAssistance, job.phase != .committing,
                               let content = try await library.content(job.content), content.length > 1024 * 1024 {
                                usedWifi = try await transferThroughWifi(job, session: currentSession, runner: transfers)
                            } else { _ = try await transfers.run(job.id, session: currentSession) }
                            if kind == .course && !usedWifi {
                                currentInventory = try await inventories.collect(session: currentSession, maximumEntries: 100_000)
                                guard connectionOperation == operation else { throw CancellationError() }
                            }
                        } else { unsupported = true }
                    case .courseBaseline(let job):
                        if currentSession.device.readerCapabilities.supportsCourseBaselineImport {
                            if wifiAssistance, job.phase != .committing,
                               let content = try await library.content(job.content), content.length > 1024 * 1024 {
                                usedWifi = try await transferThroughWifi(job, session: currentSession, runner: transfers)
                            } else { _ = try await transfers.runCourseBaseline(job.id, session: currentSession) }
                        } else { unsupported = true }
                    case .abort(let job): _ = try await transfers.abort(job.id, session: currentSession)
                    case .remove(let manifest):
                        if currentSession.device.readerCapabilities.supportsRemoval(of: manifest.kind) {
                            let job = try await library.queueRemoval(manifest: manifest, inventory: currentInventory,
                                installation: currentSession.installation)
                            _ = try await transfers.removeContent(job.id, session: currentSession)
                        } else { unsupported = true }
                    case .inspect, .staleGeneration: unsupported = true
                    }
                    await reloadPendingTransfers(operation: operation)
                    if usedWifi {
                        currentSession = try await restoreBluetoothAfterWifi(currentSession, operation: operation)
                        guard let refreshed = self.inventory else { throw StoreError.invalidValue }
                        currentInventory = refreshed
                        usedWifi = false
                    }
                }
                guard connectionOperation == operation else { return }
                if case .ready = transport.state {} else { return }
                if unsupported {
                    error = String(localized: "Some content operations are not supported yet. Pending jobs have been retained.")
                }
                self.inventory = nil
            } catch is CancellationError {
                if connectionOperation == operation { self.inventory = nil }
            }
            catch {
                if connectionOperation == operation {
                    if wifiHandoffInProgress {
                        _ = try? await restoreBluetoothAfterWifi(session, operation: operation)
                    }
                    guard connectionOperation == operation else { return }
                    self.inventory = nil
                    if let admission = error as? CourseTransferAdmissionError {
                        switch admission {
                        case .differentCourse:
                            if (authenticated?.device.readerCapabilities ?? session.device.readerCapabilities).supportsCourseSwitch {
                                self.error = String(localized: "This pack belongs to a different course. Refresh installed content, then open this pack in Library to confirm a course switch. The current course and pending work have been retained.")
                            } else {
                                self.error = String(localized: "This reader does not support switching courses through the companion. Keep the current course or install compatible firmware before trying again. Pending work has been retained.")
                            }
                        case .multipleActiveCourses:
                            self.error = String(localized: "The reader reported more than one active course. Reconnect and refresh installed content before retrying. Pending work has been retained.")
                        case .incompatibleHistory:
                            self.error = String(localized: "This course update cannot preserve existing learning history. Choose a compatible edition of the same course. The installed pack and pending work have been retained.")
                        }
                    } else if let commandError = error as? TransferCommandError, commandError == .remote(.invalid) {
                        self.error = String(localized: "The reader rejected this content. Check pack compatibility and the active course before retrying. Pending work has been retained.")
                    } else if let commandError = error as? TransferCommandError, commandError == .remote(.hashMismatch) {
                        self.error = String(localized: "The reader could not verify the transferred file. Reconnect and refresh installed content before retrying. Pending work has been retained.")
                    } else {
                        self.error = String(localized: "Content transfer stopped. Reconnect and refresh installed content to resume pending work.")
                    }
                }
            }
            await reloadPendingTransfers(operation: operation)
        }
    }

    var canStageFirmware: Bool {
        guard !firmwareInstallBusy, !transferBusy, !inventoryBusy, !connectionBusy, !firmwareDownloadBusy,
              authenticated != nil, let bluetooth, case .ready = bluetooth.state else { return false }
        return true
    }
    func stageFirmware(_ content: LibraryContent) {
        guard canStageFirmware, content.kind == .firmware, let session = authenticated,
              let library, let transfers, let transport = bluetooth else { return }
        transferBusy = true
        transferNotice = nil
        let operation = connectionOperation
        transferTask = Task {
            defer {
                if connectionOperation == operation {
                    transferBusy = false; transferTask = nil
                    if wifiHandoffInProgress {
                        wifiHandoffInProgress = false
                        authenticated = nil; connected = nil; inventory = nil
                        transport.disconnect()
                    }
                }
            }
            do {
                let job = try await library.enqueueFirmware(content: content.id, reader: session.device.identity,
                    storageGeneration: session.device.storageGeneration, installation: session.installation)
                await reloadPendingTransfers(operation: operation)
                if wifiAssistance, content.length > 1024 * 1024, job.phase != .committing {
                    let usedWifi = try await transferThroughWifi(job, session: session, runner: transfers, firmware: true)
                    if usedWifi { _ = try await restoreBluetoothAfterWifi(session, operation: operation) }
                } else {
                    _ = try await transfers.stageFirmware(job.id, session: session)
                }
                guard connectionOperation == operation else { return }
                transferNotice = String(localized: "Firmware is verified and staged on the reader’s SD card. Confirm installation in Updates to flash it.")
                if let active = authenticated { await refreshFirmwareState(session: active, operation: operation) }
            } catch is CancellationError {
                if connectionOperation == operation { transferNotice = String(localized: "Firmware staging paused. Reconnect and stage this image again to resume.") }
            } catch {
                if connectionOperation == operation {
                    if wifiHandoffInProgress { _ = try? await restoreBluetoothAfterWifi(session, operation: operation) }
                    guard connectionOperation == operation else { return }
                    self.error = String(localized: "Firmware could not be staged. Check reader compatibility and battery level, then reconnect and retry. Pending work has been retained.")
                }
            }
            await reloadPendingTransfers(operation: operation)
        }
    }

    private func refreshFirmwareState(session: AuthenticatedReaderSession, operation: UUID) async {
        guard let library else { return }
        do {
            let jobs = try await library.stagedFirmwareJobs(reader: session.device.identity,
                generation: session.device.storageGeneration, installation: session.installation)
            var staged: [ContentID: TransferJob] = [:]
            for job in jobs where staged[job.content] == nil { staged[job.content] = job }
            let saved = try await library.firmwareInstallations(reader: session.device.identity, installation: session.installation)
            var installations: [ContentID: FirmwareInstallation] = [:]
            var verified = false
            for item in saved {
                if !item.bootVerified, item.receipt.generation == session.device.storageGeneration {
                    verified = try await library.verifyFirmwareInstallation(item.receipt.transaction,
                        device: session.device, installation: session.installation) || verified
                }
                if installations[item.receipt.image] == nil {
                    installations[item.receipt.image] = try await library.firmwareInstallation(item.receipt.transaction)
                }
            }
            guard connectionOperation == operation else { return }
            stagedFirmware = staged; firmwareInstallations = installations
            if verified { firmwareNotice = String(localized: "Firmware installation verified after reconnect.") }
        } catch {
            if connectionOperation == operation {
                firmwareNotice = String(localized: "Firmware installation status could not be checked. Reconnect to retry verification.")
            }
        }
    }
    func canInstallFirmware(_ job: TransferJob) -> Bool {
        guard canStageFirmware, let session = authenticated else { return false }
        return job.phase == .completed && job.reader == session.device.identity &&
            job.storageGeneration == session.device.storageGeneration && job.installation == session.installation
    }
    func installFirmware(_ job: TransferJob) {
        guard canInstallFirmware(job), let session = authenticated, let transfers else { return }
        firmwareInstallBusy = true; transferBusy = true
        firmwareNotice = nil
        let operation = connectionOperation
        transferTask = Task {
            defer {
                firmwareInstallBusy = false
                if connectionOperation == operation { transferBusy = false; transferTask = nil }
            }
            do {
                let installation = try await transfers.installFirmware(job.id, session: session)
                guard connectionOperation == operation else { return }
                firmwareInstallations[job.content] = installation
                firmwareNotice = installation.bootVerified
                    ? String(localized: "Firmware installation verified after reconnect.")
                    : String(localized: "Firmware installation requested. Reconnect after the reader restarts to verify the update.")
                if !installation.bootVerified { disconnectReader() }
            } catch {
                if connectionOperation == operation {
                    firmwareNotice = String(localized: "Firmware installation was not confirmed. Reconnect to check the running image before retrying.")
                    await refreshFirmwareState(session: session, operation: operation)
                }
            }
        }
    }

    func pauseHistorySync() { inventoryTask?.cancel() }

    var canCancelHistorySync: Bool {
        guard !inventoryBusy, !transferBusy, !connectionBusy, !historySyncActive,
              let job = historySyncJob, let session = authenticated, let bluetooth,
              case .ready = bluetooth.state,
              job.phase != .completed, job.abortState != .completed else { return false }
        return job.reader == session.device.identity && job.declaration.generation == session.device.storageGeneration &&
            job.declaration.owner == session.installation &&
            session.device.capabilities & (JournalExportPage.capability | JournalMergeReadiness.capability) ==
                (JournalExportPage.capability | JournalMergeReadiness.capability)
    }
    func cancelHistorySync(_ expected: JournalMergeJob) async throws {
        guard canCancelHistorySync, historySyncJob == expected, let session = authenticated, let library else {
            throw ReaderSessionError.busy
        }
        let operation = connectionOperation
        transferBusy = true
        defer { if connectionOperation == operation { transferBusy = false; historyAbortTask = nil } }
        let task = Task { try await journalMerges.abort(transaction: expected.id, session: session, library: library) }
        historyAbortTask = task
        do {
            let completed = try await task.value
            guard connectionOperation == operation else { throw CancellationError() }
            historySyncJob = completed
            historySyncError = nil
            historySyncNotice = completed.phase == .completed
                ? String(localized: "History was already committed and has been retained.")
                : String(localized: "Pending history upload cancelled. Preserved history remains in the library.")
        } catch {
            let retained = try? await library.journalMergeJob(expected.id)
            guard connectionOperation == operation else { throw CancellationError() }
            historySyncJob = retained
            historySyncNotice = nil
            throw error
        }
    }

    private func synchronizeReaderHistory(session: AuthenticatedReaderSession, inventory: ReaderInventory,
                                          library: LibraryStore, operation: UUID) async throws {
        let prepared = try await journalSync.prepare(session: session, inventory: inventory, library: library)
        try Task.checkCancellation()
        guard connectionOperation == operation else { throw CancellationError() }
        historySyncJob = nil
        switch prepared {
        case .cancellationPending(let job):
            historySyncJob = job
            historySyncNotice = String(localized: "History cancellation needs confirmation. Retry cancellation on this reader.")
        case .upload(let job):
            historySyncJob = job
            historySyncNotice = String(localized: "Syncing history")
            let completed: JournalMergeJob
            do {
                if wifiAssistance, UInt64(job.declaration.merged.count - job.acknowledgedCount) * 1024 > 1024 * 1024 {
                    completed = try await mergeHistoryThroughWifi(job, session: session, library: library, operation: operation)
                } else {
                    completed = try await journalMerges.run(transaction: job.id, session: session, library: library)
                }
            } catch {
                if let current = try? await library.journalMergeJob(job.id), current.phase != .completed {
                    let paused = try? await library.updateJournalMerge(current, phase: current.phase,
                        acknowledgedCount: current.acknowledgedCount, paused: true)
                    if connectionOperation == operation { historySyncJob = paused ?? current }
                }
                throw error
            }
            guard connectionOperation == operation else { throw CancellationError() }
            historySyncJob = completed
            guard let active = authenticated,
                  active.device.identity == session.device.identity,
                  active.device.storageGeneration == session.device.storageGeneration,
                  active.installation == session.installation else { throw ReaderSessionError.wrongReader }
            let refreshed = try await inventories.collect(session: active, maximumEntries: 100_000)
            guard connectionOperation == operation else { throw CancellationError() }
            self.inventory = refreshed
            let verified = try await journalSync.prepare(session: active, inventory: refreshed, library: library)
            guard connectionOperation == operation else { throw CancellationError() }
            if case .upToDate(let checkpoint) = verified {
                let complete = try await library.recordSuccessfulReaderSync(checkpoint, inventory: refreshed)
                let readers = try await library.savedReaders()
                guard connectionOperation == operation else { throw CancellationError() }
                savedReaders = readers
                historySyncNotice = complete ? String(localized: "History synchronized.")
                    : String(localized: "History is verified. Finish pending content or resolve conflicts to complete sync.")
            } else {
                if case .upload(let pending) = verified { historySyncJob = pending }
                historySyncNotice = String(localized: "More history changes are pending. Sync again to finish.")
            }
        case .upgradeRequired:
            historySyncNotice = String(localized: "Update the reader firmware before uploading history.")
        case .upToDate(let checkpoint):
            let complete = try await library.recordSuccessfulReaderSync(checkpoint, inventory: inventory)
            let readers = try await library.savedReaders()
            guard connectionOperation == operation else { throw CancellationError() }
            savedReaders = readers
            historySyncNotice = complete ? String(localized: "History is up to date.")
                : String(localized: "History is verified. Finish pending content or resolve conflicts to complete sync.")
        case .installationPending:
            historySyncNotice = String(localized: "Finish or cancel the pending learning installation before syncing history.")
        case .blocked(.migrationRequired):
            historySyncNotice = String(localized: "Preserve and review this reader’s learning backup before syncing history.")
        case .blocked(.unavailable):
            historySyncNotice = String(localized: "The reader’s learning state needs recovery before history can be uploaded. Reopen Connect & Sync and retry.")
        case .blocked(.noCourse):
            historySyncNotice = String(localized: "Reader history was imported; history upload is unavailable in the current reader state.")
        case .blocked(.ready):
            throw ReaderJournalSynchronizerError.invalidSnapshot
        }
    }

    private func transferThroughWifi(_ job: TransferJob, session: AuthenticatedReaderSession,
                                        runner: TransferRunner, firmware: Bool = false) async throws -> Bool {
        let mode = wifiNetworkMode
        let preparation: FirmwareHandoffPreparation?
        let staged: TransferJob
        guard let library else { throw StoreError.missingContent }
        let baseline = try await library.courseBaselineConfirmation(job.id) != nil
        guard !firmware || !baseline else { throw StoreError.conflictingJob }
        if firmware {
            let prepared = try await runner.prepareFirmwareHandoff(job.id, session: session)
            preparation = prepared; staged = prepared.job
        } else {
            preparation = nil
            staged = baseline ? try await runner.prepareCourseBaselineHandoff(job.id, session: session)
                : try await runner.prepareForHandoff(job.id, session: session)
        }
        if staged.phase == .completed { return false }
        if let content = try await library.content(job.content), staged.durableOffset == content.length {
            if firmware { _ = try await runner.stageFirmware(job.id, session: session) }
            else if baseline { _ = try await runner.runCourseBaseline(job.id, session: session) }
            else { _ = try await runner.run(job.id, session: session) }
            return false
        }
        try Task.checkCancellation()
        wifiHandoffInProgress = true
        let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
        let negotiation: WifiHandoffNegotiation
        do {
            negotiation = try await wifiNegotiator.negotiate(session: session, transaction: transaction, mode: mode)
        } catch WifiHandoffCommandError.control(let code) {
            if (code == 1 || code == 5), let bluetooth, case .ready = bluetooth.state {
                wifiHandoffInProgress = false
                if firmware { _ = try await runner.stageFirmware(job.id, session: session) }
                else if baseline { _ = try await runner.runCourseBaseline(job.id, session: session) }
                else { _ = try await runner.run(job.id, session: session) }
                return false
            }
            throw WifiHandoffCommandError.control(code)
        }
        var lease: WifiHotspotLease?
        #if os(iOS)
        if mode == .hotspot { lease = try await WifiHotspotJoiner.apply(negotiation) }
        else { try await requestManualWifiJoin(negotiation) }
        #else
        try await requestManualWifiJoin(negotiation)
        #endif
        defer { lease?.close() }
        let handoff = try await WifiHandoffConnector.prepare(offer: negotiation.network.offer,
            reader: job.reader, storageGeneration: job.storageGeneration, installation: job.installation,
            transaction: transaction, receivedAtNanoseconds: negotiation.receivedAtNanoseconds)
        do {
            if let preparation { _ = try await runner.stageFirmware(preparation, session: session, handoff: handoff) }
            else if baseline { _ = try await runner.runCourseBaseline(job.id, session: session, handoff: handoff) }
            else { _ = try await runner.run(job.id, session: session, handoff: handoff) }
            try await handoff.finish(requestID: UInt32.random(in: 1...UInt32.max))
        } catch {
            await handoff.close()
            throw error
        }
        return true
    }

    private func migrateThroughWifi(_ job: TintaMigrationJob, session: AuthenticatedReaderSession,
                                    library: LibraryStore, operation: UUID) async throws -> TintaMigrationJob {
        try Task.checkCancellation()
        let prepared = try await migrations.prepareForHandoff(transaction: job.id, session: session, library: library)
        if prepared.phase == .completed { return prepared }
        if prepared.acknowledgedCount == prepared.admission.merge.merged.count {
            return try await migrations.run(transaction: job.id, session: session, library: library)
        }
        let mode = wifiNetworkMode
        wifiHandoffInProgress = true
        let negotiation: WifiHandoffNegotiation
        do {
            negotiation = try await wifiNegotiator.negotiate(session: session, transaction: job.id, mode: mode)
        } catch WifiHandoffCommandError.control(let code) {
            if (code == 1 || code == 5), let bluetooth, case .ready = bluetooth.state {
                wifiHandoffInProgress = false
                return try await migrations.run(transaction: job.id, session: session, library: library)
            }
            throw WifiHandoffCommandError.control(code)
        }
        var lease: WifiHotspotLease?
        #if os(iOS)
        if mode == .hotspot { lease = try await WifiHotspotJoiner.apply(negotiation) }
        else { try await requestManualWifiJoin(negotiation) }
        #else
        try await requestManualWifiJoin(negotiation)
        #endif
        defer { lease?.close() }
        let handoff = try await WifiHandoffConnector.prepare(offer: negotiation.network.offer,
            reader: job.admission.reader, storageGeneration: job.admission.merge.generation,
            installation: job.admission.merge.owner, transaction: job.id,
            receivedAtNanoseconds: negotiation.receivedAtNanoseconds)
        do {
            let completed = try await migrations.run(transaction: job.id, wifi: handoff, library: library)
            try await handoff.finish(requestID: UInt32.random(in: 1...UInt32.max))
            _ = try await restoreBluetoothAfterWifi(session, operation: operation)
            return completed
        } catch {
            await handoff.close()
            throw error
        }
    }

    private func mergeHistoryThroughWifi(_ job: JournalMergeJob, session: AuthenticatedReaderSession,
                                    library: LibraryStore, operation: UUID) async throws -> JournalMergeJob {
        try Task.checkCancellation()
        let prepared = try await journalMerges.prepareForHandoff(transaction: job.id, session: session, library: library)
        if prepared.phase == .completed { return prepared }
        if prepared.acknowledgedCount == prepared.declaration.merged.count {
            return try await journalMerges.run(transaction: job.id, session: session, library: library)
        }
        let mode = wifiNetworkMode
        wifiHandoffInProgress = true
        let negotiation: WifiHandoffNegotiation
        do {
            negotiation = try await wifiNegotiator.negotiate(session: session, transaction: job.id, mode: mode)
        } catch WifiHandoffCommandError.control(let code) {
            if (code == 1 || code == 5), let bluetooth, case .ready = bluetooth.state {
                wifiHandoffInProgress = false
                return try await journalMerges.run(transaction: job.id, session: session, library: library)
            }
            throw WifiHandoffCommandError.control(code)
        }
        var lease: WifiHotspotLease?
        #if os(iOS)
        if mode == .hotspot { lease = try await WifiHotspotJoiner.apply(negotiation) }
        else { try await requestManualWifiJoin(negotiation) }
        #else
        try await requestManualWifiJoin(negotiation)
        #endif
        defer { lease?.close() }
        let handoff = try await WifiHandoffConnector.prepare(offer: negotiation.network.offer,
            reader: job.reader, storageGeneration: job.declaration.generation,
            installation: job.declaration.owner, transaction: job.id,
            receivedAtNanoseconds: negotiation.receivedAtNanoseconds)
        do {
            let completed = try await journalMerges.run(transaction: job.id, wifi: handoff, library: library)
            try await handoff.finish(requestID: UInt32.random(in: 1...UInt32.max))
            _ = try await restoreBluetoothAfterWifi(session, operation: operation)
            return completed
        } catch {
            await handoff.close()
            throw error
        }
    }

    private func restoreBluetoothAfterWifi(_ previous: AuthenticatedReaderSession,
                                          operation: UUID) async throws -> AuthenticatedReaderSession {
        guard connectionOperation == operation, let bluetooth else { throw CancellationError() }
        try await bluetooth.reconnectAfterHandoff()
        let restored = try await sessions.authenticate(transport: bluetooth)
        guard connectionOperation == operation else { throw CancellationError() }
        guard restored.device.identity == previous.device.identity,
              restored.device.storageGeneration == previous.device.storageGeneration,
              restored.installation == previous.installation else { throw StoreError.invalidValue }
        let refreshed = try await inventories.collect(session: restored, maximumEntries: 100_000)
        guard connectionOperation == operation, refreshed.complete,
              refreshed.reader == restored.device.identity,
              refreshed.generation == restored.device.storageGeneration else { throw StoreError.invalidValue }
        authenticated = restored
        connected = restored.device
        inventory = refreshed
        wifiHandoffInProgress = false
        await refreshFirmwareState(session: restored, operation: operation)
        return restored
    }

    private func requestManualWifiJoin(_ negotiation: WifiHandoffNegotiation) async throws {
        let request = try WifiManualJoinRequest(negotiation)
        wifiManualJoin = request
        defer { if wifiManualJoin === request { wifiManualJoin = nil } }
        try await request.waitForConfirmation()
    }

    private func reloadPendingTransfers(operation: UUID) async {
        guard let library, let device = connected, connectionOperation == operation else { return }
        do {
            let jobs = try await library.pendingJobs().filter { $0.reader == device.identity }
            let removals = try await library.pendingRemovalJobs().filter { $0.reader == device.identity }
            let imports = try await library.pendingReaderImports().filter { $0.reader == device.identity }
            guard connectionOperation == operation else { return }
            pendingTransfers = jobs
            pendingRemovals = removals
            pendingReaderImports = imports
        } catch {
            if connectionOperation == operation { error = String(localized: "Pending transfers could not be loaded.") }
        }
    }

    private func rememberReader(_ device: DeviceDescriptor, operation: UUID) async {
        guard let library else { return }
        do {
            try await library.saveReader(device)
            savedReaders = try await library.savedReaders()
            await reloadPendingTransfers(operation: operation)
        } catch {
            if connectionOperation == operation {
                error = String(localized: "The reader connected, but its details could not be saved.")
            }
        }
    }

    var connectionStatus: String {
        if wifiHandoffInProgress { return String(localized: "Wi-Fi transfer") }
        switch bluetoothState {
        case .unavailable: return String(localized: "Bluetooth unavailable")
        case .idle: return String(localized: "Disconnected")
        case .scanning: return String(localized: "Finding readers")
        case .connecting: return String(localized: "Connecting")
        case .ready: return connected == nil ? String(localized: "Authenticating") : String(localized: "Connected")
        }
    }

    private func startReaderImportCleanup() {
        guard importCleanupTask == nil, !transferBusy, !inventoryBusy, !connectionBusy,
              !readerImportActive, let readerImports else { return }
        importCleanupTask = Task {
            defer { importCleanupTask = nil }
            var cursor: UUID?
            do {
                repeat {
                    try Task.checkCancellation()
                    guard !transferBusy, !inventoryBusy, !connectionBusy, !readerImportActive else { return }
                    let report = try await readerImports.cleanupFinishedImports(after: cursor)
                    if !report.failedJobs.isEmpty {
                        self.error = String(localized: "Temporary reader import files could not be cleared.")
                    }
                    cursor = report.nextCursor
                } while cursor != nil
            } catch {
                if !Task.isCancelled, error as? ReaderImportRunnerError != .busy {
                    self.error = String(localized: "Temporary reader import files could not be cleared.")
                }
            }
        }
    }

    func reload() async {
        guard let library else { return }
        do {
            var loaded: [LibraryContent] = []
            let ids = try await library.libraryContentIDs()
            loaded.reserveCapacity(ids.count)
            for id in ids {
                if let content = try await library.content(id) { loaded.append(content) }
            }
            var identities: [ContentID: Data] = [:]
            var verified: Set<ContentID> = []
            var details: [ContentID: CoursePackDetails] = [:]
            for content in loaded where content.kind == .course {
                if let pack = try await library.coursePackDetails(content.id) {
                    verified.insert(content.id); details[content.id] = pack
                }
                if let identity = try await library.courseIdentity(content.id) { identities[content.id] = identity }
            }
            courseIdentities = identities; verifiedCourses = verified; courseDetails = details
            var removed: [LibraryContent] = []
            let removedIDs = try await library.deletedLibraryContentIDs()
            removed.reserveCapacity(removedIDs.count)
            for id in removedIDs {
                if let content = try await library.content(id) { removed.append(content) }
            }
            removedContents = removed
            contents = loaded.sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
            contentTitles = Dictionary(uniqueKeysWithValues: loaded.map { ($0.id, $0.title) })
            savedReaders = try await library.savedReaders()
            var selections: [Data: Set<ContentID>] = [:]
            for reader in savedReaders {
                selections[reader.id] = Set(try await library.readerSelections(reader: reader.id)
                    .filter(\.selected).map(\.content))
            }
            readerSelections = selections
            startReaderImportCleanup()
        } catch {
            self.error = String(localized: "Your library could not be loaded.")
        }
    }
    func title(for id: ContentID) -> String? { contentTitles[id] }

    func bookmarks(_ content: ContentID) async throws -> [BookmarkState] {
        guard let library else { throw StoreError.missingContent }
        return try await library.bookmarks(content: content.digest)
    }

    func canReviewCourseBaseline(_ content: ContentID) -> Bool {
        guard !transferBusy, !inventoryBusy, !connectionBusy, let session = authenticated,
              session.device.readerCapabilities.supportsCourseBaselineReview,
              verifiedCourses.contains(content), courseIdentities[content] != nil else { return false }
        return true
    }

    func reviewCourseBaseline(_ content: ContentID) async throws -> CourseBaselineReview {
        guard canReviewCourseBaseline(content), let session = authenticated,
              let course = courseIdentities[content] else { throw ReaderSessionError.busy }
        let operation = connectionOperation
        transferBusy = true
        defer { if connectionOperation == operation { transferBusy = false } }
        let review = try await baselineReviews.collect(session: session, course: course)
        try Task.checkCancellation()
        guard connectionOperation == operation, authenticated?.device.identity == review.reader,
              authenticated?.device.storageGeneration == review.generation,
              courseIdentities[content] == review.course else { throw ReaderSessionError.busy }
        return review
    }

    func canQueueCourseBaseline(_ content: ContentID, review: CourseBaselineReview) -> Bool {
        guard review.isolated, canReviewCourseBaseline(content), let session = authenticated,
              session.device.readerCapabilities.supportsCourseBaselineImport,
              session.device.identity == review.reader, session.device.storageGeneration == review.generation,
              courseIdentities[content] == review.course else { return false }
        return true
    }

    func queueCourseBaseline(_ content: ContentID, review: CourseBaselineReview) async -> Bool {
        guard canQueueCourseBaseline(content, review: review), let session = authenticated,
              let library, let contentVault else { return false }
        let operation = connectionOperation
        transferBusy = true
        defer { if connectionOperation == operation { transferBusy = false } }
        do {
            let object = try await contentVault.verifiedObject(content)
            let details = try CoursePackDetails(CoursePackInspector.inspect(object.url))
            guard try await library.coursePackDetails(content) == details else { throw VaultError.integrity }
            try Task.checkCancellation()
            guard connectionOperation == operation, authenticated?.device.identity == review.reader,
                  authenticated?.device.storageGeneration == review.generation,
                  courseIdentities[content] == review.course else { throw ReaderSessionError.busy }
            _ = try await library.queueCourseBaselineImport(content: content, review: review, installation: session.installation)
            await reload()
            await reloadPendingTransfers(operation: operation)
            return connectionOperation == operation
        } catch {
            if connectionOperation == operation {
                error = String(localized: "The original pack archive could not be queued. Refresh the reader review and try again.")
            }
            return false
        }
    }

    func switchInventory(_ content: ContentID) -> ReaderInventory? {
        guard !transferBusy, !inventoryBusy, !connectionBusy, let session = authenticated,
              session.device.readerCapabilities.supportsCourseSwitch, let inventory, inventory.complete,
              inventory.reader == session.device.identity, inventory.generation == session.device.storageGeneration,
              let course = courseIdentities[content], verifiedCourses.contains(content) else { return nil }
        let active = inventory.contents.filter { $0.kind == .course }
        guard active.count <= 1, let bound = inventory.boundCourse,
              bound.logicalIdentity.contains(where: { $0 != 0 }),
              bound.logicalIdentity != course else { return nil }
        return inventory
    }
    func queueCourseSwitch(_ content: ContentID, reviewed: ReaderInventory) async -> Bool {
        guard let current = switchInventory(content), current.reader == reviewed.reader,
              current.generation == reviewed.generation, current.contents == reviewed.contents,
              current.courseContext == reviewed.courseContext,
              let session = authenticated, let library, let contentVault else { return false }
        let operation = connectionOperation
        transferBusy = true
        defer { if connectionOperation == operation { transferBusy = false } }
        do {
            let object = try await contentVault.verifiedObject(content)
            let details = try CoursePackDetails(CoursePackInspector.inspect(object.url))
            guard try await library.coursePackDetails(content) == details else { throw VaultError.integrity }
            _ = try await library.queueCourseSwitch(content: content, inventory: reviewed, installation: session.installation)
            await reload()
            await reloadPendingTransfers(operation: operation)
            return true
        } catch {
            if connectionOperation == operation {
                error = String(localized: "The course switch could not be queued. Refresh the reader inventory and review the switch again.")
            }
            return false
        }
    }

    func tintaHistory(_ content: ContentID) async throws -> TintaSnapshot {
        guard let library, let course = try await library.courseIdentity(content) else { throw StoreError.missingContent }
        return try await library.replayTinta(course: course)
    }
    var canPreserveLearningBackup: Bool {
        authenticated != nil && !transferBusy && !inventoryBusy && !connectionBusy
    }
    func preserveTintaBackup(_ content: ContentID) async throws {
        guard canPreserveLearningBackup, let session = authenticated, let library, let contentVault,
              let backupStaging else { throw ReaderSessionError.busy }
        let operation = connectionOperation
        transferBusy = true
        defer { if connectionOperation == operation { transferBusy = false } }
        guard let course = try await library.courseIdentity(content) else { throw StoreError.missingContent }
        let key = "learning-backup-" + [session.device.identity, session.device.storageGeneration, course]
            .map { $0.map { String(format: "%02x", $0) }.joined() }.joined(separator: "-")
        let transaction = try await library.beginLegacyBackupExport(reader: session.device.identity,
            generation: session.device.storageGeneration, course: course,
            previousTransaction: UserDefaults.standard.data(forKey: key))
        UserDefaults.standard.removeObject(forKey: key)
        learningBackupProgress = nil
        let task = Task {
            try await learningBackups.collect(session: session, course: course, transaction: transaction,
                bound: true, capture: true, staging: backupStaging, library: library, vault: contentVault,
                progress: { [weak self] value in
                    guard let self else { return }
                    await self.updateLearningBackupProgress(value, operation: operation)
                })
        }
        learningBackupTask = task
        defer { if connectionOperation == operation { learningBackupTask = nil; learningBackupProgress = nil } }
        let backup = try await task.value
        try await library.completeLegacyBackupExport(reader: session.device.identity,
            generation: session.device.storageGeneration, course: course, transaction: transaction, backup: backup)
    }
    private func updateLearningBackupProgress(_ value: LegacyBackupProgress, operation: UUID) {
        if connectionOperation == operation { learningBackupProgress = value }
    }
    func pauseLearningBackup() { learningBackupTask?.cancel() }
    func tintaBackups(_ content: ContentID) async throws -> [VerifiedLegacyBackup] {
        guard let library, let contentVault, let course = try await library.courseIdentity(content) else {
            throw StoreError.missingContent
        }
        return try await library.verifiedLegacyBackups(course: course, vault: contentVault)
    }
    func tintaReadingConflicts(_ content: ContentID, backup: ContentID) async throws -> [LegacyReadingConflictDetails] {
        guard let library, let contentVault, let course = try await library.courseIdentity(content) else {
            throw StoreError.missingContent
        }
        return try await contentVault.legacyReadingOptions(backup: backup, course: content,
                                                          confirmedCourseIdentity: course)
    }
    func tintaOverlaps(_ backup: ContentID) async throws -> [LegacyHistoryOverlap] {
        guard let library, let contentVault else { throw StoreError.missingContent }
        return try await library.legacyOverlapCandidates(backup: backup, vault: contentVault)
    }
    func confirmSharedTintaBackup(_ backup: ContentID, canonical: ContentID) async throws {
        guard let library, let contentVault else { throw StoreError.missingContent }
        _ = try await library.confirmSharedLegacyBackup(backup, canonical: canonical, vault: contentVault)
    }
    func tintaBackupPreferences(_ backup: ContentID) async throws -> [PreferenceBody] {
        guard let contentVault else { throw StoreError.missingContent }
        return try await contentVault.legacyBackupSnapshot(backup).profile.portablePreferences()
    }
    func importTintaBackupPreferences(_ backup: ContentID, expected: [PreferenceBody]) async throws {
        guard let library, let contentVault else { throw StoreError.missingContent }
        let credentials = PairingVault(storage: KeychainCredentialStorage(service: "dev.lila.companion.credentials"))
        let origin = try await credentials.installationIdentity()
        _ = try await library.importLegacyPreferences(backup: backup, origin: origin, expected: expected, vault: contentVault)
    }
    func verifiedTintaBackup(_ backup: ContentID) async throws -> LegacyBackupManifest {
        guard let contentVault else { throw StoreError.missingContent }
        return try await contentVault.verifiedLegacyBackup(backup)
    }
    func sharedTintaBackup(_ backup: ContentID) async throws -> ContentID? {
        guard let library else { throw StoreError.missingContent }
        return try await library.sharedLegacyCanonical(backup)
    }
    func tintaMigrationReview(_ content: ContentID, backup: ContentID) async throws
        -> (SchedulerConfiguration, LegacyMigrationDraft?) {
        guard let library, let contentVault, let course = try await library.courseIdentity(content) else {
            throw StoreError.missingContent
        }
        let snapshot = try await contentVault.legacyBackupSnapshot(backup)
        guard snapshot.manifest.course == course else { throw StoreError.invalidValue }
        let draft = try await library.legacyMigrationDraft(backup)
        guard draft == nil || (draft?.course == content && draft?.confirmedCourseIdentity == course) else {
            throw StoreError.invalidValue
        }
        return (snapshot.profile.scheduler, draft)
    }
    func saveTintaMigrationReview(_ content: ContentID, backup: ContentID, configuration: SchedulerConfiguration,
        readings: [UInt32: Set<UInt32>], independent: Set<ContentID>) async throws -> LegacyMigrationDraft {
        guard let library, let contentVault, let course = try await library.courseIdentity(content) else {
            throw StoreError.missingContent
        }
        _ = try await library.saveLegacyMigrationDraft(backup: backup, course: content,
            confirmedCourseIdentity: course, configuration: configuration, vault: contentVault,
            confirmedReadingResolutions: readings, confirmedIndependentBackups: independent)
        guard let draft = try await library.legacyMigrationDraft(backup), draft.course == content,
              draft.confirmedCourseIdentity == course, draft.confirmedReadingResolutions == readings,
              Set(draft.independentBackups) == independent,
              draft.retentionBasisPoints == configuration.retentionBasisPoints,
              draft.maximumInterval == configuration.maximumInterval else { throw StoreError.invalidValue }
        return draft
    }
    func applyTintaMigration(_ content: ContentID, backup: ContentID, expectedDraft: LegacyMigrationDraft) async throws {
        guard let library, let contentVault, let draft = try await library.legacyMigrationDraft(backup),
              let course = try await library.courseIdentity(content), draft.course == content,
              draft.confirmedCourseIdentity == course, draft == expectedDraft else { throw StoreError.invalidValue }
        let credentials = PairingVault(storage: KeychainCredentialStorage(service: "dev.lila.companion.credentials"))
        let origin = try await credentials.installationIdentity()
        let plan = try await library.prepareLegacyMigration(backup: backup, origin: origin, vault: contentVault,
                                                           expectedDraft: expectedDraft)
        _ = try await library.installLegacyMigration(plan, backup: backup, vault: contentVault, expectedDraft: expectedDraft)
    }

    var canInstallTintaMigration: Bool {
        canPreserveLearningBackup && inventory?.complete == true &&
            (authenticated?.device.capabilities ?? 0) & JournalExportPage.capability != 0
    }
    func savedTintaMigration(_ backup: ContentID) async throws -> TintaMigrationJob? {
        guard let library, let session = authenticated else { return nil }
        return try await library.latestTintaMigration(backup: backup, reader: session.device.identity,
                                                      generation: session.device.storageGeneration)
    }
    func installTintaMigration(_ content: ContentID, backup: ContentID, expectedDraft: LegacyMigrationDraft?) async throws -> TintaMigrationJob {
        guard canInstallTintaMigration, let session = authenticated, let library, let contentVault else {
            throw ReaderSessionError.busy
        }
        let operation = connectionOperation
        transferBusy = true
        defer {
            if connectionOperation == operation {
                transferBusy = false; migrationTask = nil
                if wifiHandoffInProgress {
                    wifiHandoffInProgress = false
                    authenticated = nil; connected = nil; inventory = nil
                    bluetooth?.disconnect()
                }
            }
        }
        let task = Task {
            let job: TintaMigrationJob
            if let saved = try await library.latestTintaMigration(backup: backup, reader: session.device.identity,
                                                                   generation: session.device.storageGeneration), saved.abortState != .completed {
                guard saved.admission.resource == content.digest else { throw StoreError.conflictingJob }
                job = saved
            } else {
                let currentInventory = try await inventories.collect(session: session, maximumEntries: 100_000)
                let exported = try await journalExports.collect(session: session, library: library, maximumEvents: 100_000)
                let snapshot = try await session.journalState(requestID: UInt32.random(in: 1...UInt32.max))
                try Task.checkCancellation()
                job = try await library.prepareReaderTintaMigration(backup: backup, content: content, owner: session.installation,
                    previous: exported.mutations, snapshot: snapshot, inventory: currentInventory, vault: contentVault,
                    expectedDraft: expectedDraft)
            }
            try Task.checkCancellation()
            if wifiAssistance, job.phase != .completed,
               UInt64(job.admission.merge.merged.count - job.acknowledgedCount) * 1024 > 1024 * 1024 {
                return try await migrateThroughWifi(job, session: session, library: library, operation: operation)
            }
            return try await migrations.run(transaction: job.id, session: session, library: library)
        }
        migrationTask = task
        let completed = try await task.value
        guard connectionOperation == operation else { throw CancellationError() }
        return completed
    }
    func abortTintaMigration(_ job: TintaMigrationJob) async throws -> TintaMigrationJob {
        guard canInstallTintaMigration, let session = authenticated, let library else { throw ReaderSessionError.busy }
        guard job.admission.reader == session.device.identity,
              job.admission.merge.generation == session.device.storageGeneration,
              job.admission.merge.owner == session.installation else { throw ReaderSessionError.wrongReader }
        let operation = connectionOperation
        transferBusy = true
        defer { if connectionOperation == operation { transferBusy = false; migrationTask = nil } }
        let task = Task { try await migrations.abort(transaction: job.id, session: session, library: library) }
        migrationTask = task
        let aborted = try await task.value
        guard connectionOperation == operation else { throw CancellationError() }
        return aborted
    }
    func pauseTintaMigration() { migrationTask?.cancel() }

    func resolveBookmark(_ content: ContentID, bookmark: BookmarkState, value: BookmarkValue?) async -> Bool {
        guard let library else { return false }
        do {
            let credentials = PairingVault(storage: KeychainCredentialStorage(service: "dev.lila.companion.credentials"))
            let origin = try await credentials.installationIdentity()
            let body = try BookmarkBody(identity: bookmark.identity, value: value)
            _ = try await library.resolveBookmark(origin: origin, content: content.digest,
                bookmark: body, expectedHeads: bookmark.resolutionAncestors)
            return true
        } catch {
            error = String(localized: "The bookmark could not be resolved. Refresh bookmarks and try again.")
            return false
        }
    }

    func portablePreferences() async throws -> [PreferenceState] {
        guard let library else { throw StoreError.missingContent }
        return try await library.preferences()
    }

    func resolvePreference(_ state: PreferenceState, candidate: PreferenceCandidate) async -> Bool {
        guard let library, candidate.body.key == state.key, state.candidates.contains(candidate) else { return false }
        do {
            let credentials = PairingVault(storage: KeychainCredentialStorage(service: "dev.lila.companion.credentials"))
            let origin = try await credentials.installationIdentity()
            _ = try await library.resolvePreference(origin: origin, preference: candidate.body,
                expectedHeads: state.resolutionAncestors)
            return true
        } catch {
            error = String(localized: "The preference could not be resolved. Refresh preferences and try again.")
            return false
        }
    }

    func readingPositions(_ content: ContentID) async throws -> ReadingPositions {
        guard let library else { throw StoreError.missingContent }
        return try await library.readingPositions(content: content.digest)
    }

    func resolvePosition(_ content: ContentID, position: ReadingPosition, heads: [EventIdentity]) async -> Bool {
        guard let library else { return false }
        do {
            let credentials = PairingVault(storage: KeychainCredentialStorage(service: "dev.lila.companion.credentials"))
            let origin = try await credentials.installationIdentity()
            _ = try await library.resolveReadingPosition(origin: origin, content: content.digest,
                anchor: position.anchor, expectedHeads: heads)
            return true
        } catch {
            error = String(localized: "The reading position could not be resolved. Refresh the positions and try again.")
            return false
        }
    }


    func changeLibraryVisibility(_ id: ContentID, removed: Bool) async {
        guard !changingLibrary, let library else { return }
        changingLibrary = true
        defer { changingLibrary = false }
        do {
            let credentials = PairingVault(storage: KeychainCredentialStorage(service: "dev.lila.companion.credentials"))
            let origin = try await credentials.installationIdentity()
            _ = try await library.setLibraryVisibility(id, removed: removed, origin: origin)
            await reload()
            await reloadPendingTransfers(operation: connectionOperation)
        } catch { error = String(localized: "The library change could not be saved.") }
    }

    func phaseLabel(for job: TransferJob) -> String {
        switch job.phase {
        case .queued: String(localized: "Queued")
        case .transferring: String(localized: "Transferring")
        case .committing: String(localized: "Finishing installation")
        case .paused: String(localized: "Paused")
        case .completed: String(localized: "Completed")
        case .failed: String(localized: "Needs attention")
        case .aborted: String(localized: "Cancelled")
        }
    }


    func confirmCourse(_ id: ContentID, identity: Data) async -> Bool {
        guard let library else { return false }
        do {
            try await library.associateCourse(id, confirmedIdentity: identity)
            await reload()
            return courseIdentities[id] == identity
        } catch {
            error = String(localized: "The course association could not be saved. Existing learning history was not changed.")
            return false
        }
    }


    func setSelection(reader: Data, content: ContentID, selected: Bool) async {
        guard !savingSelection, let library else { return }
        savingSelection = true
        defer { savingSelection = false }
        do {
            try await library.setReaderSelection(reader: reader, content: content, selected: selected)
            readerSelections[reader] = Set(try await library.readerSelections(reader: reader)
                .filter(\.selected).map(\.content))
        } catch {
            error = String(localized: "The reader’s content selection could not be saved.")
        }
    }

    var canImport: Bool { importer != nil && !importing }

    @discardableResult
    func requestImport(_ urls: [URL]) -> Bool {
        guard canImport, !urls.isEmpty, urls.allSatisfy(\.isFileURL) else { return false }
        importing = true
        let scopes = urls.map { ($0, $0.startAccessingSecurityScopedResource()) }
        Task {
            defer {
                for (url, scoped) in scopes where scoped { url.stopAccessingSecurityScopedResource() }
                importing = false
            }
            await importFiles(urls)
        }
        return true
    }

    func checkStableRelease() {
        guard !releaseCheckBusy, !firmwareDownloadBusy else { return }
        releaseCheckBusy = true
        releaseCheckTask = Task {
            defer { releaseCheckBusy = false; releaseCheckTask = nil }
            do { releaseManifest = try await releaseDiscovery.latest() }
            catch is CancellationError { }
            catch { error = String(localized: "Stable releases could not be checked. Try again or import a release manifest.") }
        }
    }
    func cancelReleaseCheck() { releaseCheckTask?.cancel() }

    func importReleaseManifest(_ url: URL) {
        guard !firmwareDownloadBusy, !releaseCheckBusy else { return }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let file = try FileHandle(forReadingFrom: url)
            defer { try? file.close() }
            let bytes = try file.read(upToCount: 262145) ?? Data()
            releaseManifest = try CompanionReleaseManifest.decode(bytes, allowReleaseCandidates: allowReleaseCandidates)
        } catch {
            releaseManifest = nil
            error = String(localized: "The release manifest could not be imported.")
        }
    }
    var canDownloadFirmware: Bool { importer != nil && !firmwareDownloadBusy && !releaseCheckBusy }
    func downloadFirmware(_ asset: CompanionReleaseManifest.FirmwareAsset) {
        guard !firmwareDownloadBusy, !releaseCheckBusy, let importer else { return }
        firmwareDownloadBusy = true
        firmwareDownloadTask = Task {
            defer { firmwareDownloadBusy = false; firmwareDownloadTask = nil }
            do {
                _ = try await FirmwareDownloader(importer: importer).download(asset)
                await reload()
            } catch is CancellationError { }
            catch { error = String(localized: "Firmware could not be downloaded or verified.") }
        }
    }
    func cancelFirmwareDownload() { firmwareDownloadTask?.cancel() }

    private func importFiles(_ urls: [URL]) async {
        guard let importer else { return }
        for url in urls {
            do {
                try Task.checkCancellation()
                _ = try await importer.importFile(url)
            } catch is CancellationError {
                break
            } catch {
                self.error = String(localized: "A file could not be imported. Check that it is complete and supported.")
            }
        }
        await reload()
    }
}

@main @MainActor
struct LilaCompanionApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @State private var model = CompanionModel()
    @State private var showImporter = false
    @State private var removalCandidate: LibraryContent?

    var body: some Scene {
        WindowGroup {
            TabView {
                Tab("Devices", systemImage: "externaldrive") {
                    DevicesView(model: model)
                }
                Tab("Library", systemImage: "books.vertical") {
                    NavigationStack {
                        List {
                            ForEach(model.contents, id: \.id) { content in
                                Group {
                                    if content.kind == .course {
                                        NavigationLink {
                                            CourseAssociationView(model: model, content: content)
                                        } label: { LibraryContentLabel(content: content) }
                                    } else if content.kind == .epub {
                                        NavigationLink { ReadingPositionsView(model: model, content: content) }
                                            label: { LibraryContentLabel(content: content) }
                                    } else { LibraryContentLabel(content: content) }
                                }
                                .contextMenu {
                                    Button("Remove from library", role: .destructive) { removalCandidate = content }
                                        .disabled(model.changingLibrary)
                                }
                            }
                            if !model.removedContents.isEmpty {
                                Section("Removed content") {
                                    ForEach(model.removedContents, id: \.id) { content in
                                        VStack(alignment: .leading) {
                                            LibraryContentLabel(content: content)
                                            Button("Restore to library") {
                                                Task { await model.changeLibraryVisibility(content.id, removed: false) }
                                            }.disabled(model.changingLibrary)
                                        }
                                    }
                                }
                            }
                        }
                        .overlay {
                            if model.contents.isEmpty && model.removedContents.isEmpty {
                                ContentUnavailableView("Your library is empty", systemImage: "books.vertical",
                                    description: Text("Import books and Tinta course packs to get started."))
                            }
                        }
                        .navigationTitle("Library")
                        .toolbar {
                            Button("Import files", systemImage: "plus") { showImporter = true }
                                .accessibilityIdentifier("library.import")
                                .disabled(!model.canImport)
                        }
                    }
                    .dropDestination(for: URL.self) { urls, _ in model.requestImport(urls) }
                }
                Tab("Updates", systemImage: "arrow.down.circle") {
                    NavigationStack {
                        FirmwareUpdatesView(model: model)
                    }
                }
                Tab("Settings", systemImage: "gearshape") {
                    NavigationStack {
                        Form {
                            Section("Library") {
                                LabeledContent("Storage", value: String(localized: "On this device"))
                            }
                            Section("Preferences") {
                                NavigationLink("Portable preferences") { PortablePreferencesView(model: model) }
                                    .accessibilityIdentifier("settings.preferences")
                            }
                            CloudSettingsSection(model: model)
                        }.navigationTitle("Settings")
                    }
                }
            }
            .tabViewStyle(.sidebarAdaptable)
            .onChange(of: scenePhase) { _, phase in
                if phase == .active {
                    Task { await model.resumeCloudIfEnabled() }
                    Task { await model.resumeReaderWorkIfAvailable() }
                }
            }
            .task { await model.reload(); await model.resumeCloudIfEnabled() }
            .confirmationDialog("Remove this content from your library?", isPresented: Binding(
                get: { removalCandidate != nil }, set: { if !$0 { removalCandidate = nil } })) {
                    Button("Remove from library", role: .destructive) {
                        guard let content = removalCandidate else { return }
                        removalCandidate = nil
                        Task { await model.changeLibraryVisibility(content.id, removed: true) }
                    }
                    Button("Cancel", role: .cancel) { removalCandidate = nil }
                } message: {
                    Text("This removes the selection from all readers and retains files needed for recovery. The library decision is shared with your other apps when iCloud sync is enabled. Supported content is removed from compatible readers during synchronization. Restoring does not select the content again.")
                }
            .onOpenURL { url in
                guard url.isFileURL else {
                    model.error = String(localized: "Only local content files can be imported.")
                    return
                }
                if !model.requestImport([url]) {
                    model.error = String(localized: "The file could not be accepted. Wait for the current import to finish and try again.")
                }
            }
            .fileImporter(isPresented: $showImporter, allowedContentTypes: [.data], allowsMultipleSelection: true) { result in
                switch result {
                case .success(let urls): model.requestImport(urls)
                case .failure: model.error = String(localized: "The selected files could not be opened.")
                }
            }
            .alert("Unable to complete the action", isPresented: Binding(
                get: { model.error != nil }, set: { if !$0 { model.error = nil } })) {
                    Button("OK", role: .cancel) { model.error = nil }
                } message: { Text(model.error ?? "") }
        }
        #if os(macOS)
        .commands {
            CommandGroup(after: .newItem) {
                Button("Import files…") { showImporter = true }
                    .keyboardShortcut("o", modifiers: .command)
                    .disabled(!model.canImport)
            }
        }
        #endif
    }
}

private struct DevicesView: View {
    @Bindable var model: CompanionModel
    @State private var removalChoice: ContentManifest?
    @State private var historyCancelChoice: JournalMergeJob?
    var body: some View {
        NavigationStack {
            List {
                Section("Connection") {
                    LabeledContent("Status", value: model.connectionStatus)
                    if let device = model.connected {
                        LabeledContent("Battery") { Text(Double(device.batteryPercent) / 100, format: .percent) }
                        LabeledContent("Reader identity", value: device.identity.map { String(format: "%02x", $0) }.joined())
                        Button("Disconnect") { model.disconnectReader() }
                        NavigationLink("Choose content for this reader") {
                            ReaderContentSelectionView(model: model, reader: device.identity)
                        }
                    } else if model.pairingRequired {
                        Text("Pair this reader with this app installation.")
                        Button("Pair reader") { model.pairReader() }.disabled(model.connectionBusy)
                        Button("Cancel", role: .cancel) { model.disconnectReader() }
                    } else {
                        Button("Find readers") { model.findReaders() }
                            .accessibilityIdentifier("devices.find")
                            .disabled(model.connectionBusy)
                        if model.connectionBusy {
                            ProgressView()
                            Button("Cancel", role: .cancel) { model.disconnectReader() }
                        }
                    }
                }
                if model.connected == nil, !model.pairingRequired {
                    Section("Nearby readers") {
                        ForEach(model.readers) { reader in
                            Button { model.connectReader(reader) } label: {
                                Label(reader.advertisedName ?? String(localized: "Reader"), systemImage: "externaldrive")
                            }.disabled(model.connectionBusy)
                        }
                        if model.readers.isEmpty { Text("Open Connect & Sync on your reader, then find readers.") }
                    }
                }
                if let request = model.wifiManualJoin {
                    Section("Join Wi-Fi") {
                        Text("Use your system Wi-Fi controls to join this network, then continue.")
                        LabeledContent("Network", value: request.ssid)
                        if let password = request.password {
                            LabeledContent("Password", value: password).privacySensitive()
                        }
                        Button("Connected; continue") { request.confirm() }
                        Button("Cancel Wi-Fi transfer", role: .cancel) { request.cancel() }
                    }
                }
                if let notice = model.transferNotice {
                    Section { Text(notice) }
                }
                if model.connected != nil {
                    Section("Content transfers") {
                        Toggle("Use Wi-Fi for large transfers", isOn: $model.wifiAssistance)
                            .disabled(model.transferBusy)
                        if model.wifiAssistance {
                            Picker("Wi-Fi network", selection: $model.wifiNetworkMode) {
                                Text("Reader hotspot").tag(WifiNetworkMode.hotspot)
                                Text("Saved reader network").tag(WifiNetworkMode.savedNetwork)
                            }.disabled(model.transferBusy)
                        }
                        Button("Transfer selected content") { model.transferSelectedContent() }
                            .disabled(!model.canTransferSelectedContent)
                        if model.transferBusy {
                            ProgressView("Transferring content…")
                            Button("Pause transfer") { model.pauseContentTransfer() }
                        }
                        ForEach(model.pendingTransfers, id: \.id) { job in
                            VStack(alignment: .leading) {
                                Text(model.title(for: job.content) ?? String(localized: "Content"))
                                Text(model.phaseLabel(for: job)).font(.caption)
                                if let device = model.connected, job.storageGeneration != device.storageGeneration {
                                    Text("This transfer belongs to a different SD card.").font(.caption)
                                }
                                Text(job.durableOffset, format: .number).font(.caption)
                                Text("Bytes saved on reader").font(.caption)
                                if job.phase != .committing, job.phase != .completed, job.phase != .aborted {
                                    Button("Cancel transfer", role: .destructive) { model.cancelContentTransfer(job) }
                                        .disabled(model.transferBusy || model.inventoryBusy || model.connectionBusy ||
                                            job.storageGeneration != model.connected?.storageGeneration)
                                }
                            }.accessibilityElement(children: .combine)
                        }
                        ForEach(model.pendingReaderImports, id: \.id) { job in
                            VStack(alignment: .leading) {
                                Text("Reader import pending")
                                Text(job.manifest.content.hex).font(.caption).foregroundStyle(.secondary)
                                Text(job.acknowledgedOffset, format: .number).font(.caption)
                                Text("Bytes saved in companion").font(.caption)
                                if job.generation != model.connected?.storageGeneration {
                                    Text("This import belongs to a different SD card.").font(.caption)
                                }
                                Button("Cancel reader import", role: .destructive) {
                                    Task { await model.cancelReaderImport(job) }
                                }.disabled(!model.canCancelReaderImport)
                            }.accessibilityElement(children: .combine)
                        }
                        ForEach(model.pendingRemovals, id: \.id) { job in
                            VStack(alignment: .leading) {
                                Text(model.title(for: job.request.manifest.content) ?? String(localized: "Content"))
                                Text("Reader removal pending").font(.caption)
                                if job.request.generation != model.connected?.storageGeneration {
                                    Text("This removal belongs to a different SD card.").font(.caption)
                                }
                            }
                        }
                        Text("Refresh installed content before starting or resuming transfers.")
                    }
                    Section("Installed content") {
                        Button("Refresh and sync") { model.refreshInventory() }
                            .disabled(model.inventoryBusy || model.connectionBusy || model.transferBusy)
                        if model.inventoryBusy {
                            ProgressView()
                        } else if let inventory = model.inventory {
                            ForEach(inventory.contents, id: \.content) { entry in
                                VStack(alignment: .leading) {
                                    Text(model.title(for: entry.content) ?? String(localized: "Content"))
                                    Text(entry.content.hex).font(.caption).foregroundStyle(.secondary)
                                    let pending = model.pendingReaderImport(for: entry)
                                    if model.connected?.readerCapabilities.supportsContentMetadata == true,
                                       pending != nil || !model.contents.contains(where: { $0.id == entry.content }),
                                       !model.removedContents.contains(where: { $0.id == entry.content }) {
                                        Button(pending != nil ? String(localized: "Resume reader import") : String(localized: "Import into library")) {
                                            model.importInstalledContent(entry, resuming: pending)
                                        }.disabled(!model.canImportInstalledContent(entry))
                                    }
                                    if model.connected?.readerCapabilities.supportsRemoval(of: entry.kind) == true {
                                        Button("Remove from this reader", role: .destructive) { removalChoice = entry }
                                            .disabled(!model.canRemoveInstalledContent(entry))
                                    }
                                }.accessibilityElement(children: .combine)
                            }
                            if inventory.contents.isEmpty { Text("No installed content") }
                        } else if let error = model.inventoryError {
                            Text(error)
                        } else {
                            Text("Refresh to see this reader’s installed content.")
                        }
                        if model.readerImportActive {
                            Button("Pause reader import") { model.pauseReaderImport() }
                        }
                        if model.historySyncActive {
                            Button("Pause history sync") { model.pauseHistorySync() }
                        }
                        if let job = model.historySyncJob, job.phase != .completed, job.abortState != .completed {
                            Button(job.abortState == .requested ? String(localized: "Retry history cancellation") :
                                job.phase == .committing ? String(localized: "Check result or cancel history upload") :
                                String(localized: "Cancel pending history upload"), role: .destructive) {
                                historyCancelChoice = job
                            }.disabled(!model.canCancelHistorySync)
                        }
                        if let notice = model.historySyncNotice { Text(notice) }
                        if let error = model.historySyncError { Text(error) }
                        if model.historySyncNeedsPreferences {
                            NavigationLink("Resolve portable preference conflicts") { PortablePreferencesView(model: model) }
                        }
                    }
                }
                if !model.savedReaders.isEmpty {
                    Section("Saved readers") {
                        ForEach(model.savedReaders) { reader in
                            VStack(alignment: .leading) {
                                Text(reader.device.identity.map { String(format: "%02x", $0) }.joined()).font(.caption)
                                LabeledContent("Last connected") { Text(reader.lastConnected, format: .dateTime) }
                                LabeledContent("Last successful sync") {
                                    if let date = reader.lastSuccessfulSync { Text(date, format: .dateTime) }
                                    else { Text("No completed sync") }
                                }
                                NavigationLink("Choose content for this reader") {
                                    ReaderContentSelectionView(model: model, reader: reader.id)
                                }
                            }.accessibilityElement(children: .combine)
                        }
                    }
                }
            }.navigationTitle("Devices")
                .onChange(of: model.connected?.identity) { _, _ in historyCancelChoice = nil }
                .onChange(of: model.connected?.storageGeneration) { _, _ in historyCancelChoice = nil }
                .confirmationDialog("Cancel pending history upload", isPresented: Binding(
                    get: { historyCancelChoice != nil }, set: { if !$0 { historyCancelChoice = nil } })) {
                    if let job = historyCancelChoice {
                        Button("Check result or cancel upload", role: .destructive) {
                            historyCancelChoice = nil
                            Task {
                                do { try await model.cancelHistorySync(job) }
                                catch is CancellationError { }
                                catch { model.error = String(localized: "Cancellation was not confirmed. Reconnect to the same reader and retry cancellation.") }
                            }
                        }
                    }
                    Button("Keep upload", role: .cancel) { historyCancelChoice = nil }
                } message: {
                    Text("Uncommitted uploads will be cancelled. Already committed history is retained, and the library history remains available.")
                }
                .confirmationDialog("Remove this content from the reader?", isPresented: Binding(
                    get: { removalChoice != nil }, set: { if !$0 { removalChoice = nil } })) {
                    if let manifest = removalChoice {
                        Button("Remove from this reader", role: .destructive) {
                            removalChoice = nil
                            Task { await model.removeInstalledContent(manifest) }
                        }
                    }
                    Button("Cancel", role: .cancel) { removalChoice = nil }
                } message: {
                    if removalChoice?.kind == .font {
                        Text("The companion library copy is retained. If this font is in use, the reader switches to a built-in font. An interrupted removal will resume when you synchronize this reader.")
                    } else {
                        Text("The companion library copy is retained. An interrupted removal will resume when you synchronize this reader.")
                    }
                }
        }
    }
}


private struct LibraryContentLabel: View {
    let content: LibraryContent
    var body: some View {
        VStack(alignment: .leading) {
            Text(content.title).font(.headline)
            Text(content.originalFilename).font(.caption).foregroundStyle(.secondary)
            if !content.languages.isEmpty {
                Text(content.languages.joined(separator: ", ")).font(.caption)
            }
        }.accessibilityElement(children: .combine)
    }
}

private struct CourseBaselineReviewView: View {
    @Bindable var model: CompanionModel
    let content: LibraryContent
    @State private var review: CourseBaselineReview?
    @State private var loading = false
    @State private var failed = false
    @State private var attempt = 0
    @State private var confirmingOriginal = false
    @State private var saving = false
    @State private var queued = false

    var body: some View {
        Form {
            Section { LibraryContentLabel(content: content) }
            if loading { ProgressView("Reading learning files…") }
            if failed {
                Section {
                    Text("Learning files could not be reviewed. Reconnect the reader and try again.")
                    Button("Retry") { attempt += 1 }
                        .disabled(!model.canReviewCourseBaseline(content.id))
                }
            }
            if let review, model.connected?.identity == review.reader,
               model.connected?.storageGeneration == review.generation {
                Section("Reader review") {
                    LabeledContent("Reader identity", value: hex(review.reader))
                    LabeledContent("Storage generation", value: hex(review.generation))
                    if review.isolated {
                        LabeledContent("Course identity", value: hex(review.course))
                    } else {
                        LabeledContent("Proposed course identity", value: hex(review.course))
                        Text("These learning files are not yet assigned to a course. Review and migration approval are required before they can be isolated or merged.")
                    }
                    LabeledContent("Review hash", value: hex(review.hash))
                    Text("This review records the reader’s files at one moment. Confirming the original pack and checking learning history are required before installation.")
                }
                Section("Learning files") {
                    ForEach(review.files.indices, id: \.self) { index in
                        let file = review.files[index]
                        VStack(alignment: .leading, spacing: 4) {
                            Text(file.name).font(.headline)
                            if file.present { Text("Present") } else { Text("Missing") }
                            if file.present {
                                Text(ByteCountFormatter.string(fromByteCount: Int64(file.length), countStyle: .file))
                                Text(hex(file.hash)).font(.caption.monospaced()).textSelection(.enabled)
                            }
                        }.accessibilityElement(children: .combine)
                    }
                }
                Button("Refresh reader review") { attempt += 1 }
                    .disabled(saving || !model.canReviewCourseBaseline(content.id))
                Section("Original learning pack") {
                    Text("Confirm only if this is the original pack used to create these learning files. Matching the language or course title is not enough.")
                    Button("Confirm original pack…") { confirmingOriginal = true }
                        .disabled(saving || queued || !model.canQueueCourseBaseline(content.id, review: review))
                    if queued {
                        Text("Original pack archive queued. Use Transfer selected content to send it to the reader.")
                    }
                }
            }
        }
        .navigationTitle("Reader learning review")
        .confirmationDialog("Is this the original learning pack?", isPresented: $confirmingOriginal, titleVisibility: .visible) {
            if let review {
                Button("Confirm and queue original pack") {
                    saving = true
                    Task {
                        queued = await model.queueCourseBaseline(content.id, review: review)
                        saving = false
                    }
                }
            }
            Button("Cancel", role: .cancel) { }
        } message: {
            Text("The reader will preserve an archive and check it against the reviewed learning files. This confirmation does not authorize a different course or resolve ambiguous legacy history.")
        }
        .task(id: attempt) {
            review = nil; failed = false; loading = true; queued = false
            defer { loading = false }
            do { review = try await model.reviewCourseBaseline(content.id) }
            catch is CancellationError { }
            catch { failed = true }
        }
    }

    private func hex(_ bytes: Data) -> String {
        bytes.map { String(format: "%02x", $0) }.joined()
    }
}

private struct CourseAssociationView: View {
    @Bindable var model: CompanionModel
    let content: LibraryContent
    @State private var pendingIdentity: Data?
    @State private var saving = false
    @State private var pendingSwitch: ReaderInventory?
    @State private var switchQueued = false
    private var existingCourses: [LibraryContent] {
        guard content.languages.count == 1, let language = content.languages.first else { return [] }
        return model.contents.filter {
            $0.kind == .course && $0.id != content.id && model.courseIdentities[$0.id] != nil &&
                $0.languages.count == 1 && $0.languages.first?.lowercased() == language.lowercased()
        }
    }
    var body: some View {
        Form {
            Section { LibraryContentLabel(content: content) }
            if let pack = model.courseDetails[content.id] {
                Section("Pack details") {
                    LabeledContent("Language", value: Locale.current.localizedString(forIdentifier: pack.locale) ?? pack.locale)
                    LabeledContent("Pack format", value: "\(pack.major).\(pack.minor)")
                    LabeledContent("Content version", value: String(pack.contentVersion))
                    LabeledContent("Content hash") {
                        Text(content.id.digest.map { String(format: "%02x", $0) }.joined())
                            .font(.caption.monospaced())
                            .textSelection(.enabled)
                    }
                }
            }
            if model.courseIdentities[content.id] != nil {
                Section("Course identity") {
                    Text("Course identity confirmed")
                    Text("This association is saved. Compatibility checks are still required before learning history can be applied.")
                    if model.switchInventory(content.id) != nil {
                        Button("Switch connected reader to this course") {
                            pendingSwitch = model.switchInventory(content.id)
                        }.disabled(saving)
                    }
                    if switchQueued {
                        Text("Course switch queued. Use Transfer selected content to install it.")
                    }
                    NavigationLink("Review connected reader’s learning files") {
                        CourseBaselineReviewView(model: model, content: content)
                    }.disabled(!model.canReviewCourseBaseline(content.id))
                    NavigationLink("Saved learning history") { TintaHistoryView(model: model, content: content) }
                    NavigationLink("Preserved learning backups") { TintaBackupListView(model: model, content: content) }
                }
            } else if !model.verifiedCourses.contains(content.id) {
                Text("Import this pack again to validate it before confirming its course identity.")
            } else {
                Section("New course") {
                    Text("Choose this for a different course with separate learning history.")
                    Text("A pack for another language needs its own course identity.")
                    Button("Create a new course identity") {
                        let value = UUID().uuid
                        pendingIdentity = withUnsafeBytes(of: value) { Data($0) }
                    }.disabled(saving)
                }
                if !existingCourses.isEmpty {
                    Section("Edition of an existing course") {
                        Text("Choose an existing course only when this pack is an edition of the same course. Matching language or title is not enough.")
                        ForEach(existingCourses, id: \.id) { course in
                            Button { pendingIdentity = model.courseIdentities[course.id] } label: {
                                LibraryContentLabel(content: course)
                            }.disabled(saving)
                        }
                    }
                }
                if saving { ProgressView("Saving course identity…") }
            }
        }
        .navigationTitle("Tinta course")
        .confirmationDialog("Switch the reader’s Tinta course?", isPresented: Binding(
            get: { pendingSwitch != nil }, set: { if !$0 { pendingSwitch = nil } })) {
                Button("Confirm switch and queue") {
                    guard let reviewed = pendingSwitch else { return }
                    pendingSwitch = nil; saving = true
                    Task {
                        switchQueued = await model.queueCourseSwitch(content.id, reviewed: reviewed)
                        saving = false
                    }
                }
                Button("Cancel", role: .cancel) { pendingSwitch = nil }
            } message: {
                Text("This replaces the reader’s active course pack. Learning progress stays separate for each course and the previous course’s files are preserved. Other course selections for this reader will be cleared.")
            }
        .confirmationDialog("Confirm course identity", isPresented: Binding(
            get: { pendingIdentity != nil }, set: { if !$0 { pendingIdentity = nil } })) {
                Button("Confirm") {
                    guard let identity = pendingIdentity else { return }
                    pendingIdentity = nil; saving = true
                    Task { _ = await model.confirmCourse(content.id, identity: identity); saving = false }
                }
                Button("Cancel", role: .cancel) { pendingIdentity = nil }
            } message: {
                Text("This choice is permanent for this pack. It records course identity and does not move or apply learning history.")
            }
    }
}


private struct TintaBackupListView: View {
    @Bindable var model: CompanionModel
    let content: LibraryContent
    @State private var backups: [VerifiedLegacyBackup] = []
    @State private var loading = false
    @State private var failed = false
    @State private var preserving = false
    @State private var preservationFailed = false
    var body: some View {
        List {
            Section { LibraryContentLabel(content: content) }
            Section {
                Button("Preserve learning from connected reader") {
                    Task {
                        preserving = true; preservationFailed = false
                        defer { preserving = false }
                        do { try await model.preserveTintaBackup(content.id); await reload() }
                        catch is CancellationError { preservationFailed = false }
                        catch { preservationFailed = true }
                    }
                }.disabled(preserving || loading || !model.canPreserveLearningBackup)
                if preserving {
                    if let progress = model.learningBackupProgress {
                        ProgressView("Preserving learning backup…", value: Double(progress.completed), total: Double(max(1, progress.total)))
                    } else { ProgressView("Preserving learning backup…") }
                    Button("Pause backup") { model.pauseLearningBackup() }
                }
                if preservationFailed { Text("Learning backup could not be completed. Reconnect and retry.") }
            }
            if loading { ProgressView("Verifying learning backups…") }
            else if failed { Text("Learning backups could not be verified.") }
            else if backups.isEmpty { Text("No preserved learning backups") }
            else {
                ForEach(backups) { backup in
                    Section {
                        LabeledContent("Backup identity") { Text(backup.id.hex).textSelection(.enabled) }
                        LabeledContent("Source reader") { Text(hex(backup.manifest.reader)).textSelection(.enabled) }
                        LabeledContent("Card generation") { Text(hex(backup.manifest.generation)).textSelection(.enabled) }
                        ForEach(backup.manifest.files, id: \.role) { file in
                            LabeledContent(roleLabel(file.role)) { Text(file.length, format: .number) }
                        }
                        NavigationLink("Reading completion conflicts") {
                            TintaReadingConflictView(model: model, content: content, backup: backup.id)
                        }
                        NavigationLink("Overlapping learning history") {
                            TintaOverlapView(model: model, backup: backup.id)
                        }
                        NavigationLink("Review backed-up Tinta preferences") {
                            TintaBackupPreferencesView(model: model, backup: backup.id)
                        }
                        NavigationLink("Review migration decisions") {
                            TintaMigrationReviewView(model: model, content: content, backup: backup.id)
                        }
                    }
                }
            }
            Section {
                Text("These backups have been checked against their saved hashes. Connect the reader with this course installed to preserve its learning files. Review migration decisions before importing them into local history. Install reviewed learning history from the migration review screen.")
            }
        }
        .navigationTitle("Preserved learning backups")
        .toolbar { Button("Refresh") { Task { await reload() } }.disabled(loading) }
        .task { await reload() }
    }
    private func hex(_ value: Data) -> String { value.map { String(format: "%02x", $0) }.joined() }
    private func roleLabel(_ role: LegacyBackupRole) -> String {
        switch role {
        case .reviews: return String(localized: "Review journal bytes")
        case .items: return String(localized: "Item state bytes")
        case .profile: return String(localized: "Learner profile bytes")
        case .lessons: return String(localized: "Lesson completion bytes")
        case .readings: return String(localized: "Reading completion bytes")
        case .starred: return String(localized: "Starred item bytes")
        case .usage: return String(localized: "Diagnostic usage bytes")
        case .days: return String(localized: "Study day totals bytes")
        case .session: return String(localized: "Session state bytes")
        }
    }
    private func reload() async {
        guard !loading else { return }
        loading = true; failed = false
        defer { loading = false }
        do { backups = try await model.tintaBackups(content.id) }
        catch { backups = []; failed = true }
    }
}

private struct TintaBackupPreferencesView: View {
    @Bindable var model: CompanionModel
    let backup: ContentID
    @State private var preferences: [PreferenceBody] = []
    @State private var busy = false
    @State private var failed = false
    @State private var confirmation = false
    @State private var imported = false
    @State private var importFailed = false
    var body: some View {
        Form {
            LabeledContent("Backup identity") { Text(backup.hex).textSelection(.enabled) }
            if busy { ProgressView("Checking backed-up preferences…") }
            else if failed { Text("Backed-up preferences could not be verified.") }
            else {
                Section("Backed-up Tinta preferences") {
                    ForEach(preferences, id: \.key) { preference in
                        LabeledContent(PreferencePresentation.title(preference.key)) {
                            Text(PreferencePresentation.value(preference))
                        }
                    }
                }
                Button("Import these preferences") { confirmation = true }.disabled(preferences.count != 9)
            }
            if imported { Text("Backed-up preferences are saved in this app") }
            if importFailed { Text("Preferences could not be imported. Refresh and verify the backup before retrying.") }
            NavigationLink("Resolve portable preference conflicts") { PortablePreferencesView(model: model) }
            Text("These choices can be included in reader history installation. Conflicting saved preferences require an explicit choice. Device clock and other device-only settings stay on their reader.")
        }
        .disabled(busy)
        .navigationTitle("Review backed-up Tinta preferences")
        .toolbar { Button("Refresh") { Task { await reload() } }.disabled(busy) }
        .task { await reload() }
        .confirmationDialog("Import backed-up Tinta preferences", isPresented: $confirmation) {
            Button("Import preferences") {
                let expected = preferences
                busy = true; importFailed = false
                Task {
                    defer { busy = false }
                    do { try await model.importTintaBackupPreferences(backup, expected: expected); imported = true }
                    catch { importFailed = true }
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Import the displayed portable Tinta preferences from this verified backup. Existing choices remain available for conflict resolution. Retrying this import reuses its saved events. The original backup stays preserved.")
        }
    }
    private func reload() async {
        guard !busy else { return }
        busy = true; failed = false; importFailed = false
        defer { busy = false }
        do { preferences = try await model.tintaBackupPreferences(backup) }
        catch { preferences = []; failed = true }
    }
}

private struct TintaMigrationReviewView: View {
    @Bindable var model: CompanionModel
    let content: LibraryContent
    let backup: ContentID
    @State private var configuration: SchedulerConfiguration?
    @State private var conflicts: [LegacyReadingConflictDetails] = []
    @State private var overlaps: [LegacyHistoryOverlap] = []
    @State private var readings: [UInt32: Set<UInt32>] = [:]
    @State private var independent: Set<ContentID> = []
    @State private var busy = false
    @State private var failed = false
    @State private var saveFailed = false
    @State private var saved = false
    @State private var importConfirmation = false
    @State private var imported = false
    @State private var importFailed = false
    @State private var installConfirmation = false
    @State private var abortConfirmation = false
    @State private var abortFailed = false
    @State private var aborting = false
    @State private var installing = false
    @State private var installFailed = false
    @State private var preferenceConflict = false
    @State private var migrationJob: TintaMigrationJob?
    @State private var sourceManifest: LegacyBackupManifest?
    @State private var sharedBackup: ContentID?
    @State private var reviewedDraft: LegacyMigrationDraft?
    private var connectionBinding: Data? {
        guard let connected = model.connected else { return nil }
        return connected.identity + connected.storageGeneration
    }
    private var matchesBackupReader: Bool {
        guard let sourceManifest, let connected = model.connected else { return false }
        return sourceManifest.reader == connected.identity && sourceManifest.generation == connected.storageGeneration
    }
    private var hasActiveMigration: Bool {
        migrationJob != nil && migrationJob?.abortState != .completed
    }
    private var complete: Bool {
        configuration != nil && conflicts.allSatisfy { readings[$0.legacyKey] != nil }
            && overlaps.allSatisfy { independent.contains($0.secondBackup) }
    }
    var body: some View {
        Form {
            Section { LibraryContentLabel(content: content) }
            NavigationLink("Review backed-up Tinta preferences") {
                TintaBackupPreferencesView(model: model, backup: backup)
            }.disabled(installing)
            Text("Import backed-up preferences before starting a new installation. An installation already in progress keeps its original choices.")
            if busy { ProgressView("Preparing migration review…") }
            else if failed { Text("Migration review could not be prepared. Verify the backup and course association.") }
            else if let sharedBackup {
                Section("Confirmed shared learning history") {
                    Text(sharedBackup.hex).textSelection(.enabled)
                    Text("Installation reuses the confirmed shared history without importing this backup again.")
                }
            } else if let configuration {
                Section("Saved scheduler settings") {
                    LabeledContent("Retention basis points") { Text(configuration.retentionBasisPoints, format: .number) }
                    LabeledContent("Maximum interval days") { Text(configuration.maximumInterval, format: .number) }
                }
                ForEach(conflicts) { conflict in
                    Section("Choose completed readings") {
                        Text("Select every reading you completed, or explicitly choose none.")
                        ForEach(conflict.candidates) { candidate in
                            Toggle(isOn: Binding(get: { readings[conflict.legacyKey]?.contains(candidate.id) ?? false },
                                set: { selected in
                                    var values = readings[conflict.legacyKey] ?? []
                                    if selected { values.insert(candidate.id) } else { values.remove(candidate.id) }
                                    readings[conflict.legacyKey] = values; saved = false
                                })) {
                                    VStack(alignment: .leading) {
                                        Text(candidate.title)
                                        Text(candidate.kind == .dialogue ? String(localized: "Dialogue") : String(localized: "Story"))
                                        if let unit = candidate.unitNumber, let lesson = candidate.lessonNumber {
                                            LabeledContent("Unit") { Text(unit, format: .number) }
                                            LabeledContent("Lesson") { Text(lesson, format: .number) }
                                        }
                                    }
                                }
                        }
                        Button("None of these readings was completed") {
                            readings[conflict.legacyKey] = []; saved = false
                        }
                        if readings[conflict.legacyKey] == nil { Text("A decision is required") }
                    }.disabled(installing || migrationJob != nil)
                }
                ForEach(overlaps, id: \.secondBackup) { overlap in
                    Section("Overlapping backup") {
                        Text(overlap.secondBackup.hex).textSelection(.enabled)
                        Toggle("I confirm these are independent study histories", isOn: Binding(
                            get: { independent.contains(overlap.secondBackup) },
                            set: { value in
                                if value { independent.insert(overlap.secondBackup) }
                                else { independent.remove(overlap.secondBackup) }
                                saved = false
                            }))
                        Text("Confirm only when this backup represents separate study activity. If the history was copied between readers, review shared history instead.")
                    }.disabled(installing || migrationJob != nil)
                }
                Button("Save migration review") {
                    busy = true; saveFailed = false; saved = false
                    Task {
                        defer { busy = false }
                        do {
                            reviewedDraft = try await model.saveTintaMigrationReview(content.id, backup: backup,
                                configuration: configuration, readings: readings, independent: independent)
                            saved = true
                        } catch { saveFailed = true }
                    }
                }.disabled(!complete || busy || installing || migrationJob != nil)
            }
            if saved {
                Text("Migration review saved")
                Button("Import reviewed history into this app") { importConfirmation = true }.disabled(busy || installing || migrationJob != nil)
            }
            if imported { Text("Reviewed learning history is saved in this app") }
            if saved || sharedBackup != nil || hasActiveMigration, migrationJob?.phase != .completed,
               migrationJob?.abortState != .requested {
                Button(!hasActiveMigration ? String(localized: "Install reviewed history on connected reader") : String(localized: "Resume learning history installation")) {
                    installConfirmation = true
                }.disabled(busy || installing || !model.canInstallTintaMigration || !matchesBackupReader)
                Text("Connect the reader with this course installed. Preserve a current learning backup before installing reviewed history.")
                if !matchesBackupReader { Text("Connect the source reader with the same card used for this backup.") }
            }
            if let job = migrationJob, job.abortState != .completed, job.phase != .completed {
                Button(job.abortState == .requested ? String(localized: "Retry cancellation") : job.phase == .committing ? String(localized: "Check result or cancel pending installation") : String(localized: "Cancel learning history installation"), role: .destructive) {
                    abortConfirmation = true
                }.disabled(busy || installing || !model.canInstallTintaMigration || !matchesBackupReader)
            }
            if installing {
                if aborting { ProgressView("Cancelling learning history installation…") }
                else { ProgressView("Installing learning history…") }
                Button(aborting ? String(localized: "Pause cancellation") : String(localized: "Pause learning history installation")) { model.pauseTintaMigration() }
            }
            if migrationJob?.abortState == .completed { Text("Learning history installation cancelled; preserved history is still available") }
            else if migrationJob?.abortState == .requested { Text("Cancellation is pending. Reconnect the source reader and retry cancellation.") }
            else if migrationJob?.phase == .completed { Text("Learning history installed on reader") }
            else if migrationJob?.paused == true { Text("Learning history installation is paused") }
            if installFailed && !preferenceConflict { Text("Learning history could not be installed. Reconnect the reader and verify its course, current backup, and saved review before retrying.") }

            if preferenceConflict {
                Text("Resolve conflicting Tinta preferences before preparing this installation.")
                NavigationLink("Resolve portable preference conflicts") { PortablePreferencesView(model: model) }
            }
            if abortFailed { Text("Cancellation could not be confirmed. Reconnect the source reader and retry; the saved job remains pending.") }
            if migrationJob?.phase == .committing, migrationJob?.abortState != .completed { Text("Installation has reached commit. Resume or check the reader’s result before starting another job.") }
            if importFailed { Text("History could not be imported. Refresh the review and check compatibility and overlapping backups before retrying.") }
            if saveFailed { Text("Review could not be saved. Refresh and review the current backups and choices.") }
            Section {
                Text("These decisions are saved locally for this exact backup and course pack. Migration will recheck compatibility and overlapping backups before importing history.")
            }
        }
        .disabled(busy)
        .navigationTitle("Review migration decisions")
        .toolbar { Button("Refresh") { Task { await reload() } }.disabled(busy || installing) }
        .task(id: connectionBinding) { await reload() }
        .onChange(of: connectionBinding) { _, _ in
            migrationJob = nil; installConfirmation = false; abortConfirmation = false
            if installing { model.pauseTintaMigration() }
        }
        .onChange(of: installing) { _, active in
            if !active { Task { await reload() } }
        }
        .confirmationDialog("Cancel learning history installation", isPresented: $abortConfirmation) {
            Button("Cancel installation", role: .destructive) {
                guard let job = migrationJob else { return }
                installing = true; aborting = true; abortFailed = false
                Task {
                    defer { installing = false; aborting = false }
                    do { migrationJob = try await model.abortTintaMigration(job) }
                    catch {
                        abortFailed = true
                        migrationJob = try? await model.savedTintaMigration(backup)
                    }
                }
            }
            Button("Keep installation", role: .cancel) {}
        } message: {
            Text("Check the pending reader installation and cancel it if it has not been committed. An already committed installation is confirmed and retained. The original backup and app history remain available. Retry this check if the connection is interrupted.")
        }
        .confirmationDialog("Install reviewed learning history", isPresented: $installConfirmation) {
            Button("Install on reader") {
                guard reviewedDraft != nil || sharedBackup != nil || migrationJob != nil else { installFailed = true; return }
                installing = true; installFailed = false; preferenceConflict = false
                Task {
                    defer { installing = false }
                    do {
                        migrationJob = try await model.installTintaMigration(content.id, backup: backup, expectedDraft: reviewedDraft)
                        imported = true
                    } catch is CancellationError { migrationJob = try? await model.savedTintaMigration(backup) }
                    catch {
                        installFailed = true; preferenceConflict = error as? StoreError == .unresolvedPreferences
                        migrationJob = try? await model.savedTintaMigration(backup)
                    }
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Install this reviewed history and other saved history for this course on the connected reader. Learning files will be rebuilt together; the original backup stays preserved. Interrupted work can be resumed.")
        }
        .confirmationDialog("Import reviewed learning history", isPresented: $importConfirmation) {
            Button("Import history") {
                guard let reviewedDraft else { importFailed = true; return }
                busy = true; imported = false; importFailed = false
                Task {
                    defer { busy = false }
                    do { try await model.applyTintaMigration(content.id, backup: backup, expectedDraft: reviewedDraft); imported = true }
                    catch { importFailed = true }
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Import this backup using the saved reading and independent-history decisions. The backup remains preserved. This updates local app history; it does not install learning state on a reader.")
        }
    }
    private func reload() async {
        guard !busy, !installing else { return }
        busy = true; failed = false; saved = false; saveFailed = false
        imported = false; importFailed = false
        reviewedDraft = nil; sharedBackup = nil; sourceManifest = nil; migrationJob = nil
        configuration = nil; conflicts = []; overlaps = []; readings = [:]; independent = []
        defer { busy = false }
        do {
            sourceManifest = try await model.verifiedTintaBackup(backup)
            try Task.checkCancellation()
            migrationJob = try await model.savedTintaMigration(backup)
            sharedBackup = try await model.sharedTintaBackup(backup)
            let review = try await model.tintaMigrationReview(content.id, backup: backup)
            let loadedConflicts = try await model.tintaReadingConflicts(content.id, backup: backup)
            let loadedOverlaps = try await model.tintaOverlaps(backup)
            configuration = review.0; conflicts = loadedConflicts; overlaps = loadedOverlaps
            if let draft = review.1 {
                readings = draft.confirmedReadingResolutions
                independent = Set(draft.independentBackups)
                saved = true
                reviewedDraft = draft
            }
        } catch { failed = true }
    }
}

private struct TintaOverlapView: View {
    @Bindable var model: CompanionModel
    let backup: ContentID
    @State private var overlaps: [LegacyHistoryOverlap] = []
    @State private var busy = false
    @State private var failed = false
    @State private var actionFailed = false
    @State private var confirmed = false
    @State private var pending: ContentID?
    var body: some View {
        List {
            if busy { ProgressView("Checking overlapping history…") }
            else if failed { Text("Overlapping history could not be verified.") }
            else if overlaps.isEmpty { Text("No overlapping learning backups") }
            else {
                ForEach(overlaps, id: \.secondBackup) { overlap in
                    Section {
                        LabeledContent("Matching backup") { Text(overlap.secondBackup.hex).textSelection(.enabled) }
                        switch overlap.evidence {
                        case .identicalLearnerFiles:
                            Text("Learner files are identical")
                            Button("Confirm this is the same history") { pending = overlap.secondBackup }
                                .disabled(busy)
                        case .sharedReviewPrefix(let records):
                            LabeledContent("Matching initial review records") { Text(records, format: .number) }
                            Text("A shared prefix requires further reconciliation before shared history can be imported.")
                        }
                    }
                }
            }
            if actionFailed {
                Text("Shared history could not be confirmed. The matching backup must already be migrated, and this backup must not already have a separate learning migration or preference import.")
            }
            if confirmed { Text("Shared history confirmed without adding duplicate events") }
            Section {
                Text("Matching files do not prove that two readers share the same study history. Confirm only when you know these backups represent the same history. Both backups remain preserved.")
            }
        }
        .navigationTitle("Overlapping learning history")
        .toolbar { Button("Refresh") { Task { await reload() } }.disabled(busy) }
        .task { await reload() }
        .confirmationDialog("Confirm shared learning history", isPresented: Binding(
            get: { pending != nil }, set: { if !$0 { pending = nil } })) {
                Button("Confirm") {
                    guard let canonical = pending else { return }
                    pending = nil; busy = true; actionFailed = false; confirmed = false
                    Task {
                        defer { busy = false }
                        do { try await model.confirmSharedTintaBackup(backup, canonical: canonical); confirmed = true }
                        catch { actionFailed = true }
                    }
                }
                Button("Cancel", role: .cancel) { pending = nil }
            } message: {
                Text("Use the matching backup's installed migration as this backup's shared history. This decision prevents importing this backup independently.")
            }
    }
    private func reload() async {
        guard !busy else { return }
        busy = true; failed = false; actionFailed = false; confirmed = false
        defer { busy = false }
        do { overlaps = try await model.tintaOverlaps(backup) }
        catch { overlaps = []; failed = true }
    }
}

private struct TintaReadingConflictView: View {
    @Bindable var model: CompanionModel
    let content: LibraryContent
    let backup: ContentID
    @State private var conflicts: [LegacyReadingConflictDetails] = []
    @State private var loading = false
    @State private var failed = false
    var body: some View {
        List {
            Section { LibraryContentLabel(content: content) }
            if loading { ProgressView("Checking reading completions…") }
            else if failed { Text("Reading completions could not be verified against this pack.") }
            else if conflicts.isEmpty { Text("No ambiguous reading completions") }
            else {
                ForEach(conflicts) { conflict in
                    Section("Matching readings") {
                        ForEach(conflict.candidates) { candidate in
                            VStack(alignment: .leading) {
                                Text(candidate.title).font(.headline)
                                Text(candidate.kind == .dialogue ? String(localized: "Dialogue") : String(localized: "Story"))
                                    .foregroundStyle(.secondary)
                                if let unit = candidate.unitNumber, let lesson = candidate.lessonNumber {
                                    LabeledContent("Unit") { Text(unit, format: .number) }
                                    LabeledContent("Lesson") { Text(lesson, format: .number) }
                                } else { Text("Reading without an assigned lesson") }
                            }.accessibilityElement(children: .combine)
                        }
                    }
                }
            }
            Section {
                Text("Older backups may use the same completion key for several readings. These candidates require an explicit decision during migration. Viewing them does not change completion state.")
            }
        }
        .navigationTitle("Reading completion conflicts")
        .toolbar { Button("Refresh") { Task { await reload() } }.disabled(loading) }
        .task { await reload() }
    }
    private func reload() async {
        guard !loading else { return }
        loading = true; failed = false
        defer { loading = false }
        do { conflicts = try await model.tintaReadingConflicts(content.id, backup: backup) }
        catch { conflicts = []; failed = true }
    }
}

private struct TintaHistoryView: View {
    @Bindable var model: CompanionModel
    let content: LibraryContent
    @State private var snapshot: TintaSnapshot?
    @State private var loading = false
    @State private var failed = false
    private var dailyTotals: [UInt16: StudyTotals] { snapshot?.studyTotals.values.first ?? [:] }
    var body: some View {
        List {
            Section { LibraryContentLabel(content: content) }
            if loading { ProgressView("Loading learning history…") }
            else if failed { Text("Learning history could not be loaded.") }
            else if let snapshot {
                Section("Learning state") {
                    LabeledContent("Saved items") { Text(snapshot.items.count, format: .number) }
                    LabeledContent("Suspended items") {
                        Text(snapshot.items.values.filter { $0.bytes[14] & 1 != 0 }.count, format: .number)
                    }
                    LabeledContent("Starred items") {
                        Text(snapshot.items.values.filter { $0.bytes[14] & 4 != 0 }.count, format: .number)
                    }
                    LabeledContent("Completed lessons") { Text(snapshot.completedLessons.count, format: .number) }
                    LabeledContent("Completed readings") { Text(snapshot.completedReadings.count, format: .number) }
                }
                Section("Study totals") {
                    if dailyTotals.isEmpty { Text("No study totals saved") }
                    ForEach(dailyTotals.keys.sorted(), id: \.self) { day in
                        if let totals = dailyTotals[day] {
                            VStack(alignment: .leading) {
                                LabeledContent("Study day") { Text(day, format: .number) }
                                LabeledContent("New items") { Text(totals.newItems, format: .number) }
                                LabeledContent("Reviews") { Text(totals.reviews, format: .number) }
                            }.accessibilityElement(children: .contain)
                        }
                    }
                }
            }
            Section {
                Text("This is history saved in this app for the confirmed course, including its associated editions. Connecting a reader that supports journal export imports its saved history. Sending merged learning history to the reader is not available yet. Private iCloud synchronization can be enabled in Settings.")
            }
        }
        .navigationTitle("Saved learning history")
        .toolbar { Button("Refresh") { Task { await reload() } }.disabled(loading) }
        .task { await reload() }
    }
    private func reload() async {
        guard !loading else { return }
        loading = true; failed = false
        defer { loading = false }
        do { snapshot = try await model.tintaHistory(content.id) }
        catch { snapshot = nil; failed = true }
    }
}

private struct ReaderContentSelectionView: View {
    @Bindable var model: CompanionModel
    let reader: Data
    var body: some View {
        List {
            Section {
                Text("Choose the content you want on this reader. Selections are saved on this device; changing them does not immediately transfer or remove content.")
            }
            Section("Content") {
                ForEach(model.contents.filter { $0.kind != .firmware }, id: \.id) { content in
                    Toggle(isOn: Binding(
                        get: { model.readerSelections[reader]?.contains(content.id) == true },
                        set: { selected in
                            Task { await model.setSelection(reader: reader, content: content.id, selected: selected) }
                        })) { LibraryContentLabel(content: content) }
                        .disabled(model.savingSelection)
                }
                if model.contents.isEmpty { Text("Import files to choose content for this reader.") }
            }
            if model.savingSelection { ProgressView("Saving selection…") }
        }
        .navigationTitle("Reader content")
        .task { await model.reload() }
    }
}


private struct ReadingPositionsView: View {
    @Bindable var model: CompanionModel
    let content: LibraryContent
    @State private var positions: ReadingPositions?
    @State private var loading = false
    @State private var failed = false
    @State private var choice: ReadingPosition?
    var body: some View {
        List {
            Section {
                LibraryContentLabel(content: content)
                NavigationLink("Bookmarks") { BookmarksView(model: model, content: content) }
            }
            if loading { ProgressView("Loading reading positions…") }
            else if failed { Text("Reading positions could not be loaded.") }
            else if let positions {
                if positions.requiresResolution {
                    Text("This book has different reading positions. Choose the position you want to keep.")
                } else if positions.candidates.isEmpty { Text("No reading position saved") }
                ForEach(positions.candidates, id: \.identity) { position in
                    VStack(alignment: .leading) {
                        LabeledContent("Section") { Text(Int(position.anchor.spine) + 1, format: .number) }
                        LabeledContent("Position in section") { Text(position.anchor.visibleTextOffset, format: .number) }
                        if positions.requiresResolution {
                            Button("Keep this position") { choice = position }
                        }
                    }.accessibilityElement(children: .contain)
                }
            }
            Text("These are saved positions for this book. Sync a compatible reader to exchange reading history. Private iCloud synchronization can be enabled in Settings.")
        }
        .navigationTitle("Reading positions")
        .toolbar { Button("Refresh") { Task { await reload() } }.disabled(loading) }
        .task { await reload() }
        .confirmationDialog("Keep this reading position?", isPresented: Binding(
            get: { choice != nil }, set: { if !$0 { choice = nil } })) {
                Button("Keep position") {
                    guard let choice, let positions else { return }
                    self.choice = nil; loading = true
                    Task {
                        _ = await model.resolvePosition(content.id, position: choice, heads: positions.resolutionAncestors)
                        loading = false
                        await reload()
                    }
                }
                Button("Cancel", role: .cancel) { choice = nil }
            }
    }
    private func reload() async {
        guard !loading else { return }
        loading = true; failed = false
        defer { loading = false }
        do { positions = try await model.readingPositions(content.id) }
        catch { positions = nil; failed = true }
    }
}


private struct BookmarksView: View {
    @Bindable var model: CompanionModel
    let content: LibraryContent
    @State private var bookmarks: [BookmarkState] = []
    @State private var loading = false
    @State private var failed = false
    @State private var choice: (bookmark: BookmarkState, candidate: BookmarkCandidate)?
    var body: some View {
        List {
            if loading { ProgressView("Loading bookmarks…") }
            else if failed { Text("Bookmarks could not be loaded.") }
            else {
                ForEach(bookmarks.filter { !$0.isDeleted }, id: \.identity) { bookmark in
                    Section {
                        if bookmark.requiresResolution { Text("This bookmark has conflicting changes. Choose the version to keep.") }
                        ForEach(bookmark.candidates, id: \.event) { candidate in
                            VStack(alignment: .leading) {
                                if let value = candidate.value {
                                    Text(value.name.isEmpty ? String(localized: "Bookmark") : value.name).font(.headline)
                                    if !value.summary.isEmpty { Text(value.summary) }
                                    LabeledContent("Section") { Text(Int(value.anchor.spine) + 1, format: .number) }
                                    LabeledContent("Position in section") { Text(value.anchor.visibleTextOffset, format: .number) }
                                    if bookmark.requiresResolution {
                                        Button("Keep this bookmark") { choice = (bookmark, candidate) }
                                    }
                                } else {
                                    Text("This bookmark was removed.")
                                    if bookmark.requiresResolution {
                                        Button("Keep removal", role: .destructive) { choice = (bookmark, candidate) }
                                    }
                                }
                            }.accessibilityElement(children: .contain)
                        }
                    }
                }
                if bookmarks.allSatisfy(\.isDeleted) { Text("No bookmarks saved") }
            }
            Text("These are bookmarks saved in this app. Reader history exchange is not available yet. Private iCloud synchronization can be enabled in Settings.")
        }
        .navigationTitle("Bookmarks")
        .toolbar { Button("Refresh") { Task { await reload() } }.disabled(loading) }
        .task { await reload() }
        .confirmationDialog("Keep this bookmark version?", isPresented: Binding(
            get: { choice != nil }, set: { if !$0 { choice = nil } })) {
                Button("Confirm") {
                    guard let choice else { return }
                    self.choice = nil; loading = true
                    Task {
                        _ = await model.resolveBookmark(content.id, bookmark: choice.bookmark, value: choice.candidate.value)
                        loading = false
                        await reload()
                    }
                }
                Button("Cancel", role: .cancel) { choice = nil }
            }
    }
    private func reload() async {
        guard !loading else { return }
        loading = true; failed = false
        defer { loading = false }
        do { bookmarks = try await model.bookmarks(content.id) }
        catch { bookmarks = []; failed = true }
    }
}

private struct PortablePreferencesView: View {
    @Bindable var model: CompanionModel
    @State private var preferences: [PreferenceState] = []
    @State private var loading = false
    @State private var failed = false
    @State private var choice: PreferenceCandidate?
    @State private var chosenState: PreferenceState?

    var body: some View {
        List {
            if loading { ProgressView("Loading preferences…") }
            else if failed { Text("Preferences could not be loaded.") }
            else if preferences.isEmpty { Text("No portable preferences saved") }
            ForEach(preferences, id: \.key) { state in
                Section(PreferencePresentation.title(state.key)) {
                    if state.requiresResolution {
                        Text("This preference has conflicting changes. Choose the value to keep.")
                    }
                    ForEach(state.candidates, id: \.event) { candidate in
                        VStack(alignment: .leading) {
                            Text(PreferencePresentation.value(candidate.body))
                            if let content = candidate.body.requiredContent {
                                LabeledContent("Required content", value: content.name)
                            }
                            if state.requiresResolution {
                                Button("Keep this value") { chosenState = state; choice = candidate }
                                    .disabled(loading)
                            }
                        }.accessibilityElement(children: .contain)
                    }
                }
            }
        }
        .navigationTitle("Portable preferences")
        .toolbar { Button("Refresh") { Task { await reload() } }.disabled(loading) }
        .task { await reload() }
        .confirmationDialog("Keep this preference value?", isPresented: Binding(
            get: { choice != nil }, set: { if !$0 { choice = nil; chosenState = nil } })) {
                Button("Keep value") {
                    guard let choice, let chosenState else { return }
                    self.choice = nil; self.chosenState = nil; loading = true
                    Task {
                        _ = await model.resolvePreference(chosenState, candidate: choice)
                        loading = false
                        await reload()
                    }
                }
                Button("Cancel", role: .cancel) { choice = nil; chosenState = nil }
            }
    }
    private func reload() async {
        guard !loading else { return }
        loading = true; failed = false
        defer { loading = false }
        do { preferences = try await model.portablePreferences() }
        catch { preferences = []; failed = true }
    }
}

private enum PreferencePresentation {
    static func value(_ body: PreferenceBody) -> String {
        switch body.value {
        case let .integer(number):
            switch body.key {
            case .paragraphSpacing, .hyphenation, .textAntiAliasing, .embeddedStyle, .focusReading,
                 .tintaShowVulgar, .tintaTypedAnswers:
                return number == 0 ? String(localized: "Disabled") : String(localized: "Enabled")
            case .tintaLanguage:
                return number == 0 ? String(localized: "English") :
                    number == 1 ? String(localized: "Spanish") : String(localized: "Automatic")
            case .tintaTextSize:
                return number == 0 ? String(localized: "Small") :
                    number == 1 ? String(localized: "Medium") : String(localized: "Large")
            case .tintaRetentionPermille: return (Double(number) / 1000).formatted(.percent)
            default: return number.formatted()
            }
        case let .languageTag(tag): return tag
        case let .content(selection): return selection?.name ?? String(localized: "None")
        }
    }
    static func title(_ key: PreferenceKey) -> String {
        switch key {
        case .fontSelection: String(localized: "Font")
        case .fontPointSize: String(localized: "Font size")
        case .lineSpacing: String(localized: "Line spacing")
        case .alignment: String(localized: "Text alignment")
        case .paragraphSpacing: String(localized: "Paragraph spacing")
        case .wordSpacing: String(localized: "Word spacing")
        case .characterSpacing: String(localized: "Character spacing")
        case .margin: String(localized: "Margins")
        case .hyphenation: String(localized: "Hyphenation")
        case .language: String(localized: "Language")
        case .dictionary: String(localized: "Dictionary")
        case .textAntiAliasing: String(localized: "Text smoothing")
        case .embeddedStyle: String(localized: "Book styles")
        case .focusReading: String(localized: "Focus reading")
        case .tintaNewPerDay: String(localized: "Tinta new items per day")
        case .tintaReviewCap: String(localized: "Tinta review limit")
        case .tintaRetentionPermille: String(localized: "Tinta retention target")
        case .tintaMaximumInterval: String(localized: "Tinta maximum interval")
        case .tintaSessionSize: String(localized: "Tinta session size")
        case .tintaTextSize: String(localized: "Tinta text size")
        case .tintaLanguage: String(localized: "Tinta interface language")
        case .tintaShowVulgar: String(localized: "Tinta sensitive vocabulary")
        case .tintaTypedAnswers: String(localized: "Tinta typed answers")
        }
    }
}

private struct CloudSettingsSection: View {
    @Bindable var model: CompanionModel
    @State private var confirmation = false
    var body: some View {
        Section("Private iCloud library") {
            Text("Synchronize EPUBs, Tinta packs, fonts, dictionaries and saved journal history with your private iCloud account. Library removal and restore decisions are included; files remain preserved. Reader pairing credentials stay on this device.")
            if !model.cloudConfigured { Text("CloudKit setup is required for this build") }
            else {
                switch model.cloudStatus {
                case .disabled: Text("Synchronization disabled")
                case .ready: Text("Journal synchronization enabled")
                case .accountConfirmationRequired: Text("Confirm the current iCloud account before synchronizing")
                case .failed(let failure):
                    switch failure {
                    case .quotaExceeded: Text("Your iCloud storage is full. Manage storage in iCloud settings before retrying.")
                    case .networkUnavailable: Text("The network is unavailable. Reconnect before retrying synchronization.")
                    case .accountUnavailable: Text("Your iCloud account is unavailable or changed. Check the account before retrying.")
                    case .serviceUnavailable: Text("iCloud is temporarily unavailable. Wait before retrying.")
                    case .journalConflict: Text("Cloud history failed integrity or identity checks. Synchronization stopped; local history remains available.")
                    case .localPersistence: Text("Cloud synchronization state could not be saved locally. Check device storage before retrying.")
                    case .retryDeferred(let deadline):
                        Text("Wait until the retry time before synchronizing again")
                        Text(deadline, style: .time)
                    case .other: Text("Journal synchronization needs attention")
                    }
                }
                if model.cloudBusy { ProgressView("Synchronizing iCloud history…") }
                Button("Check iCloud account") { Task { await model.checkCloudAccount() } }.disabled(model.cloudBusy)
                if model.cloudAccount != nil {
                    LabeledContent("Observed iCloud account") { Text(model.cloudAccount ?? "").textSelection(.enabled) }
                    Button("Synchronize reviewed account") { confirmation = true }.disabled(model.cloudBusy)
                }
                Button("Disable iCloud history sync") { Task { await model.stopCloud() } }.disabled(model.cloudBusy)
            }
            if model.cloudFailed {
                Text("iCloud history could not be synchronized. Check your account, storage quota and network, then retry. Local reader operations remain available.")
            }
        }
        .confirmationDialog("Synchronize private iCloud history", isPresented: $confirmation) {
            Button("Synchronize") {
                guard let account = model.cloudAccount else { return }
                Task { await model.synchronizeCloud(confirmedAccount: account) }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Upload this app's content files and saved journal history to the current private iCloud account and import its content and journal records. Confirm only if this is the account you want to use for this library.")
        }
    }
}


private struct FirmwareUpdatesView: View {
    @State private var pendingInstall: TransferJob?
    @Bindable var model: CompanionModel
    @State private var showManifestImporter = false
    @State private var board: Board = .x4
    var body: some View {
        Form {
            Section("Release") {
                if model.releaseCheckBusy {
                    ProgressView("Checking stable releases")
                    Button("Cancel release check") { model.cancelReleaseCheck() }
                } else {
                    Button("Check stable releases", systemImage: "arrow.clockwise") { model.checkStableRelease() }
                        .accessibilityIdentifier("updates.check")
                        .disabled(model.firmwareDownloadBusy)
                }

                Toggle("Include release candidates", isOn: $model.allowReleaseCandidates)
                    .disabled(model.firmwareDownloadBusy || model.releaseCheckBusy)
                    .onChange(of: model.allowReleaseCandidates) { model.releaseManifest = nil }
                Button("Import release manifest", systemImage: "doc.badge.plus") { showManifestImporter = true }
                    .accessibilityIdentifier("updates.importManifest")
                    .disabled(model.firmwareDownloadBusy || model.releaseCheckBusy)
                if let release = model.releaseManifest { LabeledContent("Version", value: release.version) }
            }
            if let release = model.releaseManifest {
                Section("Firmware") {
                    Picker("Reader model", selection: $board) {
                        Text("X4").tag(Board.x4)
                        Text("Sticky").tag(Board.sticky)
                        Text("X4 Pro").tag(Board.x4Pro)
                        Text("X4 Classic").tag(Board.x4Classic)
                        Text("Paper Mono").tag(Board.paperMono)
                    }.disabled(model.firmwareDownloadBusy)
                    if let asset = release.firmware(for: board) {
                        Text(asset.name)
                        if model.firmwareDownloadBusy {
                            ProgressView("Downloading firmware")
                            Button("Cancel download") { model.cancelFirmwareDownload() }
                        } else {
                            Button("Download firmware", systemImage: "arrow.down.circle") { model.downloadFirmware(asset) }
                                .disabled(!model.canDownloadFirmware)
                        }
                    }
                }
            }
            Section {
                Text("Stage firmware on the connected reader, then confirm installation. Reconnect after restart to verify the update.")
                    .foregroundStyle(.secondary)
                if model.firmwareInstallBusy { ProgressView("Installing firmware") }
                else if model.transferBusy {
                    ProgressView("Transferring content…")
                    Button("Pause transfer") { model.pauseContentTransfer() }
                }
                if let notice = model.firmwareNotice { Text(notice).foregroundStyle(.secondary) }
                if let notice = model.transferNotice { Text(notice).foregroundStyle(.secondary) }
                ForEach(model.contents.filter { $0.kind == .firmware }, id: \.id) { content in
                    VStack(alignment: .leading) {
                        Text(content.title)
                        Button("Stage firmware on connected reader") { model.stageFirmware(content) }
                            .disabled(!model.canStageFirmware)
                        if let job = model.stagedFirmware[content.id] {
                            Button("Install staged firmware") { pendingInstall = job }
                                .disabled(!model.canInstallFirmware(job))
                        }
                        if let installation = model.firmwareInstallations[content.id] {
                            Text(installation.bootVerified ? String(localized: "Boot previously verified") : String(localized: "Awaiting boot verification"))
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
        }
        .confirmationDialog("Install firmware on connected reader?", isPresented: Binding(
            get: { pendingInstall != nil }, set: { if !$0 { pendingInstall = nil } })) {
            if let job = pendingInstall {
                Button("Install and restart reader") { pendingInstall = nil; model.installFirmware(job) }
            }
            Button("Cancel", role: .cancel) { pendingInstall = nil }
        } message: {
            Text("Compatibility and battery will be checked again. Keep the reader powered on until it restarts, then reconnect to verify the update.")
        }
        .navigationTitle("Updates")
        .onAppear { if let device = model.connected { board = device.board } }
        .fileImporter(isPresented: $showManifestImporter, allowedContentTypes: [.json]) { result in
            switch result {
            case .success(let url): model.importReleaseManifest(url)
            case .failure: model.error = String(localized: "The release manifest could not be imported.")
            }
        }
    }
}

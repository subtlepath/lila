import Foundation

public protocol CompanionTransport: Sendable {
    func exchange(_ request: ControlFrame) async throws -> ControlFrame
}

public enum TransferRunnerError: Error, Equatable, Sendable {
    case removalRejected(ContentRemovalResult)
    case busy, handoffUnavailable, wrongInstallation, wrongReader, wrongStorage, wrongHandoff, unsupportedProtocol, unsupportedContent, deselected, abortPending, aborted, invalidOffset, requestIDsExhausted
}

public actor TransferRunner {
    private let library: LibraryStore
    private let vault: ContentVault
    private var running = false
    private var requestID: UInt32 = 0
    public init(library: LibraryStore, vault: ContentVault) { self.library = library; self.vault = vault }
    public func removeContent(_ id: UUID, session: AuthenticatedReaderSession) async throws -> ContentRemovalJob {
        try await removeContent(id, device: session.device, installation: session.installation, transport: session)
    }
    func removeContent(_ id: UUID, device: DeviceDescriptor, installation: Data,
                       transport: any CompanionTransport) async throws -> ContentRemovalJob {
        guard !running else { throw TransferRunnerError.busy }
        running = true
        defer { running = false }
        guard let job = try await library.removalJob(id) else { throw StoreError.missingJob }
        guard job.reader == device.identity else { throw TransferRunnerError.wrongReader }
        guard job.request.owner == installation else { throw TransferRunnerError.wrongInstallation }
        guard job.request.generation == device.storageGeneration else { throw TransferRunnerError.wrongStorage }
        if job.phase == .completed { return job }
        guard device.minimumProtocol <= 1, device.maximumProtocol >= 1 else { throw TransferRunnerError.unsupportedProtocol }
        guard device.readerCapabilities.supportsRemoval(of: job.request.manifest.kind) else {
            throw TransferRunnerError.unsupportedContent
        }
        try Task.checkCancellation()
        do {
            try await library.checkpointRemoval(id, phase: .removing)
            let request = try ControlFrame(command: .removeContent, requestID: nextRequestID(), payload: job.request.encoded)
            let response = try await transport.exchange(request)
            guard response.response, response.command == request.command, response.requestID == request.requestID else {
                throw ProtocolError.value
            }
            let reply = try ContentRemovalReply(decoding: response.payload, request: job.request)
            guard reply.result == .ok else { throw TransferRunnerError.removalRejected(reply.result) }
            try await library.checkpointRemoval(id, phase: .completed)
            guard let completed = try await library.removalJob(id) else { throw StoreError.missingJob }
            return completed
        } catch {
            if let current = try? await library.removalJob(id), current.phase == .removing {
                try? await library.checkpointRemoval(id, phase: .paused)
            }
            throw error
        }
    }
    public func installFirmware(_ id: UUID, session: AuthenticatedReaderSession) async throws -> FirmwareInstallation {
        guard !running else { throw TransferRunnerError.busy }
        running = true
        defer { running = false }
        let receipt = try await library.firmwareStagingReceipt(id)
        guard receipt.installation == session.installation else { throw TransferRunnerError.wrongInstallation }
        guard receipt.reader == session.device.identity else { throw TransferRunnerError.wrongReader }
        guard receipt.generation == session.device.storageGeneration else { throw TransferRunnerError.wrongStorage }
        let info = try await session.firmwareInfo(requestID: nextRequestID())
        return try await performFirmwareInstallation(id, device: session.device, installation: session.installation,
            info: info, transport: session)
    }
    func installFirmware(_ id: UUID, device: DeviceDescriptor, installation: Data, info: FirmwareReaderInfo,
                         transport: any CompanionTransport) async throws -> FirmwareInstallation {
        guard !running else { throw TransferRunnerError.busy }
        running = true
        defer { running = false }
        return try await performFirmwareInstallation(id, device: device, installation: installation,
            info: info, transport: transport)
    }
    private func performFirmwareInstallation(_ id: UUID, device: DeviceDescriptor, installation: Data,
                                            info: FirmwareReaderInfo, transport: any CompanionTransport) async throws -> FirmwareInstallation {
        let receipt = try await library.firmwareStagingReceipt(id)
        guard receipt.installation == installation else { throw TransferRunnerError.wrongInstallation }
        guard receipt.reader == device.identity else { throw TransferRunnerError.wrongReader }
        guard receipt.generation == device.storageGeneration else { throw TransferRunnerError.wrongStorage }
        if let saved = try await library.firmwareInstallation(id) {
            if try await library.verifyFirmwareInstallation(id, device: device, installation: installation) {
                guard let verified = try await library.firmwareInstallation(id) else { throw StoreError.missingJob }
                return verified
            }
            guard !saved.bootVerified else { throw StoreError.invalidTransition }
        }
        _ = try await library.admitFirmwareTransfer(receipt.image, device: device, info: info, vault: vault)
        let prepared = try await library.prepareFirmwareInstallation(id, device: device, info: info, installation: installation)
        if try await library.verifyFirmwareInstallation(id, device: device, installation: installation) {
            guard let verified = try await library.firmwareInstallation(id) else { throw StoreError.missingJob }
            return verified
        }
        try Task.checkCancellation()
        let request = try prepared.request.controlFrame(requestID: nextRequestID())
        try prepared.request.validateAcceptance(await transport.exchange(request), request: request)
        return prepared
    }
    public func stageFirmware(_ id: UUID, session: AuthenticatedReaderSession) async throws -> FirmwareStagingReceipt {
        guard !running else { throw TransferRunnerError.busy }
        running = true
        defer { running = false }
        guard let job = try await library.job(id), let content = try await library.content(job.content),
              content.kind == .firmware else { throw StoreError.missingContent }
        guard job.installation == session.installation else { throw TransferRunnerError.wrongInstallation }
        guard job.reader == session.device.identity else { throw TransferRunnerError.wrongReader }
        guard job.storageGeneration == session.device.storageGeneration else { throw TransferRunnerError.wrongStorage }
        let info = try await session.firmwareInfo(requestID: nextRequestID())
        return try await performFirmwareStaging(id, device: session.device, info: info, transport: session)
    }
    // Authenticated session supplies the fresh information in the public entry point.
    func stageFirmware(_ id: UUID, device: DeviceDescriptor, info: FirmwareReaderInfo,
                       transport: any CompanionTransport) async throws -> FirmwareStagingReceipt {
        guard !running else { throw TransferRunnerError.busy }
        running = true
        defer { running = false }
        return try await performFirmwareStaging(id, device: device, info: info, transport: transport)
    }
    private func performFirmwareStaging(_ id: UUID, device: DeviceDescriptor, info: FirmwareReaderInfo,
                                        transport: any CompanionTransport) async throws -> FirmwareStagingReceipt {
        guard let job = try await library.job(id) else { throw StoreError.missingJob }
        _ = try await library.admitFirmwareTransfer(job.content, device: device, info: info, vault: vault)
        let fresh = try info.refreshedDevice(device)
        do {
            let staged = try await transfer(id, device: fresh, transport: transport,
                requireSelection: false, prepareOnly: false, firmwareAdmitted: true)
            return try await library.firmwareStagingReceipt(staged.id)
        } catch {
            if let current = try? await library.job(id), current.phase != .committing,
               current.phase != .completed, current.phase != .aborted, current.phase != .failed {
                try? await library.checkpoint(id, offset: current.durableOffset, phase: .paused)
            }
            throw error
        }
    }
    public func prepareFirmwareHandoff(_ id: UUID, session: AuthenticatedReaderSession) async throws -> FirmwareHandoffPreparation {
        guard !running else { throw TransferRunnerError.busy }
        running = true
        defer { running = false }
        guard let job = try await library.job(id) else { throw StoreError.missingJob }
        guard job.installation == session.installation else { throw TransferRunnerError.wrongInstallation }
        guard job.reader == session.device.identity else { throw TransferRunnerError.wrongReader }
        guard job.storageGeneration == session.device.storageGeneration else { throw TransferRunnerError.wrongStorage }
        let info = try await session.firmwareInfo(requestID: nextRequestID())
        return try await performFirmwareHandoffPreparation(id, device: session.device, info: info, transport: session)
    }
    func prepareFirmwareHandoff(_ id: UUID, device: DeviceDescriptor, info: FirmwareReaderInfo,
                                transport: any CompanionTransport) async throws -> FirmwareHandoffPreparation {
        guard !running else { throw TransferRunnerError.busy }
        running = true
        defer { running = false }
        return try await performFirmwareHandoffPreparation(id, device: device, info: info, transport: transport)
    }
    private func performFirmwareHandoffPreparation(_ id: UUID, device: DeviceDescriptor, info: FirmwareReaderInfo,
                                                   transport: any CompanionTransport) async throws -> FirmwareHandoffPreparation {
        guard let job = try await library.job(id) else { throw StoreError.missingJob }
        _ = try await library.admitFirmwareTransfer(job.content, device: device, info: info, vault: vault)
        let fresh = try info.refreshedDevice(device)
        do {
            let prepared = try await transfer(id, device: fresh, transport: transport,
                requireSelection: false, prepareOnly: true, firmwareAdmitted: true)
            return FirmwareHandoffPreparation(job: prepared, info: info)
        } catch {
            if let current = try? await library.job(id), current.phase != .committing,
               current.phase != .completed, current.phase != .aborted, current.phase != .failed {
                try? await library.checkpoint(id, offset: current.durableOffset, phase: .paused)
            }
            throw error
        }
    }
    public func stageFirmware(_ preparation: FirmwareHandoffPreparation, session: AuthenticatedReaderSession,
                              handoff: WifiHandoffTransport) async throws -> FirmwareStagingReceipt {
        try await authorizeHandoff(preparation.job.id, session: session, handoff: handoff)
        guard let current = try await library.job(preparation.job.id),
              current.content == preparation.job.content,
              current.reader == preparation.job.reader,
              current.storageGeneration == preparation.job.storageGeneration,
              current.installation == preparation.job.installation else { throw TransferRunnerError.wrongHandoff }
        return try await stageFirmware(current.id, device: session.device, info: preparation.info, transport: handoff)
    }
    public func run(_ id: UUID, session: AuthenticatedReaderSession) async throws -> TransferJob {
        guard let job = try await library.job(id) else { throw StoreError.missingJob }
        guard job.installation == session.installation else { throw TransferRunnerError.wrongInstallation }
        return try await run(id, device: session.device, transport: session, requireSelection: true)
    }
    public func prepareForHandoff(_ id: UUID, session: AuthenticatedReaderSession) async throws -> TransferJob {
        guard let job = try await library.job(id) else { throw StoreError.missingJob }
        guard job.installation == session.installation else { throw TransferRunnerError.wrongInstallation }
        return try await run(id, device: session.device, transport: session, requireSelection: true, prepareOnly: true)
    }
    public func run(_ id: UUID, session: AuthenticatedReaderSession, handoff: WifiHandoffTransport) async throws -> TransferJob {
        try await authorizeHandoff(id, session: session, handoff: handoff)
        return try await run(id, device: session.device, transport: handoff, requireSelection: true)
    }
    public func abort(_ id: UUID, session: AuthenticatedReaderSession, handoff: WifiHandoffTransport) async throws -> TransferJob {
        try await authorizeHandoff(id, session: session, handoff: handoff)
        return try await abort(id, device: session.device, transport: handoff)
    }
    private func authorizeHandoff(_ id: UUID, session: AuthenticatedReaderSession, handoff: WifiHandoffTransport) async throws {
        guard let job = try await library.job(id) else { throw StoreError.missingJob }
        guard job.installation == session.installation else { throw TransferRunnerError.wrongInstallation }
        guard job.reader == session.device.identity else { throw TransferRunnerError.wrongReader }
        guard job.storageGeneration == session.device.storageGeneration else { throw TransferRunnerError.wrongStorage }
        let transaction = withUnsafeBytes(of: id.uuid) { Data($0) }
        guard await handoff.matches(reader: job.reader, storageGeneration: job.storageGeneration,
            installation: job.installation, transaction: transaction) else { throw TransferRunnerError.wrongHandoff }
    }
    public func prepareDeclaration(_ id: UUID, session: AuthenticatedReaderSession) async throws -> TransferDeclaration {
        try await prepareDeclaration(id, device: session.device, installation: session.installation)
    }
    private func validateCourse(_ content: LibraryContent, job: UUID, url: URL, device: DeviceDescriptor) async throws {
        let metadata = try CoursePackInspector.inspect(url)
        let details = try CoursePackDetails(metadata)
        guard let stored = try await library.coursePackDetails(content.id), stored == details,
              content.languages == [details.locale] else { throw StoreError.invalidValue }
        guard device.readerCapabilities.contains(details.requiredReaderCapabilities) else {
            throw TransferRunnerError.unsupportedContent
        }
        if try await library.courseSwitchConfirmation(job) != nil && !device.readerCapabilities.supportsCourseSwitch {
            throw TransferRunnerError.unsupportedContent
        }
    }
    func prepareDeclaration(_ id: UUID, device: DeviceDescriptor, installation: Data) async throws -> TransferDeclaration {
        guard !running else { throw TransferRunnerError.busy }
        running = true
        defer { running = false }
        try Task.checkCancellation()
        guard let job = try await library.job(id) else { throw StoreError.missingJob }
        guard job.installation == installation else { throw TransferRunnerError.wrongInstallation }
        guard job.reader == device.identity else { throw TransferRunnerError.wrongReader }
        guard job.storageGeneration == device.storageGeneration else { throw TransferRunnerError.wrongStorage }
        guard device.minimumProtocol <= 1, device.maximumProtocol >= 1 else { throw TransferRunnerError.unsupportedProtocol }
        guard let content = try await library.content(job.content) else { throw StoreError.missingContent }
        guard content.kind == .epub || content.kind == .font || content.kind == .course && device.readerCapabilities.supportsCourseTransfer ||
              content.kind == .dictionary && device.readerCapabilities.supportsDictionaryTransfer else {
            throw TransferRunnerError.unsupportedContent
        }
        let object = try await vault.verifiedObject(job.content)
        if content.kind == .course { try await validateCourse(content, job: job.id, url: object.url, device: device) }
        if content.kind == .font {
            let plan = try FontTransferPlan(content: content)
            try plan.admit(device)
            if plan.formatVersion == 4 { _ = try BitmapFontInspector.inspect(object.url) }
            else { _ = try VectorFontInspector.inspect(object.url) }
        }
        if content.kind == .dictionary {
            try DictionaryTransferPlan(content: content).admit(device)
            _ = try DictionaryBundleInspector.inspect(object.url, scratchDirectory: object.url.deletingLastPathComponent())
        }
        try Task.checkCancellation()
        return try await library.prepareTransferDeclaration(id, verifiedLength: object.length)
    }
    public func abort(_ id: UUID, session: AuthenticatedReaderSession) async throws -> TransferJob {
        guard let job = try await library.job(id) else { throw StoreError.missingJob }
        guard job.installation == session.installation else { throw TransferRunnerError.wrongInstallation }
        return try await abort(id, device: session.device, transport: session)
    }
    func abort(_ id: UUID, device: DeviceDescriptor, transport: any CompanionTransport) async throws -> TransferJob {
        guard !running else { throw TransferRunnerError.busy }
        running = true
        defer { running = false }
        try Task.checkCancellation()
        guard let job = try await library.job(id), let content = try await library.content(job.content) else { throw StoreError.missingJob }
        guard job.reader == device.identity else { throw TransferRunnerError.wrongReader }
        guard job.storageGeneration == device.storageGeneration else { throw TransferRunnerError.wrongStorage }
        guard device.minimumProtocol <= 1, device.maximumProtocol >= 1 else { throw TransferRunnerError.unsupportedProtocol }
        if job.phase == .aborted { return job }
        try await library.requestTransferAbort(id)
        let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
        let expected = try TransferState(transaction: transaction, owner: job.installation,
            storageGeneration: job.storageGeneration, contentHash: job.content.digest, length: content.length)
        var offset = job.durableOffset
        do {
            let state = try await send(TransferCommands.transaction(.abort, identity: transaction, requestID: nextRequestID()),
                expected: expected, transport: transport)
            offset = state.durableOffset
        } catch TransferCommandError.remote(.noTransaction) {
            // The authenticated reader has recovered storage and has no staged transaction.
        }
        try await library.checkpoint(id, offset: offset, phase: .aborted)
        guard let aborted = try await library.job(id) else { throw StoreError.missingJob }
        return aborted
    }
    func run(_ id: UUID, device: DeviceDescriptor, transport: any CompanionTransport, requireSelection: Bool = false, prepareOnly: Bool = false) async throws -> TransferJob {
        guard !running else { throw TransferRunnerError.busy }
        running = true
        defer { running = false }
        do { return try await transfer(id, device: device, transport: transport, requireSelection: requireSelection, prepareOnly: prepareOnly) }
        catch {
            if let job = try? await library.job(id), job.phase != .committing,
               job.phase != .completed, job.phase != .aborted, job.phase != .failed {
                try? await library.checkpoint(id, offset: job.durableOffset, phase: .paused)
            }
            throw error
        }
    }
    private func transfer(_ id: UUID, device: DeviceDescriptor, transport: any CompanionTransport, requireSelection: Bool, prepareOnly: Bool, firmwareAdmitted: Bool = false) async throws -> TransferJob {
        try Task.checkCancellation()
        guard let job = try await library.job(id), let content = try await library.content(job.content) else { throw StoreError.missingJob }
        guard job.reader == device.identity else { throw TransferRunnerError.wrongReader }
        guard job.storageGeneration == device.storageGeneration else { throw TransferRunnerError.wrongStorage }
        guard device.minimumProtocol <= 1, device.maximumProtocol >= 1 else { throw TransferRunnerError.unsupportedProtocol }
        guard content.kind == .epub || content.kind == .course && device.readerCapabilities.supportsCourseTransfer ||
              content.kind == .font || content.kind == .dictionary && device.readerCapabilities.supportsDictionaryTransfer ||
              content.kind == .firmware && firmwareAdmitted else {
            throw TransferRunnerError.unsupportedContent
        }
        if job.phase == .completed { return job }
        if prepareOnly {
            guard content.length > 1024 * 1024, job.phase != .committing else { throw TransferRunnerError.handoffUnavailable }
        }
        guard job.phase != .aborted else { throw TransferRunnerError.aborted }
        guard try await !library.hasTransferAbort(id) else { throw TransferRunnerError.abortPending }
        let checkSelection = requireSelection && job.phase != .committing
        if checkSelection { try await ensureSelected(job) }
        let object = try await vault.verifiedObject(job.content)
        if content.kind == .course { try await validateCourse(content, job: job.id, url: object.url, device: device) }
        if content.kind == .font {
            let plan = try FontTransferPlan(content: content)
            try plan.admit(device)
            if plan.formatVersion == 4 { _ = try BitmapFontInspector.inspect(object.url) }
            else { _ = try VectorFontInspector.inspect(object.url) }
        }
        if content.kind == .dictionary {
            try DictionaryTransferPlan(content: content).admit(device)
            _ = try DictionaryBundleInspector.inspect(object.url, scratchDirectory: object.url.deletingLastPathComponent())
        }
        guard object.length == content.length else { throw VaultError.integrity }
        let transaction = withUnsafeBytes(of: job.id.uuid) { Data($0) }
        let expected = try TransferState(transaction: transaction, owner: job.installation,
                                         storageGeneration: job.storageGeneration, contentHash: job.content.digest, length: content.length)
        let begin: ControlFrame
        if content.kind == .font {
            let declaration = try await library.prepareTransferDeclaration(id, verifiedLength: object.length)
            let plan = try await library.fontTransferPlan(id)
            guard declaration.state == expected, plan == (try FontTransferPlan(content: content)) else {
                throw StoreError.conflictingJob
            }
            begin = try TransferCommands.beginFont(declaration, plan: plan, requestID: nextRequestID())
        } else if content.kind == .course || content.kind == .dictionary || content.kind == .firmware {
            let declaration = try await library.prepareTransferDeclaration(id, verifiedLength: object.length)
            guard declaration.state == expected else { throw StoreError.conflictingJob }
            begin = try TransferCommands.begin(declaration, requestID: nextRequestID())
        } else {
            begin = try TransferCommands.begin(expected, requestID: nextRequestID())
        }
        let switchConsent = try await library.courseSwitchConfirmation(id)
        if switchConsent != nil && !device.readerCapabilities.supportsCourseSwitch {
            throw TransferRunnerError.unsupportedContent
        }
        var state = try await send(begin, expected: expected, transport: transport)
        if state.phase == .receiving, let consent = switchConsent {
            try Task.checkCancellation()
            if let handoff = transport as? WifiHandoffTransport {
                try await handoff.authorizeCourseSwitch(consent, requestID: nextRequestID())
            } else {
                let request = try ControlFrame(command: .exchangeChanges, requestID: nextRequestID(), payload: consent.bytes)
                try CourseSwitchReply.validate(await transport.exchange(request), request: request)
            }
        }
        if state.phase == .aborted { throw TransferRunnerError.aborted }
        if prepareOnly && state.phase == .installing {
            try await library.checkpoint(id, offset: state.length, phase: .committing)
            throw TransferRunnerError.handoffUnavailable
        }
        if prepareOnly && state.phase != .committed {
            guard state.phase == .receiving || state.phase == .verified else { throw TransferRunnerError.handoffUnavailable }
            try Task.checkCancellation()
            if checkSelection { try await ensureSelected(job) }
            guard try await !library.hasTransferAbort(id) else { throw TransferRunnerError.abortPending }
            try await library.checkpoint(id, offset: state.durableOffset, phase: .paused)
            guard let prepared = try await library.job(id) else { throw StoreError.missingJob }
            return prepared
        }
        if state.phase != .committed {
            try await library.checkpoint(id, offset: state.durableOffset,
                                         phase: state.phase == .receiving || (checkSelection && state.phase == .verified) ? .transferring : .committing)
            let input = try FileHandle(forReadingFrom: object.url)
            defer { try? input.close() }
            while state.durableOffset < state.length {
                try Task.checkCancellation()
                if checkSelection { try await ensureSelected(job) }
                let offset = state.durableOffset
                try input.seek(toOffset: offset)
                let count = Int(min(UInt64(TransferCommands.maximumChunk), state.length - offset))
                let bytes = try input.read(upToCount: count) ?? Data()
                guard bytes.count == count else { throw VaultError.integrity }
                state = try await send(TransferCommands.chunk(transaction: transaction, offset: offset, bytes: bytes,
                                                               requestID: nextRequestID()), expected: expected, transport: transport)
                guard state.durableOffset >= offset + UInt64(count) else { throw TransferRunnerError.invalidOffset }
                try await library.checkpoint(id, offset: state.durableOffset, phase: .transferring)
            }
            try Task.checkCancellation()
            if checkSelection { try await library.commitSelectedTransfer(id) }
            else { try await library.checkpoint(id, offset: state.length, phase: .committing) }
            do {
                state = try await send(TransferCommands.transaction(.commit, identity: transaction, requestID: nextRequestID()),
                                       expected: expected, transport: transport)
            } catch TransferCommandError.remote(.invalid) {
                // A rejected request alone does not establish whether installation started.
                if let confirmed = try? await send(TransferCommands.transaction(.transferStatus, identity: transaction,
                    requestID: nextRequestID()), expected: expected, transport: transport),
                   confirmed.phase == .receiving || confirmed.phase == .verified {
                    try await library.checkpoint(id, offset: confirmed.durableOffset, phase: .failed)
                }
                throw TransferCommandError.remote(.invalid)
            }
        } else {
            try await library.checkpoint(id, offset: state.length, phase: .committing)
        }
        try await library.checkpoint(id, offset: state.length, phase: .completed)
        guard let completed = try await library.job(id) else { throw StoreError.missingJob }
        return completed
    }
    private func send(_ request: ControlFrame, expected: TransferState, transport: any CompanionTransport) async throws -> TransferState {
        let response = try await transport.exchange(request)
        return try TransferCommands.response(response, to: request, expected: expected)
    }
    private func nextRequestID() throws -> UInt32 {
        guard requestID < UInt32.max else { throw TransferRunnerError.requestIDsExhausted }
        requestID += 1
        return requestID
    }
    private func ensureSelected(_ job: TransferJob) async throws {
        guard try await library.isReaderContentSelected(reader: job.reader, content: job.content) else {
            throw TransferRunnerError.deselected
        }
    }
}

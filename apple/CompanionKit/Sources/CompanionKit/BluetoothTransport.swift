#if canImport(CoreBluetooth)
import Foundation
import CoreBluetooth

public struct BluetoothReader: Identifiable, Sendable {
    public let id: UUID
    public let advertisedName: String?
}
public enum BluetoothTransportError: Error, Sendable {
    case unavailable, busy, disconnected, missingService, timeout, malformedResponse, exhausted
}

@MainActor
public final class BluetoothTransport: NSObject, SessionTransport,
    @preconcurrency CBCentralManagerDelegate, @preconcurrency CBPeripheralDelegate {
    public enum State: Sendable { case unavailable, idle, scanning, connecting, ready }
    public private(set) var state: State = .unavailable
    public var onStateChanged: (@MainActor (State) -> Void)?
    public var onReadersChanged: (@MainActor ([BluetoothReader]) -> Void)?
    private static let service = CBUUID(string: "e1b11000-7c43-4d94-86d6-c9c71ec60101")
    private static let inputID = CBUUID(string: "e1b11001-7c43-4d94-86d6-c9c71ec60101")
    private static let outputID = CBUUID(string: "e1b11002-7c43-4d94-86d6-c9c71ec60101")
    private var central: CBCentralManager!
    private var discovered: [UUID: CBPeripheral] = [:]
    private var selected: CBPeripheral?
    private var handoffPeripheral: UUID?
    private var closing: CBPeripheral?
    private var input: CBCharacteristic?
    private var output: CBCharacteristic?
    private var connecting: CheckedContinuation<Void, Error>?
    private var connectionToken: UInt64 = 0
    private var operation: UInt64 = 0
    private var wireID: UInt32 = 0
    private var pending: Request?
    private var assembler = FrameAssembler()
    private var timer: Task<Void, Never>?
    private struct Request {
        let token: UInt64
        let wire: UInt32
        let original: ControlFrame
        let bytes: Data
        let continuation: CheckedContinuation<ControlFrame, Error>
        var sent = 0
        var writing = false
        var received: ControlFrame?
    }
    public override init() {
        super.init()
        discovered.reserveCapacity(32)
        central = CBCentralManager(delegate: self, queue: .main)
    }
    public func scan() throws {
        guard central.state == .poweredOn else { throw BluetoothTransportError.unavailable }
        guard selected == nil, closing == nil else { throw BluetoothTransportError.busy }
        discovered.removeAll(keepingCapacity: true)
        onReadersChanged?([])
        central.scanForPeripherals(withServices: [Self.service], options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        changeState(.scanning)
    }
    public func stopScanning() {
        central.stopScan()
        if state == .scanning { changeState(.idle) }
    }
    public func connect(_ id: UUID, timeout: Duration = .seconds(30)) async throws {
        try Task.checkCancellation()
        guard timeout > .zero else { throw BluetoothTransportError.timeout }
        guard central.state == .poweredOn else { throw BluetoothTransportError.unavailable }
        guard selected == nil, closing == nil, connecting == nil else { throw BluetoothTransportError.busy }
        guard let peripheral = discovered[id] else { throw BluetoothTransportError.disconnected }
        handoffPeripheral = id
        let token = try nextOperation()
        connectionToken = token
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                connecting = continuation
                selected = peripheral
                peripheral.delegate = self
                central.stopScan()
                changeState(.connecting)
                startTimer(token, timeout: timeout)
                central.connect(peripheral)
            }
        } onCancel: {
            Task { @MainActor [weak self] in
                guard let self, self.connectionToken == token, self.connecting != nil else { return }
                self.disconnect(reason: CancellationError())
            }
        }
    }
    public func disconnect() { disconnect(reason: BluetoothTransportError.disconnected) }
    public func reconnectAfterHandoff() async throws {
        guard let id = selected?.identifier ?? closing?.identifier ?? handoffPeripheral else {
            throw BluetoothTransportError.disconnected
        }
        handoffPeripheral = id
        disconnect()
        let deadline = ContinuousClock.now.advanced(by: .seconds(35))
        while ContinuousClock.now < deadline {
            try Task.checkCancellation()
            if closing != nil {
                try await Task.sleep(for: .milliseconds(200))
                continue
            }
            do { try await connect(id, timeout: ContinuousClock.now.duration(to: deadline)); return }
            catch is CancellationError { throw CancellationError() }
            catch {
                guard central.state == .poweredOn else { throw BluetoothTransportError.unavailable }
                try await Task.sleep(for: .milliseconds(200))
            }
        }
        throw BluetoothTransportError.disconnected
    }
    public func sessionIdentity() async throws -> UInt64 {
        guard state == .ready, selected != nil else { throw BluetoothTransportError.disconnected }
        return connectionToken
    }
    public func exchange(_ request: ControlFrame, connection: UInt64) async throws -> ControlFrame {
        guard connection == connectionToken else { throw ReaderSessionError.staleConnection }
        return try await exchange(request)
    }
    public func exchange(_ request: ControlFrame) async throws -> ControlFrame {
        try Task.checkCancellation()
        guard state == .ready, selected != nil, input != nil, output?.isNotifying == true else {
            throw BluetoothTransportError.disconnected
        }
        guard pending == nil else { throw BluetoothTransportError.busy }
        guard !request.response, wireID < UInt32.max else { throw BluetoothTransportError.exhausted }
        let token = try nextOperation()
        wireID += 1
        let frame = try ControlFrame(command: request.command, requestID: wireID, payload: request.payload)
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                assembler.reset()
                pending = Request(token: token, wire: wireID, original: request, bytes: frame.encoded(), continuation: continuation)
                startTimer(token)
                writeNext()
            }
        } onCancel: {
            Task { @MainActor [weak self] in
                guard let self, self.pending?.token == token else { return }
                self.disconnect(reason: CancellationError())
            }
        }
    }
    private func nextOperation() throws -> UInt64 {
        guard operation < UInt64.max else { throw BluetoothTransportError.exhausted }
        operation += 1
        return operation
    }
    private func startTimer(_ token: UInt64, timeout: Duration = .seconds(30)) {
        timer?.cancel()
        timer = Task { @MainActor [weak self] in
            do { try await Task.sleep(for: timeout) } catch { return }
            guard let self, self.pending?.token == token || (self.connectionToken == token && self.connecting != nil) else { return }
            self.disconnect(reason: BluetoothTransportError.timeout)
        }
    }
    private func writeNext() {
        guard let peripheral = selected, let input, var request = pending,
              !request.writing, request.sent < request.bytes.count else { return }
        let maximum = min(244, peripheral.maximumWriteValueLength(for: .withResponse))
        guard maximum > 0 else { disconnect(reason: BluetoothTransportError.unavailable); return }
        let end = min(request.bytes.count, request.sent + maximum)
        let fragment = Data(request.bytes[request.sent ..< end])
        request.sent = end; request.writing = true; pending = request
        peripheral.writeValue(fragment, for: input, type: .withResponse)
    }
    private func changeState(_ next: State) { state = next; onStateChanged?(next) }
    private func completeIfReady() {
        guard let request = pending, !request.writing, request.sent == request.bytes.count,
              let response = request.received else { return }
        pending = nil
        timer?.cancel(); timer = nil
        request.continuation.resume(returning: response)
    }
    private func disconnect(reason: Error) {
        if central.isScanning { central.stopScan() }
        timer?.cancel(); timer = nil
        let connection = connecting; connecting = nil
        let request = pending; pending = nil
        assembler.reset(); input = nil; output = nil
        if let selected {
            selected.delegate = nil
            closing = selected
            self.selected = nil
            central.cancelPeripheralConnection(selected)
        }
        changeState(central.state == .poweredOn ? .idle : .unavailable)
        connection?.resume(throwing: reason)
        request?.continuation.resume(throwing: reason)
    }
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central === self.central else { return }
        if central.state != .poweredOn { disconnect(reason: BluetoothTransportError.unavailable); closing = nil }
        else if selected == nil { changeState(.idle) }
    }
    public func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                               advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard central === self.central, discovered.count < 32 || discovered[peripheral.identifier] != nil else { return }
        discovered[peripheral.identifier] = peripheral
        onReadersChanged?(discovered.values.map { BluetoothReader(id: $0.identifier, advertisedName: $0.name) })
    }
    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard central === self.central, peripheral === selected else { return }
        peripheral.discoverServices([Self.service])
    }
    public func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        guard central === self.central, peripheral === selected else { return }
        selected = nil
        disconnect(reason: error ?? BluetoothTransportError.disconnected)
    }
    public func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        guard central === self.central else { return }
        if peripheral === closing { closing = nil; return }
        guard peripheral === selected else { return }
        selected = nil
        disconnect(reason: error ?? BluetoothTransportError.disconnected)
    }
    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard peripheral === selected else { return }
        guard error == nil, let service = peripheral.services?.first(where: { $0.uuid == Self.service }) else {
            disconnect(reason: error ?? BluetoothTransportError.missingService); return
        }
        peripheral.discoverCharacteristics([Self.inputID, Self.outputID], for: service)
    }
    public func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard peripheral === selected, service.uuid == Self.service else { return }
        input = service.characteristics?.first(where: { $0.uuid == Self.inputID && $0.properties.contains(.write) })
        output = service.characteristics?.first(where: { $0.uuid == Self.outputID && $0.properties.contains(.notify) })
        guard error == nil, input != nil, let output else { disconnect(reason: error ?? BluetoothTransportError.missingService); return }
        peripheral.setNotifyValue(true, for: output)
    }
    public func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard peripheral === selected, characteristic === output else { return }
        guard error == nil, characteristic.isNotifying else { disconnect(reason: error ?? BluetoothTransportError.disconnected); return }
        guard let continuation = connecting else { return }
        timer?.cancel(); timer = nil
        connecting = nil
        changeState(.ready)
        continuation.resume()
    }
    public func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard peripheral === selected, characteristic === input, pending?.writing == true else { return }
        if let error { disconnect(reason: error); return }
        pending?.writing = false
        writeNext()
        completeIfReady()
    }
    public func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard peripheral === selected, characteristic === output, let request = pending else { return }
        guard error == nil, let fragment = characteristic.value else { disconnect(reason: error ?? BluetoothTransportError.malformedResponse); return }
        do {
            guard let frame = try assembler.append(fragment, authenticated: true) else { return }
            guard frame.requestID == request.wire else { return }
            guard frame.response, frame.command == request.original.command || frame.command == .error else {
                throw BluetoothTransportError.malformedResponse
            }
            let mapped = try ControlFrame(command: frame.command, response: true, requestID: request.original.requestID, payload: frame.payload)
            pending?.received = mapped
            completeIfReady()
        } catch { disconnect(reason: error) }
    }
}
#endif

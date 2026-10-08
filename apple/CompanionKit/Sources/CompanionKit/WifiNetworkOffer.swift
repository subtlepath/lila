import Foundation

public enum WifiNetworkMode: UInt8, Sendable, Hashable { case savedNetwork = 1, hotspot = 2 }

// Ephemeral key/password material; never persist, cloud-sync or log the envelope.
public struct WifiNetworkOffer: Equatable, Sendable {
    public static let headerSize = 125
    public static let maximumSize = 220
    public let offer: WifiHandoffOffer
    public let mode: WifiNetworkMode
    public let ssid: Data
    public let password: Data

    public init(offer: WifiHandoffOffer, mode: WifiNetworkMode, ssid: Data, password: Data = Data()) throws {
        guard !ssid.isEmpty, ssid.count <= 32, !ssid.contains(0) else { throw ProtocolError.value }
        switch mode {
        case .savedNetwork:
            guard password.isEmpty else { throw ProtocolError.value }
        case .hotspot:
            guard (8...63).contains(password.count),
                  ssid.allSatisfy({ (32...126).contains($0) }),
                  password.allSatisfy({ (32...126).contains($0) }) else { throw ProtocolError.value }
        }
        self.offer = offer; self.mode = mode; self.ssid = ssid; self.password = password
    }
    public init(decoding bytes: Data) throws {
        guard (Self.headerSize...Self.maximumSize).contains(bytes.count) else { throw ProtocolError.length }
        var input = ByteReader(bytes)
        guard try input.number(1) == 1 else { throw ProtocolError.version }
        let offer = try WifiHandoffOffer(decoding: input.take(WifiHandoffOffer.encodedSize))
        guard let mode = WifiNetworkMode(rawValue: UInt8(try input.number(1))) else { throw ProtocolError.value }
        let ssidLength = Int(try input.number(1)), passwordLength = Int(try input.number(1))
        guard bytes.count == Self.headerSize + ssidLength + passwordLength else { throw ProtocolError.length }
        try self.init(offer: offer, mode: mode, ssid: input.take(ssidLength), password: input.take(passwordLength))
    }
    public var encoded: Data {
        var bytes = Data([1])
        bytes.reserveCapacity(Self.headerSize + ssid.count + password.count)
        bytes.append(offer.encoded)
        bytes.append(mode.rawValue); bytes.append(UInt8(ssid.count)); bytes.append(UInt8(password.count))
        bytes.append(ssid); bytes.append(password)
        return bytes
    }
}

// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "CompanionKit",
    platforms: [.iOS(.v18), .macOS(.v15)],
    products: [.library(name: "CompanionKit", targets: ["CompanionKit"])],
    dependencies: [
        .package(url: "https://github.com/weichsel/ZIPFoundation.git", exact: "0.9.20"),
        .package(url: "https://github.com/apple/swift-crypto.git", exact: "3.10.0")
    ],
    targets: [
        .systemLibrary(name: "CSQLite", pkgConfig: "sqlite3", providers: [.brew(["sqlite3"])]),
        .systemLibrary(name: "CCompanionZlib", pkgConfig: "zlib", providers: [.brew(["zlib"])]),
        .target(name: "CTintaScheduler", exclude: ["engine"], cxxSettings: [.headerSearchPath("engine")]),
        .target(name: "CompanionKit", dependencies: ["CTintaScheduler","CSQLite", "CCompanionZlib", .product(name: "ZIPFoundation", package: "ZIPFoundation"),
            .product(name: "Crypto", package: "swift-crypto", condition: .when(platforms: [.linux]))]),
        .testTarget(name: "CompanionKitTests", dependencies: ["CompanionKit", "CSQLite",
            .product(name: "ZIPFoundation", package: "ZIPFoundation")])
    ],
    cxxLanguageStandard: .cxx20
)

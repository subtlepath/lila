import Foundation
import XCTest
@testable import CompanionKit

final class FontTransferPlanTests: XCTestCase {
    private func content(_ name: String) throws -> LibraryContent {
        LibraryContent(id: try ContentID(String(repeating: "a", count: 64)), kind: .font,
                       length: 142, title: "Font", originalFilename: name)
    }
    func testBitmapSizesShareFamilyAndNormalizeExtension() throws {
        XCTAssertEqual(try FontTransferPlan(content: content("Example_14.CPFONT")).destination,
                       "/fonts/Example/Example_14.cpfont")
        XCTAssertEqual(try FontTransferPlan(content: content("Example_18.cpfont")).destination,
                       "/fonts/Example/Example_18.cpfont")
        XCTAssertEqual(try FontTransferPlan(content: content("Example_18.cpfont")).formatVersion, 4)
        XCTAssertEqual(try FontTransferPlan(content: content("Example.OTF")).destination, "/fonts/Example.otf")
    }
    func testRejectsUnsupportedRegistryNamesTraversalAndOverlongUtf8Paths() throws {
        for filename in ["Example.cpfont", "Example_0.cpfont", "Example_256.cpfont", "Example_+14.cpfont",
                         "../Example_14.cpfont", "Example\\_14.cpfont", ".Example_14.cpfont", "_Example_14.cpfont",
                         "Example\u{0}_14.cpfont", String(repeating: "é", count: 40) + "_14.cpfont"] {
            XCTAssertThrowsError(try FontTransferPlan(content: content(filename)), filename)
        }
    }
}

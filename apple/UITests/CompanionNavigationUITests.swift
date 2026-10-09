import Foundation
import XCTest

final class CompanionNavigationUITests: XCTestCase {
    @MainActor
    func testDestinationsAndLibraryRemainAvailableAfterRelaunch() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["-AppleLanguages", "(en)", "-AppleLocale", "en_US"]
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["devices.find"].waitForExistence(timeout: 10))
        try select("Library", in: app)
        let importer = app.buttons["library.import"]
        XCTAssertTrue(importer.waitForExistence(timeout: 5))
        XCTAssertTrue(importer.isEnabled, "The persistent library must initialize successfully")
        try select("Updates", in: app)
        XCTAssertTrue(app.buttons["updates.check"].waitForExistence(timeout: 5))
        try select("Settings", in: app)
        XCTAssertTrue(app.buttons["settings.preferences"].waitForExistence(timeout: 5))
        try select("Devices", in: app)
        XCTAssertTrue(app.buttons["devices.find"].waitForExistence(timeout: 5))
        app.terminate()
        app.launch()
        XCTAssertTrue(app.buttons["devices.find"].waitForExistence(timeout: 10))
        try select("Library", in: app)
        XCTAssertTrue(importer.waitForExistence(timeout: 5))
        XCTAssertTrue(importer.isEnabled)
    }

    @MainActor
    func testContentAndReleasePickersCanBeCancelled() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["-AppleLanguages", "(en)", "-AppleLocale", "en_US"]
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["devices.find"].waitForExistence(timeout: 10))
        try select("Library", in: app)
        let importer = app.buttons["library.import"]
        XCTAssertTrue(importer.waitForExistence(timeout: 5))
        XCTAssertTrue(importer.isEnabled)
        activate(importer)
        try cancelPicker(in: app)
        XCTAssertTrue(importer.isEnabled)
        try select("Updates", in: app)
        let manifestImporter = app.buttons["updates.importManifest"]
        XCTAssertTrue(manifestImporter.waitForExistence(timeout: 5))
        XCTAssertTrue(manifestImporter.isEnabled)
        activate(manifestImporter)
        try cancelPicker(in: app)
        XCTAssertTrue(manifestImporter.isEnabled)
    }

#if os(macOS)
    @MainActor
    func testCommandOOpensImportFromDevices() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["-AppleLanguages", "(en)", "-AppleLocale", "en_US"]
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["devices.find"].waitForExistence(timeout: 10))
        app.typeKey("o", modifierFlags: .command)
        try cancelPicker(in: app)
        XCTAssertTrue(app.buttons["devices.find"].isHittable)
    }
#endif

    @MainActor
    private func cancelPicker(in app: XCUIApplication) throws {
        let cancel = app.buttons["Cancel"]
        XCTAssertTrue(cancel.waitForExistence(timeout: 5), "The system file picker must open")
        activate(cancel)
        let dismissed = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: cancel)
        wait(for: [dismissed], timeout: 5)
        let error = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == true"),
            object: app.alerts["Unable to complete the action"])
        error.isInverted = true
        wait(for: [error], timeout: 1)
    }

    @MainActor
    private func activate(_ control: XCUIElement) {
#if os(macOS)
        control.click()
#else
        control.tap()
#endif
    }

    @MainActor
    private func select(_ name: String, in app: XCUIApplication) throws {
        // sidebarAdaptable uses a tab bar on compact iOS and sidebar controls on Mac/iPad.
        let controls = [app.tabBars.buttons[name], app.buttons[name], app.radioButtons[name], app.cells[name]]
        let control = try XCTUnwrap(controls.first { $0.exists && $0.isHittable }, "Missing destination: \(name)")
        activate(control)
    }
}

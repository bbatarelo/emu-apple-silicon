//
//  TrackerPreHostApp.swift
//  TrackerPreHost
//
//  Host application for the TrackerPreDriver system extension.
//
//  A DriverKit dext is not installed on its own; it ships inside an app that
//  requests activation. This app is intentionally minimal for now and grows
//  into the SwiftUI diagnostics/control surface later
//  (EMU_Tracker_Pre_Development_Guidelines.md Part III).
//

import SwiftUI

@main
struct TrackerPreHostApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .windowResizability(.contentSize)
    }
}

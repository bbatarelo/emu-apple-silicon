//
//  ContentView.swift
//  TrackerPreHost
//
//  Deliberately minimal, per preliminary.md: driver status plus activate and
//  deactivate. The diagnostics UI described in the guidelines comes later, once
//  the driver can report structured statistics over a user client.
//

import SwiftUI

struct ContentView: View {
    @StateObject private var manager = SystemExtensionManager()

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("E-MU Tracker Pre Driver")
                .font(.title2)
                .fontWeight(.semibold)

            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text("Driver status:")
                    .foregroundStyle(.secondary)
                Text(manager.status.message)
                    .textSelection(.enabled)
            }
            .fixedSize(horizontal: false, vertical: true)

            HStack {
                Button("Activate Driver") { manager.activate() }
                Button("Deactivate Driver") { manager.deactivate() }
            }
        }
        .padding(24)
        .frame(minWidth: 420, alignment: .leading)
    }
}

#Preview {
    ContentView()
}

//
//  SystemExtensionManager.swift
//  TrackerPreHost
//
//  Wraps OSSystemExtensionManager activation/deactivation requests.
//

import Foundation
import OSLog
import SystemExtensions

@MainActor
final class SystemExtensionManager: NSObject, ObservableObject {

    /// Must match the dext's PRODUCT_BUNDLE_IDENTIFIER.
    static let driverBundleID = "net.quantum-bit.TrackerPreDriver"

    enum Status: Equatable {
        case idle
        case working(String)
        case needsApproval
        case activated
        case failed(String)

        var message: String {
            switch self {
            case .idle:                return "Not activated"
            case .working(let what):   return what
            case .needsApproval:       return "Approve in System Settings > General > Login Items & Extensions > Driver Extensions"
            case .activated:           return "Active"
            case .failed(let why):     return "Failed: \(why)"
            }
        }
    }

    @Published private(set) var status: Status = .idle

    private let log = Logger(subsystem: "net.quantum-bit.TrackerPreHost",
                             category: "SystemExtension")

    func activate() {
        status = .working("Activating…")
        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: Self.driverBundleID,
            queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func deactivate() {
        status = .working("Deactivating…")
        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: Self.driverBundleID,
            queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }
}

extension SystemExtensionManager: OSSystemExtensionRequestDelegate {

    nonisolated func request(_ request: OSSystemExtensionRequest,
                             actionForReplacingExtension existing: OSSystemExtensionProperties,
                             withExtension replacement: OSSystemExtensionProperties)
    -> OSSystemExtensionRequest.ReplacementAction {
        // During development the version string frequently does not change even
        // though the code did, so always take the newly built extension.
        return .replace
    }

    nonisolated func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        Task { @MainActor in
            self.log.notice("Extension request needs user approval")
            self.status = .needsApproval
        }
    }

    nonisolated func request(_ request: OSSystemExtensionRequest,
                             didFinishWithResult result: OSSystemExtensionRequest.Result) {
        Task { @MainActor in
            switch result {
            case .completed:
                self.log.notice("Extension request completed")
                self.status = .activated
            case .willCompleteAfterReboot:
                self.status = .failed("Completes after reboot")
            @unknown default:
                self.status = .failed("Unknown result \(result.rawValue)")
            }
        }
    }

    nonisolated func request(_ request: OSSystemExtensionRequest,
                             didFailWithError error: Error) {
        Task { @MainActor in
            self.log.error("Extension request failed: \(error.localizedDescription)")
            self.status = .failed(error.localizedDescription)
        }
    }
}

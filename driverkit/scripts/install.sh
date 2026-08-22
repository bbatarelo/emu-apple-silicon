#!/usr/bin/env bash
#
# Installs and activates the driver by launching the host app, which submits the
# OSSystemExtensionRequest. There is no supported way to activate a dext without
# its containing application.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

app="$(app_path)"
[[ -d "$app" ]] || fail "no built app at $app. Run: make build"

if [[ "$SIGNING" == "0" ]]; then
    fail "the app was built unsigned (SIGNING=0), so the system will refuse to
       activate the extension.

       This is expected until Apple grants the DriverKit entitlements.
       See docs/driverkit-entitlement-request.md.

       Once a DriverKit provisioning profile exists:
           make build SIGNING=1 && make install SIGNING=1"
fi

if ! systemextensionsctl developer 2>&1 | grep -qi "developer mode is enabled"; then
    warn "system extension developer mode is off"
    info "unsigned/development builds will be rejected. Enable with:"
    info "    systemextensionsctl developer on"
fi

bold "Launching $app"
info "click 'Activate Driver', then approve in System Settings >"
info "General > Login Items & Extensions > Driver Extensions"
open "$app"

bold "Follow the driver's own log with: make logs"

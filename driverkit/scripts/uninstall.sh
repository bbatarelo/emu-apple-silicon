#!/usr/bin/env bash
#
# Deactivates the driver extension.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

bold "Currently installed driver extensions"
systemextensionsctl list | sed 's/^/  /'

if ! systemextensionsctl list | grep -q "$DRIVER_BUNDLE_ID"; then
    bold "$DRIVER_BUNDLE_ID is not installed; nothing to do"
    exit 0
fi

app="$(app_path)"
if [[ -d "$app" ]]; then
    bold "Deactivating via the host app"
    info "click 'Deactivate Driver' in the window that opens"
    open "$app"
else
    warn "no built app available to submit the deactivation request"
    info "rebuild with 'make build', or reset all extensions with:"
    info "    systemextensionsctl reset    (requires SIP disabled)"
fi

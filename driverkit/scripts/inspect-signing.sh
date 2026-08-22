#!/usr/bin/env bash
#
# Reports the signing/entitlement state of the built products and of the machine,
# so that "why will this not load" has a single answer to look at.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

bold "Signing identities"
security find-identity -v -p codesigning 2>/dev/null | sed 's/^/  /' || info "none"

bold "Provisioning profiles"
profile_dir="$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"
if [[ -d "$profile_dir" ]] && compgen -G "$profile_dir/*.mobileprovision" >/dev/null; then
    for p in "$profile_dir"/*.mobileprovision; do
        plist="$(security cms -D -i "$p" 2>/dev/null)" || continue
        name="$(plutil -extract Name raw -o - - <<<"$plist" 2>/dev/null || echo '?')"
        expires="$(plutil -extract ExpirationDate raw -o - - <<<"$plist" 2>/dev/null || echo '?')"
        info "$name  (expires $expires)"
        # A DriverKit profile is the thing currently missing; call it out clearly.
        if plutil -extract Entitlements.com\\.apple\\.developer\\.driverkit raw -o - - <<<"$plist" >/dev/null 2>&1; then
            info "    ^ carries com.apple.developer.driverkit"
        fi
    done
else
    info "none installed"
fi

bold "DriverKit entitlement status"
identities="$(security find-identity -v -p codesigning 2>/dev/null || true)"
if grep -q "Apple Development" <<<"$identities"; then
    if ! grep -rl "com.apple.developer.driverkit" "$profile_dir" >/dev/null 2>&1; then
        warn "no profile on this machine carries com.apple.developer.driverkit"
        info "the dext can be built and linked, but cannot be signed or loaded"
        info "see docs/driverkit-entitlement-request.md"
    fi
fi

app="$(app_path)"
if [[ ! -d "$app" ]]; then
    bold "No built app at $app (run: make build)"
    exit 0
fi

bold "Host app signature"
codesign -dv --verbose=4 "$app" 2>&1 | sed 's/^/  /' || info "unsigned"

bold "Host app entitlements"
codesign -d --entitlements :- "$app" 2>/dev/null | sed 's/^/  /' || info "none"

dext="$(embedded_driver_path)"
if [[ -d "$dext" ]]; then
    bold "Embedded dext signature"
    codesign -dv --verbose=4 "$dext" 2>&1 | sed 's/^/  /' || info "unsigned"

    bold "Embedded dext entitlements"
    codesign -d --entitlements :- "$dext" 2>/dev/null | sed 's/^/  /' || info "none"
fi

bold "System extension developer mode"
systemextensionsctl developer 2>&1 | sed 's/^/  /' || true

bold "Installed driver extensions"
systemextensionsctl list 2>&1 | sed 's/^/  /' || true

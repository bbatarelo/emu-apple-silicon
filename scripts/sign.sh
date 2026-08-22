#!/usr/bin/env bash
#
# Signs the plug-in bundle.
#
# coreaudiod loads plug-ins into a helper that carries
# com.apple.security.cs.disable-library-validation, so an ordinary Apple
# Development certificate is accepted -- no Developer ID and no notarisation are
# needed to run this on your own machine. Ad-hoc signing works too, which is why
# no Apple Developer account is required at all.

set -euo pipefail
bundle="${1:?usage: sign.sh <bundle>}"

identity="$(security find-identity -v -p codesigning 2>/dev/null \
            | awk '/Apple Development/ {print $2; exit}')"

if [[ -z "$identity" ]]; then
    identity="-"
    echo "  signing ad-hoc (no Apple Development certificate found)"
else
    echo "  signing with Apple Development certificate"
fi

codesign --force --sign "$identity" "$bundle" 2>/dev/null

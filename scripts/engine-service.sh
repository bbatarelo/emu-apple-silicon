#!/usr/bin/env bash
#
# The engine process of the optional two-part build, as a launchd service.
#
#     sudo engine-service.sh install <engine> <user>   register and start it
#     sudo engine-service.sh remove                    stop it and unregister
#          engine-service.sh reload  <engine>          replace the running engine
#          engine-service.sh status
#          engine-service.sh guard-clean <build dir>   refuse if the service runs from it
#
# install and remove are the only parts that need root, and `make install`
# and `make uninstall` call them; reload is the everyday command and needs
# none. The engine itself always runs as <user>, never as root.
#
# Nothing here kills an engine. Stopping is asked for -- by `ctl shutdown` or
# by launchd's SIGTERM -- and the engine answers once USB has been handed
# back. If it does not answer, the files stay where they are and this says so.

set -euo pipefail

service="net.batarelo.EMUTrackerPre.Engine"
plist="/Library/LaunchDaemons/$service.plist"

die() { echo "engine-service: $*" >&2; exit 1; }
loaded() { launchctl print "system/$service" >/dev/null 2>&1; }

# Waits for the service to leave launchd's table: up to 25 s, which is longer
# than launchd itself waits before giving up on a clean exit.
wait_unloaded() {
    for _ in $(seq 1 250); do
        loaded || return 0
        sleep 0.1
    done
    return 1
}

# The executable the registered service runs, or nothing.
registered_engine() {
    [[ -e "$plist" ]] || return 0
    /usr/libexec/PlistBuddy -c "Print :ProgramArguments:0" "$plist" 2>/dev/null || true
}

field() {   # field <name> : first value of "name" in ctl's output on stdin
    sed -n "s/.*\"$1\": \"\{0,1\}\([^\",]*\)\"\{0,1\},\{0,1\}\$/\1/p" | head -1
}

case "${1:-}" in
install)
    engine="${2:?usage: engine-service.sh install <engine> <user>}"
    user="${3:?usage: engine-service.sh install <engine> <user>}"
    [[ $EUID -eq 0 ]] || die "install needs root (run it through make install MODE=two-part)"
    [[ "$user" != root ]] && id -u "$user" >/dev/null 2>&1 || die "'$user' is not an ordinary user"
    [[ "$engine" = /* && -f "$engine" && ! -L "$engine" && -x "$engine" ]] \
        || die "$engine is not an executable file at an absolute path"
    [[ "$(stat -f %Su "$engine")" == "$user" ]] || die "$engine is not owned by $user"

    if loaded; then
        launchctl bootout "system/$service" 2>/dev/null || true
        wait_unloaded || die "the running engine did not stop; nothing was changed"
    fi

    tmp="$(mktemp "$plist.XXXXXX")"
    cat > "$tmp" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$service</string>
	<key>ProgramArguments</key>
	<array>
		<string>$engine</string>
		<string>$service</string>
	</array>
	<key>UserName</key>
	<string>$user</string>
	<key>MachServices</key>
	<dict>
		<key>$service</key>
		<true/>
	</dict>
	<key>KeepAlive</key>
	<true/>
	<key>ThrottleInterval</key>
	<integer>1</integer>
	<key>ProcessType</key>
	<string>Interactive</string>
	<key>LegacyTimers</key>
	<true/>
</dict>
</plist>
EOF
    chown root:wheel "$tmp"
    chmod 644 "$tmp"
    mv -f "$tmp" "$plist"
    launchctl bootstrap system "$plist" || die "launchd refused $plist; it was left in place for inspection"
    loaded || die "the service did not register"
    echo "engine service registered: $engine as $user"
    ;;

remove)
    [[ $EUID -eq 0 ]] || die "remove needs root (run it through make uninstall or make install)"
    if loaded; then
        launchctl bootout "system/$service" 2>/dev/null || true
        wait_unloaded || die "the engine is still registered; $plist was kept"
    fi
    if [[ -e "$plist" ]]; then
        unlink "$plist"
        echo "engine service removed"
    fi
    ;;

reload)
    engine="${2:?usage: engine-service.sh reload <engine>}"
    [[ $EUID -ne 0 ]] || die "reload runs as you, not as root"
    loaded || die "the engine service is not installed (make install MODE=two-part)"
    # The service runs one particular file. Replacing a different one -- this
    # checkout's, when the service was installed from another -- would report
    # a reload of an engine that never changed.
    running_from="$(registered_engine)"
    [[ "$running_from" == "$engine" ]] \
        || die "the installed engine is $running_from, not $engine; reload from that checkout, or reinstall from this one"

    before="$("$engine" ctl status)" || die "the running engine did not answer; nothing was changed"
    old_pid="$(field pid <<<"$before")"
    old_build="$(field build <<<"$before")"
    hal_before="$(field halPid <<<"$before")"
    coreaudio_before="$(pgrep -x coreaudiod || true)"

    # The new image is already in place (make put it there); the old process
    # still runs the old one. Ask it to leave: it answers only after its
    # streams have stopped, and launchd then starts the new image.
    "$engine" ctl shutdown >/dev/null || die "the engine refused to shut down; it is still running"

    after=""
    new_pid=""
    for _ in $(seq 1 150); do
        if after="$("$engine" ctl status 2>/dev/null)"; then
            new_pid="$(field pid <<<"$after")"
            [[ -n "$new_pid" && "$new_pid" != "$old_pid" ]] && break
        fi
        new_pid=""
        sleep 0.1
    done
    [[ -n "$new_pid" ]] || die "no new engine appeared within 15 s"

    # The HAL reconnects on its own within a second or two.
    hal_after="0"
    for _ in $(seq 1 100); do
        after="$("$engine" ctl status 2>/dev/null)" || true
        hal_after="$(field halPid <<<"$after")"
        [[ "${hal_after:-0}" != 0 ]] && break
        sleep 0.1
    done
    coreaudio_after="$(pgrep -x coreaudiod || true)"

    echo "engine     pid $old_pid -> $new_pid"
    echo "           $old_build -> $(field build <<<"$after")"
    echo "coreaudiod pid $coreaudio_before -> $coreaudio_after"
    echo "HAL host   pid ${hal_before:-0} -> ${hal_after:-0}"
    [[ "$coreaudio_before" == "$coreaudio_after" ]] || die "coreaudiod restarted during the reload"
    if [[ "${hal_after:-0}" == 0 ]]; then
        echo "engine-service: the HAL has not reconnected yet; check: make engine-status" >&2
        exit 1
    fi
    [[ "${hal_before:-0}" == 0 || "$hal_before" == "$hal_after" ]] \
        || die "the HAL's host process changed during the reload"
    echo "engine replaced; Core Audio and the HAL were not restarted."
    echo "A device that was streaming is offline until its client reopens it;"
    echo "  EMU_DEVICE=<id> build/bin/hal-check fault none   brings it back at once."
    ;;

guard-clean)
    dir="${2:?usage: engine-service.sh guard-clean <build dir>}"
    running_from="$(registered_engine)"
    if [[ -n "$running_from" && "$running_from" == "$dir"/* ]]; then
        die "the installed engine service runs $running_from.
  Removing it would leave a service launchd cannot start. First either
    make install      (back to the release driver; removes the service), or
    make uninstall"
    fi
    ;;

status)
    if loaded; then echo "engine service: registered"; else echo "engine service: not registered"; fi
    [[ -e "$plist" ]] && echo "  $plist" || true
    ;;

*)
    die "usage: engine-service.sh install <engine> <user> | remove | reload <engine> | status | guard-clean <dir>"
    ;;
esac

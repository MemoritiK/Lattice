#!/usr/bin/env sh
# ---------------------------------------------------------------------------
# Lattice -- uninstall
# ---------------------------------------------------------------------------

set -eu

PREFIX="${PREFIX:-/usr/local}"
BINDIR="$PREFIX/bin"
APPDIR="$PREFIX/share/applications"

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------------------
# Stop anything currently running
# ---------------------------------------------------------------------------

# Kill resident UI + daemon if they're running. Ignore errors -- they
# may not be running at all.
pkill -x lattice        2>/dev/null || true
pkill -x lattice-daemon 2>/dev/null || true

# Also try the pid files, in case the process name was changed.
for pidfile in /tmp/lattice/ui.pid /tmp/lattice/daemon.pid; do
    if [ -r "$pidfile" ]; then
        pid=$(cat "$pidfile" 2>/dev/null || true)
        if [ -n "${pid:-}" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    fi
done

# Give them a moment to exit, then force.
sleep 0.3
pkill -9 -x lattice        2>/dev/null || true
pkill -9 -x lattice-daemon 2>/dev/null || true

# Clean up the runtime directory.
rm -rf /tmp/lattice

# ---------------------------------------------------------------------------
# Remove installed files
# ---------------------------------------------------------------------------

say "Removing binaries from $BINDIR"
rm -f "$BINDIR/lattice"
rm -f "$BINDIR/lattice-daemon"

say "Removing .desktop from $APPDIR"
rm -f "$APPDIR/lattice.desktop"

# Refresh the desktop-entry cache.
update-desktop-database "$APPDIR" 2>/dev/null || true

say "Done."

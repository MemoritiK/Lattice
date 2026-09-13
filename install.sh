#!/usr/bin/env sh
# ---------------------------------------------------------------------------
# Lattice -- build & install
# ---------------------------------------------------------------------------

set -eu

PREFIX="${PREFIX:-/usr/local}"
BINDIR="$PREFIX/bin"
APPDIR="$PREFIX/share/applications"
ICON_NAME="view-app-grid"

CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra}"

UI_PKGS="gtk+-3.0 gtk-layer-shell-0"
DAEMON_PKGS="garcon-1"

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

say "Building lattice"
$CXX $CXXFLAGS ui.cpp db.cpp -o lattice \
    $(pkg-config --cflags --libs $UI_PKGS)

say "Building lattice-daemon"
$CXX $CXXFLAGS -g -O0 daemon.cpp -o lattice-daemon \
    $(pkg-config --cflags --libs $DAEMON_PKGS)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

say "Installing binaries to $BINDIR"
install -d "$BINDIR"
install -m 0755 lattice        "$BINDIR/lattice"
install -m 0755 lattice-daemon "$BINDIR/lattice-daemon"

say "Installing .desktop to $APPDIR"
install -d "$APPDIR"
cat > "$APPDIR/lattice.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=Lattice
GenericName=Application Launcher
Comment=Grid-based application launcher
Exec=$BINDIR/lattice
Icon=$ICON_NAME
Terminal=false
Categories=Utility;System;
Keywords=launcher;apps;grid;lattice;
StartupNotify=false
NoDisplay=false
EOF
chmod 0644 "$APPDIR/lattice.desktop"

# Refresh the desktop-entry cache so the launcher shows up immediately.
update-desktop-database "$APPDIR" 2>/dev/null || true

say "Done."
printf '\n'
printf '  lattice         -> %s\n' "$BINDIR/lattice"
printf '  lattice-daemon  -> %s\n' "$BINDIR/lattice-daemon"
printf '  .desktop        -> %s\n' "$APPDIR/lattice.desktop"

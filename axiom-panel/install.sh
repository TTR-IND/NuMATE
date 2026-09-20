#!/bin/sh
# Replace MATE's native panel launch with axiom-panel.
#
# mate-session starts the panel as a required component:
#   org.mate.session.required-components panel  ->  "mate-panel"
# It resolves that name to mate-panel.desktop, whose stock file lives at
#   /usr/share/applications/mate-panel.desktop
#   Exec=mate-panel
#   X-MATE-Autostart-Phase=Panel
#   X-MATE-Provides=panel
#   X-MATE-AutoRestart=true
#
# XDG application lookup prefers ~/.local/share/applications over /usr.
# Shadowing that desktop file is the intercept. No /usr/bin swap, no root.
#
# The NuMATE installer (stage 7) invokes this script with PREFIX=/usr/local
# and then writes the same desktop intercept system-wide so new accounts
# resolve it too. Do not replace this file with an autostart wrapper.
#
# Usage:
#   ./install.sh              build + install for this user
#   ./install.sh --uninstall  restore stock mate-panel
#   PREFIX=/usr/local ./install.sh   system-wide binary (still user desktop override)

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${PREFIX:-"$HOME/.local"}
BINDIR=$PREFIX/bin
APPDIR=${XDG_DATA_HOME:-$HOME/.local/share}/applications
AUTOSTART=${XDG_CONFIG_HOME:-$HOME/.config}/autostart
SAVED=${XDG_CONFIG_HOME:-$HOME/.config}/mate-session/saved-session
BIN=$BINDIR/axiom-panel
DESKTOP=$APPDIR/mate-panel.desktop
AUTO_DESKTOP=$AUTOSTART/mate-panel.desktop
OWN_DESKTOP=$APPDIR/axiom-panel.desktop

write_desktop()
{
	path=$1
	mkdir -p "$(dirname -- "$path")"
	cat >"$path" <<EOF
[Desktop Entry]
Type=Application
Name=Panel
Comment=Axiom-Shell panel for MATE
Icon=mate-panel
Exec=$BIN
StartupNotify=true
Terminal=false
Categories=GTK;System;Core;
OnlyShowIn=MATE;
NoDisplay=true
X-MATE-AutoRestart=true
X-MATE-Autostart-Phase=Panel
X-MATE-Provides=panel
X-MATE-Autostart-Notify=true
X-GNOME-Autostart-Phase=Panel
EOF
}

patch_saved_session()
{
	restore=$1
	[ -d "$SAVED" ] || return 0
	for f in "$SAVED"/*.desktop; do
		[ -f "$f" ] || continue
		if grep -qE '^Exec=(mate-panel|axiom-panel)\b' "$f"; then
			if [ "$restore" = 1 ]; then
				sed -i 's|^Exec=.*|Exec=mate-panel|' "$f"
			else
				sed -i "s|^Exec=.*|Exec=$BIN|" "$f"
			fi
		fi
	done
}

install_bin()
{
	# /usr/local/bin exists on Devuan and is not user-writable.
	# mkdir -p on an existing dir succeeds; install does not.
	if [ ! -d "$BINDIR" ]; then
		mkdir -p "$BINDIR" 2>/dev/null || sudo mkdir -p "$BINDIR"
	fi
	if [ -w "$BINDIR" ]; then
		install -m 755 "$ROOT/axiom-panel" "$BIN"
	else
		sudo install -m 755 "$ROOT/axiom-panel" "$BIN"
	fi
}

remove_bin()
{
	if [ -e "$BIN" ]; then
		rm -f "$BIN" 2>/dev/null || sudo rm -f "$BIN"
	fi
}

uninstall()
{
	rm -f "$DESKTOP" "$OWN_DESKTOP" "$AUTO_DESKTOP"
	remove_bin
	patch_saved_session 1
	if command -v gsettings >/dev/null 2>&1; then
		gsettings reset org.mate.session.required-components panel 2>/dev/null || true
	fi
	if command -v axiom-panel >/dev/null 2>&1 || [ -x "$BIN" ]; then
		:
	fi
	killall axiom-panel 2>/dev/null || true
	if command -v mate-panel >/dev/null 2>&1; then
		mate-panel >/dev/null 2>&1 &
	fi
	echo "axiom-panel removed. Stock mate-panel restored for the next session."
	exit 0
}

if [ "${1:-}" = "--uninstall" ] || [ "${1:-}" = "uninstall" ]; then
	uninstall
fi

if ! command -v pkg-config >/dev/null 2>&1; then
	echo "pkg-config is required" >&2
	exit 1
fi
if ! pkg-config --exists gtk+-3.0 gdk-x11-3.0 x11 gio-unix-2.0; then
	echo "missing build deps: gtk+-3.0 gdk-x11-3.0 x11 gio-unix-2.0" >&2
	exit 1
fi

make -C "$ROOT"

mkdir -p "$APPDIR" "$AUTOSTART"
install_bin

# Shadow the name mate-session already looks up.
write_desktop "$DESKTOP"
write_desktop "$OWN_DESKTOP"
# Some distros also list the panel under XDG autostart.
write_desktop "$AUTO_DESKTOP"
patch_saved_session 0

if command -v gsettings >/dev/null 2>&1; then
	# Keep the component id as mate-panel so a stock session file still
	# resolves through our shadowed desktop. Pointing it at axiom-panel
	# is the belt; the desktop shadow is the braces.
	gsettings set org.mate.session.required-components panel mate-panel 2>/dev/null || true
fi

# Drop the running stock panel and take the strut.
killall mate-panel 2>/dev/null || true
killall axiom-panel 2>/dev/null || true
"$BIN" >/dev/null 2>&1 &

echo "axiom-panel installed."
echo "  binary     $BIN"
echo "  intercept  $DESKTOP"
echo "mate-session will launch it as the Panel component on next login."
echo "Revert with: $0 --uninstall"

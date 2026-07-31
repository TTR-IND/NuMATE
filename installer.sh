#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
# NuMATE — Desktop Environment Installer
#
# Takes a Devuan system with the standard MATE desktop and turns it into
# NuMATE: MATE's session infrastructure underneath, NuMATE's shell and
# NuMate-Settings on top, with a curated application set.
#
# This installer is a DESKTOP ENVIRONMENT installer and nothing else.
# It does not touch the kernel, the display server, or the init system.
#
# Stages:
#   1. Standard MATE desktop for Devuan
#   2. Purge the MATE components NuMATE replaces
#   3. Curated applications  (Nemo, Engrampa, Pluma, Waterfox, GParted)
#   4. Default-application bindings
#   5. Fonts, themes, cursors, icons, Nemo integration
#   6. NuMate-Settings   (fetched from GitHub — the only settings app)
#   7. NuMATE shell     (built from bin/gonzo-shell.c in this repo)
#   8. Desktop defaults (system-wide dconf + live session)
#
# ── Run this from inside a clone of the NuMATE repo. ──────────────────────
# It resolves repo assets (theme/, nemo/, bin/, backgrounds/) relative to
# its own path. `curl ... | bash` has no "own path" — $0 would be `bash` —
# so those stages would silently find nothing. Clone first, then run.
#
# ── Run as your normal user, NOT with sudo. ───────────────────────────────
# The script calls sudo itself for the commands that need root. Stage 8
# writes into your live session's dconf, which needs your real DISPLAY and
# DBUS_SESSION_BUS_ADDRESS — sudo/su do not forward those.
#
#   ./installer.sh                  — full install
#   ./installer.sh --skip-waterfox  — skip the Waterfox tarball stage
#   ./installer.sh --skip-theme     — skip fonts/themes/cursors
#   ./installer.sh --skip-settings  — skip fetching/building NuMate-Settings
#   ./installer.sh --skip-shell     — skip building the NuMATE shell
#   ./installer.sh --dry-run        — print every stage, change nothing
#
# © 2026 Josh A. Wheatstone — Torfaen Technology Research IND. — AGPLv3
# ═══════════════════════════════════════════════════════════════════════════
set -eu

NUMATE_VERSION="0.1.0"
SCRIPT_DIR="$(readlink -f "$(dirname "$0")")"

# ── Single source of truth for every value used in more than one place ─────
# Anything referenced by two or more stages is defined exactly once, here.
# A second copy of any of these is a bug, not a convenience.
WATERFOX_VERSION="6.6.17"
WATERFOX_URL="https://cdn.waterfox.com/waterfox/releases/${WATERFOX_VERSION}/Linux_x86_64/waterfox-${WATERFOX_VERSION}.tar.bz2"
WATERFOX_PREFIX="/opt/waterfox"
WATERFOX_BIN="/usr/local/bin/waterfox"
WATERFOX_DESKTOP="waterfox.desktop"

SETTINGS_REPO="https://github.com/TTR-IND/NuMate-Settings.git"
SETTINGS_BIN="numate-settings"

GTK_THEME="Fluent-grey-Dark"
WM_THEME="Fluent-grey-Dark-compact"
CURSOR_THEME="Qogir-cursors"
ICON_THEME="Yaru"
UI_FONT="Lato Light 10"
DOC_FONT="DejaVu Sans Book 10"
# Source and destination basenames match deliberately — the file is not
# renamed on install, so a wallpaper set by hand from the repo copy and one
# set by the installer resolve to the same picture-filename in dconf.
WALLPAPER_NAME="numate-wall.png"
WALLPAPER_SRC="$SCRIPT_DIR/backgrounds/$WALLPAPER_NAME"
WALLPAPER_DEST="/usr/share/backgrounds/$WALLPAPER_NAME"

OPT_SKIP_WATERFOX=0
OPT_SKIP_THEME=0
OPT_SKIP_SETTINGS=0
OPT_SKIP_SHELL=0
OPT_DRY_RUN=0

for _arg in "$@"; do
    case "$_arg" in
        --skip-waterfox) OPT_SKIP_WATERFOX=1 ;;
        --skip-theme)    OPT_SKIP_THEME=1 ;;
        --skip-settings) OPT_SKIP_SETTINGS=1 ;;
        --skip-shell)    OPT_SKIP_SHELL=1 ;;
        --dry-run)       OPT_DRY_RUN=1 ;;
        --help|-h)
            sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "Unknown option: $_arg  (try --help)" >&2; exit 1 ;;
    esac
done

# ── Presentation ───────────────────────────────────────────────────────────
_b="" _dim="" _g="" _y="" _r="" _c="" _rst=""
if [ -t 1 ]; then
    _b="\033[1m" _dim="\033[2m" _g="\033[32m" _y="\033[33m"
    _r="\033[31m" _c="\033[36m" _rst="\033[0m"
fi

TOTAL_STAGES=8
STAGE_N=0

banner() {
    printf "\n"
    printf "${_c}    ╭───────────────────────────────────────────────╮${_rst}\n"
    # Box interior is 47 columns wide (see the border rules above). Each
    # line's pad width is 47 minus its own literal prefix — 11 for the
    # first (3 spaces + "NuMATE" + 2 spaces), 4 for the second (3 + "v").
    printf "${_c}    │${_rst}   ${_b}NuMATE${_rst}  ${_dim}%-36s${_rst}${_c}│${_rst}\n" "MATE, brought forward."
    printf "${_c}    │${_rst}   ${_dim}v%-43s${_rst}${_c}│${_rst}\n" "$NUMATE_VERSION"
    printf "${_c}    ╰───────────────────────────────────────────────╯${_rst}\n\n"
}

stage() {
    STAGE_N=$((STAGE_N + 1))
    printf "\n${_b}${_c}[%d/%d]${_rst} ${_b}%s${_rst}\n" "$STAGE_N" "$TOTAL_STAGES" "$*"
    printf "${_dim}      %s${_rst}\n" "────────────────────────────────────────────────"
}

ok()   { printf "      ${_g}✓${_rst}  %s\n" "$*"; }
info() { printf "      ${_dim}·${_rst}  ${_dim}%s${_rst}\n" "$*"; }
warn() { printf "      ${_y}!${_rst}  %s\n" "$*"; }
die()  { printf "      ${_r}✗${_rst}  %s\n" "$*" >&2; exit 1; }

# Every mutating command routes through run(). --dry-run then costs one
# branch in one place rather than a conditional at every call site.
run() {
    if [ "$OPT_DRY_RUN" -eq 1 ]; then
        printf "      ${_dim}[dry-run] %s${_rst}\n" "$*"
        return 0
    fi
    "$@"
}

# ── Guards ─────────────────────────────────────────────────────────────────
[ "$(id -u)" -ne 0 ] || die "Do not run as root or with sudo — run as your normal user.
         The script calls sudo itself where root is actually needed, and
         stage 8 must write into YOUR session's dconf, which root cannot do."

command -v sudo >/dev/null 2>&1 || die "sudo not found — required for the root-owned stages."
command -v apt  >/dev/null 2>&1 || die "apt not found — this installer targets Devuan/Debian."

if [ -r /etc/os-release ]; then
    _distro="$(. /etc/os-release && echo "${ID:-unknown}")"
    case "$_distro" in
        devuan) : ;;
        debian) warn "Detected Debian, not Devuan — should work, but untested." ;;
        *)      warn "Detected '$_distro' — NuMATE targets Devuan. Proceeding anyway." ;;
    esac
fi

banner
[ "$OPT_DRY_RUN" -eq 1 ] && warn "DRY RUN — nothing will be modified."

printf "  ${_b}This installer will:${_rst}\n"
printf "    %s\n" \
    "install the standard MATE desktop, then strip the parts NuMATE replaces" \
    "install Nemo, Engrampa, Pluma, Waterfox and GParted as the defaults" \
    "install NuMATE fonts, themes, cursors and Nemo integration" \
    "fetch and build NuMate-Settings — the single settings application" \
    "build the NuMATE shell and autostart it in the MATE session"
printf "\n  ${_dim}It will NOT touch your kernel, display server, or init system.${_rst}\n"

# Refresh sudo once up front so the long apt stages don't stall on a
# password prompt buried in suppressed output.
if [ "$OPT_DRY_RUN" -eq 0 ]; then
    printf "\n"
    sudo -v || die "sudo authentication failed."
fi

run sudo apt-get update -qq >/dev/null 2>&1 || warn "apt update reported warnings — continuing."


# ═══ 1 ═══ Standard MATE desktop ═══════════════════════════════════════════
stage "Installing the standard MATE desktop"
info "This is the full metapackage — the next stage strips it back."
info "First run downloads a few hundred MB; this takes a while."

run sudo apt-get install -y mate-desktop-environment >/dev/null 2>&1 \
    && ok "mate-desktop-environment installed" \
    || die "MATE install failed — resolve apt errors and re-run."


# ═══ 2 ═══ Purge what NuMATE replaces ══════════════════════════════════════
stage "Removing the MATE components NuMATE replaces"

# The requested removal list, exactly as specified. Two entries are NOT
# Debian/Devuan package names — they are binaries inside other packages,
# verified against the Debian package file lists:
#
#   mate-power-preferences  → /usr/bin/mate-power-preferences, shipped by
#                             mate-power-manager. That same package also
#                             ships /usr/sbin/mate-power-backlight-helper
#                             and org.mate.power.policy, which the NuMATE
#                             shell calls for permission-free brightness
#                             control. Purging the package to get rid of
#                             the preferences GUI would take the backlight
#                             backend with it and break the shell's
#                             brightness slider.
#
#                             Resolution: the package STAYS. The GUI is
#                             hidden instead by masking its .desktop entry
#                             (below), which removes it from every menu and
#                             launcher while leaving the backend intact.
#                             This is a deliberate, flagged deviation from
#                             a literal package purge — see README.
#
#   mate-network-properties → /usr/bin/mate-network-properties, shipped by
#                             mate-control-center, which IS on this list.
#                             Purging mate-control-center removes it. No
#                             separate action needed, and no deviation.
#
PURGE_PACKAGES="mate-control-center mate-panel mate-applets caja file-roller"

# Protect the load-bearing infrastructure BEFORE purging. Removing caja and
# mate-panel takes the mate-desktop-environment metapackage with them (they
# are its dependencies), which leaves everything it pulled in looking
# orphaned. Marking the keep-list manual first means a later autoremove —
# ours or the user's — cannot cascade into the session infrastructure.
KEEP_PACKAGES="mate-desktop-common mate-session-manager mate-settings-daemon \
marco mate-polkit mate-power-manager mate-terminal atril eom"

info "Protecting session infrastructure from autoremove cascade..."
run sudo apt-mark manual $KEEP_PACKAGES >/dev/null 2>&1 || true
ok "Kept: session manager, settings daemon, marco, polkit, power backend"

for _pkg in $PURGE_PACKAGES; do
    if ! apt-cache show "$_pkg" >/dev/null 2>&1; then
        warn "$_pkg — no such package in this suite; skipped"
        continue
    fi
    if dpkg -l "$_pkg" 2>/dev/null | grep -q '^ii'; then
        run sudo apt-get purge -y "$_pkg" >/dev/null 2>&1 \
            && ok "purged $_pkg" \
            || warn "purge of $_pkg reported errors"
    else
        info "$_pkg not installed — nothing to purge"
    fi
done

# mate-power-preferences: hide the GUI, keep the backend. Masking by
# writing a NoDisplay override into /usr/local/share/applications, which
# XDG_DATA_DIRS ranks above /usr/share — so this survives package upgrades
# of mate-power-manager without editing files apt owns.
run sudo install -d /usr/local/share/applications
if [ "$OPT_DRY_RUN" -eq 0 ]; then
    sudo tee /usr/local/share/applications/mate-power-preferences.desktop >/dev/null <<'MASK_POWER_PREFS'
[Desktop Entry]
Type=Application
Name=MATE Power Preferences
Exec=/usr/bin/mate-power-preferences
NoDisplay=true
# NuMATE: hidden, not removed. Its package (mate-power-manager) also ships
# the backlight helper and polkit policy the NuMATE shell needs for
# brightness control. NuMate-Settings owns power configuration instead.
MASK_POWER_PREFS
fi
ok "mate-power-preferences hidden (backlight backend deliberately kept)"

# One targeted autoremove now that the keep-list is pinned manual.
run sudo apt-get autoremove -y >/dev/null 2>&1 || true
ok "orphaned dependencies cleaned"


# ═══ 3 ═══ Curated applications ════════════════════════════════════════════
stage "Installing the NuMATE application set"

# nemo Recommends nemo-fileroller, which would drag file-roller back in
# immediately after stage 2 purged it. --no-install-recommends on nemo
# alone is too blunt (it drops genuinely wanted extensions too), so
# nemo-fileroller is excluded by name instead and the Engrampa .nemo_action
# files in nemo/ provide the archive context menu in its place.
APT_APPS="nemo nemo-image-converter nemo-share nemo-audio-tab nemo-python \
engrampa pluma gparted"

run sudo apt-get install -y $APT_APPS >/dev/null 2>&1 \
    && ok "Nemo (+ extensions), Engrampa, Pluma, GParted" \
    || die "application install failed"

if dpkg -l nemo-fileroller 2>/dev/null | grep -q '^ii'; then
    run sudo apt-get purge -y nemo-fileroller >/dev/null 2>&1 || true
    ok "nemo-fileroller removed (Engrampa actions replace it)"
fi

# ── Waterfox ───────────────────────────────────────────────────────────────
# Not packaged for Devuan — installed from the upstream tarball into /opt,
# which is exactly what the FHS reserves for self-contained third-party
# software. One versioned directory, one stable symlink; upgrading is
# replacing the directory, and nothing else in the system points at the
# version number.
if [ "$OPT_SKIP_WATERFOX" -eq 1 ]; then
    warn "--skip-waterfox: skipping Waterfox"
elif [ "$(dpkg --print-architecture)" != "amd64" ]; then
    warn "Waterfox upstream ships x86_64 only — architecture is $(dpkg --print-architecture); skipped"
else
    info "Downloading Waterfox $WATERFOX_VERSION..."
    run sudo apt-get install -y curl bzip2 ca-certificates >/dev/null 2>&1 || true

    _wf_tmp="$(mktemp -d)"
    trap 'rm -rf "$_wf_tmp"' EXIT

    if [ "$OPT_DRY_RUN" -eq 1 ]; then
        info "[dry-run] fetch + install $WATERFOX_URL → $WATERFOX_PREFIX"
    elif curl -fL# "$WATERFOX_URL" -o "$_wf_tmp/waterfox.tar.bz2"; then
        # Verify before extracting. A captive portal or CDN error page
        # returns HTTP 200 with HTML, which tar would fail on confusingly.
        if tar -tjf "$_wf_tmp/waterfox.tar.bz2" >/dev/null 2>&1; then
            sudo rm -rf "$WATERFOX_PREFIX"
            sudo install -d "$WATERFOX_PREFIX"
            # Upstream tarball has a single top-level waterfox/ directory.
            sudo tar -xjf "$_wf_tmp/waterfox.tar.bz2" -C "$WATERFOX_PREFIX" --strip-components=1
            sudo ln -sf "$WATERFOX_PREFIX/waterfox" "$WATERFOX_BIN"
            ok "Waterfox $WATERFOX_VERSION → $WATERFOX_PREFIX"

            sudo tee "/usr/share/applications/$WATERFOX_DESKTOP" >/dev/null <<WATERFOX_DESKTOP_ENTRY
[Desktop Entry]
Type=Application
Name=Waterfox
GenericName=Web Browser
Comment=Browse the World Wide Web
Exec=$WATERFOX_BIN %u
Icon=$WATERFOX_PREFIX/browser/chrome/icons/default/default128.png
Terminal=false
Categories=Network;WebBrowser;
MimeType=text/html;text/xml;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
StartupWMClass=waterfox
WATERFOX_DESKTOP_ENTRY
            ok "Waterfox desktop entry installed"

            # Register with update-alternatives so `x-www-browser` and
            # anything asking the alternatives system resolves to Waterfox.
            sudo update-alternatives --install /usr/bin/x-www-browser x-www-browser "$WATERFOX_BIN" 200 >/dev/null 2>&1 || true
            sudo update-alternatives --set x-www-browser "$WATERFOX_BIN" >/dev/null 2>&1 || true
            ok "Waterfox registered as x-www-browser"
        else
            warn "Downloaded file is not a valid bzip2 tarball — Waterfox skipped"
            warn "(check $WATERFOX_URL is still valid)"
        fi
    else
        warn "Waterfox download failed — skipped, everything else continues"
    fi
    rm -rf "$_wf_tmp"; trap - EXIT
fi


# ═══ 4 ═══ Default application bindings ════════════════════════════════════
stage "Setting default applications"

# xdg-mime writes per-user defaults; the system-wide equivalent is
# /usr/share/applications/mimeapps.list. Both are written: the system file
# so new accounts inherit the defaults, the per-user call so the invoking
# user's existing profile actually changes now.
set_default() {
    _desktop="$1"; shift
    for _mime in "$@"; do
        run xdg-mime default "$_desktop" "$_mime" 2>/dev/null || true
    done
}

WEB_MIMES="text/html application/xhtml+xml x-scheme-handler/http x-scheme-handler/https"
ARCHIVE_MIMES="application/zip application/x-7z-compressed application/x-rar \
application/x-tar application/gzip application/x-bzip2 application/x-xz \
application/x-compressed-tar application/x-bzip-compressed-tar application/x-xz-compressed-tar"
TEXT_MIMES="text/plain text/x-csrc text/x-chdr text/markdown application/x-shellscript"
FOLDER_MIMES="inode/directory"

if [ "$OPT_SKIP_WATERFOX" -eq 0 ] && [ -f "/usr/share/applications/$WATERFOX_DESKTOP" ]; then
    set_default "$WATERFOX_DESKTOP" $WEB_MIMES
    run xdg-settings set default-web-browser "$WATERFOX_DESKTOP" 2>/dev/null || true
    ok "Waterfox — default web browser"
fi
set_default engrampa.desktop $ARCHIVE_MIMES ; ok "Engrampa — default archive manager"
set_default pluma.desktop    $TEXT_MIMES    ; ok "Pluma — default text editor"
set_default nemo.desktop     $FOLDER_MIMES  ; ok "Nemo — default file manager"

if [ "$OPT_DRY_RUN" -eq 0 ]; then
    sudo tee /usr/share/applications/mimeapps.list >/dev/null <<MIMEAPPS
[Default Applications]
inode/directory=nemo.desktop
text/plain=pluma.desktop
text/html=$WATERFOX_DESKTOP
application/xhtml+xml=$WATERFOX_DESKTOP
x-scheme-handler/http=$WATERFOX_DESKTOP
x-scheme-handler/https=$WATERFOX_DESKTOP
application/zip=engrampa.desktop
application/x-7z-compressed=engrampa.desktop
application/x-rar=engrampa.desktop
application/x-tar=engrampa.desktop
application/gzip=engrampa.desktop
application/x-bzip2=engrampa.desktop
application/x-xz=engrampa.desktop
application/x-compressed-tar=engrampa.desktop
MIMEAPPS
fi
ok "System-wide defaults → /usr/share/applications/mimeapps.list"
run sudo update-desktop-database /usr/share/applications >/dev/null 2>&1 || true


# ═══ 5 ═══ Fonts, themes, cursors, Nemo integration ════════════════════════
if [ "$OPT_SKIP_THEME" -eq 1 ]; then
    stage "Fonts, themes and Nemo integration"
    warn "--skip-theme: skipping"
else
    stage "Installing fonts, themes, cursors and Nemo integration"

    run sudo apt-get install -y yaru-theme-icon yaru-theme-gtk yaru-theme-sound \
        >/dev/null 2>&1 && ok "Yaru icon, GTK and sound themes" \
        || warn "Yaru theme install failed — check repo availability"

    # Lato's Light weight registers under fontconfig as its own family
    # ("Lato Light", not a style of "Lato"), which is why UI_FONT above
    # resolves correctly rather than silently falling back.
    run sudo apt-get install -y fonts-lato fonts-dejavu-core \
        >/dev/null 2>&1 && ok "Lato + DejaVu Sans fonts" \
        || warn "font install failed"

    # Runtime requirements for the vendored theme. The GTK2 half of Fluent
    # needs the Murrine engine present or GTK2 applications silently fall
    # back to a default look with no error; gnome-themes-extra supplies the
    # base widget assets Fluent builds on. These are not build tools — the
    # theme ships prebuilt — but the theme is inert without them.
    run sudo apt-get install -y gtk2-engines-murrine gtk2-engines-pixbuf gnome-themes-extra \
        >/dev/null 2>&1 && ok "Murrine engine + base widget assets" \
        || warn "theme engine install failed — GTK2 apps may look unthemed"

    # ── Fluent GTK theme + Qogir cursors (vendored in theme/) ─────────────
    # Shipped in this repo rather than fetched. Two reasons that outweigh the
    # repo size: the install needs no network beyond apt, and the exact theme
    # revision NuMATE was designed against is the one that gets installed —
    # an upstream rename or restyle cannot silently change the desktop out
    # from under a release.
    #
    # Both are GPL-3.0, and because they ARE redistributed here, upstream's
    # COPYING files inside each theme directory are part of that compliance
    # and must not be stripped. See THIRD-PARTY-LICENCES.md.
    _theme_src="$SCRIPT_DIR/theme"
    if [ ! -d "$_theme_src" ]; then
        warn "theme/ not found at $_theme_src — GTK/cursor themes skipped"
        warn "(expected if you are not running this from inside the repo)"
    else
        run sudo install -d /usr/share/themes /usr/share/icons

        # GTK theme and window-border theme are separate top-level
        # directories, per Fluent's own layout.
        for _variant in "$GTK_THEME" "$WM_THEME"; do
            if [ -d "$_theme_src/gtk-themes/$_variant" ]; then
                run sudo rm -rf "/usr/share/themes/$_variant"
                run sudo cp -rL "$_theme_src/gtk-themes/$_variant" /usr/share/themes/
                ok "$_variant → /usr/share/themes/"
            else
                warn "$_variant missing from theme/gtk-themes/ — skipped"
            fi
        done

        if [ -d "$_theme_src/cursors/$CURSOR_THEME" ]; then
            run sudo rm -rf "/usr/share/icons/$CURSOR_THEME"
            run sudo cp -rL "$_theme_src/cursors/$CURSOR_THEME" /usr/share/icons/
            ok "$CURSOR_THEME → /usr/share/icons/"
        else
            warn "$CURSOR_THEME missing from theme/cursors/ — skipped"
        fi
    fi

    # ── Wallpaper ──────────────────────────────────────────────────────────
    if [ -f "$WALLPAPER_SRC" ]; then
        run sudo install -Dm644 "$WALLPAPER_SRC" "$WALLPAPER_DEST"
        ok "wallpaper → $WALLPAPER_DEST"
    else
        warn "$WALLPAPER_SRC not found — default wallpaper skipped"
    fi

    # ── Nemo actions (Engrampa + wallpaper context menus) ──────────────────
    _nemo_src="$SCRIPT_DIR/nemo"
    if [ -d "$_nemo_src" ]; then
        run sudo install -d /usr/share/nemo/actions
        for _action in "$_nemo_src"/*.nemo_action; do
            [ -e "$_action" ] || continue
            run sudo install -Dm644 "$_action" "/usr/share/nemo/actions/$(basename "$_action")"
        done
        ok "Nemo actions → /usr/share/nemo/actions/"

        if [ -f "$SCRIPT_DIR/bin/numate-set-wallpaper" ]; then
            run sudo install -Dm755 "$SCRIPT_DIR/bin/numate-set-wallpaper" \
                /usr/local/bin/numate-set-wallpaper
            ok "numate-set-wallpaper → /usr/local/bin/"
        else
            warn "bin/numate-set-wallpaper missing — the wallpaper action will not work"
        fi
    else
        warn "nemo/ not found — Nemo actions skipped"
    fi

    # ── Nemo GTK overrides ─────────────────────────────────────────────────
    # These belong in ~/.config/gtk-3.0/gtk.css, NOT inside a theme: GTK
    # loads that file at the highest style-provider priority regardless of
    # which theme is active, so the overrides survive a theme change.
    # Written to /etc/skel for future accounts and directly into every
    # existing real account — unconditionally, every run. Relying on
    # fall-through here has failed before; forcing the file into place has
    # no failure mode worth the subtlety.
    _nemo_css="$_nemo_src/gtk.css"
    if [ -f "$_nemo_css" ]; then
        run sudo install -Dm644 "$_nemo_css" /etc/skel/.config/gtk-3.0/gtk.css
        ok "Nemo gtk.css → /etc/skel (future accounts)"
        if [ "$OPT_DRY_RUN" -eq 0 ]; then
            while IFS=: read -r _u _p _uid _gid _gecos _home _sh; do
                [ "$_uid" -ge 1000 ] && [ "$_uid" -lt 60000 ] || continue
                [ -d "$_home" ] || continue
                sudo install -Dm644 "$_nemo_css" "$_home/.config/gtk-3.0/gtk.css"
                sudo chown -R "$_uid:$_gid" "$_home/.config/gtk-3.0" 2>/dev/null || true
            done < /etc/passwd
        fi
        ok "Nemo gtk.css → existing accounts"
    else
        warn "nemo/gtk.css not found — Nemo CSS overrides skipped"
    fi
fi


# ═══ 6 ═══ NuMate-Settings ════════════════════════════════════════════════
if [ "$OPT_SKIP_SETTINGS" -eq 1 ]; then
    stage "NuMate-Settings"
    warn "--skip-settings: skipping — NuMATE will have NO settings application"
else
    stage "Fetching and building NuMate-Settings"
    info "The single settings application — mate-control-center is gone."

    # Exactly the dependencies NuMate-Settings documents, plus the toolchain.
    # It builds with make, not a bundled install.sh.
    run sudo apt-get install -y git gcc make pkg-config \
        libgtk-3-dev libmatemixer-dev libmate-desktop-dev libxml2-dev \
        libxrandr-dev libxcursor-dev libnm-dev libx11-dev \
        >/dev/null 2>&1 && ok "build toolchain + NuMate-Settings headers" \
        || die "build dependency install failed"

    _gs_dir="$(mktemp -d)"
    if [ "$OPT_DRY_RUN" -eq 1 ]; then
        info "[dry-run] git clone $SETTINGS_REPO && make && make install"
    elif git clone --depth 1 "$SETTINGS_REPO" "$_gs_dir/NuMate-Settings" >/dev/null 2>&1; then
        ok "cloned $SETTINGS_REPO"

        # A running instance holds the old inode open across a replace.
        pkill -9 "$SETTINGS_BIN" 2>/dev/null || true

        if ( cd "$_gs_dir/NuMate-Settings" && make >/dev/null 2>&1 \
             && sudo make install prefix=/usr/local >/dev/null 2>&1 ); then
            ok "NuMate-Settings built and installed"

            # Verify rather than trust `make install` exiting 0. The shell's
            # settings button launches this binary by name and has no visible
            # failure path — if the name is wrong the button is silently dead,
            # which is the worst possible way to find out.
            if command -v "$SETTINGS_BIN" >/dev/null 2>&1; then
                ok "$SETTINGS_BIN on PATH — shell settings button will work"
            else
                warn "$SETTINGS_BIN is NOT on PATH after install"
                warn "the shell's settings button will do nothing until it is"
            fi
        else
            warn "NuMate-Settings build failed — run make by hand in the repo to see why"
        fi
    else
        warn "Could not clone $SETTINGS_REPO"
        warn "If the repo is not published yet, run with --skip-settings and"
        warn "install NuMate-Settings by hand once it is up."
    fi
    rm -rf "$_gs_dir"
fi


# ═══ 7 ═══ NuMATE shell ════════════════════════════════════════════════════
if [ "$OPT_SKIP_SHELL" -eq 1 ]; then
    stage "NuMATE shell"
    warn "--skip-shell: skipping — you will log into a session with no shell"
else
    stage "Building the NuMATE shell"

    run sudo apt-get install -y gcc pkg-config \
        libgtk-3-dev libwnck-3-dev libjson-glib-dev libdbusmenu-gtk3-dev \
        libmatemixer-dev libx11-dev \
        >/dev/null 2>&1 && ok "shell build dependencies" \
        || die "shell dependency install failed"

    _shell_src="$SCRIPT_DIR/bin/gonzo-shell.c"
    if [ ! -f "$_shell_src" ]; then
        warn "bin/gonzo-shell.c not found — shell build skipped"
    elif [ "$OPT_DRY_RUN" -eq 1 ]; then
        info "[dry-run] compile gonzo-shell.c → /usr/local/bin/gonzo-shell"
    else
        _build="$(mktemp -d)"
        gcc "$_shell_src" -o "$_build/gonzo-shell" \
            $(pkg-config --cflags --libs gtk+-3.0 libwnck-3.0 json-glib-1.0 \
                          libmatemixer dbusmenu-gtk3-0.4) \
            -lX11 -lm -DWNCK_I_KNOW_THIS_IS_UNSTABLE \
            && ok "shell compiled" \
            || die "shell build failed — see the compiler output above"

        # Replacing a binary while the old one runs leaves a stale process
        # holding the previous inode. These are the invoking user's own
        # processes; no root needed to signal them.
        pkill -9 gonzo-shell 2>/dev/null || true

        sudo install -Dm755 "$_build/gonzo-shell" /usr/local/bin/gonzo-shell
        ok "gonzo-shell → /usr/local/bin/"
        rm -rf "$_build"
    fi

    # ── Register the shell as a SESSION COMPONENT, not an autostart entry ──
    # This is the part that makes the shell the interface rather than just
    # another program that happens to start.
    #
    # mate-session tracks three required components in
    # org.mate.session.required-components: windowmanager, panel, filemanager.
    # Stage 2 purged mate-panel, but that key still names "mate-panel" — so
    # mate-session starts a session, fails to launch its required panel, and
    # depending on version either logs it or throws a "component failed"
    # dialogue on every login. Leaving the key stale and bolting the shell on
    # via /etc/xdg/autostart would paper over that: the shell would appear,
    # and the session would still be reporting a missing required component
    # underneath.
    #
    # Naming the shell as the panel component fixes the cause. It also buys
    # restart-on-crash for free — mate-session relaunches required components,
    # which an autostart entry does not guarantee.
    #
    # Required components are looked up in /usr/share/applications by desktop
    # id, NOT in /etc/xdg/autostart, so the entry goes there.
    run sudo install -d /usr/share/applications
    if [ "$OPT_DRY_RUN" -eq 0 ]; then
        sudo tee /usr/share/applications/numate-shell.desktop >/dev/null <<'SHELL_COMPONENT'
[Desktop Entry]
Type=Application
Name=NuMATE Shell
Comment=Desktop shell — dock, launcher, tray, notifications
Exec=/usr/local/bin/gonzo-shell
OnlyShowIn=MATE;
X-MATE-Autostart-Phase=Panel
X-MATE-Autostart-Notify=true
X-MATE-AutoRestart=true
NoDisplay=true
SHELL_COMPONENT
    fi
    ok "session component → /usr/share/applications/numate-shell.desktop"

    # An autostart entry from a previous NuMATE install would now launch a
    # SECOND shell alongside the session component — two docks, two trays,
    # two processes racing for the same StatusNotifier bus name. Remove it.
    if [ -f /etc/xdg/autostart/numate-shell.desktop ]; then
        run sudo rm -f /etc/xdg/autostart/numate-shell.desktop
        ok "removed stale autostart entry (superseded by session component)"
    fi

    # Point the session at it. Written system-wide in stage 8 as well, for
    # accounts that do not exist yet; set live here so the very next login
    # of THIS user already uses it.
    if [ "$OPT_DRY_RUN" -eq 0 ] && [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
        gsettings set org.mate.session.required-components panel 'numate-shell' 2>/dev/null \
            && ok "session panel component → numate-shell" \
            || warn "could not set required panel component — check schema is installed"
        gsettings set org.mate.session.required-components filemanager 'nemo' 2>/dev/null \
            && ok "session file manager component → nemo" || true
    else
        info "no session bus — session components set at next login via dconf"
    fi

    # ── Nemo as the desktop ────────────────────────────────────────────────
    # Nemo ships nemo-autostart.desktop, but it is gated OnlyShowIn=X-Cinnamon
    # — MATE does not advertise itself as X-Cinnamon, so that entry never
    # fires here. nemo-desktop itself has no session check; only the .desktop
    # file does. This entry removes a Cinnamon-only gate that was never
    # applicable, rather than working around anything.
    if [ "$OPT_DRY_RUN" -eq 0 ]; then
        sudo tee /etc/xdg/autostart/numate-nemo-desktop.desktop >/dev/null <<'NEMO_AUTOSTART'
[Desktop Entry]
Type=Application
Name=Nemo Desktop
Comment=Draws the desktop icons and background (NuMATE: not Cinnamon-gated)
Exec=nemo-desktop
AutostartCondition=GSettings org.nemo.desktop show-desktop-icons
X-GNOME-AutoRestart=true
X-MATE-Autostart-Phase=Desktop
NoDisplay=true
NEMO_AUTOSTART
    fi
    ok "Nemo desktop autostart → /etc/xdg/autostart/"
fi


# ═══ 8 ═══ Desktop defaults ════════════════════════════════════════════════
stage "Applying NuMATE desktop defaults"

# Two paths, because they solve two different problems:
#   /etc/dconf/db/local.d  — defaults for accounts that do not exist yet.
#   gsettings set          — the invoking user's live session, because
#                            dconf's per-user layer wins over the system
#                            default once ANY value exists for a key.
# Writing only the first silently does nothing for the current user.
run sudo install -d /etc/dconf/db/local.d /etc/dconf/db/local.d/locks
if [ "$OPT_DRY_RUN" -eq 0 ]; then
    sudo tee /etc/dconf/db/local.d/00-numate >/dev/null <<DCONF_DEFAULTS
# NuMATE desktop defaults — system-wide dconf.
# Users can override any of these; this only sets what a fresh account
# starts with.

[org/mate/interface]
gtk-theme='$GTK_THEME'
icon-theme='$ICON_THEME'
font-name='$UI_FONT'
document-font-name='$DOC_FONT'

[org/mate/Marco/general]
theme='$WM_THEME'

[org/mate/session/required-components]
# The shell IS the interface, so it is a required session component rather
# than an autostart entry: mate-session brings it up as part of the session
# and restarts it if it dies. Stage 2 purged mate-panel, so leaving the
# stock value here would leave every session reporting a missing required
# component. Values are desktop ids resolved from /usr/share/applications.
panel='numate-shell'
filemanager='nemo'

[org/mate/peripherals-mouse]
cursor-theme='$CURSOR_THEME'

[org/mate/sound]
theme-name='$ICON_THEME'
event-sounds=true

[org/mate/background]
# mate-desktop's background daemon can draw desktop icons independently of
# Caja. Left true, it fights nemo-desktop for the X root window: two icon
# grids, two right-click menus, last writer per frame wins. Nemo owns the
# desktop in NuMATE, so this must be off.
show-desktop-icons=false
picture-filename='$WALLPAPER_DEST'
picture-options='zoom'

[org/nemo/desktop]
# The counterpart to the key above — tells nemo-desktop to manage the
# desktop. Marked "deprecated" in its schema description, but it is still
# the literal AutostartCondition Nemo's own autostart entry checks;
# "deprecated" here means Nemo's preferences UI no longer exposes a
# toggle, not that the key is inert.
show-desktop-icons=true

[org/nemo/window-state]
start-with-menu-bar=false
DCONF_DEFAULTS
fi
ok "defaults → /etc/dconf/db/local.d/00-numate"

if [ ! -f /etc/dconf/profile/user ]; then
    run sudo install -d /etc/dconf/profile
    if [ "$OPT_DRY_RUN" -eq 0 ]; then
        sudo tee /etc/dconf/profile/user >/dev/null <<'DCONF_PROFILE'
user-db:user
system-db:local
DCONF_PROFILE
    fi
    ok "dconf profile created → /etc/dconf/profile/user"
else
    info "dconf profile already present — left unmodified"
fi

run sudo dconf update >/dev/null 2>&1 || true

# Live session. Run directly, not via su — this script already runs as the
# target user inside their real session, so DISPLAY and
# DBUS_SESSION_BUS_ADDRESS are already correct. `su -` would spawn a fresh
# login shell without them and fail with "Cannot autolaunch D-Bus".
if [ "$OPT_DRY_RUN" -eq 0 ] && [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    gsettings set org.mate.interface gtk-theme "$GTK_THEME"
    gsettings set org.mate.interface icon-theme "$ICON_THEME"
    gsettings set org.mate.interface font-name "$UI_FONT"
    gsettings set org.mate.interface document-font-name "$DOC_FONT"
    gsettings set org.mate.Marco.general theme "$WM_THEME"
    gsettings set org.mate.peripherals-mouse cursor-theme "$CURSOR_THEME"
    gsettings set org.mate.sound theme-name "$ICON_THEME"
    gsettings set org.mate.sound event-sounds true
    gsettings set org.mate.background show-desktop-icons false
    gsettings set org.mate.background picture-filename "$WALLPAPER_DEST"
    gsettings set org.mate.background picture-options 'zoom'
    gsettings set org.nemo.desktop show-desktop-icons true
    gsettings set org.nemo.window-state start-with-menu-bar false
    gsettings set org.mate.session.required-components panel 'numate-shell' 2>/dev/null || true
    gsettings set org.mate.session.required-components filemanager 'nemo' 2>/dev/null || true
    ok "defaults applied to the current session"
else
    info "no session bus detected — defaults will apply at next login"
fi


# ═══ Summary ═══════════════════════════════════════════════════════════════
printf "\n"
printf "${_c}    ╭───────────────────────────────────────────────╮${_rst}\n"
_done_msg="NuMATE $NUMATE_VERSION installed"
printf "${_c}    │${_rst}   ${_b}${_g}%-44s${_rst}${_c}│${_rst}\n" "$_done_msg"
printf "${_c}    ╰───────────────────────────────────────────────╯${_rst}\n\n"

printf "  ${_b}Desktop${_rst}\n"
printf "    %s\n" \
    "MATE session infrastructure (session manager, settings daemon, marco)" \
    "NuMATE shell — the interface: dock, launcher, tray, notifications" \
    "NuMate-Settings — the single settings application" \
    "Nemo — file manager and desktop"
printf "\n  ${_b}Applications${_rst}\n"
printf "    %s\n" \
    "Waterfox $WATERFOX_VERSION  — default web browser" \
    "Engrampa       — default archive manager" \
    "Pluma          — default text editor" \
    "GParted        — disk management"
printf "\n  ${_b}Next${_rst}\n"
printf "    %s\n" \
    "Log out and back in to a MATE session — the shell starts automatically."
printf "\n  ${_dim}mate-power-manager was kept deliberately: its preferences GUI is${_rst}\n"
printf "  ${_dim}hidden, but the backlight helper the shell needs is still there.${_rst}\n\n"

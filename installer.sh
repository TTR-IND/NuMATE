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
#   3. Curated applications  (Nemo, Engrampa, Pluma, Firefox, GParted)
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
#   ./installer.sh --skip-theme     — skip fonts/themes/cursors
#   ./installer.sh --skip-settings  — skip fetching/building NuMate-Settings
#   ./installer.sh --skip-shell     — skip building the NuMATE shell
#   ./installer.sh --dry-run        — print every stage, change nothing
#
# ── Developer options ─────────────────────────────────────────────────────
# For re-running part of a failed install. They assume the earlier stages
# already completed on this machine; on a fresh system, do not use them.
#
#   ./installer.sh --list-stages    — list the stages and their numbers
#   ./installer.sh --from-stage=5   — start at stage 5, run 5 through 8
#   ./installer.sh --only-stage=3   — run stage 3 and nothing else
#
# © 2026 Josh A. Wheatstone — Torfaen Technology Research IND. — AGPLv3
# ═══════════════════════════════════════════════════════════════════════════
set -eu

NUMATE_VERSION="0.1.0"
SCRIPT_DIR="$(readlink -f "$(dirname "$0")")"

# ── Single source of truth for every value used in more than one place ─────
# Anything referenced by two or more stages is defined exactly once, here.
# A second copy of any of these is a bug, not a convenience.
# ── Build dependencies ─────────────────────────────────────────────────────
# Verified against a real Devuan install: every one of these exists and
# resolves. Defined once and shared by stages 6 and 7 rather than listed
# twice, because the two builds overlap almost completely and a package
# added to one list but not the other is a failure that only shows up on
# whichever stage was missed.
#
# The pkg-config module each -dev package provides is noted where the names
# differ, since that mapping is not guessable (module "libnm" comes from
# package libnm-dev; module "mate-desktop-2.0" from libmate-desktop-dev).
BUILD_TOOLCHAIN="gcc make pkg-config git"

BUILD_LIBS="libgtk-3-dev \
libglib2.0-dev \
libmatemixer-dev \
libmate-desktop-dev \
libxml2-dev \
libxrandr-dev \
libxcursor-dev \
libnm-dev \
libx11-dev"

# Shell-only additions. The shell links wnck, json-glib and dbusmenu, which
# the settings app does not use.
SHELL_LIBS="libwnck-3-dev libjson-glib-dev libdbusmenu-gtk3-dev"

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

OPT_SKIP_THEME=0
OPT_SKIP_SETTINGS=0
OPT_SKIP_SHELL=0
OPT_DRY_RUN=0
OPT_FROM_STAGE=1
OPT_ONLY_STAGE=0

for _arg in "$@"; do
    case "$_arg" in
        --skip-theme)    OPT_SKIP_THEME=1 ;;
        --skip-settings) OPT_SKIP_SETTINGS=1 ;;
        --skip-shell)    OPT_SKIP_SHELL=1 ;;
        --dry-run)       OPT_DRY_RUN=1 ;;
        --from-stage=*)  OPT_FROM_STAGE="${_arg#*=}" ;;
        --only-stage=*)  OPT_ONLY_STAGE="${_arg#*=}" ;;
        --list-stages)
            printf "NuMATE installer stages:\n"
            printf "  %s\n" \
                "1  Standard MATE desktop" \
                "2  Purge what NuMATE replaces" \
                "3  Curated applications" \
                "4  Default application bindings" \
                "5  Fonts, themes, cursors, Nemo integration" \
                "6  NuMate-Settings" \
                "7  NuMATE shell" \
                "8  Desktop defaults"
            exit 0 ;;
        --help|-h)
            sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "Unknown option: $_arg  (try --help)" >&2; exit 1 ;;
    esac
done

case "$OPT_FROM_STAGE" in ''|*[!0-9]*) echo "--from-stage needs a number 1-8" >&2; exit 1 ;; esac
case "$OPT_ONLY_STAGE" in ''|*[!0-9]*) echo "--only-stage needs a number 1-8" >&2; exit 1 ;; esac
if [ "$OPT_FROM_STAGE" -lt 1 ] || [ "$OPT_FROM_STAGE" -gt 8 ]; then
    echo "--from-stage must be 1-8 (see --list-stages)" >&2; exit 1
fi
if [ "$OPT_ONLY_STAGE" -gt 8 ]; then
    echo "--only-stage must be 1-8 (see --list-stages)" >&2; exit 1
fi

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

# STAGE_N is set by the dispatcher before each call rather than incremented
# here, so that --from-stage=5 prints [5/8] and not [1/8]. A progress
# counter that lies about which stage failed is worse than none.
stage() {
    printf "\n${_b}${_c}[%d/%d]${_rst} ${_b}%s${_rst}\n" "$STAGE_N" "$TOTAL_STAGES" "$*"
    printf "${_dim}      %s${_rst}\n" "────────────────────────────────────────────────"
}

ok()   { printf "      ${_g}✓${_rst}  %s\n" "$*"; }
info() { printf "      ${_dim}·${_rst}  ${_dim}%s${_rst}\n" "$*"; }
warn() { printf "      ${_y}!${_rst}  %s\n" "$*"; }
die()  { printf "      ${_r}✗${_rst}  %s\n" "$*" >&2; exit 1; }

# ── apt with retry ─────────────────────────────────────────────────────────
# Every package install in this script goes through apt_install(). One
# definition, so retry behaviour cannot differ between call sites.
#
# Why retry at all: apt downloads each package separately, and a dropped
# connection or a mirror hiccup fails the whole transaction. Without retry
# a single lost .deb means starting the several-hundred-megabyte MATE
# download again. apt keeps what it already fetched in /var/cache/apt/
# archives, so a retry resumes rather than restarts — it only re-fetches
# what is actually still missing.
#
# Success is decided by dpkg state, not by apt's exit code. apt exits
# non-zero for trigger warnings and unrelated postinst noise; the question
# that matters is whether the packages are installed.
#
# Usage: apt_install <label> <pkg>...
#   returns 0 if every package ended up installed, 1 otherwise
#   on failure, APT_FAILED holds the packages still missing
APT_RETRIES=4
APT_LOG=""
APT_FAILED=""

pkg_installed() {
    dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "ok installed"
}

apt_install() {
    _label="$1"; shift
    _want="$*"
    APT_FAILED=""

    [ -n "$_want" ] || return 0

    if [ "$OPT_DRY_RUN" -eq 1 ]; then
        info "[dry-run] apt-get install -y $_want"
        return 0
    fi

    APT_LOG="$(mktemp)"
    _try=1
    while [ "$_try" -le "$APT_RETRIES" ]; do
        [ "$_try" -gt 1 ] && info "retry $_try/$APT_RETRIES — resuming, already-downloaded packages are kept"

        sudo apt-get install -y \
            -o Acquire::Retries=3 \
            -o Acquire::http::Timeout=30 \
            -o Acquire::https::Timeout=30 \
            $_want >"$APT_LOG" 2>&1 || true

        # Decide on state.
        APT_FAILED=""
        for _p in $_want; do
            pkg_installed "$_p" || APT_FAILED="$APT_FAILED $_p"
        done
        [ -z "$APT_FAILED" ] && { rm -f "$APT_LOG"; return 0; }

        # Distinguish "cannot ever work" from "try again". A package that
        # does not exist in the suite will never appear no matter how many
        # times we retry, so retrying it just wastes the user's time.
        _retryable=0
        for _p in $APT_FAILED; do
            apt-cache show "$_p" >/dev/null 2>&1 && _retryable=1
        done
        if [ "$_retryable" -eq 0 ]; then
            warn "$_label: package(s) not present in this suite:$APT_FAILED"
            return 1
        fi

        if grep -qiE 'temporary failure|could not resolve|connection failed|connection timed out|unable to connect|hash sum mismatch|failed to fetch|no route to host|network is unreachable' "$APT_LOG"; then
            warn "$_label: network or mirror problem on attempt $_try"
            _try=$((_try + 1))
            [ "$_try" -le "$APT_RETRIES" ] && sleep $((_try * 5))
            continue
        fi

        # Not a network problem — retrying will not change the outcome.
        break
    done

    warn "$_label: failed —$APT_FAILED"
    warn "last 20 lines of apt output:"
    tail -20 "$APT_LOG" >&2
    rm -f "$APT_LOG"
    return 1
}

# Browser resolution lives in one place because two stages need it: stage 3
# to install it, stage 4 to bind it to MIME types. Duplicating the candidate
# list would let the two drift, and a stage started in isolation via
# --from-stage would inherit nothing. Idempotent and safe to call repeatedly.
BROWSER_PKG=""
BROWSER_DESKTOP=""
resolve_browser() {
    BROWSER_PKG=""
    BROWSER_DESKTOP=""
    for _cand in firefox-esr firefox; do
        if apt-cache show "$_cand" >/dev/null 2>&1; then
            BROWSER_PKG="$_cand"
            break
        fi
    done
    if [ -n "$BROWSER_PKG" ] && [ -f "/usr/share/applications/$BROWSER_PKG.desktop" ]; then
        BROWSER_DESKTOP="$BROWSER_PKG.desktop"
    fi
}

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
    "install Nemo, Engrampa, Pluma, Firefox and GParted as the defaults" \
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
stage_1() {
stage "Installing the standard MATE desktop"
info "This is the full metapackage — the next stage strips it back."
info "First run downloads a few hundred MB; this takes a while."

apt_install "MATE desktop" mate-desktop-environment \
    && ok "mate-desktop-environment installed" \
    || die "MATE install failed — see the apt output above."
}

# ═══ 2 ═══ Purge what NuMATE replaces ══════════════════════════════════════
stage_2() {
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
}

# ═══ 3 ═══ Curated applications ════════════════════════════════════════════
stage_3() {
stage "Installing the NuMATE application set"

# Everything here comes from Devuan's own repositories. No tarballs, no
# third-party downloads, nothing from /opt — if it is not packaged, it is
# not installed by this script.

# Split into two lists, because they have different failure semantics.
#
# CORE is the desktop. If any of it is missing, the install is broken and
# should stop loudly rather than leave a half-built system.
#
# EXTRAS are Nemo extensions. A missing extension is a cosmetic loss, not a
# broken desktop. Installing the whole set in one apt call means one
# unavailable package fails the entire transaction and takes the file
# manager down with it — which is exactly how "application install failed"
# happened, with the output suppressed so it named no package.
#
# Extras are therefore installed individually, best-effort, and each one
# reports for itself.
CORE_APPS="nemo engrampa pluma gparted"
EXTRA_APPS="nemo-image-converter nemo-share nemo-audio-tab nemo-python"

# The browser package differs between suites: Debian and Devuan stable ship
# firefox-esr, while firefox proper appears only in unstable and backports.
# Resolve it once against what apt actually has, rather than hardcoding a
# name that is wrong on half the targets.
resolve_browser

if [ -n "$BROWSER_PKG" ]; then
    CORE_APPS="$CORE_APPS $BROWSER_PKG"
    info "browser package resolved to $BROWSER_PKG"
else
    warn "neither firefox-esr nor firefox is available — no browser installed"
fi

# Run ONCE, capturing output. Never re-run to obtain diagnostics: the second
# run observes different state than the first (the first may have already
# unpacked and configured), so it reports on a situation that no longer
# exists and discards the original error permanently.
#
# Then decide on STATE, not on exit status. apt exits non-zero for reasons
# that do not mean "the packages are missing" — a trigger warning, a
# postinst on an unrelated package, a dpkg return path. The question this
# stage actually needs answered is "is every core package installed?", and
# dpkg-query answers that directly. Exit status is only a hint about where
# to look.
apt_install "core applications" $CORE_APPS \
    || die "cannot continue without the core applications"
ok "Nemo, Engrampa, Pluma, GParted${BROWSER_PKG:+, $BROWSER_PKG}"

# Extras: one at a time, never fatal.
for _pkg in $EXTRA_APPS; do
    if ! apt-cache show "$_pkg" >/dev/null 2>&1; then
        warn "$_pkg not available in this suite — skipped"
    elif apt_install "$_pkg" "$_pkg"; then
        ok "$_pkg"
    else
        warn "$_pkg did not install — skipped, desktop is unaffected"
    fi
done

# nemo Recommends nemo-fileroller, which drags file-roller back in after
# stage 2 purged it. The Engrampa .nemo_action files in nemo/ provide the
# archive context menu instead.
if dpkg -l nemo-fileroller 2>/dev/null | grep -q '^ii'; then
    run sudo apt-get purge -y nemo-fileroller >/dev/null 2>&1 || true
    ok "nemo-fileroller removed (Engrampa actions replace it)"
fi
}

# ═══ 4 ═══ Default application bindings ════════════════════════════════════
stage_4() {
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

# The desktop id follows the package name on both Debian and Devuan
# (firefox-esr.desktop / firefox.desktop), but it is checked rather than
# assumed — a wrong id here fails silently and leaves the browser unbound.
# Re-resolved rather than inherited from stage 3, so that starting at
# stage 4 with --from-stage still knows which browser is installed.
resolve_browser

if [ -n "$BROWSER_DESKTOP" ]; then
    set_default "$BROWSER_DESKTOP" $WEB_MIMES
    run xdg-settings set default-web-browser "$BROWSER_DESKTOP" 2>/dev/null || true
    ok "$BROWSER_PKG — default web browser"
else
    warn "no browser desktop entry found — web defaults left unchanged"
fi
set_default engrampa.desktop $ARCHIVE_MIMES ; ok "Engrampa — default archive manager"
set_default pluma.desktop    $TEXT_MIMES    ; ok "Pluma — default text editor"
set_default nemo.desktop     $FOLDER_MIMES  ; ok "Nemo — default file manager"

if [ "$OPT_DRY_RUN" -eq 0 ]; then
    # Written without the browser lines if no browser resolved, rather than
    # writing a mimeapps.list pointing at a .desktop that does not exist —
    # a dangling default is worse than no default, because xdg-open fails
    # instead of falling through to the next candidate.
    _browser_mimes=""
    if [ -n "$BROWSER_DESKTOP" ]; then
        _browser_mimes="text/html=$BROWSER_DESKTOP
application/xhtml+xml=$BROWSER_DESKTOP
x-scheme-handler/http=$BROWSER_DESKTOP
x-scheme-handler/https=$BROWSER_DESKTOP"
    fi

    sudo tee /usr/share/applications/mimeapps.list >/dev/null <<MIMEAPPS
[Default Applications]
inode/directory=nemo.desktop
text/plain=pluma.desktop
$_browser_mimes
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
}

# ═══ 5 ═══ Fonts, themes, cursors, Nemo integration ════════════════════════
stage_5() {
if [ "$OPT_SKIP_THEME" -eq 1 ]; then
    stage "Fonts, themes and Nemo integration"
    warn "--skip-theme: skipping"
else
    stage "Installing fonts, themes, cursors and Nemo integration"

    apt_install "Yaru themes" yaru-theme-icon yaru-theme-gtk yaru-theme-sound \
        && ok "Yaru icon, GTK and sound themes" \
        || warn "Yaru theme install failed — desktop will use default icons"

    # Lato's Light weight registers under fontconfig as its own family
    # ("Lato Light", not a style of "Lato"), which is why UI_FONT above
    # resolves correctly rather than silently falling back.
    apt_install "fonts" fonts-lato fonts-dejavu-core \
        && ok "Lato + DejaVu Sans fonts" \
        || warn "font install failed — UI font will fall back"

    # Runtime requirements for the vendored theme. The GTK2 half of Fluent
    # needs the Murrine engine present or GTK2 applications silently fall
    # back to a default look with no error; gnome-themes-extra supplies the
    # base widget assets Fluent builds on. These are not build tools — the
    # theme ships prebuilt — but the theme is inert without them.
    apt_install "theme engines" gtk2-engines-murrine gtk2-engines-pixbuf gnome-themes-extra \
        && ok "Murrine engine + base widget assets" \
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
}

# ═══ 6 ═══ NuMate-Settings ════════════════════════════════════════════════
stage_6() {
if [ "$OPT_SKIP_SETTINGS" -eq 1 ]; then
    stage "NuMate-Settings"
    warn "--skip-settings: skipping — NuMATE will have NO settings application"
else
    stage "Fetching and building NuMate-Settings"
    info "The single settings application — mate-control-center is gone."

    # Exactly the dependencies NuMate-Settings documents, plus the toolchain.
    # It builds with make, not a bundled install.sh.
    apt_install "build dependencies" $BUILD_TOOLCHAIN $BUILD_LIBS \
        && ok "build toolchain + NuMate-Settings headers" \
        || die "build dependency install failed — see the apt output above"

    # Verify by pkg-config, not by apt. The build consumes modules, and a
    # module that does not resolve fails the compile no matter what dpkg
    # thinks about the package that was supposed to provide it.
    if [ "$OPT_DRY_RUN" -eq 0 ]; then
        _unresolved=""
        for _m in gtk+-3.0 glib-2.0 gio-2.0 gio-unix-2.0 libmatemixer \
                  mate-desktop-2.0 libxml-2.0 xrandr xcursor libnm x11; do
            pkg-config --exists "$_m" 2>/dev/null || _unresolved="$_unresolved $_m"
        done
        if [ -n "$_unresolved" ]; then
            warn "these pkg-config modules do not resolve:$_unresolved"
            die "the build cannot succeed until they do"
        fi
        ok "all 11 pkg-config modules resolve"
    fi

    _gs_dir="$(mktemp -d)"
    if [ "$OPT_DRY_RUN" -eq 1 ]; then
        info "[dry-run] git clone $SETTINGS_REPO && make && make install"
    elif git clone --depth 1 "$SETTINGS_REPO" "$_gs_dir/NuMate-Settings" >/dev/null 2>&1; then
        ok "cloned $SETTINGS_REPO"

        # A running instance holds the old inode open across a replace.
        pkill -9 "$SETTINGS_BIN" 2>/dev/null || true

        # Compile and install are SEPARATE steps with separate logs. Chained
        # with && behind one suppressed redirect, a failing `make install`
        # is indistinguishable from a failing `make`, and the message names
        # the wrong one — which is exactly what "build failed" reported when
        # the compile had actually succeeded.
        _mk_log="$(mktemp)"
        _build_ok=0
        if ( cd "$_gs_dir/NuMate-Settings" && make ) >"$_mk_log" 2>&1; then
            ok "NuMate-Settings compiled"
            _build_ok=1
        else
            warn "COMPILE failed — last 25 lines:"
            tail -25 "$_mk_log" >&2
        fi

        if [ "$_build_ok" -eq 1 ]; then
            if ( cd "$_gs_dir/NuMate-Settings" && sudo make install prefix=/usr/local ) \
                   >"$_mk_log" 2>&1; then
                ok "NuMate-Settings installed"
            else
                warn "compile succeeded but INSTALL failed — last 25 lines:"
                tail -25 "$_mk_log" >&2
            fi
        fi
        rm -f "$_mk_log"

        if [ "$_build_ok" -eq 1 ]; then

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
        fi
    else
        warn "Could not clone $SETTINGS_REPO"
        warn "If the repo is not published yet, run with --skip-settings and"
        warn "install NuMate-Settings by hand once it is up."
    fi
    rm -rf "$_gs_dir"
fi
}

# ═══ 7 ═══ NuMATE shell ════════════════════════════════════════════════════
stage_7() {
if [ "$OPT_SKIP_SHELL" -eq 1 ]; then
    stage "NuMATE shell"
    warn "--skip-shell: skipping — you will log into a session with no shell"
else
    stage "Building the NuMATE shell"

    apt_install "shell dependencies" $BUILD_TOOLCHAIN $BUILD_LIBS $SHELL_LIBS \
        && ok "shell build dependencies" \
        || die "shell dependency install failed — see the apt output above"

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
}

# ═══ 8 ═══ Desktop defaults ════════════════════════════════════════════════
stage_8() {
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

# `dconf update` is what compiles /etc/dconf/db/local.d into the binary
# /etc/dconf/db/local that the GIO backend actually reads. Without it every
# gsettings call emits "unable to open file '/etc/dconf/db/local'" and the
# system-wide defaults never reach a new account.
#
# The command ships in dconf-cli, which is NOT pulled in by a minimal
# Devuan install — gsettings comes from libglib2.0-bin and works without
# it, so the absence is invisible until the warnings appear.
apt_install "dconf tools" dconf-cli dconf-gsettings-backend \
    || warn "could not install dconf-cli — system-wide defaults may not compile"

if [ "$OPT_DRY_RUN" -eq 0 ]; then
    _dconf_log="$(mktemp)"
    sudo dconf update >"$_dconf_log" 2>&1 || true

    # Verify by state: the database file either exists or it does not.
    # `dconf update` exits 0 in situations where it has written nothing.
    if [ -f /etc/dconf/db/local ]; then
        ok "dconf database compiled → /etc/dconf/db/local"
    else
        warn "/etc/dconf/db/local was NOT created"
        warn "system-wide defaults will not apply to new accounts"
        [ -s "$_dconf_log" ] && tail -10 "$_dconf_log" >&2
    fi
    rm -f "$_dconf_log"
else
    info "[dry-run] dconf update"
fi

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
}

# ═══ Dispatcher ════════════════════════════════════════════════════════════
# Stages are functions so they can be addressed individually. The order is
# fixed and the list is the single definition of what a full install is —
# adding a stage means adding it here and nowhere else.

if [ "$OPT_ONLY_STAGE" -gt 0 ]; then
    _first="$OPT_ONLY_STAGE"; _last="$OPT_ONLY_STAGE"
else
    _first="$OPT_FROM_STAGE"; _last=8
fi

if [ "$_first" -ne 1 ] || [ "$_last" -ne 8 ]; then
    warn "developer mode: running stages $_first-$_last only"
    warn "earlier stages are assumed already complete on this machine"
fi

_n="$_first"
while [ "$_n" -le "$_last" ]; do
    STAGE_N="$_n"
    "stage_$_n"
    _n=$((_n + 1))
done

if [ "$_last" -ne 8 ]; then
    printf "\n"
    ok "stages $_first-$_last complete (partial run — no summary)"
    printf "\n"
    exit 0
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
    "${BROWSER_PKG:-no browser} — default web browser" \
    "Engrampa       — default archive manager" \
    "Pluma          — default text editor" \
    "GParted        — disk management"
printf "\n  ${_b}Next${_rst}\n"
printf "    %s\n" \
    "Reboot to start NuMATE — you will be prompted below."
printf "\n  ${_dim}mate-power-manager was kept deliberately: its preferences GUI is${_rst}\n"
printf "  ${_dim}hidden, but the backlight helper the shell needs is still there.${_rst}\n\n"

# ── Reboot ─────────────────────────────────────────────────────────────────
# A reboot rather than just a re-login: this run replaced the session's
# required panel component, installed a new session component, and compiled
# a new system-wide dconf database. A fresh login picks up most of that, but
# mate-session, dbus and the settings daemon are all long-lived and a reboot
# is the one action guaranteed to start every part of the desktop from the
# state now on disk.
#
# Read from /dev/tty rather than stdin so the prompt still works if the
# script was invoked with its input redirected.
if [ "$OPT_DRY_RUN" -eq 1 ]; then
    info "[dry-run] would prompt to reboot"
elif [ ! -t 0 ] && [ ! -e /dev/tty ]; then
    warn "not an interactive terminal — reboot when convenient"
else
    printf "  ${_b}Reboot now to start NuMATE?${_rst} [Y/n] "
    _reply=""
    read -r _reply </dev/tty 2>/dev/null || _reply="n"
    case "${_reply:-y}" in
        [Nn]*)
            printf "\n"
            info "Not rebooting. Run 'sudo reboot' when you are ready."
            printf "\n"
            ;;
        *)
            printf "\n"
            info "Rebooting in 5 seconds — press Ctrl-C to cancel."
            sleep 5
            sudo reboot
            ;;
    esac
fi

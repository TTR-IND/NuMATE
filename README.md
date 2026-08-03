# NuMATE

### MATE, brought forward.

![NuMATE desktop](scrn1.png)

![NuMATE settings](scrn2.png)

NuMATE is a modernisation of the MATE desktop for people who still want a **proper, traditional desktop**.

MATE got the desktop model right. NuMATE keeps that model while bringing the implementation, appearance, and hardware support forward.

No workflow reinvention. No bloat. No web-app desktop. No chasing whatever desktop fashion is currently fashionable.

Just a fast, clean desktop that gets out of your way.

## Installation

NuMATE installs on top of Devuan systems running x11 or xLibre.

Clone the current pre-release tag and run the installer as your normal user —
**not** with `sudo`:

```sh
git clone --branch 1.0-Preview --depth 1 https://github.com/TTR-IND/NuMATE.git
cd NuMATE
./installer.sh
```

`--branch 1.0-Preview` matters. Cloning without it gives you `main`, which is the
development branch and is not what you want unless you are working on NuMATE
itself. `--depth 1` skips the history, which is a smaller download.

You can also download the release zip from the
[**Releases**](https://github.com/TTR-IND/NuMATE/releases) page and unpack it —
the result is the same tree.

The installer calls `sudo` itself for the steps that need root. Running the whole
thing as root breaks the final stage, which writes into your live session's dconf
and needs your real `DISPLAY` and `DBUS_SESSION_BUS_ADDRESS`.

`curl | bash` will not work — the installer resolves `theme/`, `nemo/` and `bin/`
relative to its own path, and a piped invocation has no own path. Clone first.

### Options

| Flag | Effect |
|---|---|
| `--dry-run` | Print every stage, change nothing. Run this first. |
| `--skip-theme` | Skip fonts, themes and cursors |
| `--skip-settings` | Skip fetching and building NuMate-Settings |
| `--skip-shell` | Skip building the NuMATE shell |

### Developer options

For re-running part of a failed install. They assume the earlier stages already
completed on this machine — **do not use them on a fresh system.**

| Flag | Effect |
|---|---|
| `--list-stages` | List the eight stages and their numbers |
| `--from-stage=5` | Start at stage 5 and run through to the end |
| `--only-stage=3` | Run stage 3 and nothing else |

Stage numbers are stable and printed in the `[n/8]` progress marker, so the
number in a failure message is the number to resume from.

## What the installer does

It is a **desktop environment installer and nothing else**. It does not touch the
kernel, the display server, or the init system.

1. **Standard MATE** — installs `mate-desktop-environment` in full.
2. **Strip** — purges the components NuMATE replaces: `mate-control-center`,
   `mate-panel`, `mate-applets`, `caja`, `file-roller`.
3. **Applications** — Nemo (file manager *and* desktop), Engrampa (archiver),
   Pluma (text editor), Waterfox (browser), GParted.

   Waterfox is installed from [BrowserWorks' signed apt
   repository](https://download.opensuse.org/repositories/isv:/BrowserWorks/Debian_13/),
   registered by the installer with its key pinned via `signed-by=` so it can
   only validate that one source. It is a normal apt package and updates with
   the rest of the system — no tarball, nothing in `/opt`.

   **That repository currently ships a beta** (6.7.0~beta.3 as of August 2026).
   If the repository cannot be reached, no browser is installed and the
   installer says so — re-run stage 3 once it is available.
4. **Defaults** — binds those applications to their MIME types, system-wide and
   for the invoking user.
5. **Appearance** — Fluent-grey-Dark, Qogir cursors, Yaru icons, Lato Light and
   DejaVu Sans, plus the Nemo context-menu actions and GTK overrides.

   The GTK theme and cursors are **vendored in `theme/`**, so an install needs no
   network beyond apt and always gets the exact revision NuMATE was designed
   against. Both are GPL-3.0 and their licence files must stay in place — see
   [THIRD-PARTY-LICENCES.md](THIRD-PARTY-LICENCES.md).
6. **NuMate-Settings** — the newest published release is downloaded from
   [its own repository](https://github.com/TTR-IND/NuMate-Settings/releases) and
   built with `make`. It is the only settings application; `mate-control-center`
   is gone. Only released versions are installed — if no release can be
   downloaded the stage stops rather than falling back to development code.
7. **Shell** — builds `bin/gonzo-shell.c` and registers it as the MATE session's
   **required `panel` component**, taking the slot `mate-panel` used to occupy.
   Not an autostart entry: mate-session brings the shell up as part of the
   session and restarts it if it dies. The shell *is* the interface.
8. **Desktop defaults** — written both as system-wide dconf defaults (for
   accounts that do not exist yet) and directly into the current session.

### Two deliberate deviations

Both are flagged here rather than resolved silently.

**`mate-power-preferences` is hidden, not purged.** It is not a package — it is
`/usr/bin/mate-power-preferences`, shipped by `mate-power-manager`. That same
package also ships `/usr/sbin/mate-power-backlight-helper` and its polkit policy,
which the NuMATE shell calls for permission-free brightness control. Purging the
package to remove the GUI would take the backlight backend with it and break the
brightness slider. The installer instead masks the `.desktop` entry via
`/usr/local/share/applications`, which `XDG_DATA_DIRS` ranks above `/usr/share` —
so the GUI disappears from every menu and the masking survives package upgrades.

**`mate-network-properties` needs no separate action.** It is also not a package —
it is `/usr/bin/mate-network-properties`, shipped by `mate-control-center`, which
*is* purged. Removing that package removes this binary.

## Components

| Piece | Source | Installed to |
|---|---|---|
| NuMATE shell | `bin/gonzo-shell.c` in this repo | `/usr/local/bin/gonzo-shell` |
| NuMate-Settings | [its newest release](https://github.com/TTR-IND/NuMate-Settings/releases) | `/usr/local/bin/numate-settings` |
| Wallpaper helper | `bin/numate-set-wallpaper` | `/usr/local/bin/` |
| Nemo actions | `nemo/*.nemo_action` | `/usr/share/nemo/actions/` |
| Themes and cursors | `theme/` (vendored) | `/usr/share/themes/`, `/usr/share/icons/` |

## Licensing

NuMATE is AGPLv3. **NuMate-Settings is GPL-2.0-only** and must therefore always
remain a separate process launched by the shell — never linked into it. See the
audit for why.

 Applications come from Devuan's own repositories; the GTK
theme and cursors in `theme/` are GPL-3.0 and are redistributed with this
repository, which is compatible but carries attribution obligations. See
[THIRD-PARTY-LICENCES.md](THIRD-PARTY-LICENCES.md) for the full audit and the
open items.

NuMate-Settings is a **separate repository and a build-time dependency**, not a
vendored copy. It owns the definition of how it is built; this installer clones it
and calls its `install.sh` rather than duplicating that compile line.

## Why NuMATE?

There is a large group of Linux users who don't want a radically different way of using their computer.

They want:

* A conventional desktop and windowed workflow
* A dock or panel that stays where they expect it
* Straightforward application launching
* Sensible settings
* Familiar interaction patterns
* Modern visual design
* Excellent performance on modest hardware
* A desktop that doesn't require a powerful GPU to feel responsive

NuMATE exists for those people.

It is **MATE modernised, not MATE reinvented**.

## AI Disclosure

Because apparently this needs to be said upfront:

**The shell is written entirely by me.** I did it myself because I wanted it done right.

AI was used with strict heuristic guidance for the following:

- **Architectural decision-making** — discussing design patterns, weighing trade-offs.
- **Research and inspiration** — understanding how projects like Cairo-Dock, UKUI, and ChromeOS handle certain problems.
- **Problem-solving** — bouncing ideas around when I'm stuck.
- **Learning the language** — no one can become a C expert overnight and I would like to have my desktop during my lifetime.

Some concepts in NuMATE are inspired by (but not copied from) projects including:

- **Cairo-Dock** — for its elegant dock behaviour.
- **UKUI** — for modern settings presentation.
- **ChromeOS** — for its clean, focused desktop metaphor.

**Here's what NuMATE does NOT tolerate:** spaghetti code, unnecessary complexity, and sloppy architecture.

## Contributions

NuMATE does not accept outside contributions — no pull requests, no feature requests, no roadmap input.

This isn't a swipe at anyone who'd want to. It's that NuMATE is built to one person's judgement about what a desktop should be, and that only works if the judgement stays singular. A tiling mode, a different launcher paradigm, chat-app integration — these might be good ideas for a different desktop. They're not what this one is for.

You're welcome to read the code, learn from it, and run it. If you want a desktop shaped differently, fork it and take it wherever you like — that's what the license is for.

None of this means I've got my head in the sand. I keep an eye on social media and read the feedback that comes in. If something raised there lines up with where the rest of the industry is actually going, not just one person's preference, I'll consider it. The decision's still mine, but I'm not ignoring the room.

## Moving off GitHub

NuMATE is in the process of migrating off Git and GitHub to **Chunk**, an in-house version control and repository hosting system running on our own infrastructure. The repository and its history will remain browsable once the migration is complete; the links in this README will be updated to point there when it happens. Until then, the GitHub links below are current.


## About the Founder

My name is Josh. I'm the founder and lead technical director of NuMATE.

For the past ten years, I've worked in domestic appliance repair—consoles, phones, televisions, tablets, and more. I've dug through thousands of different firmware images and system implementations inside jukeboxes, gambling machines, and all sorts of embedded hardware that most people never think about.

That work taught me something: **software can be fast and efficient, or it can be bloated and incoherent**. I know what makes software fast and efficient because I've spent years making it work on machines that had no business running anything at all.

NuMATE is my attempt to consolidate that experience into something everyone can download and use. It's my vision of how a modern desktop should feel—responsive, clean, and built for people who just want to get work done.

## Design

NuMATE takes inspiration from contemporary desktops such as ChromeOS and UKUI while retaining the usability and familiarity that made traditional Linux desktops good in the first place.

The desktop shell uses a clean dock-oriented interface with restrained visual effects, adaptive behaviour and a deliberately compact footprint.

The settings application brings the same philosophy to system configuration: modern presentation without turning basic configuration into an exercise in archaeology.

## Engineering

NuMATE's Shell is written in **C** and designed around the principle that a desktop environment should not need a space-age computer to display a few windows.

The goal is simple:

> **It's a desktop, not a space rocket.**

That means keeping the dependency graph sane, avoiding unnecessary abstraction, and paying attention to startup time, memory consumption and responsiveness.

NuMATE is intended to remain comfortable on hardware ranging from modern systems down to machines that other contemporary desktops have long since abandoned.

## Built for the MATE user

NuMATE isn't trying to convince you that the traditional desktop is obsolete.

If you've used MATE for years and thought:

> *"I love this desktop. I just wish somebody would actually modernise it."*

NuMATE is for you.

The familiar desktop metaphor stays.

The outdated implementation doesn't have to.

## Goals

* **Fast** — responsive on everything from modern workstations to genuinely old hardware.
* **Lightweight** — minimal resource consumption and dependency bloat.
* **Familiar** — preserve the traditional desktop workflow.
* **Modern** — contemporary visual design and current hardware support.
* **Native** — native applications and native toolkits; no web-stack nonsense.
* **Portable** — remain independent of any particular Linux distribution or init system.
* **Maintainable** — clean, understandable C code rather than layers of unnecessary abstraction.

## Status

NuMATE is currently under active development. I have been working on the project for nearly a year now and hope to have a release ready soon. Thank you very much for your interest.

The project is being built incrementally, with the desktop shell, settings, and supporting components being modernised and consolidated into a coherent desktop environment.

## Philosophy

NuMATE doesn't need to revolutionise the desktop.

It needs to make the boring things **excellent**.

Open the menu.

Launch an application.

Move a window.

Switch windows.

Open a file.

Change a display setting.

Connect to a network.

Adjust the volume.

Lock the screen.

Close the application.

These things should simply work.

**NuMATE — a proper desktop, modernised.**

> © 2026 Josh A. Wheatstone - Torfaen Technology Research IND. - AGPLV3
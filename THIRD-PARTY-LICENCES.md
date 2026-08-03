# Third-party components

NuMATE itself is **AGPLv3**. Applications are installed from Devuan's own
repositories and are not redistributed here. The GTK theme and cursors **are**
vendored in `theme/` and therefore *are* redistributed by this repository —
which is a deliberate trade (offline installs, and a pinned revision that
upstream cannot change underneath a release) with real compliance obligations
attached. Those are set out below.

## Themes and cursors (vendored in `theme/`)

Both are GPL-3.0, which is compatible with AGPLv3 — they can be distributed
alongside NuMATE without either licence infecting the other, because they are
separate works aggregated in one repository rather than linked into one program.

| Component | Upstream | Licence | Licence file present |
|---|---|---|---|
| Fluent GTK theme | [vinceliuice/Fluent-gtk-theme](https://github.com/vinceliuice/Fluent-gtk-theme) | **GPL-3.0** | ✅ `COPYING` in each theme directory |
| Qogir cursors | [vinceliuice/Qogir-icon-theme](https://github.com/vinceliuice/Qogir-icon-theme) | **GPL-3.0** (inferred) | ❌ **none — see flag** |
| capitaine-cursors | [keeferrourke/capitaine-cursors](https://github.com/keeferrourke/capitaine-cursors) | **LGPL-3.0** | Upstream of Qogir cursors |
| Yaru icons | [ubuntu/yaru](https://github.com/ubuntu/yaru) (via `yaru-theme-icon`) | **CC-BY-SA 4.0** (assets) / **GPL-3.0** (scripts) | Distribution package — not vendored |

**Do not strip the `COPYING` files** from `theme/gtk-themes/*/`. GPL-3.0 requires
the licence text to travel with redistributed code, and those files are how
NuMATE satisfies that. They are the compliance, not clutter.

### ⚠ Flag: Qogir cursors ship no licence file

`theme/cursors/Qogir-cursors/` and `Qogir-white-cursors/` contain only
`index.theme` and the compiled cursors — **no `COPYING`, no `LICENSE`**. The
licence has to be inferred from the parent repository (GPL-3.0) and from
upstream's README, which states the cursors are *"based on capitaine-cursors"*
(LGPL-3.0).

That inference is almost certainly correct, but it is an inference — and because
these files are now vendored, **NuMATE is redistributing them**. That raises this
from a low-severity note to something worth ten minutes:

- **Cheap fix, do it now:** add a short `theme/cursors/README.md` recording where
  the cursors came from, the upstream repo URL, the inferred licence, and the
  capitaine-cursors attribution. Attribution you can evidence is most of what
  GPL-3.0 §4 and LGPL-3.0 actually ask for here.
- **Proper fix:** open an issue on Qogir-icon-theme asking for a `LICENSE` in
  `src/cursors/`, then drop the real file in. Costs nothing and settles it
  permanently.

## Applications (distribution packages)

All installed from Devuan/Debian's own repositories under their existing
packaging. No redistribution by NuMATE.

| Component | Licence |
|---|---|
| MATE desktop (session manager, settings daemon, marco, polkit, power manager) | GPL-2.0-or-later, LGPL-2.1 |
| Nemo + extensions | GPL-2.0-or-later |
| Engrampa | GPL-2.0-or-later |
| Pluma | GPL-2.0-or-later |
| GParted | GPL-2.0-or-later |
| Waterfox (`waterfox`) | MPL 2.0 — from BrowserWorks' own signed apt repository |
| Lato font (`fonts-lato`) | SIL Open Font License 1.1 |
| DejaVu fonts (`fonts-dejavu-core`) | Bitstream Vera Licence (permissive) |

Applications come from Devuan's own repositories, except Waterfox, which comes
from BrowserWorks' signed apt repository as an ordinary `.deb`.

That distinction matters for the trademark question that applied to the old
tarball approach: the package is **built and published by BrowserWorks
themselves**, and NuMATE neither repackages nor redistributes it — it registers
their repository and lets apt install it. Shipping a prebuilt NuMATE image with
Waterfox preinstalled would still be redistribution and is worth confirming with
them first.

### ⚠ Flag: that repository currently publishes a beta

Verified 2026-08-02: the only package in
`isv:/BrowserWorks/Debian_13` is `waterfox_6.7.0~beta.3-0`. This is a beta
channel, not a stable one.

Not a licensing problem — MPL 2.0 applies either way — but it means a released
NuMATE installs a beta browser as the system default. Worth a decision before
tagging a release. Re-check the repository contents at release time, since this
will change without notice.

## NuMate-Settings

Lives in [its own repository](https://github.com/TTR-IND/NuMate-Settings) and is
built at install time by stage 6. It is **not** vendored here.

### ⚠ Flag: it is GPL-2.0-**only**, and NuMATE is AGPLv3

Per its own licence notes, every file in NuMate-Settings is GPL-2.0-or-later
*except* the vendored wallpaper backend lifted from mate-control-center
(`mate-wp-info`, `mate-wp-item`, `mate-wp-xml`), which is version-2-**only** with
no upgrade clause. The most restrictive term governs the combined binary, so
`numate-settings` as a whole is **GPL-2.0-only**.

GPL-2.0-only is **incompatible with AGPLv3** for the purpose of forming a single
combined work. That is not a problem here, and the reason is worth stating
precisely because it constrains future changes:

- `numate-settings` is a **separate program in a separate process**. The shell
  launches it by name via `launch_cmd_cb` — fork and exec, no linking, no
  shared address space, no `dlopen`.
- Two programs that merely invoke each other are *aggregation*, not a combined
  work. Each keeps its own licence. This is the same relationship any shell has
  with any program it starts.

**What this forbids, permanently:** never link NuMate-Settings code into the
NuMATE shell, never `dlopen` it, never copy a panel's source into
`gonzo-shell.c`, and never move the shell and the settings app into one binary.
Any of those creates a combined work of GPL-2.0-only and AGPLv3 code, which
cannot be lawfully distributed. Keeping them in separate repositories makes that
boundary hard to cross by accident — which is the strongest argument for the
split.

If that ever becomes limiting, the fix is to replace the three `mate-wp-*` files
with an original implementation. They are the only v2-only files in the tree;
without them NuMate-Settings would be GPL-2.0-or-later and could move to GPL-3.0.

### ✅ Previously-flagged items now closed

The two high-severity items from the earlier audit have been resolved upstream in
that repository:

- **Per-file "or later" verification** — done, and it found exactly the case that
  mattered: the `mate-wp-*` files are v2-only. That is precisely the check that
  needed doing, and it changed the answer.
- **In-source attribution** — each file carries an `SPDX-License-Identifier`,
  upstream authorship notices are untouched, and changes are recorded in each
  file's header under `MODIFICATIONS` so the delta survives loss of version
  control history.

## Summary of open items

| # | Item | Severity | Action |
|---|---|---|---|
| 1 | ~~Per-file "or later" check on MATE/UKUI code~~ | ✅ **Closed** | Done — found `mate-wp-*` is v2-only |
| 2 | ~~In-source attribution headers~~ | ✅ **Closed** | SPDX + `MODIFICATIONS` headers in place |
| 3 | Provenance of `numate-wall.png` | **Medium** | Confirm authorship; it is redistributed |
| 4 | Qogir cursors have no licence file, and are vendored | **Medium** | `theme/cursors/README.md` records provenance + attribution |
| 5 | Waterfox trademark if shipping an ISO | **Low** (not currently) | Installed from BrowserWorks' own repo; confirm before distributing images |
| 6 | Never link NuMate-Settings into the shell | **Standing rule** | GPL-2.0-only vs AGPLv3 — separate processes only |

Items 1 and 2 are closed. Items 3, 4 and 5 remain: 3 and 4 concern assets this
repository actually distributes and are worth clearing before it goes public;
5 only becomes live if you ship prebuilt images. Item 6 is not a task; it is a
constraint to respect from here on.

Item 5 reopened when Waterfox came back. It is much smaller than it was under
the old tarball approach — NuMATE registers BrowserWorks' repository and lets
apt install their own package, so nothing is repackaged or redistributed — but
it stops being merely theoretical the moment a NuMATE ISO ships with Waterfox
preinstalled.

Item 4 is the one that changed when the theme went back to being vendored: while
the installer fetched the cursors, NuMATE was not redistributing them and the
missing licence file was upstream's problem. Vendoring makes it yours. A short
provenance README in `theme/cursors/` closes it.

---

*This is an engineering audit, not legal advice. For anything commercial or
anything involving shipping prebuilt images, get a lawyer to look at it.*
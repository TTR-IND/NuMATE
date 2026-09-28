# Qogir cursors — provenance and attribution

These cursor themes are **not original NuMATE work**. They are vendored here so
that installs work offline and against a fixed revision. This file records where
they came from, because the upstream directories ship no licence file of their
own and NuMATE redistributes them.

## Source

| | |
|---|---|
| Directories | `Qogir-cursors`, `Qogir-white-cursors` |
| Upstream project | [vinceliuice/Qogir-icon-theme](https://github.com/vinceliuice/Qogir-icon-theme) |
| Upstream path | `src/cursors/dist` → `Qogir-cursors`, `src/cursors/dist-Dark` → `Qogir-white-cursors` |
| Upstream licence | GPL-3.0 (repository licence) |
| Author | Vince Liuice |

## Derivation

Upstream's own README for `src/cursors` states the cursor set is
**based on [capitaine-cursors](https://github.com/keeferrourke/capitaine-cursors)**
by Keefer Rourke, which is licensed **LGPL-3.0**.

Both licences in that chain are compatible with NuMATE's AGPLv3 for the purpose
of distributing these files alongside it — they are separate artwork assets
aggregated in this repository, not code linked into the NuMATE shell.

## Known gap

Neither cursor directory contains a `COPYING` or `LICENSE` file upstream, so the
licence above is **inferred** from the parent repository and README rather than
declared in the files themselves. That inference is well-supported but it is an
inference, and it is recorded as an open item in
[`../../THIRD-PARTY-LICENCES.md`](../../THIRD-PARTY-LICENCES.md).

If you are shipping prebuilt NuMATE images rather than source, resolve this
first — the clean fix is an upstream issue asking for a `LICENSE` file in
`src/cursors/`, after which the real file replaces this note.

## If you update these cursors

Re-copy from upstream's `src/cursors/dist*` directories and update this file if
the upstream licence position has changed. Do not delete this file when
refreshing the assets.

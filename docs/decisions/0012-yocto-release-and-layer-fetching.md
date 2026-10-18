# ADR-0012: Yocto Wrynose (6.0, LTS), with the layers cloned by hand

**Status:** Accepted
**Date:** 2026-10-18
**Phase:** 3

## Context

ADR-0011 chose Yocto for the gateway image. Two things are still open: which Yocto
release to use, and how to get the pieces (the build tool and the layers) so that the
same versions can be fetched again.

Facts checked on 2026-10-18 (Yocto wiki, the Yocto documentation, and the git repos):

- Supported releases: Wrynose 6.0 (April 2026, **LTS**, supported until April 2030).
  Scarthgap 5.0 is an older LTS, supported until April 2028. Blacksail 6.1 (October 2026)
  is not an LTS.
- `meta-raspberrypi` has a `wrynose` branch (and `scarthgap`). It contains the machines
  `raspberrypi0-2w` and `raspberrypi0-2w-64`. Its layer depends only on the core layer.
- The classic route is to clone **Poky**, one repo that bundles everything. **Poky has no
  Wrynose branch** (its newest branch is `walnascar`). The Wrynose documentation uses a new
  tool, `bitbake-setup`, to fetch the layers instead.
- The core layer on `wrynose` sets `BB_MIN_VERSION = "2.18.0"` (`meta/conf/sanity.conf`),
  and the `bitbake` branch `2.18` is version 2.18.0.
- The build tool looks for BitBake next to the core layer (`../bitbake`), so the repos
  can sit side by side in one folder.

## Options considered

### Release

#### Option A: Wrynose 6.0 (LTS)
- Pros: the newest LTS, supported long past this project; the documentation I read is for
  this release.
- Cons: newer than most online tutorials and than most of what I (and Claude) know by
  heart, so I rely on the documentation, and some tutorials will not apply (for example
  everything that says "clone poky").

#### Option B: Scarthgap 5.0 (LTS)
- Pros: many more tutorials and answered questions online; Poky works the classic way.
- Cons: older, supported for about 1.5 years more only.

### How to fetch the layers

#### Option A: clone each repo by hand into a folder outside this repo
- How it works: four `git clone` commands, one branch each; the exact commit of each is
  written down in the docs.
- Pros: I see every piece and what a layer is; the simplest to understand.
- Cons: I have to keep the list of versions up to date by hand.

#### Option B: git submodules in this repo
- Cons: submodules are a common source of confusion for a beginner; no benefit now.

#### Option C: a fetching tool (`bitbake-setup`, the new official one, or `kas`)
- Pros: the most reproducible: one file in the repo describes the layers and their
  versions and one command fetches them.
- Cons: one more tool before I have seen the pieces it hides.

## Decision

**Wrynose, with the four repos cloned by hand (`bitbake` 2.18, `openembedded-core`,
`meta-yocto` and `meta-raspberrypi`, all on `wrynose`) into `~/yocto/`, outside this
repo.** The commit of each is recorded in `docs/architecture.md` once cloned. Wrynose
because it is the current LTS and its documentation is what I can verify; by hand because
the point of this phase is to learn what the pieces are before a tool hides them.

## Consequences

- Makes easier: seeing exactly which layers exist and what `bblayers.conf` and `local.conf`
  point at; support until 2030.
- Makes harder: tutorials written for Poky need translating (`poky/` becomes the three
  separate repos); I must record the commits myself.
- Revisit: move to `bitbake-setup` or `kas` (a new ADR) once I understand the pieces, to
  make the fetch a single reproducible file.
- Not verified: that these four branches build together into an image for
  `raspberrypi0-2w` (only that the branches exist and the version requirement matches);
  whether `meta-yocto` is needed or the core layer alone would do.
- Only my own layer and the configuration files will go into this repo; the cloned repos
  and the build directory stay outside it.

## Interview angle

"I chose the current LTS, Yocto Wrynose. Its documentation had replaced the single Poky
repo with a setup tool, so I cloned BitBake and the layers separately, pinned each to a
commit, and kept my own layer apart. That showed me what each layer is, and I know how to
move to a tool like `bitbake-setup` or `kas` for reproducibility."

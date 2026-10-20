# ADR-0014: Skip the `buildpaths` quality check for the `perl` recipe

**Status:** Accepted (a workaround; the cause is not found)
**Date:** 2026-10-18
**Phase:** 3

## Context

The first long `bitbake core-image-base` run (4 h 12 min, see ADR-0011) ended with a
quality-check failure in the `perl` recipe. Yocto runs a check (`buildpaths`) on every
package: if a file contains the path of the build folder (`TMPDIR`), the build fails,
because the same recipe built in another folder would give a different file, so the build
would not be reproducible.

The file that failed was `libperl.so`. From the log I took that the path comes from a
`config.h` that the perl build regenerates (the variable `ARCHLIB_EXP` holds a path from
the build folder). The perl recipe already has a `sed` command that is meant to clean such
paths, but that command runs before `do_compile`, and during `do_compile` the file is
generated again, so the cleaned version is overwritten. I did not find out why the file is
regenerated.

Facts from the finished build (2026-10-19):

- With the skip in place the build passed: 5695 tasks, all succeeded.
- The final image contains **no package whose name starts with `perl`** (checked in the
  image's package manifest, 2008 packages). So the path never reaches the image. I do not
  know why `perl` is built at all: something has it as a build-time dependency, or as a
  dependency of a package that is not in the image.

## Options considered

### Option A: switch the check off for this one recipe
- How it works: `INSANE_SKIP:perl += "buildpaths"` in `local.conf`. Yocto then skips only
  this check, only for the recipe `perl`.
- Pros: one line; every other recipe is still checked; the build continues.
- Cons: it hides a real finding instead of explaining it. If `perl` ever lands in the image,
  the image will contain a path of my build folder (and my user name) and will differ from a
  build done elsewhere.

### Option B: find what pulls `perl` in and remove that dependency
- How it works: ask bitbake why perl is built (`bitbake -g core-image-base`, then read the
  dependency graph), and remove or replace the recipe or the feature that needs it.
- Pros: if the image does not need perl, the problem disappears and the build gets shorter.
- Cons: needs the dependency graph read first; it may turn out that a needed recipe depends
  on perl only at build time, in which case nothing can be removed.

### Option C: find why the file is regenerated and fix the recipe
- How it works: read the perl recipe and its `do_compile` log, find what regenerates
  `config.h`, and patch it in a `.bbappend` in a layer of my own (or report it upstream).
- Pros: the real fix, and a good thing to be able to say in an interview.
- Cons: the most time; perl's build system is large and I did not choose to learn it now.

## Decision

**Option A for now.** The build had run for four hours, the failure is in a recipe whose
output is not in the image, and the skip is limited to one check on one recipe. I record it
as a workaround, not a fix. Options B and C stay open as follow-ups.

## Consequences

- Makes easier: the image builds; every other recipe still has the `buildpaths` check.
- Makes harder: the line in `local.conf` can be forgotten. It currently has no comment;
  one that points to this ADR still has to be added there (and in the copy of `local.conf`
  that goes into this repo).
- Not verified: why `perl` is built; why `config.h` is regenerated; that the
  image is identical when built in another folder.
- Follow-up: (1) read the dependency graph and record what needs `perl` (option B);
  (2) if `perl` is ever added to the image on purpose, redo the decision and look at
  option C before shipping, because the path would then be inside the image.

## Interview angle

"A Yocto quality check failed on `perl` because the build folder's path was inside
`libperl.so`. I checked that the package was not part of the image, switched that one
check off for that one recipe, and wrote down why, what I had not found out, and the two
ways to fix it properly. I would rather document a known workaround than hide it."

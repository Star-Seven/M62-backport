# AGENTS.md

## What this repository is

Android kernel source for Exynos 9820/9825 (Samsung M62 and siblings), based on
Linux 4.14 (OpenELA). This is a *backport* tree: upstream SUSFS and
KernelSU-Next are adapted down to a 4.14 kernel, and the result is packaged as
the "Star7KRNL" flashable ZIP.

Relevant branches:

- `susfs-v230` — the active branch (SUSFS v2.3.x + KernelSU-Next legacy glue).
- `bpf111` — default branch; unrelated BPF backport work.

`KernelSU/` is a git submodule pointing at
`star-star-dev/KernelSU-Next`, branch `legacy-susfs-v230`. Changes to KernelSU
or SUSFS glue usually land in that repo first, then the submodule pointer here
is updated.

## Kernel and package versioning

The version is derived automatically at build time; there is no number to
maintain by hand.

Scheme: `Star7KRNL-Quant-v<MAJOR>.<MINOR>.<PATCH>`

- `MAJOR.MINOR` mirrors the bundled SUSFS release, read from
  `include/linux/susfs.h` (`SUSFS_VERSION`). SUSFS v2.3.0 → `v2.3.x`.
- `PATCH` is the number of commits since `include/linux/susfs.h` last changed
  (`git rev-list --count <last-susfs.h-commit>..HEAD`). Every adaptation,
  backport, or repack raises it by one on its own; pulling a new SUSFS release
  moves the baseline forward and PATCH restarts at 0 without any manual reset.

So the version always matches reality: change SUSFS and the minor tracks it,
change anything else and the patch ticks up.

`kernel-version.sh` is the only supported way to read it:

```
./kernel-version.sh get        # Star7KRNL-Quant-v2.3.4
./kernel-version.sh num        # v2.3.4
./kernel-version.sh localver   # -Star7KRNL-Quant-v2.3.4 (make LOCALVERSION)
./kernel-version.sh check      # fail if MAJOR.MINOR != bundled SUSFS version
```

`CONFIG_LOCALVERSION` in the defconfig holds only the brand prefix
(`"-Star7KRNL-Quant"`). `build.sh` appends the derived `vX.Y.Z` through a make
`LOCALVERSION=` override, so `uname -r` becomes
`<kernelver>-Star7KRNL-Quant-vX.Y.Z` and the defconfig is never edited for a
release. Because PATCH is a commit count, CI checks out with `fetch-depth: 0`.

### Rule: no manual bump; just commit

Backporting SUSFS or KernelSU-Next changes needs no version edit:

1. Update the `KernelSU` submodule to the new `legacy-susfs-v230` commit.
2. Re-apply / fix the SUSFS and KernelSU glue for 4.14 and commit.
3. Run `./kernel-version.sh check` (CI does this too) and confirm the derived
   version reads correctly; `get` should show the new PATCH level.
4. Build and verify the reported version in `uname -r`.

If a SUSFS release changes, `MAJOR.MINOR` follows `SUSFS_VERSION` automatically.
Use the `version` workflow-dispatch input only to force an explicit tag.

CI enforces step 3: both jobs in `.github/workflows/main.yml` run
`./kernel-version.sh check` in the "Resolve package version" step, so a SUSFS
bump without a matching minor fails the build early instead of shipping a stale
version.

The KernelSU-Next version the manager displays comes from the *other* repo. It
reports `KSU_VERSION` (a versionCode the manager compares against its own) and
`KSU_VERSION_TAG` (from `git describe --tags --abbrev=0`, so only tags
reachable from `legacy-susfs-v230` count). When updating the submodule, confirm
that branch carries its own release tag, e.g. `v3.4.2-legacy-susfs-v230`;
without one, `describe` silently falls back to an older tag from a different
release line and the manager shows a mismatched kernel version. The CI clone
fetches tags, so a pushed tag resolves on the next build.

`build.sh` and the workflow both call `kernel-version.sh`; do not reintroduce
inline `grep`/`sed` parsing of `CONFIG_LOCALVERSION`.

## Building

```
./build.sh -m all -k y -r n -v "$(./kernel-version.sh get)"
```

Options:

- `-m, --model` — model code (e.g. `d2xks`) or `all`
- `-k, --ksu` — include KernelSU (`y`/`n`)
- `-r, --recovery` — build for Android recovery
- `-v, --version` — package version tag; defaults to the version derived from
  the tree by `kernel-version.sh`

The first run downloads a prebuilt clang toolchain into `toolchain/`.

## Conventions

- The defconfig files use CRLF line endings. When editing them
  programmatically, preserve CRLF; a whole-file line-ending flip produces an
  unreviewable diff.
- `.github/` is matched by the `.*` rule in `.gitignore`. The workflow file is
  tracked already, so stage it with `git add -f` when modifying it.
- Do not push to the default branch (`bpf111`) or to `main`; work on a feature
  branch.
- Never put AI/agent attribution in commits: no `Co-authored-by: openhands`,
  no `openhands@all-hands.dev` author/committer/tagger, no "generated with"
  trailers. Use the repo's normal identity
  (`star-star-dev <xingyuan53@qq.com>`). Automated attribution on a personal
  account risks the account being flagged, so keep history indistinguishable
  from the maintainer's own commits.

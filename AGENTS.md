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

The single source of truth for the released version is:

```
arch/arm64/configs/exynos9820_defconfig:50
CONFIG_LOCALVERSION="-Star7KRNL-Quant-v2.3.1"
```

Scheme: `Star7KRNL-Quant-v<MAJOR>.<MINOR>.<PATCH>`

- `MAJOR.MINOR` mirrors the bundled SUSFS release (`include/linux/susfs.h`,
  `SUSFS_VERSION`). SUSFS v2.3.0 → kernel version `v2.3.x`.
- `PATCH` increments for each fork-side backport, adaptation, or repack that
  does not change the SUSFS baseline.

`kernel-version.sh` is the only supported way to read or change it:

```
./kernel-version.sh get        # Star7KRNL-Quant-v2.3.1
./kernel-version.sh num        # v2.3.1
./kernel-version.sh check      # fail if MAJOR.MINOR != bundled SUSFS version
./kernel-version.sh bump       # v2.3.1 -> v2.3.2 (edits the defconfig)
./kernel-version.sh set v2.4.0 # set explicitly
```

### Rule: bump the version whenever you adapt SUSFS or KernelSU-Next

Any time upstream SUSFS or KernelSU-Next changes are backported into this tree,
bump the version *in the same change* as the adaptation:

1. Update the `KernelSU` submodule to the new `legacy-susfs-v230` commit.
2. Re-apply / fix the SUSFS and KernelSU glue for 4.14.
3. Bump the version:
   - SUSFS baseline changed (e.g. v2.3.0 → v2.4.0):
     `./kernel-version.sh set v2.4.0`
   - Only the fork adaptation changed: `./kernel-version.sh bump`
4. Run `./kernel-version.sh check` and make sure it passes.
5. Build and verify the reported version in `uname -r`.

CI enforces step 4: both jobs in `.github/workflows/main.yml` run
`./kernel-version.sh check` in the "Resolve package version" step, so a SUSFS
bump without a matching version bump fails the build early instead of shipping
a stale version.

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
- `-v, --version` — package version tag; defaults to `CONFIG_LOCALVERSION`

The first run downloads a prebuilt clang toolchain into `toolchain/`.

## Conventions

- The defconfig files use CRLF line endings. When editing them
  programmatically, preserve CRLF; a whole-file line-ending flip produces an
  unreviewable diff.
- `.github/` is matched by the `.*` rule in `.gitignore`. The workflow file is
  tracked already, so stage it with `git add -f` when modifying it.
- Do not push to the default branch (`bpf111`) or to `main`; work on a feature
  branch.

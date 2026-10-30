#!/bin/sh
# Star7KRNL M62-backport kernel/package version helper.
#
# Scheme:  Star7KRNL-Quant-v<MAJOR>.<MINOR>.<PATCH>
#
# Both axes are derived from the tree, so no manual bump is needed:
#   MAJOR.MINOR  the bundled SUSFS release, read from
#                include/linux/susfs.h (SUSFS_VERSION)
#   PATCH        number of commits since the SUSFS_VERSION line last changed
#
# Because PATCH is counted from the last commit that changed the SUSFS_VERSION
# line, pulling a new SUSFS release moves the baseline forward and PATCH
# restarts at 0 on its own. Editing susfs.h for an adaptation (new structs,
# declarations, hooks) does not move it, so every backport, KernelSU-Next
# update, or repack ticks PATCH up by one. Nothing to remember.
#
# The version is injected into the kernel at build time as a make LOCALVERSION
# override, so CONFIG_LOCALVERSION in the defconfig is only the brand prefix and
# is never rewritten. `uname -r` ends up as <prefix>-vX.Y.Z.
#
# Usage:
#   kernel-version.sh get        # print Star7KRNL-Quant-v2.3.4
#   kernel-version.sh num        # print v2.3.4
#   kernel-version.sh localver   # print -Star7KRNL-Quant-v2.3.4 (make override)
#   kernel-version.sh check      # sanity-check the derived version

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SUSFS_HDR="$ROOT/include/linux/susfs.h"
PREFIX="Star7KRNL-Quant-"

die() {
    echo "kernel-version: $*" >&2
    exit 1
}

read_susfs_version() {
    [ -f "$SUSFS_HDR" ] || die "missing $SUSFS_HDR"
    sed -n 's/^#define SUSFS_VERSION "v\{0,1\}\([0-9][0-9.]*\)"\r\{0,1\}$/\1/p' "$SUSFS_HDR" | head -n1
}

major_minor() {
    printf '%s\n' "${1#v}" | cut -d. -f1,2
}

# Number of commits since the SUSFS release last changed. Falls back to 0 when
# the tree is not a git checkout (e.g. a source tarball), which keeps builds
# working.
#
# The baseline is the last commit that changed the SUSFS_VERSION line itself,
# not the last commit that touched susfs.h. A backport/adaptation necessarily
# edits susfs.h (new structs, declarations, hook macros) while keeping the
# release unchanged, so anchoring on the file would move the baseline forward
# on every port and reset PATCH to 0, making the version regress. Anchoring on
# the version line means only a genuine SUSFS release bump moves it; every
# adaptation, KernelSU-Next update, or repack ticks PATCH up instead.
patch_level() {
    command -v git >/dev/null 2>&1 || { echo 0; return; }
    git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo 0; return; }
    # git log takes a pathspec relative to the repository root, not an
    # absolute path, so the header is referenced by its in-tree path.
    base=$(git -C "$ROOT" log -n1 --format=%H -G'^#define[[:space:]]SUSFS_VERSION' -- include/linux/susfs.h 2>/dev/null || true)
    [ -n "$base" ] || base=$(git -C "$ROOT" log -n1 --format=%H -- include/linux/susfs.h 2>/dev/null || true)
    [ -n "$base" ] || { echo 0; return; }
    git -C "$ROOT" rev-list --count "$base"..HEAD 2>/dev/null || echo 0
}

compute_version() {
    susfs=$(read_susfs_version)
    [ -n "$susfs" ] || die "could not read SUSFS_VERSION from $SUSFS_HDR"
    printf 'v%s.%s\n' "$(major_minor "$susfs")" "$(patch_level)"
}

cmd=${1:-get}

case "$cmd" in
    get)
        printf '%s%s\n' "$PREFIX" "$(compute_version)"
        ;;
    num)
        compute_version
        ;;
    localver)
        printf '%s%s%s\n' "-" "$PREFIX" "$(compute_version)"
        ;;
    check)
        v=$(compute_version)
        susfs=$(read_susfs_version)
        [ -n "$susfs" ] || die "could not read SUSFS_VERSION from $SUSFS_HDR"
        case "$v" in
            v[0-9]*.[0-9]*.[0-9]*) ;;
            *) die "derived version '$v' is malformed" ;;
        esac
        if [ "$(major_minor "$v")" != "$(major_minor "$susfs")" ]; then
            die "version $v does not match bundled susfs $susfs (expected MAJOR.MINOR $(major_minor "$susfs"))"
        fi
        echo "version $v matches bundled susfs $susfs"
        ;;
    *)
        die "unknown command '$cmd' (use get|num|localver|check)"
        ;;
esac

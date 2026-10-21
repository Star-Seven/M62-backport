#!/bin/sh
# Star7KRNL M62-backport kernel/package version helper.
#
# Scheme:  Star7KRNL-Quant-v<MAJOR>.<MINOR>.<PATCH>
#   MAJOR.MINOR  tracks the bundled susfs release (include/linux/susfs.h SUSFS_VERSION)
#   PATCH        increments on each fork-side backport/adaptation/repack
#
# The single source of truth is CONFIG_LOCALVERSION in
# arch/arm64/configs/exynos9820_defconfig. It is read by the kernel build
# (so it shows up in `uname -r`), by build.sh and by the release workflow.
#
# Usage:
#   kernel-version.sh get            # print Star7KRNL-Quant-v2.3.1
#   kernel-version.sh num            # print v2.3.1
#   kernel-version.sh check          # fail if MAJOR.MINOR != bundled susfs version
#   kernel-version.sh bump           # v2.3.1 -> v2.3.2
#   kernel-version.sh set v2.4.0     # set explicitly

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEFCONFIG="$ROOT/arch/arm64/configs/exynos9820_defconfig"
SUSFS_HDR="$ROOT/include/linux/susfs.h"
PREFIX="Star7KRNL-Quant-"

die() {
    echo "kernel-version: $*" >&2
    exit 1
}

read_localversion() {
    [ -f "$DEFCONFIG" ] || die "missing $DEFCONFIG"
    lv=$(sed -n 's/^CONFIG_LOCALVERSION="\(.*\)"\r\{0,1\}$/\1/p' "$DEFCONFIG" | head -n1)
    case "$lv" in
        "-$PREFIX"v*) printf '%s\n' "${lv#-}" ;;
        *) die "CONFIG_LOCALVERSION '$lv' does not start with '-$PREFIX'" ;;
    esac
}

read_version() {
    printf '%s\n' "${1#"$PREFIX"}"
}

read_susfs_version() {
    [ -f "$SUSFS_HDR" ] || die "missing $SUSFS_HDR"
    sed -n 's/^#define SUSFS_VERSION "v\{0,1\}\([0-9][0-9.]*\)"\r\{0,1\}$/\1/p' "$SUSFS_HDR" | head -n1
}

write_version() {
    v=$1
    tmp=$(mktemp)
    # Preserve the file's CRLF line endings (the defconfig is CRLF throughout).
    sed "s|^CONFIG_LOCALVERSION=.*|CONFIG_LOCALVERSION=\"-$PREFIX$v\"\r|" "$DEFCONFIG" > "$tmp"
    chmod 644 "$tmp"
    mv "$tmp" "$DEFCONFIG"
}

major_minor() {
    printf '%s\n' "${1#v}" | cut -d. -f1,2
}

cmd=${1:-get}

case "$cmd" in
    get)
        read_localversion
        ;;
    num)
        read_version "$(read_localversion)"
        ;;
    check)
        ver=$(read_version "$(read_localversion)")
        susfs=$(read_susfs_version)
        [ -n "$susfs" ] || die "could not read SUSFS_VERSION from $SUSFS_HDR"
        if [ "$(major_minor "$ver")" != "$(major_minor "$susfs")" ]; then
            die "version $ver does not match bundled susfs $susfs (expected MAJOR.MINOR $(major_minor "$susfs"))"
        fi
        echo "version $ver matches bundled susfs $susfs"
        ;;
    bump)
        ver=$(read_version "$(read_localversion)")
        body=${ver#v}
        major=$(printf '%s' "$body" | cut -d. -f1)
        minor=$(printf '%s' "$body" | cut -d. -f2)
        patch=$(printf '%s' "$body" | cut -d. -f3)
        [ -n "$major" ] && [ -n "$minor" ] || die "malformed version '$ver'"
        case "$patch" in ''|*[!0-9]*) patch=0 ;; esac
        new="v$major.$minor.$((patch + 1))"
        write_version "$new"
        echo "$new"
        ;;
    set)
        [ $# -eq 2 ] || die "set needs a version, e.g. set v2.4.0"
        new=$2
        case "$new" in v*) ;; *) new="v$new" ;; esac
        printf '%s' "${new#v}" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
            || die "version must look like vX.Y.Z"
        write_version "$new"
        echo "$new"
        ;;
    *)
        die "unknown command '$cmd' (use get|num|check|bump|set)"
        ;;
esac

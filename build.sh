#!/bin/bash

abort()
{
    cd -
    echo "-----------------------------------------------"
    echo "Kernel compilation failed! Exiting..."
    echo "-----------------------------------------------"
    exit -1
}

unset_flags()
{
    cat << EOF
Usage: $(basename "$0") [options]
Options:
    -m, --model [value]     Specify the model code (e.g. d2xks) or 'all'
    -k, --ksu [Y/n]         Include KernelSU
    -r, --recovery [y/N]    Compile kernel for an Android Recovery
    -v, --version [value]   Package version tag (defaults to CONFIG_LOCALVERSION)
EOF
}

SUPPORTED_MODELS=("beyond0lte" "beyond1lte" "beyond2lte" "beyondx" "d1" "d1xks" "d2s" "d2x" "d2xks")

while [[ $# -gt 0 ]]; do
    case "$1" in
        --model|-m) SELECTED_MODEL="$2"; shift 2 ;;
        --ksu|-k) KSU_OPTION="$2"; shift 2 ;;
        --recovery|-r) RECOVERY_OPTION="$2"; shift 2 ;;
        --version|-v) VERSION_OVERRIDE="$2"; shift 2 ;;
        *) unset_flags; exit 1 ;;
    esac
done

# For a unified package, we build all models unless one was requested
if [ -n "$SELECTED_MODEL" ] && [ "$SELECTED_MODEL" != "all" ]; then
    MODELS_TO_BUILD=("$SELECTED_MODEL")
else
    MODELS_TO_BUILD=("${SUPPORTED_MODELS[@]}")
fi

echo "Preparing the build environment..."

pushd $(dirname "$0") > /dev/null
CORES=`cat /proc/cpuinfo | grep -c processor`

# Toolchain setup (RETAINED FROM ORIGINAL)
CLANG_DIR=$PWD/toolchain/clang-r547379
PATH=$CLANG_DIR/bin:$PATH
if [ ! -f "$CLANG_DIR/bin/clang-20" ]; then
    echo "-----------------------------------------------"
    echo "Toolchain not found! Downloading..."
    echo "-----------------------------------------------"
    rm -rf $CLANG_DIR && mkdir -p $CLANG_DIR
    pushd $CLANG_DIR > /dev/null
    curl -LJOk https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/main/clang-r547379.tar.gz
    tar xf main-clang-r547379.tar.gz && rm main-clang-r547379.tar.gz
    echo "Cleaning up..."
    popd > /dev/null
fi

# Cache object compilation when ccache is present. Every run starts from a clean
# out/ directory, so without this each run pays for a full compile per SoC; the
# per-model configs differ by only a few dozen lines, so a warm cache serves most
# of those objects. Absent ccache, the build simply runs clang directly.
CCACHE_CC=""
if command -v ccache > /dev/null 2>&1; then
    CCACHE_CC="ccache clang"
    echo "ccache enabled: $(ccache --version | head -n1)"
fi

if [ -z $KSU_OPTION ]; then read -p "Include KernelSU (y/N): " KSU_OPTION; fi
RECOVERY_CONFIG=""
if [[ "$RECOVERY_OPTION" == "y" ]]; then RECOVERY_CONFIG="recovery.config"; KSU_OPTION="n"; fi
KSU_CONFIG=""
if [[ "$KSU_OPTION" == "y" ]]; then KSU_CONFIG="ksu.config"; fi

# Package version: explicit -v wins, else derive it from the tree
if [ -n "$VERSION_OVERRIDE" ]; then
    PKG_VERSION="$VERSION_OVERRIDE"
else
    PKG_VERSION=$(./kernel-version.sh get)
fi
if [ -z "$PKG_VERSION" ]; then
    echo "Error: could not determine package version (empty -v and no derivation)."
    exit 1
fi
# Drop a leading brand tag so the final name never doubles it.
PKG_TAG="${PKG_VERSION#Star7KRNL-}"
echo "Package version: $PKG_VERSION (tag: $PKG_TAG)"

# Inject the version into uname -r at build time. CONFIG_LOCALVERSION holds only
# the brand prefix ("-Star7KRNL-Quant"), so we append the version part alone;
# appending the full branded string would duplicate the prefix. An explicit -v
# is normalized the same way (with or without the brand/leading v).
VER_PART="$PKG_VERSION"
VER_PART="${VER_PART#Star7KRNL-Quant-}"
VER_PART="${VER_PART#Star7KRNL-}"
case "$VER_PART" in v*) ;; *) VER_PART="v$VER_PART" ;; esac
BUILD_LOCALVERSION="-$VER_PART"

# Per-SoC cache of the config fragment already compiled, so models that share a
# fragment do not trigger a rebuild. Rebuilding for every model would be wasted
# work, but using one fixed fragment for a whole SoC is wrong: the fragments
# differ in symbols that must match the device (CONFIG_TZDEV_DEPLOY_OLD_TZAR
# picks the TrustZone tzar the bootloader expects, and d2s/d1 need the old one).
# Keying the cache on the fragment content keeps a single build per distinct
# configuration.
declare -A COMPILED_CONF=()

# Directory for unified package output
UNIFIED_OUT="build/out/unified"
rm -rf "$UNIFIED_OUT"
mkdir -p "$UNIFIED_OUT/META-INF/com/google/android"
mkdir -p "build/out/all_images"

for MODEL in "${MODELS_TO_BUILD[@]}"; do
    case $MODEL in
        beyond0lte) BOARD=SRPRI28A016KU; SOC=exynos9820; MODEL_NAME="G970F" ;;
        beyond1lte) BOARD=SRPRI28B016KU; SOC=exynos9820; MODEL_NAME="G973F" ;;
        beyond2lte) BOARD=SRPRI17C016KU; SOC=exynos9820; MODEL_NAME="G975F" ;;
        beyondx)    BOARD=SRPSC04B014KU; SOC=exynos9820; MODEL_NAME="G977B" ;;
        d1)         BOARD=SRPSD26B009KU; SOC=exynos9825; MODEL_NAME="N970F" ;;
        d1xks)      BOARD=SRPSD23A002KU; SOC=exynos9825; MODEL_NAME="N971N" ;;
        d2s)        BOARD=SRPSC14B009KU; SOC=exynos9825; MODEL_NAME="N975F" ;;
        d2x)        BOARD=SRPSC14C009KU; SOC=exynos9825; MODEL_NAME="N976B" ;;
        d2xks)      BOARD=SRPSD23C002KU; SOC=exynos9825; MODEL_NAME="N976N" ;;
    esac

    # Phase 1: Compile Kernel if needed
    REF_CONF="$MODEL.config"
    CONF_KEY="$SOC:$(md5sum "arch/arm64/configs/$REF_CONF" | cut -d' ' -f1)"
    SHOULD_COMPILE=false
    if [ -z "${COMPILED_CONF[$CONF_KEY]}" ]; then SHOULD_COMPILE=true; fi

    MAKE_ARGS=(LLVM=1 LLVM_IAS=1 ARCH=arm64 O=out/$SOC LOCALVERSION=$BUILD_LOCALVERSION)
    # CC must remain a single make argument, so it is appended to the array
    # rather than embedded in a string that would word-split into "CC=ccache"
    # and a stray "clang" target.
    if [ -n "$CCACHE_CC" ]; then MAKE_ARGS+=("CC=$CCACHE_CC"); fi
    if [ "$SHOULD_COMPILE" = true ]; then
        echo "Compiling for $SOC ($MODEL, config $REF_CONF)..."
        mkdir -p out/$SOC
        make "${MAKE_ARGS[@]}" -j$CORES exynos9820_defconfig $REF_CONF $KSU_CONFIG $RECOVERY_CONFIG || abort
        make "${MAKE_ARGS[@]}" -j$CORES || abort
        COMPILED_CONF[$CONF_KEY]=true
    fi

    # Phase 2: Generate Images
    echo "Generating images for $MODEL..."
    TEMP_OUT="build/out/temp_$MODEL"
    mkdir -p "$TEMP_OUT"
    ./toolchain/mkdtimg cfg_create $TEMP_OUT/dtb-$SOC.img build/dtconfigs/$SOC.cfg -d out/$SOC/arch/arm64/boot/dts/exynos || abort
    ./toolchain/mkdtimg cfg_create $TEMP_OUT/dtbo-$MODEL_NAME.img build/dtconfigs/$MODEL.cfg -d out/$SOC/arch/arm64/boot/dts/samsung || abort

    pushd build/ramdisk > /dev/null
    find . ! -name . | LC_ALL=C sort | cpio -o -H newc -R root:root | gzip > "../../$TEMP_OUT/ramdisk.cpio.gz"
    popd > /dev/null

    ./toolchain/mkbootimg --base 0x10000000 --board $BOARD --cmdline 'loop.max_part=7' --hashtype sha1 \
        --header_version 1 --kernel out/$SOC/arch/arm64/boot/Image --kernel_offset 0x00008000 \
        --os_patch_level 2026-05 --os_version 16.0.0 --pagesize 2048 \
        --ramdisk $TEMP_OUT/ramdisk.cpio.gz --ramdisk_offset 0xF0000000 --second_offset 0xF0000000 \
        --tags_offset 0x00000100 -o $TEMP_OUT/$MODEL_NAME.img || abort

    # Store images for unified tar
    cp $TEMP_OUT/$MODEL_NAME.img build/out/all_images/
    cp $TEMP_OUT/dtb-$SOC.img build/out/all_images/
    cp $TEMP_OUT/dtbo-$MODEL_NAME.img build/out/all_images/
    rm -rf "$TEMP_OUT"
done

# Phase 3: Final Unified Packaging
echo "Creating unified images.tar.xz..."
pushd build/out/all_images > /dev/null
tar -cf ../images.tar *.img
popd > /dev/null
xz -f -9 build/out/images.tar

# Prepare ZIP structure
cp build/out/images.tar.xz "$UNIFIED_OUT/"
cp build/clone_header "$UNIFIED_OUT/"
cp build/unxz "$UNIFIED_OUT/"
# Use the update-binary from build/ as source if it exists, otherwise use a default
if [ -f "build/update-binary" ]; then
    sed "s/@KSU_PKG_VERSION@/${PKG_TAG}/g" build/update-binary > "$UNIFIED_OUT/META-INF/com/google/android/update-binary"
else
    # Fallback: if user didn't provide update-binary in build/, we must ensure one exists.
    # But according to instructions, build/ is the source.
    echo "Warning: build/update-binary not found, using reference one."
    sed "s/@KSU_PKG_VERSION@/${PKG_TAG}/g" META-INF/com/google/android/update-binary > "$UNIFIED_OUT/META-INF/com/google/android/update-binary"
fi

# IMPORTANT: build/updater-script is the source for Edify logic.
# However, for a "Unified" package, the installer is usually a shell script (update-binary).
# If the user wants to use updater-script, we copy it.
cp build/updater-script "$UNIFIED_OUT/META-INF/com/google/android/updater-script"

# Final ZIP
DATE=`date +"%d-%m-%Y_%H-%M-%S"`
# Drop a leading brand tag so the final name never doubles it.
PKG_TAG="${PKG_VERSION#Star7KRNL-}"
ZIP_NAME="Star7KRNL-${PKG_TAG}_Unified_KSU_${DATE}.zip"

pushd "$UNIFIED_OUT" > /dev/null
zip -r "../../$ZIP_NAME" . > /dev/null
popd > /dev/null

popd > /dev/null
echo "Unified package created: build/out/$ZIP_NAME"

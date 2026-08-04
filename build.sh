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
EOF
}

SUPPORTED_MODELS=("beyond0lte" "beyond1lte" "beyond2lte" "beyondx" "d1" "d1xks" "d2s" "d2x" "d2xks")

while [[ $# -gt 0 ]]; do
    case "$1" in
        --model|-m) SELECTED_MODEL="$2"; shift 2 ;;
        --ksu|-k) KSU_OPTION="$2"; shift 2 ;;
        --recovery|-r) RECOVERY_OPTION="$2"; shift 2 ;;
        *) unset_flags; exit 1 ;;
    esac
done

# For a unified package, we build all models
MODELS_TO_BUILD=("${SUPPORTED_MODELS[@]}")

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

if [ -z $KSU_OPTION ]; then read -p "Include KernelSU (y/N): " KSU_OPTION; fi
RECOVERY_CONFIG=""
if [[ "$RECOVERY_OPTION" == "y" ]]; then RECOVERY_CONFIG="recovery.config"; KSU_OPTION="n"; fi
KSU_CONFIG=""
if [[ "$KSU_OPTION" == "y" ]]; then KSU_CONFIG="ksu.config"; fi

# Track compiled platforms
COMPILED_9820=false
COMPILED_9825=false

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
    SHOULD_COMPILE=false
    if [[ "$SOC" == "exynos9820" && "$COMPILED_9820" == false ]]; then SHOULD_COMPILE=true; fi
    if [[ "$SOC" == "exynos9825" && "$COMPILED_9825" == false ]]; then SHOULD_COMPILE=true; fi

    MAKE_ARGS="LLVM=1 LLVM_IAS=1 ARCH=arm64 O=out/$SOC"
    if [ "$SHOULD_COMPILE" = true ]; then
        echo "Compiling for $SOC..."
        mkdir -p out/$SOC
        [[ "$SOC" == "exynos9820" ]] && REF_CONF="beyond2lte.config" || REF_CONF="d2xks.config"
        make ${MAKE_ARGS} -j$CORES exynos9820_defconfig $REF_CONF $KSU_CONFIG $RECOVERY_CONFIG || abort
        make ${MAKE_ARGS} -j$CORES || abort
        [[ "$SOC" == "exynos9820" ]] && COMPILED_9820=true || COMPILED_9825=true
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
    cp build/update-binary "$UNIFIED_OUT/META-INF/com/google/android/update-binary"
else
    # Fallback: if user didn't provide update-binary in build/, we must ensure one exists.
    # But according to instructions, build/ is the source.
    echo "Warning: build/update-binary not found, using reference one."
    cp META-INF/com/google/android/update-binary "$UNIFIED_OUT/META-INF/com/google/android/update-binary"
fi

# IMPORTANT: build/updater-script is the source for Edify logic.
# However, for a "Unified" package, the installer is usually a shell script (update-binary).
# If the user wants to use updater-script, we copy it.
cp build/updater-script "$UNIFIED_OUT/META-INF/com/google/android/updater-script"

# Final ZIP
DATE=`date +"%d-%m-%Y_%H-%M-%S"`
version=$(grep -o 'CONFIG_LOCALVERSION="[^"]*"' arch/arm64/configs/exynos9820_defconfig | cut -d '"' -f 2 | sed 's/^.//')
[[ "$KSU_OPTION" == "y" ]] && ZIP_NAME="${version}_Unified_KSU_${DATE}.zip" || ZIP_NAME="ExtremeKRNL_${version}_Unified_${DATE}.zip"

pushd "$UNIFIED_OUT" > /dev/null
zip -r "../../$ZIP_NAME" . > /dev/null
popd > /dev/null

popd > /dev/null
echo "Unified package created: build/out/$ZIP_NAME"

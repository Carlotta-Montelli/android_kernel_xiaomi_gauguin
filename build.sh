#!/bin/bash

echo -e "==========================="
echo -e "= START COMPILING KERNEL  ="
echo -e "==========================="
bold=$(tput bold)
normal=$(tput sgr0)

#apt update -y && apt upgrade -y && apt install nano bc bison ca-certificates curl flex gcc git libc6-dev libssl-dev openssl python-is-python3 ssh wget zip zstd sudo make clang gcc-arm-linux-gnueabi software-properties-common build-essential libarchive-tools gcc-aarch64-linux-gnu -y && apt install build-essential -y && apt install libssl-dev libffi-dev libncurses5-dev zlib1g zlib1g-dev libreadline-dev libbz2-dev libsqlite3-dev make gcc -y && apt install pigz -y && apt install python2 -y && apt install python3 -y


# Scrip option
while (( ${#} )); do
    case ${1} in
        "-Z"|"--zip") ZIP=true ;;
    esac
    shift
done
[[ -z ${ZIP} ]] && { echo "${bold}LOADING-_-....${normal}"; }

DEFCONFIG="gauguin_defconfig"
export KBUILD_BUILD_USER="RasyidFurina"
export TZ=Asia/Jakarta
#export KBUILD_BUILD_VERSION=1
#export KBUILD_BUILD_TIMESTAMP="Thu Jan 1 07:00:00 WIB 2023"
export KBUILD_BUILD_HOST="Fontaine-rev14-sm6350-platform"
export KERNELDIR="/root/kernel/android_kernel_xiaomi_gauguin"
export KERNELNAME="Furina"
export SRCDIR="${KERNELDIR}"
export OUTDIR="${KERNELDIR}/out"
export ANYKERNEL="${KERNELDIR}/AnyKernel3"
export DEFCONFIG="gauguin_ksu_defconfig"
export ZIP_DIR="${KERNELDIR}/files"
export IMAGE="${OUTDIR}/arch/arm64/boot/Image.gz"
export DTBO="${OUTDIR}/arch/arm64/boot/dtbo.img"
export ZIPNAME="${KERNELNAME}-Kernel-gauguin-$(date +%y%m%d-%H%M%S).zip"
export FINAL_ZIP="${ZIP_DIR}/${ZIPNAME}"
export TC_DIR="/root/kernel/tool/proton-clang"
export PATH="$TC_DIR/bin:$PATH"

if [[ $1 = "-r" || $1 = "--regen" ]]; then
make O=out ARCH=arm64 $DEFCONFIG savedefconfig
cp out/defconfig arch/arm64/configs/$DEFCONFIG
exit
fi

if [[ $1 = "-c" || $1 = "--clean" ]]; then
rm -rf out
fi

mkdir -p out
make O=out ARCH=arm64 $DEFCONFIG
make -j12 O=out ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang HOSTCXX=clang++ READELF=llvm-readelf HOSTAR=llvm-ar AR=llvm-ar AS=llvm-as NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi-  2>&1 | tee log.txt
    echo -e "==========================="
    echo -e "   COMPILE KERNEL COMPLETE "
    echo -e "==========================="

# Make ZIP using AnyKernel
# ================
rm -rf ${ZIP_DIR};
mkdir ${ZIP_DIR};
echo -e "Copying kernel image";
cp -rf -v "${IMAGE}" "${ANYKERNEL}/";
cp -rf -v "${DTBO}" "${ANYKERNEL}/";
cd -;
cd ${ANYKERNEL};
zip -r9 ${FINAL_ZIP} *;
bash ${KERNELDIR}/gf.sh ${FINAL_ZIP};
cd -;

if [[ ":v" ]]; then
exit
fi

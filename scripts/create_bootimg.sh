#!/bin/bash
# Create partitioned boot disk with FAT32 (containing kernel.elf) + OnyxFS partition
set -e

export LD_LIBRARY_PATH=/home/z/qemu-libs
export PATH="/home/z/.local/bin:$PATH"

ROOT=/home/z/my-project/onyx/OnyxKernel
BUILD=$ROOT/build
KERNEL=$ROOT/target/riscv64gc-unknown-none-elf/release/onyx-kernel

FAT_LBA=2048
SLBA=10240

dd if=/dev/zero of=$BUILD/boot.img bs=1M count=64 2>/dev/null
parted -s $BUILD/boot.img mklabel msdos 2>/dev/null
parted -s $BUILD/boot.img mkpart primary fat32 1MiB 5MiB 2>/dev/null
mkfs.fat -F 32 $BUILD/boot.img --offset=$FAT_LBA 2>/dev/null
mcopy -i $BUILD/boot.img@@$((FAT_LBA * 512)) $KERNEL ::kernel.elf
dd if=$BUILD/disk.img of=$BUILD/boot.img bs=512 seek=$SLBA conv=notrunc 2>/dev/null

ls -la $BUILD/boot.img
echo "Boot disk created."

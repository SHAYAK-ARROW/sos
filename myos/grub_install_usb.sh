#!/bin/bash
set -e
BOOT=/mnt/x/boot
if [ ! -d "$BOOT" ]; then
    echo "ERROR: $BOOT not found. Is X: drive mounted in WSL?"
    exit 1
fi
for dev in /dev/sdb /dev/sdc /dev/sdd /dev/sde; do
    if [ -b "$dev" ]; then
        echo "Trying grub-install on $dev ..."
        if grub-install --target=i386-pc --boot-directory="$BOOT" --no-floppy --force "$dev"; then
            echo "Installed to $dev"
            exit 0
        fi
    fi
done
echo "grub-install failed on sdb-sde"
exit 1

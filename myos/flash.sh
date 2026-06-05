#!/bin/bash
# =============================================================
#  flash.sh — myos Pen Drive Flash Script
#  myos/ folder এর ভেতর থেকে run করো:
#    cd myos
#    sudo bash flash.sh
# =============================================================

set -e  # কোনো error হলে থেমে যাবে

# ── রং ──
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${CYAN}"
echo "╔══════════════════════════════════════╗"
echo "║        myos Pen Drive Flasher        ║"
echo "╚══════════════════════════════════════╝"
echo -e "${NC}"

# ── Root check ──
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}[ERROR] sudo দিয়ে run করো: sudo bash flash.sh${NC}"
    exit 1
fi

# ── Script কোন directory থেকে run হচ্ছে ──
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ─────────────────────────────────────────
# STEP 1: Dependencies check + install
# ─────────────────────────────────────────
echo -e "${YELLOW}[1/6] Dependencies check করছি...${NC}"

PKGS=()
command -v i686-linux-gnu-gcc  &>/dev/null || PKGS+=(gcc-i686-linux-gnu)
command -v grub-install         &>/dev/null || PKGS+=(grub-pc-bin grub-common)
command -v xorriso              &>/dev/null || PKGS+=(xorriso)
command -v mkfs.vfat            &>/dev/null || PKGS+=(dosfstools)

if [ ${#PKGS[@]} -gt 0 ]; then
    echo -e "${YELLOW}    Installing: ${PKGS[*]}${NC}"
    apt-get update -qq
    apt-get install -y -qq "${PKGS[@]}"
fi
echo -e "${GREEN}    ✓ Dependencies OK${NC}"

# ─────────────────────────────────────────
# STEP 2: Build myos
# ─────────────────────────────────────────
echo -e "${YELLOW}[2/6] myos build করছি...${NC}"
make clean -s
make -s
if [ ! -f "myos.bin" ]; then
    echo -e "${RED}[ERROR] Build failed! myos.bin তৈরি হয়নি।${NC}"
    exit 1
fi
echo -e "${GREEN}    ✓ Build সফল (myos.bin ready)${NC}"

# ─────────────────────────────────────────
# STEP 3: Pen drive select করো
# ─────────────────────────────────────────
echo -e "${YELLOW}[3/6] Pen drive detect করছি...${NC}"
echo ""
echo "    Connected drives:"
echo "    ──────────────────────────────────────"
lsblk -d -o NAME,SIZE,MODEL,TRAN | grep -v "loop\|sr0\|zram" | \
    awk 'NR==1{print "    "$0} NR>1{print "    /dev/"$0}'
echo "    ──────────────────────────────────────"
echo ""

while true; do
    read -r -p "    Pen drive এর নাম লেখো (যেমন sdb, sdc): " DRIVE
    DRIVE="${DRIVE//\/dev\//}"  # /dev/ prefix থাকলে সরিয়ে দাও
    DEV="/dev/$DRIVE"

    if [ ! -b "$DEV" ]; then
        echo -e "${RED}    [ERROR] $DEV পাওয়া যাচ্ছে না। আবার চেষ্টা করো।${NC}"
        continue
    fi

    # Size check — 1GB এর কম হলে warn করো
    SIZE_BYTES=$(blockdev --getsize64 "$DEV")
    SIZE_GB=$(echo "scale=1; $SIZE_BYTES/1024/1024/1024" | bc)

    echo ""
    echo -e "${RED}╔══════════════════════════════════════════════╗"
    echo    "║  ⚠️  WARNING — এই drive এর সব data মুছে যাবে! ║"
    echo -e "║  Drive: $DEV  Size: ${SIZE_GB}GB$(printf '%*s' $((30 - ${#DEV} - ${#SIZE_GB})) '')║"
    echo -e "╚══════════════════════════════════════════════╝${NC}"
    echo ""
    read -r -p "    নিশ্চিত? (yes লেখো confirm করতে): " CONFIRM

    if [ "$CONFIRM" = "yes" ]; then
        break
    else
        echo -e "${YELLOW}    বাতিল। অন্য drive দাও।${NC}"
    fi
done

echo -e "${GREEN}    ✓ Drive selected: $DEV${NC}"

# ─────────────────────────────────────────
# STEP 4: Unmount + Partition
# ─────────────────────────────────────────
echo -e "${YELLOW}[4/6] Partition তৈরি করছি...${NC}"

# সব mounted partition unmount করো
for part in "$DEV"?*; do
    if mountpoint -q "$part" 2>/dev/null; then
        umount "$part" 2>/dev/null || true
    fi
done
umount "$DEV" 2>/dev/null || true

# Partition table লেখো (fdisk script)
echo -e "${YELLOW}    Partition table লিখছি...${NC}"
(
echo o      # নতুন DOS table
echo n      # নতুন partition
echo p      # primary
echo 1      # partition 1
echo        # default start
echo +256M  # 256MB boot
echo a      # bootable
echo n      # নতুন partition
echo p      # primary
echo 2      # partition 2
echo        # default start
echo        # বাকি সব
echo w      # save
) | fdisk "$DEV" > /dev/null 2>&1 || true

# Partition table reload
partprobe "$DEV" 2>/dev/null || true
sleep 2

# Partition নাম বের করো (sdb1 / sdb2 বা sdb1 / sdb2)
if [ -b "${DEV}1" ]; then
    PART1="${DEV}1"
    PART2="${DEV}2"
elif [ -b "${DEV}p1" ]; then
    PART1="${DEV}p1"
    PART2="${DEV}p2"
else
    echo -e "${RED}[ERROR] Partition তৈরি হয়নি। fdisk manually চালাও।${NC}"
    exit 1
fi

# Format
echo -e "${YELLOW}    Format করছি...${NC}"
mkfs.vfat -F 32 -n "MYOS_BOOT" "$PART1" > /dev/null
mkfs.vfat -F 16 -n "MYOS_DATA" "$PART2" > /dev/null
echo -e "${GREEN}    ✓ Partition 1 (FAT32, 256MB) — Boot${NC}"
echo -e "${GREEN}    ✓ Partition 2 (FAT16, বাকি) — Data${NC}"

# ─────────────────────────────────────────
# STEP 5: GRUB + Kernel install
# ─────────────────────────────────────────
echo -e "${YELLOW}[5/6] GRUB ও kernel install করছি...${NC}"

MNT=$(mktemp -d)
mount "$PART1" "$MNT"

mkdir -p "$MNT/boot/grub"
cp myos.bin "$MNT/boot/myos.bin"
cp grub.cfg "$MNT/boot/grub/grub.cfg"

# grub.cfg এ kernel path ঠিক করো
cat > "$MNT/boot/grub/grub.cfg" << 'GRUBCFG'
set timeout=3
set default=0

menuentry "myos" {
    multiboot /boot/myos.bin
    boot
}
GRUBCFG

grub-install \
    --target=i386-pc \
    --boot-directory="$MNT/boot" \
    --no-floppy \
    "$DEV" > /dev/null 2>&1

umount "$MNT"
echo -e "${GREEN}    ✓ GRUB installed${NC}"
echo -e "${GREEN}    ✓ myos.bin copied${NC}"

# ─────────────────────────────────────────
# STEP 6: Data partition (Partition 2)
# ─────────────────────────────────────────
echo -e "${YELLOW}[6/6] Data partition তৈরি করছি...${NC}"

mount "$PART2" "$MNT"
# OS নিজেই FAT16 init করবে boot-এ।
# একটা README রাখছি।
cat > "$MNT/README.TXT" << 'EOF'
myos Data Partition
===================
This partition is managed by myos.
Files saved in the shell will appear here.
EOF
umount "$MNT"
rmdir "$MNT"

echo -e "${GREEN}    ✓ Data partition ready${NC}"

# ─────────────────────────────────────────
# Done!
# ─────────────────────────────────────────
echo ""
echo -e "${GREEN}╔══════════════════════════════════════════╗"
echo    "║  ✅  Flash সম্পন্ন!                      ║"
echo    "║                                          ║"
echo    "║  Pen drive টা PC তে লাগাও               ║"
echo    "║  BIOS এ USB boot select করো             ║"
echo    "║  myos চালু হবে!                          ║"
echo -e "╚══════════════════════════════════════════╝${NC}"
echo ""
echo -e "  ${CYAN}Partition layout:${NC}"
echo -e "  $PART1  →  Boot (GRUB + myos kernel)"
echo -e "  $PART2  →  Data (FAT16, myos filesystem)"
echo ""

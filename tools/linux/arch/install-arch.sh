#!/usr/bin/env bash
# install-arch.sh -- unattended Arch install for AppSandbox guests.
#
# Runs inside the Arch *live* environment, started over the serial console by
# the host (arch_install.inc: arch_install -> arch_serial_trigger). The
# seed ISO (label ASBSEED) is mounted read-only at /seed and contains:
#   /seed/seed.env      per-VM settings (user, password hash, hostname, ...)
#   /seed/payload/      agent-src, asb_drm-src, dxgkrnl-src, systemd/, wsl-deps/,
#                       arch/ (this directory), ...
#
# This script handles what only the live environment can do: network, disk,
# pacstrap, files written from outside. Everything that needs the new system's
# own tools lives in chroot-arch.sh, which it runs through arch-chroot.
#
# Output goes to the serial console (/dev/ttyS0 = HCS COM1 named pipe); the
# host log-forwards every line and watches for:
#   PROGRESS:<0-100>   -> installer progress bar
#   DONE:OK            -> success (VM then powers off, host boots the VHDX)
#   DONE:FAIL <why>    -> failure
# Keep lines short and avoid progress bars: the host parses line by line.

set -uo pipefail

SEED=/seed
PAYLOAD=$SEED/payload
TARGET=/mnt
EXTRAS=/opt/appsandbox/extras      # where the payload lives inside the target
LOG=/var/log/asb-install.log

if [ -w /dev/ttyS0 ]; then
    exec > >(tee -a "$LOG" > /dev/ttyS0) 2>&1
else
    exec > >(tee -a "$LOG") 2>&1
fi

progress() { echo "PROGRESS:$1"; }
step()     { echo "==== $* (+$(( $(date +%s) - T0 ))s) ===="; }
fail()     { echo "DONE:FAIL $*"; sleep 3; sync; poweroff -f; exit 1; }
need()     { "$@" || fail "command failed: $*"; }
T0=$(date +%s); export T0          # chroot-arch.sh prints timings from the same start

# shellcheck disable=SC1091
. "$SEED/seed.env" || fail "cannot read seed.env"

# ---- 1. Network: the HCN NAT has no DHCP, the host gives a static address ----
setup_network() {
    step "network"; progress 1
    systemctl stop NetworkManager systemd-networkd 2>/dev/null || true

    local iface="" d n i
    for i in $(seq 1 30); do                     # wait for hv_netvsc to show up
        for d in /sys/class/net/*; do
            n=$(basename "$d"); [ "$n" = lo ] && continue
            iface=$n; break
        done
        [ -n "$iface" ] && break
        sleep 1
    done
    [ -n "$iface" ] || fail "no network interface (hv_netvsc not loaded?)"
    ip link set "$iface" up

    if [ -n "$ASB_NET_IP" ]; then
        ip addr flush dev "$iface"
        ip addr add "$ASB_NET_IP/${ASB_NET_PREFIX:-24}" dev "$iface"
        ip route replace default via "$ASB_NET_GW" dev "$iface"
        rm -f /etc/resolv.conf
        printf 'nameserver %s\nnameserver 8.8.8.8\n' "$ASB_NET_GW" > /etc/resolv.conf
    else
        dhcpcd -q "$iface" 2>/dev/null || dhclient "$iface" 2>/dev/null || true
    fi

    for i in $(seq 1 20); do
        if getent hosts archlinux.org >/dev/null 2>&1 || ping -c1 -W2 8.8.8.8 >/dev/null 2>&1; then
            timedatectl set-ntp true 2>/dev/null || true
            return
        fi
        sleep 2
    done
    fail "no network connectivity on $iface"
}

# ---- 2. Disk: first blank disk -> 1 GiB ESP + ext4 root; sets $ESP and $ROOT ----
partition_disk() {
    step "partition"; progress 3
    local disk p
    disk=$(lsblk -dnpo NAME,TYPE,RO | awk '$2=="disk" && $3==0 {print $1}' | while read -r d; do
               [ "$(lsblk -n "$d" | wc -l)" = 1 ] && { echo "$d"; break; }   # no partitions
           done)
    [ -n "$disk" ] || fail "no blank target disk found"
    echo "target disk: $disk"

    need sgdisk --zap-all "$disk"
    need sgdisk -n1:0:+1G -t1:ef00 -c1:ESP  "$disk"
    need sgdisk -n2:0:0   -t2:8300 -c2:root "$disk"
    partprobe "$disk"; udevadm settle
    case "$disk" in *nvme*|*mmcblk*) p=p ;; *) p= ;; esac
    ESP="${disk}${p}1"; ROOT="${disk}${p}2"

    need mkfs.fat -F32 -n ESP "$ESP"
    need mkfs.ext4 -F -L asb-root "$ROOT"
    need mount "$ROOT" "$TARGET"
    mkdir -p "$TARGET/boot"
    need mount "$ESP" "$TARGET/boot"
}

# ---- 3. Base system: pacstrap + fstab ----
install_base() {
    step "pacstrap"; progress 5
    pacman-key --init >/dev/null 2>&1 || true
    pacman-key --populate archlinux >/dev/null 2>&1 || true
    grep -q '^ParallelDownloads' /etc/pacman.conf ||
        sed -i 's/^\[options\]/[options]\nParallelDownloads = 8/' /etc/pacman.conf
    pacman -Sy --noconfirm --noprogressbar >/dev/null 2>&1 || echo "WARN: pacman -Sy failed"

    # linux-lts: dxgkrnl is out-of-tree and tracks kernel API drift.
    local pkgs=(base base-devel linux-lts linux-lts-headers mkinitcpio dkms
                networkmanager sudo nano git zstd rsync
                libdrm directx-headers alsa-lib libxcb libx11 pkgconf systemd
                gdm gnome-shell gnome-session gnome-control-center gnome-terminal
                nautilus gnome-text-editor gnome-keyring xdg-desktop-portal-gnome
                xorg-xwayland mutter
                pipewire pipewire-alsa pipewire-pulse wireplumber
                firefox ttf-dejavu noto-fonts)
    [ "${ASB_SSH:-0}" = 1 ] && pkgs+=(openssh)

    local n=0
    until pacstrap -C /etc/pacman.conf -K "$TARGET" "${pkgs[@]}" --noconfirm --noprogressbar; do
        n=$((n+1)); [ "$n" -ge 3 ] && fail "pacstrap failed after $n attempts"
        echo "pacstrap failed, retrying ($n)"; sleep 10
    done
    progress 40

    cp -f /etc/pacman.conf "$TARGET/etc/pacman.conf"
    cp -f /etc/pacman.d/*mirrorlist "$TARGET/etc/pacman.d/" 2>/dev/null || true
    genfstab -U "$TARGET" > "$TARGET/etc/fstab"
    # vfat has no file modes: the mount options decide who can read /boot (random-seed).
    sed -i '/[[:space:]]\/boot[[:space:]]/ s/fmask=0022,dmask=0022/fmask=0077,dmask=0077/' "$TARGET/etc/fstab"
}

# ---- 4. Static configuration, written from outside the chroot ----
configure_target() {
    step "configure"; progress 42
    local locale=${ASB_LOCALE:-en_US.UTF-8} tz=${ASB_TZ:-UTC} xkb=${ASB_XKB:-us}

    mkdir -p "$TARGET/opt/appsandbox"
    cp -a "$PAYLOAD" "$TARGET$EXTRAS"
    rm -f "$TARGET$EXTRAS/wsl-mesa.tar.zst"          # Ubuntu-built, unusable here

    # locale, keyboard, host name, time zone
    sed -i "s/^#\(en_US.UTF-8 UTF-8\)/\1/" "$TARGET/etc/locale.gen"
    grep -q "^$locale " "$TARGET/etc/locale.gen" ||
        sed -i "s/^#\($locale .*\)/\1/" "$TARGET/etc/locale.gen"
    echo "LANG=$locale" > "$TARGET/etc/locale.conf"
    printf 'KEYMAP=%s\n' "$xkb" > "$TARGET/etc/vconsole.conf"
    echo "$ASB_HOSTNAME" > "$TARGET/etc/hostname"
    printf '127.0.0.1 localhost\n::1 localhost\n127.0.1.1 %s\n' "$ASB_HOSTNAME" > "$TARGET/etc/hosts"
    [ -e "$TARGET/usr/share/zoneinfo/$tz" ] || tz=UTC
    ln -sf "/usr/share/zoneinfo/$tz" "$TARGET/etc/localtime"

    # keyboard layout for GNOME/GDM
    mkdir -p "$TARGET/etc/X11/xorg.conf.d"
    cat > "$TARGET/etc/X11/xorg.conf.d/00-keyboard.conf" <<EOF
Section "InputClass"
    Identifier "system-keyboard"
    MatchIsKeyboard "on"
    Option "XkbLayout" "$xkb"
    Option "XkbVariant" "${ASB_XKB_VARIANT:-}"
EndSection
EOF

    # Hyper-V drivers must be in the initramfs; the hyperv_drm/simpledrm blacklist
    # keeps asb_drm the only DRM device.
    install -m 0644 "$TARGET$EXTRAS/modprobe.d-asb_drm.conf" "$TARGET/etc/modprobe.d/asb_drm.conf" 2>/dev/null || true
    mkdir -p "$TARGET/etc/mkinitcpio.conf.d"
    echo 'MODULES=(hv_vmbus hv_storvsc hv_netvsc hv_utils hv_sock)' \
        > "$TARGET/etc/mkinitcpio.conf.d/10-appsandbox.conf"

    # kernel command line (systemd-boot entry is built from /etc/kernel/cmdline)
    mkdir -p "$TARGET/etc/kernel"
    echo "root=UUID=$(blkid -s UUID -o value "$ROOT") rw video=efifb:off video=simplefb:off console=ttyS0,115200 console=tty0" \
        > "$TARGET/etc/kernel/cmdline"
}

# ---- 5. Everything that needs the new system's own tools ----
run_chroot() {
    progress 45
    cp "$SEED/seed.env" "$TARGET/root/seed.env"
    arch-chroot "$TARGET" bash "$EXTRAS/arch/chroot-arch.sh" || fail "chroot configuration failed (rc=$?)"
    rm -f "$TARGET/root/seed.env"                    # holds the password hash
    rm -rf "$TARGET"/var/cache/pacman/pkg/*          # ~1 GB of downloaded packages
}

# ---- 6. Sanity checks, then power off for the host to boot the disk ----
verify_and_finish() {
    step "verify"; progress 95
    local f miss=0
    for f in /boot/EFI/BOOT/BOOTX64.EFI /boot/loader/entries/asb-arch.conf \
             /usr/local/bin/appsandbox-agent /usr/local/bin/appsandbox-display; do
        if [ -e "$TARGET$f" ]; then echo "OK: $f"; else echo "MISSING: $f"; miss=1; fi
    done
    find "$TARGET/usr/lib/modules" -name 'dxgkrnl.ko*' -o -name 'asb_drm.ko*' 2>/dev/null
    [ -e "$TARGET/opt/wsl-mesa/lib/x86_64-linux-gnu/dri/d3d12_dri.so" ] ||
        echo "WARN: patched Mesa missing (software desktop)"
    [ "$miss" = 0 ] || fail "required files missing after install"

    sync
    umount -R "$TARGET" 2>/dev/null || true
    progress 100
    echo "DONE:OK"
    sleep 2
    poweroff -f
}

setup_network
partition_disk
install_base
configure_target
run_chroot
verify_and_finish

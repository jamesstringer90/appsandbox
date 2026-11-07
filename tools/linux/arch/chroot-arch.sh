#!/usr/bin/env bash
# chroot-arch.sh -- configure the freshly installed system (runs under arch-chroot).
#
# Started by install-arch.sh; /root/seed.env (per-VM settings, incl. the password
# hash) is copied in just for this run. The payload is at $EXTRAS. Every function
# reports problems with "WARN:"/"FAIL:" lines and carries on, so one broken part
# (say the patched Mesa) leaves a bootable system; install-arch.sh verifies the
# files that must exist.
set -uo pipefail

# shellcheck disable=SC1091
. /root/seed.env
EXTRAS=/opt/appsandbox/extras
HERE=$EXTRAS/arch                  # the files next to this script
UNITS="appsandbox-agent appsandbox-audio appsandbox-display appsandbox-input asb-evict-simpledrm"
T0=${T0:-$(date +%s)}
MESA_PID=""

say()  { echo "$@"; }
step() { echo "==== $* (+$(( $(date +%s) - T0 ))s) ===="; }
pac()  { pacman -S --noconfirm --needed --noprogressbar "$@" >/dev/null 2>&1; }

setup_user() {
    locale-gen >/dev/null 2>&1 || say "WARN: locale-gen"
    id "$ASB_USER" >/dev/null 2>&1 || useradd -m -G wheel -s /bin/bash "$ASB_USER" || exit 10
    usermod -p "$ASB_PWHASH" "$ASB_USER" || exit 11      # hash staged by the host, never plaintext
    echo '%wheel ALL=(ALL:ALL) ALL' > /etc/sudoers.d/10-wheel
    chmod 440 /etc/sudoers.d/10-wheel

    if [ -n "${ASB_SSH_PUBKEY:-}" ]; then
        install -d -m 700 -o "$ASB_USER" -g "$ASB_USER" "/home/$ASB_USER/.ssh"
        printf '%s\n' "$ASB_SSH_PUBKEY" >> "/home/$ASB_USER/.ssh/authorized_keys"
        chown "$ASB_USER:$ASB_USER" "/home/$ASB_USER/.ssh/authorized_keys"
        chmod 600 "/home/$ASB_USER/.ssh/authorized_keys"
    fi

    mkdir -p /etc/gdm
    printf '[daemon]\nAutomaticLoginEnable=True\nAutomaticLogin=%s\nWaylandEnable=true\n' \
        "$ASB_USER" > /etc/gdm/custom.conf
}

# Patched Mesa (project's asb-mesa, pinned tag from the host). Its d3d12/dzn
# changes let the compositor present GPU buffers through asb_drm (the shared-GPU
# display path); the published package is Ubuntu-only, so it is built here into
# /opt/wsl-mesa. It runs in the background while DKMS and the agents install.
# The distro Mesa + dzn go in first: tools, and the fallback if the build fails.
start_mesa_build() {
    pac mesa libglvnd mesa-utils vulkan-tools || say "WARN: mesa install"
    pacman -Si vulkan-dzn >/dev/null 2>&1 && pac vulkan-dzn
    pac git meson ninja python-mako python-yaml python-packaging python-ply glslang \
        llvm clang libclc spirv-llvm-translator spirv-tools wayland wayland-protocols \
        libxext libxshmfence libxxf86vm libxrandr libxfixes libxdamage xorgproto bison \
        flex zstd libelf expat vulkan-icd-loader xcb-util-keysyms libunwind lm_sensors ||
        say "WARN: mesa build deps"

    echo "${ASB_MESA_TAG:-main}" > /etc/asb-mesa-tag
    install -m 0755 "$HERE/asb-mesa-rebuild" "$HERE/appsandbox-compositor" /usr/local/bin/
    install -D -m 0644 "$HERE/asb-mesa-rebuild.hook" /etc/pacman.d/hooks/asb-mesa-rebuild.hook
    say "building the patched Mesa in the background (asb-mesa ${ASB_MESA_TAG:-main})"
    /usr/local/bin/asb-mesa-rebuild & MESA_PID=$!
}

wait_mesa_build() {
    step "mesa wait"
    if wait "$MESA_PID"; then
        say "OK: patched Mesa installed to /opt/wsl-mesa"
    else
        say "WARN: patched Mesa build failed; the desktop will use software rendering"
        tail -n 20 /var/log/asb-mesa-rebuild.log 2>/dev/null
    fi
}

# dxgkrnl + asb_drm (out-of-tree) built against the installed kernel.
build_dkms() {
    local kver mod src ver
    kver=$(ls /usr/lib/modules | sort -V | tail -n1)
    say "target kernel: $kver"
    for mod in asb_drm dxgkrnl; do
        src="$EXTRAS/$mod-src"
        [ -d "$src" ] || { say "FAIL: no $src"; continue; }
        ver=$(awk -F= '/^PACKAGE_VERSION=/{gsub(/"/,"",$2); print $2}' "$src/dkms.conf" 2>/dev/null)
        ver=${ver:-1.0.0}
        rm -rf "/usr/src/$mod-$ver"; cp -r "$src" "/usr/src/$mod-$ver"
        dkms add -m "$mod" -v "$ver" 2>&1 | tail -2
        if dkms build -m "$mod" -v "$ver" -k "$kver" 2>&1 | tail -5 &&
           dkms install -m "$mod" -v "$ver" -k "$kver" 2>&1 | tail -3; then
            say "OK: dkms $mod $ver"
        else
            say "FAIL: dkms $mod (make.log tail follows)"
            find /var/lib/dkms -name make.log -exec tail -30 {} \; 2>/dev/null
        fi
    done
    depmod -a "$kver"
    echo dxgkrnl > /etc/modules-load.d/dxgkrnl.conf
    local f
    for f in "$EXTRAS"/systemd/modules-load.d-*.conf; do
        [ -f "$f" ] && install -m 0644 "$f" "/etc/modules-load.d/$(basename "$f" | sed 's/^modules-load.d-//')"
    done
}

# Guest daemons (display, input, audio, clipboard, agent) and their units.
# The Makefile's `install` target also copies units from agent-src/systemd,
# which the host prefetch does not stage (units come from $EXTRAS/systemd),
# so the binaries and the GNOME extension are installed directly.
install_agents() {
    local a=$EXTRAS/agent-src u
    if make -C "$a" all install-pointer 2>&1 | tail -5 &&
       install -m 0755 "$a"/appsandbox-{agent,display,input,audio,clipboard} /usr/local/bin/; then
        say "OK: agents built"
    else
        say "FAIL: agents build"
    fi

    for u in $UNITS; do
        [ -f "$EXTRAS/systemd/$u.service" ] && install -m 0644 "$EXTRAS/systemd/$u.service" /etc/systemd/system/
    done
    install -d /etc/systemd/system/appsandbox-agent.service.d
    printf '[Unit]\nAfter=NetworkManager.service systemd-networkd.service\n' \
        > /etc/systemd/system/appsandbox-agent.service.d/10-network-renderer.conf
    [ -f "$EXTRAS/systemd/appsandbox-clipboard.service" ] &&
        install -D -m 0644 "$EXTRAS/systemd/appsandbox-clipboard.service" /etc/systemd/user/appsandbox-clipboard.service
}

# GPU plumbing: WSL-style libs, env generator, GNOME/PipeWire drop-ins.
install_gpu_audio() {
    [ -d "$EXTRAS/wsl-deps" ] && echo "$EXTRAS/wsl-deps" > /etc/ld.so.conf.d/appsandbox-wsl-deps.conf
    ldconfig
    [ -f "$EXTRAS/50-appsandbox-gpu" ] &&
        install -D -m 0755 "$EXTRAS/50-appsandbox-gpu" /etc/systemd/user-environment-generators/50-appsandbox-gpu
    [ -f "$EXTRAS/appsandbox-gpu" ] && install -m 0755 "$EXTRAS/appsandbox-gpu" /usr/local/bin/
    ln -sf /usr/lib/wsl/lib/nvidia-smi /usr/local/bin/nvidia-smi
    install -D -m 0644 "$HERE/no-gpu.conf" /etc/systemd/user/org.gnome.Shell@.service.d/no-gpu.conf
    install -D -m 0644 "$HERE/50-appsandbox-loopback.conf" /etc/pipewire/pipewire.conf.d/50-appsandbox-loopback.conf
}

# Pointer extension on, no sleep/lock (a VM that locks itself is unusable remotely).
configure_gnome() {
    install -d /etc/dconf/profile
    printf 'user-db:user\nsystem-db:local\n' > /etc/dconf/profile/user
    install -D -m 0644 "$HERE/dconf-appsandbox.conf" /etc/dconf/db/local.d/00-appsandbox
    dconf update 2>&1 || say "WARN: dconf update"
    install -d /etc/systemd/logind.conf.d
    printf '[Login]\nHandleLidSwitch=ignore\nIdleAction=ignore\n' \
        > /etc/systemd/logind.conf.d/10-appsandbox-nosleep.conf
    systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target
}

enable_services() {
    systemctl enable NetworkManager gdm systemd-timesyncd   # Arch ships timesyncd disabled
    [ "${ASB_SSH:-0}" = 1 ] && systemctl enable sshd
    local u
    for u in $UNITS; do
        [ -f "/etc/systemd/system/$u.service" ] && systemctl enable "$u.service"
    done
    systemctl set-default graphical.target
}

# initramfs + systemd-boot on the removable path (no NVRAM dependence).
install_bootloader() {
    mkinitcpio -P 2>&1 | tail -5
    bootctl install --esp-path=/boot --no-variables 2>&1 | tail -3
    install -d /boot/loader/entries
    printf 'default asb-*\ntimeout 0\nconsole-mode keep\neditor no\n' > /boot/loader/loader.conf

    local img base
    img=$(ls /boot/vmlinuz-* | head -n1); base=${img##*/vmlinuz-}
    cat > /boot/loader/entries/asb-arch.conf <<EOF
title   Arch Linux (AppSandbox)
linux   /vmlinuz-$base
initrd  /initramfs-$base.img
options $(cat /etc/kernel/cmdline)
EOF
    say "boot entry: kernel=$base"
}

setup_user
start_mesa_build
build_dkms
install_agents
install_gpu_audio
configure_gnome
enable_services
wait_mesa_build
install_bootloader
exit 0

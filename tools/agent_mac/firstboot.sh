#!/bin/sh
# Runs once on first guest boot as root, installs the agent LaunchDaemon
# into /Library/LaunchDaemons with correct ownership, then deletes itself.
# This is the macOS analog of Windows' SetupComplete.cmd invoking
# `appsandbox-agent.exe --install`.

set -e

LOG=/var/log/appsandbox-firstboot.log
echo "=== firstboot.sh started $(date) ===" >> "$LOG"

SRC=/Library/AppSandbox/com.appsandbox.agent.plist
DST=/Library/LaunchDaemons/com.appsandbox.agent.plist

if [ ! -f "$SRC" ]; then
    echo "Missing $SRC, aborting." >> "$LOG"
    exit 1
fi

chown root:wheel /Library/AppSandbox/appsandbox-agent
chmod 755        /Library/AppSandbox/appsandbox-agent

cp "$SRC" "$DST"
chown root:wheel "$DST"
chmod 644        "$DST"

launchctl bootstrap system "$DST" >> "$LOG" 2>&1 || true

# Clipboard sync helper — user-context LaunchAgent. Place the plist in
# /Library/LaunchAgents/ so every user login gets its own instance in
# that user's GUI session (needed for NSPasteboard access).
if [ -f /Library/AppSandbox/com.appsandbox.clipboard.plist ]; then
    CLIP_SRC=/Library/AppSandbox/com.appsandbox.clipboard.plist
    CLIP_DST=/Library/LaunchAgents/com.appsandbox.clipboard.plist
    cp "$CLIP_SRC" "$CLIP_DST"
    chown root:wheel "$CLIP_DST"
    chmod 644        "$CLIP_DST"
    chown root:wheel /Library/AppSandbox/appsandbox-clipboard 2>/dev/null || true
    chmod 755        /Library/AppSandbox/appsandbox-clipboard 2>/dev/null || true
fi

# SSH: primary enablement is the launchd override DB flip the host does
# during stage. As belt-and-braces, also run the in-guest enable path,
# which works without Full Disk Access (unlike systemsetup -setremotelogin).
/bin/launchctl load -w /System/Library/LaunchDaemons/ssh.plist >> "$LOG" 2>&1 || true

# Computer name. The host drops /Library/AppSandbox/computer-name with the
# user's chosen VM name; scutil is the canonical way to set these (wraps
# SCDynamicStoreSetComputerName / SCPreferencesSetLocalHostName). The
# validator constrains input to [A-Za-z0-9-], so all three names can be
# identical and DNS-safe.
NAME_FILE=/Library/AppSandbox/computer-name
if [ -f "$NAME_FILE" ]; then
    NAME=$(tr -d '\r\n' < "$NAME_FILE")
    if [ -n "$NAME" ]; then
        /usr/sbin/scutil --set ComputerName  "$NAME" >> "$LOG" 2>&1 || true
        /usr/sbin/scutil --set HostName      "$NAME" >> "$LOG" 2>&1 || true
        /usr/sbin/scutil --set LocalHostName "$NAME" >> "$LOG" 2>&1 || true
        echo "Set computer name to $NAME" >> "$LOG"
    fi
    rm -f "$NAME_FILE"
fi

# Remove ourselves so we only run once.
rm -f /Library/LaunchDaemons/com.appsandbox.firstboot.plist
launchctl bootout system/com.appsandbox.firstboot >> "$LOG" 2>&1 || true
rm -f /Library/AppSandbox/firstboot.sh

echo "=== firstboot.sh finished $(date) ===" >> "$LOG"
exit 0

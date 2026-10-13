#!/bin/sh
# Run once as root. Publish only the configured RAM transfer rate for monitoring.
set -eu
[ "$(id -u)" = 0 ] || { echo 'Run this installer as root.' >&2; exit 1; }
command -v dmidecode >/dev/null || { echo 'dmidecode is required.' >&2; exit 1; }
helper=/usr/local/libexec/airsftp-memory-rate
unit=/etc/systemd/system/airsftp-memory-rate.service
for path in "$helper" "$unit"; do
    [ ! -e "$path" ] && [ ! -L "$path" ] || { echo "Already exists: $path" >&2; exit 1; }
done
install -d -m 0755 /usr/local/libexec
cat > "$helper" <<'HELPER'
#!/bin/sh
set -eu
export LC_ALL=C
# Read once per service invocation; no firmware scans in the UI polling loop.
dmi=$(/usr/sbin/dmidecode --type 17)
rate=$(printf '%s\n' "$dmi" | awk '
    /^Memory Device$/ { installed=0 }
    /^[ \t]*Size:/ { installed=($2 ~ /^[0-9]+$/ && $2 > 0) }
    installed && /^[ \t]*Configured (Memory|Clock) Speed:/ {
        if ($4 ~ /^[0-9]+$/ && $4 > 0 && $4 <= 100000 && ($5 == "MT/s" || $5 == "MHz")) {
            if (!min || $4 < min) min=$4
        }
    }
    END { printf "%.0f\n", min+0 }
')
tmp=$(mktemp /run/airsftp-telemetry/memory-rate.XXXXXX)
trap 'rm -f -- "$tmp"' EXIT HUP INT TERM
printf '%s\n' "$rate" > "$tmp"
chmod 0644 "$tmp"
mv -f -- "$tmp" /run/airsftp-telemetry/memory-rate-mts
HELPER
chmod 0755 "$helper"
cat > "$unit" <<'UNIT'
[Unit]
Description=AirSFTP configured memory rate cache
After=local-fs.target

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/airsftp-memory-rate
RemainAfterExit=yes
RuntimeDirectory=airsftp-telemetry
RuntimeDirectoryMode=0755
UMask=0022
TimeoutStartSec=15
NoNewPrivileges=yes
ProtectHome=yes
ProtectSystem=strict
ReadWritePaths=/run/airsftp-telemetry

[Install]
WantedBy=multi-user.target
UNIT
chmod 0644 "$unit"
sh -n "$helper"
systemd-analyze verify "$unit"
systemctl daemon-reload
systemctl enable --now airsftp-memory-rate.service
systemctl is-active airsftp-memory-rate.service
printf 'Configured memory rate (MT/s): '
cat /run/airsftp-telemetry/memory-rate-mts

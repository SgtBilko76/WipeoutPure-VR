#!/usr/bin/env bash
# Installs the Wipeout Pure VR ad hoc server on a dedicated Linux machine, as root:
#   - the binary to /usr/local/bin/wipeout-adhoc-server (next to this script, from build.sh)
#   - a system user `wipeout-adhoc` without a shell or home
#   - a systemd service `wipeout-adhoc` that starts at boot and restarts after a crash
#   - TCP 27312 (matchmaking) and 27313 (relay) opened in ufw or firewalld, when one of them is active
#
#   scp out/adhoc-server/wipeout-adhoc-server tools/quest/adhoc_server/install.sh root@SERVER:/tmp/
#   ssh root@SERVER 'bash /tmp/install.sh'
#
# Log: journalctl -u wipeout-adhoc -f       Stop: systemctl stop wipeout-adhoc       Remove: install.sh --remove
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${PORT:-27312}"
RELAY=$((PORT + 1))
[[ $(id -u) -eq 0 ]] || { echo "run as root" >&2; exit 1; }

if [[ "${1:-}" == "--remove" ]]; then
    systemctl disable --now wipeout-adhoc 2>/dev/null || true
    rm -f /etc/systemd/system/wipeout-adhoc.service /usr/local/bin/wipeout-adhoc-server
    systemctl daemon-reload
    userdel wipeout-adhoc 2>/dev/null || true
    echo "removed (firewall rules left as they are)"
    exit 0
fi

install -m 0755 "$HERE/wipeout-adhoc-server" /usr/local/bin/wipeout-adhoc-server
id wipeout-adhoc >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin wipeout-adhoc

cat > /etc/systemd/system/wipeout-adhoc.service <<EOF
[Unit]
Description=Wipeout Pure VR ad hoc server (matchmaking TCP ${PORT}, relay TCP ${RELAY})
After=network-online.target
Wants=network-online.target

[Service]
User=wipeout-adhoc
ExecStart=/usr/local/bin/wipeout-adhoc-server ${PORT}
Restart=always
RestartSec=3
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectKernelTunables=yes
ProtectControlGroups=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_NETLINK
MemoryMax=256M

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now wipeout-adhoc

if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw allow "${PORT}/tcp" comment "Wipeout adhoc matchmaking"
    ufw allow "${RELAY}/tcp" comment "Wipeout adhoc relay"
elif command -v firewall-cmd >/dev/null && firewall-cmd --state >/dev/null 2>&1; then
    firewall-cmd --permanent --add-port="${PORT}/tcp" --add-port="${RELAY}/tcp"
    firewall-cmd --reload
else
    echo "no active ufw/firewalld: open TCP ${PORT} and ${RELAY} in your provider's firewall if it has one"
fi

sleep 1
systemctl --no-pager status wipeout-adhoc | head -5
ss -ltnp | grep -E ":(${PORT}|${RELAY})\b" || echo "warning: not listening yet"

#!/usr/bin/env bash
# Install the dashboard as a systemd service so it survives a reboot and comes
# back on its own if the board is unplugged mid-demo.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USER_NAME="${SUDO_USER:-$USER}"
PORT="${PORT:-8000}"

sudo tee /etc/systemd/system/r5f-dashboard.service >/dev/null <<EOF
[Unit]
Description=R5F FreeRTOS Protocol Simulator dashboard
After=network-online.target

[Service]
Type=simple
User=${USER_NAME}
WorkingDirectory=${ROOT}
ExecStart=/usr/bin/python3 ${ROOT}/server/app.py --port ${PORT}
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

# ModemManager probes every new ttyACM with AT commands and eats the first
# bytes of our telemetry. Tell it the Pico is not a modem.
echo 'ATTRS{idVendor}=="2e8a", ENV{ID_MM_DEVICE_IGNORE}="1"' \
  | sudo tee /etc/udev/rules.d/99-pico-no-modemmanager.rules >/dev/null
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=tty

sudo systemctl daemon-reload
sudo systemctl enable --now r5f-dashboard.service
sleep 1
sudo systemctl --no-pager --lines=5 status r5f-dashboard.service || true

echo
echo "dashboard on port ${PORT}:"
command -v tailscale >/dev/null && tailscale ip -4 2>/dev/null | sed "s|^|    http://|;s|$|:${PORT}|"
hostname -I | tr ' ' '\n' | grep -v '^$' | sed "s|^|    http://|;s|$|:${PORT}|"

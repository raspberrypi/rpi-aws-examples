#!/bin/bash
# Install this repository's device side on Raspberry Pi OS (Trixie). Run from a copy of the repository:
#
#   sudo sbc/install.sh            keygen, the key ladder helper and the hello client
#   sudo sbc/install.sh --no-client  without the client (a Greengrass core)
#   sbc/install.sh --no-apt         packages already installed (the image layers, which list them)
#   sbc/install.sh --wifi-import    also import Wi-Fi settings from the boot partition at boot (images, with iwd)
#
# Creates the service user rpi-ex and /etc/rpi-aws-examples (root:rpi-ex, 0750: rpi-ex reads, only root writes,
# so that rpi-ex cannot plant links where root writes). Safe to run again; fixes an older install's owner.
# Also runs inside an image build (rpi-image-gen), where systemd is not running: it only enables units.
set -euo pipefail

AWSIOTSDK_VERSION=1.31.0
here=$(cd "$(dirname "$0")" && pwd)
client=1 apt=1 wifi_import=0
for arg in "$@"; do
  case "$arg" in
    --no-client) client=0 ;;
    --no-apt) apt=0 ;;
    --wifi-import) wifi_import=1 ;;
    *) echo "usage: install.sh [--no-client] [--no-apt] [--wifi-import]" >&2; exit 2 ;;
  esac
done
[ "$(id -u)" -eq 0 ] || { echo "error: run with sudo" >&2; exit 1; }

export DEBIAN_FRONTEND=noninteractive
pkgs=(openssl python3-cryptography rpifwcrypto)
[ "$client" -eq 1 ] && pkgs+=(python3-venv)
[ "$apt" -eq 0 ] || apt-get install -y -q "${pkgs[@]}" >/dev/null

id rpi-ex >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin rpi-ex
install -d -m 750 -o root -g rpi-ex /etc/rpi-aws-examples
chown -h root:rpi-ex /etc/rpi-aws-examples/* 2>/dev/null || true
install -m 755 "$here/keygen/rpi-aws-keygen" /usr/local/bin/rpi-aws-keygen
install -m 755 "$here/keyladder/rpi-aws-keyhelper.py" /usr/local/sbin/rpi-aws-keyhelper
install -m 755 "$here/keyladder/rpi-aws-otp-init" /usr/local/sbin/rpi-aws-otp-init
install -m 755 "$here/greengrass/rpi-aws-gg-setup" /usr/local/sbin/rpi-aws-gg-setup
install -m 644 "$here/systemd/rpi-aws-keyunwrap.service" /etc/systemd/system/
systemctl enable rpi-aws-keyunwrap.service
if [ "$wifi_import" -eq 1 ]; then
  install -m 755 "$here/wifi/rpi-aws-wifi-import" /usr/local/sbin/rpi-aws-wifi-import
  install -m 644 "$here/systemd/rpi-aws-wifi-import.service" /etc/systemd/system/
  systemctl enable rpi-aws-wifi-import.service
fi

if [ "$client" -eq 1 ]; then
  [ -x /opt/rpi-aws-examples/venv/bin/python3 ] || python3 -m venv /opt/rpi-aws-examples/venv
  /opt/rpi-aws-examples/venv/bin/pip install -q "awsiotsdk==$AWSIOTSDK_VERSION"
  install -m 755 "$here/client/rpi-aws-hello.py" /usr/local/bin/rpi-aws-hello
  install -m 644 "$here/systemd/rpi-aws-hello.service" /etc/systemd/system/
fi
if [ -d /run/systemd/system ]; then systemctl daemon-reload; fi
echo "Installed. Next: sudo rpi-aws-otp-init, then sudo rpi-aws-keygen --store wrapped > device.csr" >&2

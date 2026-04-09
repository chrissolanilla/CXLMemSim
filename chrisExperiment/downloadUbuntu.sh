#!/usr/bin/env bash
set -euo pipefail

IMG="ubuntu-24.04-server-cloudimg-amd64.img"

if [ ! -f "$IMG" ]; then
    echo "downloading ubuntu 24.04 cloud image..."
    wget -O "$IMG" \
      https://cloud-images.ubuntu.com/releases/noble/release/ubuntu-24.04-server-cloudimg-amd64.img
fi

echo "done"

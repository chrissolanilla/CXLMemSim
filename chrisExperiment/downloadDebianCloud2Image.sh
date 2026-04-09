#!/usr/bin/env bash
set -euo pipefail

IMG="debian-nocloud.qcow2"

if [ ! -f "$IMG" ]; then
    echo "downloading debian nocloud image..."
    wget -O "$IMG" \
    https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-nocloud-amd64.qcow2
fi

echo "done"

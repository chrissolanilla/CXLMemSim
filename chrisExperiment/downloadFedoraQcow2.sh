#!/usr/bin/env bash
set -euo pipefail

IMG="fedora-basic.qcow2"

if [ ! -f "$IMG" ]; then
    echo "downloading fedora basic qcow2 image..."
    wget -O "$IMG" \
	  https://download.fedoraproject.org/pub/fedora/linux/releases/43/Cloud/x86_64/images/Fedora-Cloud-Base-Generic-43-1.6.x86_64.qcow2
fi

echo "done"

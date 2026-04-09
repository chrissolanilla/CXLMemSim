#!/usr/bin/env bash
set -euo pipefail

IMG="debian-nocloud.qcow2"

qemu-system-x86_64 \
  -enable-kvm \
  -cpu host \
  -machine q35 \
  -smp 4 \
  -m 4G \
  -drive file="$IMG",if=virtio,format=qcow2 \
  -nographic

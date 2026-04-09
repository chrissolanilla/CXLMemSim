#!/usr/bin/env bash
set -euo pipefail

IMG="ubuntu-24.04-server-cloudimg-amd64.img"

qemu-system-x86_64 \
  -enable-kvm \
  -cpu host \
  -machine q35 \
  -smp 4 \
  -m 4G \
  -drive file="$IMG",if=virtio,format=qcow2 \
  -display gtk

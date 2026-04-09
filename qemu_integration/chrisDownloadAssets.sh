#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if [ ! -f qemu.img ]; then
  echo "downloading qemu.img..."
  wget https://asplos.dev/about/qemu.img
fi

if [ ! -f bzImage ]; then
  echo "downloading bzImage..."
  wget https://asplos.dev/about/bzImage
fi

echo "done"

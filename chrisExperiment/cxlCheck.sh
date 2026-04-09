#!/usr/bin/env bash
set -euo pipefail

echo "== kernel =="
uname -r

echo
echo "== cxl devices =="
ls /sys/bus/cxl/devices || true

echo
echo "== cxl list =="
cxl list || true

echo
echo "== cxl detailed =="
cxl list -M -D -E -R || true

echo
echo "== daxctl list =="
daxctl list || true

echo
echo "== ndctl list =="
ndctl list || true

echo
echo "== memory view =="
lsmem || true
free -h || true

echo
echo "== dmesg cxl =="
dmesg | grep -i cxl || true

# QEMU Integration Notes

## What I changed
The original launcher in this repo assumed:
- QEMU installed at `/usr/local/bin/qemu-system-x86_64`
- TAP networking
- very large guest RAM (`160G`)
- additional GPU / accel setup

For local testing on a normal machine, I changed the launcher to:
- use the locally built QEMU binary from `../lib/qemu/build/qemu-system-x86_64`
- use `./bzImage`
- reduce RAM to `8G`
- remove TAP networking and GPU passthrough
- keep the CXL Type 3 and shared-memory parts

## Build QEMU
From repo root:

```bash
git submodule update --init --recursive
cd lib/qemu
rm -rf build
mkdir build
cd build
../configure --target-list=x86_64-softmmu
ninja -j$(nproc)
Note: I hit compiler warnings treated as errors with my toolchain and had to patch a couple of const issues in the QEMU tree.c

## Download Required assets
from qemu_integration you can run the script ./chrisDownloadAssets.sh
that downloads the qemu.img and the bzImage

## Launching
from qemu_integration you can run ./launch_qemu_cxl.sh
Note: you may want to use virt-customize command to change the root password. I did virt-customize -a qemu.img --root-password password:root to change it to root

## Testing if cxl lowkey works
once you got your qemu running, then you can do these commands
```
uname -r
cxl list
cxl list -M -D -E -R
ndctl list
lsmem
free -h
dmesg | grep -i cxl
```
basically checks kernel, cxl commands, memory, ect.



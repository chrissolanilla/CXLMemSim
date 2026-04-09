# #!/usr/bin/env bash
# set -euo pipefail
#
# IMG="ubuntu-24.04-server-cloudimg-amd64.img"
#
# rm -f /tmp/lsa0.raw
# truncate -s 256M /tmp/lsa0.raw
#
# qemu-system-x86_64 \
#   -enable-kvm \
#   -cpu host \
#   -M q35,cxl=on \
#   -smp 4 \
#   -m 4G,maxmem=8G,slots=8 \
#   -drive file="$IMG",format=qcow2 \
#   -object memory-backend-ram,id=vmem0,share=on,size=256M \
#   -object memory-backend-file,id=cxl-lsa0,share=on,mem-path=/tmp/lsa0.raw,size=256M \
#   -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
#   -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=2 \
#   -device cxl-type3,bus=root_port13,volatile-memdev=vmem0,lsa=cxl-lsa0,id=cxl-vmem0 \
#   -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=4G \
#   -nographic

#!/usr/bin/env bash
set -euo pipefail

IMG="ubuntu-24.04-server-cloudimg-amd64.img"

truncate -s 256M /tmp/cxl-pmem0.raw
truncate -s 256M /tmp/lsa0.raw

qemu-system-x86_64 \
  -enable-kvm \
  -cpu host \
  -M q35,cxl=on \
  -smp 4 \
  -m 4G,maxmem=8G,slots=8 \
  -drive file="$IMG",format=qcow2 \
  -object memory-backend-file,id=cxl-mem0,share=on,mem-path=/tmp/cxl-pmem0.raw,size=256M \
  -object memory-backend-file,id=cxl-lsa0,share=on,mem-path=/tmp/lsa0.raw,size=256M \
  -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
  -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=2 \
  -device cxl-type3,bus=root_port13,persistent-memdev=cxl-mem0,lsa=cxl-lsa0,id=cxl-pmem0,sn=0x1 \
  -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=4G \
  -nographic

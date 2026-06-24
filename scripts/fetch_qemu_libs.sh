#!/bin/bash
set -e
cd /home/z/qemu-pkgs/libs
fetch() {
    local pkgdir="$1"; local pattern="$2"; local outname="$3"
    local listing=$(wget -q -O - "https://deb.debian.org/debian/pool/main/${pkgdir:0:1}/$pkgdir/")
    local url=$(echo "$listing" | grep -oE "$pattern" | tail -1)
    if [ -z "$url" ]; then echo "FAIL: $outname"; return 1; fi
    wget -q --timeout=60 "https://deb.debian.org/debian/pool/main/${pkgdir:0:1}/$pkgdir/$url" -O "$outname.deb"
    echo "OK: $outname.deb"
}
fetch pmdk 'libpmem1_[^"]*_amd64\.deb' libpmem1
fetch rdma-core 'librdmacm1_[^"]*_amd64\.deb' librdmacm1
fetch rdma-core 'libibverbs1_[^"]*_amd64\.deb' libibverbs1
fetch libslirp 'libslirp0_[^"]*_amd64\.deb' libslirp0
fetch vde2 'libvdeplug2_[^"]*_amd64\.deb' libvdeplug2
fetch liburing 'liburing2_[^"]*_amd64\.deb' liburing2
fetch fuse3 'libfuse3-3_[^"]*_amd64\.deb' libfuse3-3
ls -la *.deb

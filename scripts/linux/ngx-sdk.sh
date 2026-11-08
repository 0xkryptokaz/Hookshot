#!/bin/bash
# ngx-sdk.sh: NVIDIA's DLSS SDK files for a local build with NVIDIA DLSS Super Resolution (HOOKSHOT_NGX_SDK, gpu/vk/vk_dlss.cpp).
#
# NVIDIA's files are not part of Hookshot and are never committed: they are under NVIDIA's own license (the DLSS SDK's
# LICENSE.txt, fetched beside them; read it before using them). This fetches, from NVIDIA's DLSS repository at one pinned
# release, each file checked against its SHA-256:
#   - the SDK headers and libnvsdk_ngx.a, the build's part, into <sdk folder> (default build-linux/ngx-sdk)
#   - libnvidia-ngx-dlss.so, the DLSS runtime the driver loads, into <runtime folder> when given (a game folder's ngx/, or
#     $XDG_DATA_HOME/hookshot/ngx; RX_VK_DLSS_DIR names another)
# The NGX core itself (libnvidia-ngx.so) comes with NVIDIA's Linux driver.
#
# Usage: scripts/linux/ngx-sdk.sh [<sdk folder>] [<runtime folder>]
# Then build with HOOKSHOT_NGX_SDK=<sdk folder>.
set -euo pipefail
COMMIT=9981c6350dc58c0ce21f024fd65e31e851c31689   # (v310.9.16)
BASE=https://raw.githubusercontent.com/NVIDIA/DLSS/$COMMIT
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${1:-$ROOT/build-linux/ngx-sdk}
RUNTIME=${2:-}
FILES="
    LICENSE.txt d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0
    lib/Linux_x86_64/libnvsdk_ngx.a d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df
    include/nvsdk_ngx_defs_dlssd.h 2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716
    include/nvsdk_ngx_defs_dlssg.h c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1
    include/nvsdk_ngx_defs.h ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e
    include/nvsdk_ngx_defs_vk.h 0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031
    include/nvsdk_ngx.h dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848
    include/nvsdk_ngx_helpers_cuda.h f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042
    include/nvsdk_ngx_helpers_d3d.h ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba
    include/nvsdk_ngx_helpers_dlssd_cuda.h 7e35cde214d905346a487b9a28c05d35e1c55b480e237fe3de69580a7e332730
    include/nvsdk_ngx_helpers_dlssd_d3d.h 6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293
    include/nvsdk_ngx_helpers_dlssd.h a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532
    include/nvsdk_ngx_helpers_dlssd_vk.h 11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b
    include/nvsdk_ngx_helpers_dlssg_d3d.h 6335ebcb95de39a361e87fe0257d6ce0723e5d8deaa15db3b35afacf2b3f4220
    include/nvsdk_ngx_helpers_dlssg.h 6881d21419c40c78ec948b326902ece09fc112b0223144d51c4905f3fced7a1f
    include/nvsdk_ngx_helpers_dlssg_vk.h b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316
    include/nvsdk_ngx_helpers.h 5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471
    include/nvsdk_ngx_helpers_vk.h c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623
    include/nvsdk_ngx_loader.h abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75
    include/nvsdk_ngx_params_dlssd.h d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e
    include/nvsdk_ngx_params_dlssg.h c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad
    include/nvsdk_ngx_params.h 943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc
    include/nvsdk_ngx_standalone_common.h 38413c3338b0aea0202c07da95d426bfee7080a42913823fa2d9dd585b087b53
    include/nvsdk_ngx_standalone_cuda.h b23436ed5040051ac8e81554c9af080f2463c941435bbc3b82222e5431a09499
    include/nvsdk_ngx_vk.h 2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d
"
RUNTIME_FILE="lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16 30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6"

fetch() {   # (path in the repository, its SHA-256, where it goes)
    local tmp="$3.part"
    mkdir -p "$(dirname "$3")"
    if [ -f "$3" ] && echo "$2  $3" | sha256sum -c --status; then return; fi
    curl -fsSL -o "$tmp" "$BASE/$1"
    if ! echo "$2  $tmp" | sha256sum -c --status; then
        rm -f "$tmp"; echo "ngx-sdk: $1 is not the file expected (SHA-256)"; exit 1
    fi
    mv "$tmp" "$3"
}
echo "$FILES" | while read -r path sum; do
    [ -n "$path" ] || continue
    case "$path" in lib/*) dst=$SDK/lib/$(basename "$path") ;; *) dst=$SDK/$path ;; esac
    fetch "$path" "$sum" "$dst"
done
echo "ngx-sdk: the SDK's headers and library in $SDK (NVIDIA's license: $SDK/LICENSE.txt)"
if [ -n "$RUNTIME" ]; then
    set -- $RUNTIME_FILE
    fetch "$1" "$2" "$RUNTIME/$(basename "$1")"
    echo "ngx-sdk: the DLSS runtime in $RUNTIME"
fi

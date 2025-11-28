#! /bin/bash


set -o pipefail
set -o errexit
set -o nounset
set -o errtrace

opts=""
opts="$opts --device /dev/dri "

what=${1:-}
if [[ -z "$what" ]]; then
    what=remoting
fi

cmd="bash ./build.$what.sh"

if [[ $(arch) == x86_64 ]]; then
    exec $cmd
fi
IMAGE=quay.io/crcont/remoting:v0.12.0-apir.rc3_apir.b5709-remoting-0.1.6_b2

POD_NAME=mac_ai_compiling
#podman machine ssh podman rm $POD_NAME --force

set -x
podman run \
--name $POD_NAME \
--user root:root \
--cgroupns host \
--security-opt label=disable \
--env HOME="$HOME" \
--env PERF_MODE="${PERF_MODE:-}" \
--env BENCH_MODE="${BENCH_MODE:-}" \
-v "$HOME":"$HOME" \
-w "$PWD" \
-it --rm \
$opts \
$IMAGE \
$cmd

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

SSH_PORT=$(podman machine inspect | jq  .[0].SSHConfig.Port)
SECONDS=0

echo "Waiting for the machine to be available ..."

while ! (ssh core@127.0.0.1 -p$SSH_PORT 2>&1 || true) | grep "Permission denied"; do
    had_to_wait=1
    sleep 1
done
while ! podman ps >/dev/null 2>/dev/null; do
    echo "Waiting for podman to be available ..."
    had_to_wait=1
    sleep 1
done

echo "Ready after ${SECONDS}s"

POD_NAME=mac_ai_compiling

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

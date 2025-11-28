# force isatty-->true, so that $0 |& head -50 has colors ...
rm -f READY FAILED

echo "int isatty(int fd) { return 1; }" | gcc -O2 -fpic -shared -ldl -o /tmp/isatty.so -xc -
export LD_PRELOAD=/tmp/isatty.so

TARGETS="ggml-remotingfrontend"

TARGETS="$BUILD_TARGET llama-run"
set -x
if [[ "${BENCH_MODE:-}" == "bench" ]]; then
    TARGETS="$TARGETS llama-bench"
elif [[ "${BENCH_MODE:-}" == "server" ]]; then
    TARGETS="$TARGETS llama-server"
elif [[ "${BENCH_MODE:-}" == "perf" ]]; then
    TARGETS="$TARGETS test-backend-ops"
fi

cmake --build ../build.remoting-frontend$FLAVOR --parallel 8 --target $TARGETS "$@"

if [[ $? == 0 ]]; then
    touch READY
else
    touch FAILED
fi

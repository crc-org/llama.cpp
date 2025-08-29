#! /bin/bash
if [[ ${1:-} == "strace" ]]; then
    prefix="strace"
elif [[ ${1:-} == "gdb" ]]; then
    prefix="gdb -ix .gdbinit --args"
elif [[ ${1:-} == "gdbr" ]]; then
    prefix="gdb -ex='set confirm on' -ex=run -ex=quit --args"
else
    prefix=""
fi

MODEL_HOME="$HOME/models"
MODEL=llama3.2

export GGML_DISABLE_VULKAN=1
export LD_LIBRARY_PATH=$PWD/../build.remoting-frontend/bin

LLAMA_BUILD_DIR=../build.remoting-frontend/
set -x
if [[ "${BENCH_MODE:-}" == "server" ]]; then
    cat <<EOF
###
### Running llama-server
###

EOF
    $prefix \
        $LLAMA_BUILD_DIR/bin/llama-server \
        --host 0.0.0.0 \
        --port 8080 \
        --model "$MODEL_HOME/$MODEL" \
        --n-gpu-layers 99 \
        --threads 1
elif [[ "${BENCH_MODE:-}" == "bench" ]]; then
    cat <<EOF
###
### Running llama-bench
###

EOF
    $prefix \
        $LLAMA_BUILD_DIR/bin/llama-bench \
        --model "$MODEL_HOME/$MODEL" \
        --n-gpu-layers 99
elif [[ "${BENCH_MODE:-}" == "perf" ]]; then
    cat <<EOF
###
### Running test-backend-ops perf
###

EOF
    $prefix \
        $LLAMA_BUILD_DIR/bin/test-backend-ops perf

else
    PROMPT="say nothing"
    PROMPT="tell what's GGML API"
    $prefix \
        $LLAMA_BUILD_DIR/bin/llama-run \
        --ngl 99 \
        --verbose \
        --context-size 4096 \
        "$MODEL_HOME/$MODEL" \
        "$PROMPT"
fi

#! /bin/bash
if [[ ${1:-} == "strace" ]]; then
    prefix="strace"
elif [[ ${1:-} == "gdb" ]]; then
    prefix="gdb --args"
elif [[ ${1:-} == "gdbr" ]]; then
    prefix="gdb -ex='set confirm on' -ex=run -ex=quit --args"
else
    prefix=""
fi

MODEL_HOME="$HOME/models"
export LD_LIBRARY_PATH=$PWD/../build.vulkan-linux/bin

$prefix ../build.vulkan-linux/bin/llama-run --verbose "$MODEL_HOME/llama3.2" "say nothing" --ngl 99

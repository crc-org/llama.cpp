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

BUILD_DIR_NAME=build.remoting-frontend

MODEL_HOME="$HOME/models"
export LD_LIBRARY_PATH=$PWD/../$BUILD_DIR_NAME/bin

#PROMPT="say nothing"
PROMPT="what it the vulkan API?"
$prefix ../$BUILD_DIR_NAME/bin/llama-run --verbose "$MODEL_HOME/smollm:135m" "$PROMPT" --ngl 99

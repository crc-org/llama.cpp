rm -f READY FAILED

cmake --build ../build.vulkan --parallel 8 --target llama-run

if [[ $? == 0 ]]; then
    touch READY
else
    touch FAILED
    exit 1
fi

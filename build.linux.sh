rm -f READY FAILED

cmake --build ../build.vulkan-linux --parallel 8 --target llama-run llama-server

if [[ $? == 0 ]]; then
    touch READY
else
    touch FAILED
fi

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

#rm -f /usr/lib64/libvulkan_virtio.so

ICD_DIR=/Users/kevinpouget/.local/share/vulkan/icd.d

MESA_FLAVOR=good
if [[ "$MESA_FLAVOR" == "work" ]]; then
    export VK_ICD_FILENAMES=$ICD_DIR/virtio_icd.aarch64.json
elif [[ "$MESA_FLAVOR" == "good" ]]; then
    export VK_ICD_FILENAMES=$ICD_DIR/virtio_icd.good.aarch64.json
elif [[ "$MESA_FLAVOR" == "cont" ]]; then
    export VK_ICD_FILENAMES=$ICD_DIR/virtio_icd.cont.aarch64.json
else
    echo "ERROR: invalid MESA_FLAVOR=$MESA_FLAVOR"
    exit 1
fi

# init result vtest wsi no_abort log_ctx_info cache no_sparse no_gpl
export VN_DEBUG=vtest
$prefix ../build.vulkan/bin/llama-run --verbose ~/models/llama3.2 "say nothing" --ngl 99

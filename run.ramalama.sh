ICD_DIR=/Users/kevinpouget/.local/share/vulkan/icd.d
export VK_ICD_FILENAMES=$ICD_DIR/virtio_icd.cont.aarch64.json

llama-run ~/models/llama3.2 "say nothing" --ngl 99

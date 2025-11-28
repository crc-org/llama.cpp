if [[ "${PERF_MODE:-}" ]]; then
    FLAVOR="-prod"
else
    FLAVOR=""
fi

cmake -S . -B ../build.remoting-backend$FLAVOR \
      -DGGML_REMOTINGBACKEND=ON \
      -DGGML_NATIVE=OFF \
      -DGGML_METAL=OFF \
      -DGGML_BACKEND_DL=OFF \
      -DLLAMA_CURL=OFF \
      -DGGML_VULKAN=ON \
      "$@"


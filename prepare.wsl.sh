cmake -S . -B build.windows-wsl \
      -DGGML_VIRTGPU=ON -DGGML_VIRTGPU_BACKEND=OFF -DGGML_VIRTGPU_USE_WINDOWS=ON \
      -DGGML_CPU_ARM_ARCH=native \
      -DGGML_NATIVE=OFF \
      -DGGML_OPENMP=OFF \
      -DLLAMA_CURL=OFF \
      -DCMAKE_BUILD_TYPE=Debug \
      "$@"

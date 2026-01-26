cmake -S . -B build.windows-host `
      -DGGML_VIRTGPU=ON -DGGML_VIRTGPU_BACKEND=ONLY -DGGML_VIRTGPU_USE_WINDOWS=ON `
      -DGGML_CPU_ARM_ARCH=native `
      -DGGML_NATIVE=OFF `
      -DGGML_OPENMP=OFF `
      -DLLAMA_CURL=OFF `
      -DGGML_BACKEND_DL=ON `
      -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_CXX_FLAGS="/wd4267 /wd4244 /wd4996" `
      $args

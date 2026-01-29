$env:APIR_LLAMA_CPP_GGML_LIBRARY_PATH = ".\build.windows-host\bin\Debug\ggml-cpu.dll"
$env:APIR_LLAMA_CPP_GGML_LIBRARY_REG = "ggml_backend_cpu_reg"
$env:APIR_LLAMA_CPP_GGML_LIBRARY_INIT = "ggml_backend_cpu_init"

.\build.windows-host\bin\Debug\VirtGPUWindowsBackend.exe console

#!/bin/bash

# Build script for testing Windows support in ggml-virtgpu

echo "Building ggml-virtgpu with Windows support..."

# Ensure we're in the right directory
cd "$(dirname "$0")"

# Create build directory
mkdir -p build-windows
cd build-windows

# Configure cmake with Windows support
echo "Configuring cmake with Windows support..."
cmake .. \
    -DGGML_VIRTGPU_USE_WINDOWS=ON \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_VERBOSE_MAKEFILE=ON

if [ $? -ne 0 ]; then
    echo "ERROR: CMake configuration failed"
    echo ""
    echo "Make sure you have:"
    echo "1. json-c development library installed (libjson-c-dev)"
    echo "2. Standard build tools (gcc, make)"
    echo "3. CMake 3.19 or later"
    echo ""
    echo "Install json-c on Ubuntu/Debian:"
    echo "sudo apt-get install libjson-c-dev"
    exit 1
fi

# Build
echo "Building with Windows backend..."
make -j$(nproc)

if [ $? -eq 0 ]; then
    echo "SUCCESS: ggml-virtgpu built with standalone Windows client"
    echo ""
    echo "Build configuration:"
    echo "  - GGML_VIRTGPU_USE_WINDOWS=ON"
    echo "  - Transport: TCP socket"
    echo "  - Shared memory: File-backed (/mnt/c/)"
    echo "  - Protocol: APIR over JSON"
    echo "  - Dependencies: json-c only"
    echo ""
    echo "To test:"
    echo "1. Start compatible Windows service on host (port 4660)"
    echo "2. Ensure /mnt/c/temp/ is accessible for shared memory"
    echo "3. Run: export GGML_BACKEND_DEVICE=virtgpu"
    echo "4. Test with GGML applications"
else
    echo "ERROR: Build failed"
    echo ""
    echo "Check the build output above for specific errors"
    echo "Common issues:"
    echo "1. Missing winApiRmt client library"
    echo "2. json-c not found"
    echo "3. Missing include paths"
fi

echo ""
echo "To build with Linux DRM support instead:"
echo "cmake .. -DGGML_VIRTGPU_USE_WINDOWS=OFF"
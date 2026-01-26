#!/bin/bash

# Build script for winApiRmt + ggml-virtgpu integration test

echo "Building winApiRmt Integration Test..."

# Ensure we're in the right directory
cd "$(dirname "$0")"

# Check if winApiRmt build exists
if [ ! -d "winApiRmt" ]; then
    echo "ERROR: winApiRmt directory not found"
    echo "Please ensure winApiRmt POC is present in this directory"
    exit 1
fi

# Build winApiRmt client library first if needed
if [ ! -f "winApiRmt/guest/client/libwinapi.so" ] && [ ! -f "winApiRmt/guest/client/libwinapi.a" ]; then
    echo "Building winApiRmt client library..."
    cd winApiRmt
    if [ -f "build.sh" ]; then
        ./build.sh
    else
        echo "WARNING: No build script found for winApiRmt, attempting manual build..."
        cd guest/client
        gcc -shared -fPIC -o libwinapi.so *.c -ljson-c
        cd ../..
    fi
    cd ..
fi

# Build the integration test
echo "Compiling integration test..."

# Compile flags
CFLAGS="-std=c++17 -Wall -Wextra -g -O0"
INCLUDES="-I. -IwinApiRmt -IwinApiRmt/guest/client -IwinApiRmt/common"
LIBS="-ljson-c -lpthread"

# Try to link with winApiRmt client if available
WINAPI_LIB=""
if [ -f "winApiRmt/guest/client/libwinapi.a" ]; then
    WINAPI_LIB="winApiRmt/guest/client/libwinapi.a"
elif [ -f "winApiRmt/guest/client/libwinapi.so" ]; then
    WINAPI_LIB="-LwinApiRmt/guest/client -lwinapi"
else
    echo "WARNING: No compiled winApiRmt library found"
    echo "Attempting to compile client sources directly..."

    # Compile winApiRmt sources directly
    gcc -c winApiRmt/guest/client/*.c $INCLUDES $CFLAGS
    WINAPI_OBJ="*.o"
    WINAPI_LIB="$WINAPI_OBJ"
fi

# Compile the test
echo "Compiling test-winapi-integration.cpp..."
g++ $CFLAGS $INCLUDES -o test-winapi-integration test-winapi-integration.cpp $WINAPI_LIB $LIBS

if [ $? -eq 0 ]; then
    echo "SUCCESS: Integration test compiled successfully"
    echo "Run with: ./test-winapi-integration"
    echo ""
    echo "Note: This test requires:"
    echo "1. winApiRmt Windows service running on the host"
    echo "2. Hyper-V socket or TCP connectivity to Windows host"
    echo "3. Access to /mnt/c/ for shared memory files"
else
    echo "ERROR: Compilation failed"
    echo ""
    echo "Make sure you have:"
    echo "1. json-c development library installed (libjson-c-dev)"
    echo "2. winApiRmt client code available"
    echo "3. Proper include paths"
    exit 1
fi

echo "Build complete!"
echo ""
echo "Next steps:"
echo "1. Ensure winApiRmt Windows service is running"
echo "2. Run: ./test-winapi-integration"
echo "3. If test passes, we can proceed with full ggml-virtgpu integration"
cmake_minimum_required(VERSION 3.19)
cmake_policy(SET CMP0114 NEW)

message(STATUS "Enable the VirtGPU/Virglrenderer backend library")
message(STATUS "Backend architecture supports both Linux (VirGL) and Windows (standalone service)")

# Core backend library (used by both Linux VirGL and Windows service)

ggml_add_backend_library(ggml-virtgpu-backend STATIC
                         backend.cpp
                         backend-dispatched.cpp
                         backend-dispatched-backend.cpp
                         backend-dispatched-device.cpp
                         backend-dispatched-buffer.cpp
                         backend-dispatched-buffer-type.cpp
                         shared/api_remoting.h
                         shared/apir_backend.h
                         shared/apir_cs.h
                         apir_cs_ggml-rpc-back.cpp)

# Fix Windows library output paths to match where main project expects them
if(WIN32)
    # Main project looks for: ggml-virtgpu\backend\Debug\ggml-virtgpu-backend.lib
    # So we need to put it in: ${CMAKE_BINARY_DIR}/ggml/src/ggml-virtgpu/backend/Debug/
    set_target_properties(ggml-virtgpu-backend PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ggml/src/ggml-virtgpu/backend/Debug"
        ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/ggml/src/ggml-virtgpu/backend/Debug"
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/ggml/src/ggml-virtgpu/backend/Release"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
    )
endif()

# Set C++20 standard (cross-platform compatible)
set_target_properties(ggml-virtgpu-backend PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Disable common MSVC warnings
if(MSVC)
    target_compile_options(ggml-virtgpu-backend PRIVATE
        /wd4267  # size_t to int conversion
        /wd4244  # type conversion with possible loss of data
        /wd4996  # deprecated functions
    )
endif()

# Add include directory for ggml-backend-impl.h and other core headers
target_include_directories(ggml-virtgpu-backend PRIVATE ../..)

# Windows Backend Service (optional, Windows only)
if(WIN32)
    message(STATUS "Including VirtGPU Windows Backend Service")
    add_subdirectory(windows-service)
endif()

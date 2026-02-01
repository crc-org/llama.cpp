cmake_minimum_required(VERSION 3.19)
cmake_policy(SET CMP0114 NEW)

include(ExternalProject)

# Add Windows support option
option(GGML_VIRTGPU_USE_WINDOWS "Use Windows winApiRmt transport instead of Linux DRM" OFF)

message(STATUS "Including the VirtGPU/Virglrenderer API Remoting")
if (GGML_VIRTGPU_USE_WINDOWS)
    message(STATUS "Building with Windows winApiRmt transport")
else()
    message(STATUS "Building with Linux DRM transport")
endif()

# Download venus_hw.h from virglrenderer repository
ExternalProject_Add(
    venus_hw_header
    URL https://gitlab.freedesktop.org/virgl/virglrenderer/-/raw/virglrenderer-1.2.0/src/venus_hw.h
    DOWNLOAD_NO_EXTRACT YES
    DOWNLOAD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    DOWNLOAD_NAME venus_hw.h
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    LOG_DOWNLOAD ON
)

if (NOT GGML_VIRTGPU_BACKEND STREQUAL "ONLY")
    message(STATUS "Enable the VirtGPU/Virglrenderer API Remoting frontend library")

    # Platform-specific dependencies
    if (GGML_VIRTGPU_USE_WINDOWS)
        # Windows winApiRmt dependencies
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(JSON_C REQUIRED json-c)
    else()
        # Linux DRM dependencies
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(DRM REQUIRED libdrm)
    endif()

    if (NOT GGML_BACKEND_DL)
      # cannot simply use USE_VIRTGPU, as in the 'else()' case the
      # frontend isn't compiled
      target_compile_definitions(ggml PUBLIC "GGML_USE_VIRTGPU_FRONTEND")
    endif()

    # Platform-specific source files
    if (GGML_VIRTGPU_USE_WINDOWS)
        set(VIRTGPU_SOURCES
            # Windows winApiRmt implementation with existing ggml backend
            ggml-backend-buffer.cpp
            ggml-backend.cpp
            ggml-backend-device.cpp
            ggml-backend-reg.cpp
            ggml-backend-buffer-type.cpp
            virtgpu-interface.h
            virtgpu-common.cpp
            # Add missing virtgpu-forward files needed for APIR functions
            virtgpu-forward-device.cpp
            virtgpu-forward-buffer-type.cpp
            virtgpu-forward-buffer.cpp
            virtgpu-forward-backend.cpp
            # Windows cache coherency implementation
            virtgpu-shm.cpp
            # Windows transport implementation (compiled as C++)
            winApiRmt.c
            winApiRmt.h
            ggml-winapi-client.c
            apir-minimal.h
            apir_cs_ggml-rpc-front.cpp
            ../../include/ggml-virtgpu.h
        )
    else()
        set(VIRTGPU_SOURCES
            # Linux DRM implementation
            ggml-backend-buffer.cpp
            ggml-backend.cpp
            ggml-backend-device.cpp
            ggml-backend-reg.cpp
            ggml-backend-buffer-type.cpp
            virtgpu-apir.h
            virtgpu-forward.gen.h
            virtgpu-interface.h
            virtgpu-common.cpp
            apir-minimal.h
            apir_cs_ggml-rpc-front.cpp
            virtgpu.cpp
            virtgpu-utils.cpp
            virtgpu-forward-device.cpp
            virtgpu-forward-buffer-type.cpp
            virtgpu-forward-buffer.cpp
            virtgpu-forward-backend.cpp
            virtgpu-forward-impl.h
            virtgpu-shm.cpp
            virtgpu-linux-backend.c
            ../../include/ggml-virtgpu.h
        )
    endif()

    ggml_add_backend_library(ggml-virtgpu ${VIRTGPU_SOURCES})

    # Force frontend C files to be compiled as C++ when using Windows
    if (GGML_VIRTGPU_USE_WINDOWS)
        set_source_files_properties(
            winApiRmt.c
            ggml-winapi-client.c
            PROPERTIES LANGUAGE CXX
        )
    endif()

    # Platform-specific include directories and linking
    if (GGML_VIRTGPU_USE_WINDOWS)
        # Windows standalone client configuration
        target_compile_definitions(ggml-virtgpu PRIVATE "GGML_VIRTGPU_USE_WINDOWS")

        # Link with json-c for protocol communication
        target_link_libraries(ggml-virtgpu PUBLIC ${JSON_C_LIBRARIES})
        target_include_directories(ggml-virtgpu PUBLIC ${JSON_C_INCLUDE_DIRS})
        target_compile_options(ggml-virtgpu PUBLIC ${JSON_C_CFLAGS_OTHER})

        message(STATUS "Windows standalone client: using json-c for protocol")
    else()
        # Linux DRM configuration (original)
        target_include_directories(ggml-virtgpu PUBLIC /usr/include/libdrm/)
        target_link_libraries(ggml-virtgpu PUBLIC ${DRM_LIBRARIES})
        target_include_directories(ggml-virtgpu PUBLIC ${DRM_INCLUDE_DIRS})
        target_compile_options(ggml-virtgpu PUBLIC ${DRM_CFLAGS_OTHER})
    endif()

    target_include_directories(ggml-virtgpu PUBLIC ./include)
    target_include_directories(ggml-virtgpu PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

    # Ensure venus_hw.h is downloaded before building ggml-virtgpu
    add_dependencies(ggml-virtgpu venus_hw_header)

    # Set C++20 standard for C++ files only (cross-platform compatible)
    set_target_properties(ggml-virtgpu PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    # Configure MSVC compiler options
    if(MSVC)
        target_compile_options(ggml-virtgpu PRIVATE
            /EHsc    # Enable C++ exception handling with standard semantics
            /wd4267  # size_t to int conversion
            /wd4244  # type conversion with possible loss of data
            /wd4996  # deprecated functions
        )
    endif()
else()
    message(STATUS "Not building the VirtGPU/Virglrenderer API Remoting frontend library")
endif()

if (NOT GGML_VIRTGPU_BACKEND STREQUAL "OFF")
    add_subdirectory("backend")
endif()

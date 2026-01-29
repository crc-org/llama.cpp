# CMakeLists.txt for VirtGPU Windows Backend Service
cmake_minimum_required(VERSION 3.16)

project(VirtGPUWindowsBackend
    VERSION 1.0.0
    DESCRIPTION "VirtGPU Windows Backend Service"
    LANGUAGES CXX
)

# Require C++20 (needed for designated initializers)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific settings
if(WIN32)
    # Define Windows version requirements
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-DUNICODE -D_UNICODE)
    add_definitions(-D_WIN32_WINNT=0x0A00)  # Windows 10

    # Find required packages
    find_package(PkgConfig QUIET)

    # Try to find jsoncpp using vcpkg first
    find_path(JSONCPP_INCLUDE_DIR
        NAMES json/json.h
        PATHS
            ${CMAKE_PREFIX_PATH}/include
            C:/vcpkg/installed/x64-windows/include
    )

    find_library(JSONCPP_LIBRARY
        NAMES jsoncpp
        PATHS
            ${CMAKE_PREFIX_PATH}/lib
            C:/vcpkg/installed/x64-windows/lib
    )

    if(JSONCPP_INCLUDE_DIR AND JSONCPP_LIBRARY)
        message(STATUS "Found jsoncpp: ${JSONCPP_LIBRARY}")
        set(JSONCPP_FOUND TRUE)
    else()
        message(WARNING "jsoncpp not found - you may need to install it via vcpkg")
        message(STATUS "Run: vcpkg install jsoncpp:x64-windows")
        set(JSONCPP_FOUND FALSE)
    endif()

    # Source files
    set(SOURCES
        main.cpp
        # APIR Backend Integration
        ../backend.cpp
        ../backend-dispatched.cpp
        ../backend-dispatched-device.cpp
        ../backend-dispatched-buffer-type.cpp
        ../backend-dispatched-buffer.cpp
        ../backend-dispatched-backend.cpp
        ../apir_cs_ggml-rpc-back.cpp
    )

    # Create executable
    add_executable(${PROJECT_NAME} ${SOURCES})

    # Include directories
    if(JSONCPP_FOUND)
        target_include_directories(${PROJECT_NAME} PRIVATE ${JSONCPP_INCLUDE_DIR})
    endif()

    target_include_directories(${PROJECT_NAME} PRIVATE
        # Core GGML headers
        ${CMAKE_SOURCE_DIR}/ggml/include
        ${CMAKE_SOURCE_DIR}/ggml/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/common
        # Backend headers
        ..
        ../shared
        # Local directories
        ../../common
    )

    # Link libraries
    target_link_libraries(${PROJECT_NAME}
        ggml        # Main GGML library for backend functions
        ws2_32      # Winsock2
        advapi32    # Service management
    )

    if(JSONCPP_FOUND)
        target_link_libraries(${PROJECT_NAME} ${JSONCPP_LIBRARY})
    endif()

    # Set output name
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "VirtGPUWindowsBackend"
    )

    # Enable all warnings but treat them as warnings, not errors
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /W4)
        # Enable exception handling with proper unwind semantics
        target_compile_options(${PROJECT_NAME} PRIVATE /EHsc)
        # Disable specific warnings that are common in Windows API code
        target_compile_options(${PROJECT_NAME} PRIVATE
            /wd4100    # unreferenced formal parameter
            /wd4127    # conditional expression is constant
            /wd4505    # unreferenced local function has been removed
            /wd4996    # deprecated function
        )
    endif()

    # Install target (optional)
    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION bin
    )

    # Install scripts
    install(FILES
        build.cmd
        install.cmd
        uninstall.cmd
        DESTINATION bin
    )

else()
    message(FATAL_ERROR "This service is only supported on Windows")
endif()

# Print build information
message(STATUS "Building VirtGPU Windows Backend Service")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")

if(JSONCPP_FOUND)
    message(STATUS "  jsoncpp: ${JSONCPP_LIBRARY}")
else()
    message(STATUS "  jsoncpp: NOT FOUND - install via vcpkg")
endif()

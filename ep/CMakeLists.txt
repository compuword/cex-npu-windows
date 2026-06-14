cmake_minimum_required(VERSION 3.20)
project(cex_npu_ep LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------
# Locate ONNX Runtime
# Set ONNXRUNTIME_ROOT via cmake -DONNXRUNTIME_ROOT=<path>
# or let it auto-detect from well-known install paths.
# ---------------------------------------------------------------
if(NOT DEFINED ONNXRUNTIME_ROOT)
    set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_ROOT}")
endif()

if(NOT ONNXRUNTIME_ROOT OR NOT EXISTS "${ONNXRUNTIME_ROOT}")
    message(FATAL_ERROR
        "ONNXRUNTIME_ROOT not set or does not exist.\n"
        "Usage: cmake -B build -DONNXRUNTIME_ROOT=C:/onnxruntime-win-x64-1.18.0\n"
        "Download from: https://github.com/microsoft/onnxruntime/releases"
    )
endif()

message(STATUS "ONNXRUNTIME_ROOT: ${ONNXRUNTIME_ROOT}")

find_path(ORT_INCLUDE_DIR
    NAMES onnxruntime_c_api.h onnxruntime_cxx_api.h
    HINTS
        "${ONNXRUNTIME_ROOT}/include"
        "${ONNXRUNTIME_ROOT}/include/onnxruntime"
        "${ONNXRUNTIME_ROOT}/include/onnxruntime/core/session"
    REQUIRED
)
message(STATUS "ORT headers: ${ORT_INCLUDE_DIR}")

find_library(ORT_LIB
    NAMES onnxruntime
    HINTS
        "${ONNXRUNTIME_ROOT}/lib"
        "${ONNXRUNTIME_ROOT}/lib/x64"
    REQUIRED
)
message(STATUS "ORT library: ${ORT_LIB}")

# ---------------------------------------------------------------
# Shared library (DLL)
# ---------------------------------------------------------------
add_library(cex_npu_ep SHARED cex_npu_ep.cpp)

target_include_directories(cex_npu_ep PRIVATE "${ORT_INCLUDE_DIR}")

target_link_libraries(cex_npu_ep PRIVATE
    "${ORT_LIB}"
    Rpcrt4    # UuidCreate
    kernel32
    user32
)

# ---------------------------------------------------------------
# Windows-specific DLL settings
# ---------------------------------------------------------------
if(WIN32)
    target_compile_definitions(cex_npu_ep PRIVATE
        _WINDOWS
        _USRDLL
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    set_target_properties(cex_npu_ep PROPERTIES
        OUTPUT_NAME "cex_npu_ep"
        SUFFIX ".dll"
    )
endif()

# ---------------------------------------------------------------
# Install
# ---------------------------------------------------------------
install(TARGETS cex_npu_ep
    RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}"
)

message(STATUS "Build: cmake --build build --config Release")
message(STATUS "Output: build/Release/cex_npu_ep.dll")
message(STATUS "Install to System32: cmake --install build --prefix C:/Windows/System32")

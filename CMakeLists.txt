cmake_minimum_required(VERSION 3.20)
project(gpu_denorm_probe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)

add_executable(probe_runner runner/probe_runner.cpp)
target_compile_definitions(probe_runner PRIVATE UNICODE _UNICODE)
target_link_libraries(probe_runner PRIVATE d3d12 dxgi)

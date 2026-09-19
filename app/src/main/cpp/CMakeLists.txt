cmake_minimum_required(VERSION 3.22.1)
project(dlss5vulkanlab)

add_library(dlss5vulkanlab SHARED native-lib.cpp)

find_library(log_lib log)
find_library(android_lib android)
find_library(vulkan_lib vulkan)

target_compile_features(dlss5vulkanlab PRIVATE cxx_std_17)
target_compile_options(dlss5vulkanlab PRIVATE -Wall -Wextra)

target_link_libraries(
        dlss5vulkanlab
        ${log_lib}
        ${android_lib}
        ${vulkan_lib})

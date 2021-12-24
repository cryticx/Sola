cmake_minimum_required(VERSION 3.16)

project(Sola LANGUAGES C)

add_executable(Sola main.c SolaRender.c)

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(Ktx REQUIRED)

target_link_libraries(Sola PRIVATE vulkan glfw ktx m)

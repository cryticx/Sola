cmake_minimum_required(VERSION 3.16)

project(Sola LANGUAGES C)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -rdynamic -fno-omit-frame-pointer -fsanitize=address,undefined")

add_executable(Sola main.c SolaRender.c)

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(Ktx REQUIRED)

set (assetsDir "${CMAKE_SOURCE_DIR}/assets")
set (assetsLink "${CMAKE_CURRENT_BINARY_DIR}/assets")
add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${assetsDir} ${assetsLink}
)
set (shadersDir "${CMAKE_SOURCE_DIR}/shaders")
set (shadersLink "${CMAKE_CURRENT_BINARY_DIR}/shaders")
add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${shadersDir} ${shadersLink}
)
target_link_libraries(Sola PRIVATE vulkan glfw ktx m)

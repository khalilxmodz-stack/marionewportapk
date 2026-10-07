cmake_minimum_required(VERSION 3.18.1)
project(plumber LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# NDK-provided glue that turns NativeActivity callbacks into a simple event loop.
set(APP_GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(native_app_glue STATIC ${APP_GLUE_DIR}/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${APP_GLUE_DIR})

# Make sure the glue's entry point is not stripped.
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")

add_library(plumber SHARED
    game.cpp
    main.cpp)

target_link_libraries(plumber
    native_app_glue
    android
    log
    EGL
    GLESv2)

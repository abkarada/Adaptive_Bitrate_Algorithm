cmake_minimum_required(VERSION 3.5)
project(CameraFFmpeg)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenCV
find_package(OpenCV REQUIRED)

# Find FFmpeg libraries using pkg-config
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED
        libavutil
        libavcodec
        libavformat
        libswscale
)

# ISA-L pkg-config
pkg_check_modules(ISAL REQUIRED libisal)

# Include directories
include_directories(
        ${OpenCV_INCLUDE_DIRS}
        ${FFMPEG_INCLUDE_DIRS}
        ${ISAL_INCLUDE_DIRS}
)

# Link directories
link_directories(
        ${FFMPEG_LIBRARY_DIRS}
        ${ISAL_LIBRARY_DIRS}
)

# Executable target
add_executable(camera_ffmpeg main.cpp
        udp_port_profiler.h
)

# Link libraries
target_link_libraries(camera_ffmpeg
        ${OpenCV_LIBS}
        ${FFMPEG_LIBRARIES}
        ${ISAL_LIBRARIES}
)

# Set output directory
set_target_properties(camera_ffmpeg PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

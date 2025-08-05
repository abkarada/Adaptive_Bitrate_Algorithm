cmake_minimum_required(VERSION 3.5)
project(CameraFFmpeg)

# C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Require pkg-config for dependency resolution
find_package(PkgConfig REQUIRED)

# OpenCV (capture/display)
pkg_check_modules(OPENCV REQUIRED opencv4)

# FFmpeg (encoding)
pkg_check_modules(AVUTIL   REQUIRED libavutil)
pkg_check_modules(AVCODEC  REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(SWSCALE  REQUIRED libswscale)

# Include directories
include_directories(
        ${OPENCV_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
        ${AVCODEC_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS}
        ${SWSCALE_INCLUDE_DIRS}
)

# Library directories
link_directories(
        ${OPENCV_LIBRARY_DIRS}
        ${AVUTIL_LIBRARY_DIRS}
        ${AVCODEC_LIBRARY_DIRS}
        ${AVFORMAT_LIBRARY_DIRS}
        ${SWSCALE_LIBRARY_DIRS}
)

# Executable target
add_executable(camera_ffmpeg main.cpp)

target_link_libraries(camera_ffmpeg PRIVATE
        ${AVCODEC_LIBRARIES}
        ${AVFORMAT_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SWSCALE_LIBRARIES}
        ${OPENCV_LIBRARIES}
)

# Set output directory
set_target_properties(camera_ffmpeg PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Display configuration
message(STATUS "OpenCV include dirs: ${OPENCV_INCLUDE_DIRS}")
message(STATUS "OpenCV libs       : ${OPENCV_LIBRARIES}")
message(STATUS "FFmpeg avutil inc : ${AVUTIL_INCLUDE_DIRS}")
message(STATUS "FFmpeg avcodec inc: ${AVCODEC_INCLUDE_DIRS}")
message(STATUS "FFmpeg avformat inc: ${AVFORMAT_INCLUDE_DIRS}")
message(STATUS "FFmpeg swscale inc: ${SWSCALE_INCLUDE_DIRS}")
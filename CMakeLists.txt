cmake_minimum_required(VERSION 3.5)
project(CameraFFmpeg)

# C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenCV
find_package(OpenCV REQUIRED)

# Find FFmpeg libraries using pkg-config
find_package(PkgConfig REQUIRED)

# Use pkg-config to get all FFmpeg libraries at once
pkg_check_modules(FFMPEG REQUIRED 
    libavutil
    libavcodec
    libavformat
    libswscale
)

# Include directories
include_directories(${OpenCV_INCLUDE_DIRS})
include_directories(${FFMPEG_INCLUDE_DIRS})

# Executable target
add_executable(camera_ffmpeg main.cpp)

# Link libraries
target_link_libraries(camera_ffmpeg 
    ${OpenCV_LIBS}
    ${FFMPEG_LIBRARIES}
)

# Set output directory
set_target_properties(camera_ffmpeg PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Display configuration
message(STATUS "OpenCV found: ${OpenCV_FOUND}")
message(STATUS "OpenCV version: ${OpenCV_VERSION}")
message(STATUS "FFmpeg found: ${FFMPEG_FOUND}")

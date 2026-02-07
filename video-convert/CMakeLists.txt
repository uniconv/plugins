cmake_minimum_required(VERSION 3.16)
project(video-convert-plugin VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find uniconv include directory
set(UNICONV_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../uniconv/include"
    CACHE PATH "Path to uniconv include directory")

# Find FFmpeg libraries via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(AVFILTER REQUIRED libavfilter)

# Build shared library
add_library(video_convert SHARED video_convert.cpp)

target_include_directories(video_convert PRIVATE
    ${UNICONV_INCLUDE_DIR}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${AVFILTER_INCLUDE_DIRS}
)

target_link_directories(video_convert PRIVATE
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVCODEC_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
    ${AVFILTER_LIBRARY_DIRS}
)

target_link_libraries(video_convert PRIVATE
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${AVFILTER_LIBRARIES}
)

# Set output name
set_target_properties(video_convert PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "video_convert"
)

# Copy plugin.json to build directory for local development
add_custom_command(TARGET video_convert POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "$<TARGET_FILE_DIR:video_convert>/plugin.json"
)

# Install to plugin directory
install(TARGETS video_convert
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)

install(FILES plugin.json DESTINATION .)

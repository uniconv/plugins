cmake_minimum_required(VERSION 3.16)
project(image-convert-plugin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find uniconv include directory
set(UNICONV_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../uniconv/include"
    CACHE PATH "Path to uniconv include directory")

# Find dependencies via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS REQUIRED vips-cpp)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(POPPLER_GLIB REQUIRED poppler-glib)

# Build shared library
add_library(image_convert SHARED image_convert.cpp)

target_include_directories(image_convert PRIVATE
    ${UNICONV_INCLUDE_DIR}
    ${VIPS_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${POPPLER_GLIB_INCLUDE_DIRS}
)

target_link_directories(image_convert PRIVATE
    ${VIPS_LIBRARY_DIRS}
    ${CAIRO_LIBRARY_DIRS}
    ${POPPLER_GLIB_LIBRARY_DIRS}
)
target_link_libraries(image_convert PRIVATE
    ${VIPS_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${POPPLER_GLIB_LIBRARIES}
)

# RPATH: find bundled shared libs next to the plugin
if(APPLE)
    set_target_properties(image_convert PROPERTIES
        BUILD_RPATH "@loader_path"
        INSTALL_RPATH "@loader_path")
else()
    set_target_properties(image_convert PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN")
endif()

# Set output name
set_target_properties(image_convert PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "image_convert"
)

# Copy plugin.json to build directory for local development
add_custom_command(TARGET image_convert POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "$<TARGET_FILE_DIR:image_convert>/plugin.json"
)

# Install to plugin directory
install(TARGETS image_convert
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)

install(FILES plugin.json DESTINATION .)

cmake_minimum_required(VERSION 3.16)
project(image-convert-plugin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find uniconv include directory
set(UNICONV_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../uniconv/include"
    CACHE PATH "Path to uniconv include directory")

option(VIPS_STATIC "Link libvips statically for standalone distribution" ON)

# Find libvips via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS REQUIRED vips-cpp)

# Build shared library
add_library(image_convert SHARED image_convert.cpp)

target_include_directories(image_convert PRIVATE
    ${UNICONV_INCLUDE_DIR}
    ${VIPS_INCLUDE_DIRS}
)

if(VIPS_STATIC)
    message(STATUS "libvips: static linking enabled")

    # Get complete static link flags directly from pkg-config
    # Explicitly include gobject-2.0 and glib-2.0 since some platforms (MSYS2)
    # don't list them in vips-cpp's Requires.private
    set(VIPS_MODULES vips-cpp gobject-2.0 glib-2.0)
    execute_process(
        COMMAND pkg-config --static --libs ${VIPS_MODULES}
        OUTPUT_VARIABLE _RAW_STATIC_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _PKG_RESULT
    )
    if(NOT _PKG_RESULT EQUAL 0)
        message(FATAL_ERROR "pkg-config --static --libs failed for ${VIPS_MODULES}")
    endif()

    # Parse raw flags into categorized lists
    separate_arguments(_ALL_FLAGS UNIX_COMMAND "${_RAW_STATIC_LDFLAGS}")
    set(_LINK_DIRS "")
    set(_LINK_LIBS "")
    set(_FRAMEWORK_FLAGS "")
    set(_OTHER_FLAGS "")
    set(_PREV_IS_FW FALSE)
    foreach(_f ${_ALL_FLAGS})
        if(_PREV_IS_FW)
            list(APPEND _FRAMEWORK_FLAGS "-framework ${_f}")
            set(_PREV_IS_FW FALSE)
        elseif(_f MATCHES "^-L(.+)")
            list(APPEND _LINK_DIRS "${CMAKE_MATCH_1}")
        elseif(_f MATCHES "^-l(.+)")
            list(APPEND _LINK_LIBS "${CMAKE_MATCH_1}")
        elseif(_f STREQUAL "-framework")
            set(_PREV_IS_FW TRUE)
        else()
            list(APPEND _OTHER_FLAGS "${_f}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _LINK_DIRS)
    list(REMOVE_DUPLICATES _LINK_LIBS)
    list(REMOVE_DUPLICATES _FRAMEWORK_FLAGS)
    list(REMOVE_DUPLICATES _OTHER_FLAGS)

    # Remove libraries that don't exist on Windows
    if(WIN32)
        list(REMOVE_ITEM _LINK_LIBS "dl")
    endif()

    # Set suffixes to find only static .a files
    set(_ORIG_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
    if(WIN32)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    endif()

    # Build search paths (pkg-config dirs + Homebrew lib on macOS)
    set(_SEARCH_DIRS ${_LINK_DIRS})
    if(APPLE)
        execute_process(
            COMMAND brew --prefix
            OUTPUT_VARIABLE _BREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_BREW_PREFIX)
            list(APPEND _SEARCH_DIRS "${_BREW_PREFIX}/lib")
        endif()
    endif()

    # Find static .a for each library, fall back to -l for system libs
    set(_STATIC_LIB_PATHS "")
    set(_DYNAMIC_LIBS "")
    foreach(_lib ${_LINK_LIBS})
        find_library(_STATIC_${_lib}
            NAMES ${_lib}
            PATHS ${_SEARCH_DIRS}
            NO_DEFAULT_PATH
        )
        if(_STATIC_${_lib})
            list(APPEND _STATIC_LIB_PATHS "${_STATIC_${_lib}}")
            message(STATUS "  Static: ${_STATIC_${_lib}}")
        else()
            list(APPEND _DYNAMIC_LIBS ${_lib})
            message(STATUS "  Dynamic: ${_lib}")
        endif()
    endforeach()

    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_ORIG_SUFFIXES})

    # On Linux, some static libs may use relocations incompatible with shared libs;
    # -Bsymbolic prevents symbol interposition, making them safe.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(image_convert PRIVATE "-Wl,-Bsymbolic")
    endif()

    # Link everything
    target_link_directories(image_convert PRIVATE ${_SEARCH_DIRS})
    target_link_libraries(image_convert PRIVATE
        ${_STATIC_LIB_PATHS}
        ${_DYNAMIC_LIBS}
        ${_FRAMEWORK_FLAGS}
        ${_OTHER_FLAGS}
    )
else()
    target_link_directories(image_convert PRIVATE
        ${VIPS_LIBRARY_DIRS}
    )
    target_link_libraries(image_convert PRIVATE
        ${VIPS_LIBRARIES}
    )
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

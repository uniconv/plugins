cmake_minimum_required(VERSION 3.16)
project(audio-convert-plugin VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find uniconv include directory
set(UNICONV_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../uniconv/include"
    CACHE PATH "Path to uniconv include directory")

option(FFMPEG_STATIC "Link FFmpeg statically for standalone distribution" ON)
set(DEPS_PREFIX "" CACHE PATH "Path to pre-built static dependencies (from build-ffmpeg-deps.sh)")

# Discover FFmpeg modules via pkg-config
find_package(PkgConfig REQUIRED)
set(FFMPEG_MODULES libavformat libavcodec libavutil libswresample libavfilter)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(AVFILTER REQUIRED libavfilter)

# Build shared library
add_library(audio_convert SHARED audio_convert.cpp)

target_include_directories(audio_convert PRIVATE
    ${UNICONV_INCLUDE_DIR}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVFILTER_INCLUDE_DIRS}
)

if(FFMPEG_STATIC)
    message(STATUS "FFmpeg: static linking enabled")

    # Get complete static link flags directly from pkg-config
    # (CMake's _STATIC_LIBRARIES misses some transitive deps due to parsing issues)
    execute_process(
        COMMAND pkg-config --static --libs ${FFMPEG_MODULES}
        OUTPUT_VARIABLE _RAW_STATIC_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _PKG_RESULT
    )
    if(NOT _PKG_RESULT EQUAL 0)
        message(FATAL_ERROR "pkg-config --static --libs failed for FFmpeg modules")
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

    # Set suffixes to find only static .a files
    set(_ORIG_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
    if(WIN32)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    endif()

    # Build search paths (pkg-config dirs + custom prefix + Homebrew lib on macOS)
    set(_SEARCH_DIRS ${_LINK_DIRS})
    if(DEPS_PREFIX AND EXISTS "${DEPS_PREFIX}/lib")
        list(INSERT _SEARCH_DIRS 0 "${DEPS_PREFIX}/lib")
        message(STATUS "  Using DEPS_PREFIX: ${DEPS_PREFIX}/lib")
    endif()
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

    # Link everything
    target_link_directories(audio_convert PRIVATE ${_SEARCH_DIRS})
    target_link_libraries(audio_convert PRIVATE
        ${_STATIC_LIB_PATHS}
        ${_DYNAMIC_LIBS}
        ${_FRAMEWORK_FLAGS}
        ${_OTHER_FLAGS}
    )
else()
    target_link_directories(audio_convert PRIVATE
        ${AVFORMAT_LIBRARY_DIRS}
        ${AVCODEC_LIBRARY_DIRS}
        ${AVUTIL_LIBRARY_DIRS}
        ${SWRESAMPLE_LIBRARY_DIRS}
        ${AVFILTER_LIBRARY_DIRS}
    )
    target_link_libraries(audio_convert PRIVATE
        ${AVFORMAT_LIBRARIES}
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SWRESAMPLE_LIBRARIES}
        ${AVFILTER_LIBRARIES}
    )
endif()

# Set output name
set_target_properties(audio_convert PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "audio_convert"
)

# Copy plugin.json to build directory for local development
add_custom_command(TARGET audio_convert POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "$<TARGET_FILE_DIR:audio_convert>/plugin.json"
)

# Install to plugin directory
install(TARGETS audio_convert
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)

install(FILES plugin.json DESTINATION .)

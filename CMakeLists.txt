cmake_minimum_required(VERSION 3.22)

project(Splice VERSION 1.0.0 LANGUAGES C CXX)

# Optional build number, appended as a fourth version component (1.0.0.42) to the
# app's bundle/file version, so two builds of the same release can be told apart.
# Empty for local builds.
set(SPLICE_BUILD_NUMBER "" CACHE STRING "Build number appended to the app version")
set(SPLICE_APP_VERSION "${PROJECT_VERSION}")
if(SPLICE_BUILD_NUMBER)
    string(APPEND SPLICE_APP_VERSION ".${SPLICE_BUILD_NUMBER}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
endif()

# The x64-windows-static vcpkg triplet builds FFmpeg/x264/snappy against the
# static CRT (/MT), so the app must match or the link fails with LNK2038.
# Bonus: the portable .exe then needs no VC++ redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# JUCE. Expected at external/JUCE (see README for the one-line clone); point
# SPLICE_JUCE_DIR somewhere else to reuse an existing checkout.
# ---------------------------------------------------------------------------
set(SPLICE_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE"
    CACHE PATH "Path to a JUCE framework checkout")

if(NOT EXISTS "${SPLICE_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at ${SPLICE_JUCE_DIR}. Either run:\n"
        "  git clone --depth 1 --branch 8.0.7 "
        "https://github.com/juce-framework/JUCE external/JUCE\n"
        "or set -DSPLICE_JUCE_DIR=/path/to/JUCE.")
endif()

add_subdirectory("${SPLICE_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# FFmpeg.
#
# macOS: static libs built by tools/build-ffmpeg.sh, which configures just the
# Hap encoder and MOV muxer in (stock Homebrew FFmpeg has no Hap *encoder*).
#
# Windows: FFmpeg's ./configure can't run under MSVC, and this workspace has
# no MinGW precedent (mixing MinGW-built .a archives into an MSVC-linked .exe
# is a CRT/ABI risk with nothing here to prove it out) — so Windows instead
# takes FFmpeg from vcpkg's MSVC-native port (see vcpkg.json; run through
# `cmake --preset windows` or pass -DCMAKE_TOOLCHAIN_FILE yourself). vcpkg
# builds FFmpeg's full default encoder/muxer set rather than the narrow one
# build-ffmpeg.sh selects — bigger static footprint, but the port has no
# equivalent of --disable-encoders, and the Hap encoder is in the default set
# regardless, gated only on the "snappy" feature (which vcpkg.json enables).
# ---------------------------------------------------------------------------
if(WIN32)
    find_package(FFMPEG REQUIRED)

    foreach(lib avformat avcodec avutil swscale swresample)
        add_library(ffmpeg::${lib} INTERFACE IMPORTED)
    endforeach()
    # FFMPEG_LIBRARIES carries optimized/debug keywords, which are illegal in a
    # raw INTERFACE_LINK_LIBRARIES property but understood by
    # target_link_libraries.
    target_include_directories(ffmpeg::avformat INTERFACE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(ffmpeg::avformat INTERFACE ${FFMPEG_LIBRARIES})

    # vcpkg's port pulls libsnappy and libx264 in as transitive static deps of
    # avcodec (via its own find_package(Snappy)/find_package(X264) calls), so
    # FFMPEG_LIBRARIES already carries them — nothing extra to link here.
    set(SNAPPY_LIBRARY "")
    set(X264_LIBRARY "")
else()
    set(SPLICE_FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg-install"
        CACHE PATH "Prefix of a static FFmpeg build with the Hap encoder enabled")

    if(NOT EXISTS "${SPLICE_FFMPEG_DIR}/lib/libavcodec.a")
        message(FATAL_ERROR
            "FFmpeg not found at ${SPLICE_FFMPEG_DIR}. Run tools/build-ffmpeg.sh first.")
    endif()

    foreach(lib avformat avcodec avutil swscale swresample)
        add_library(ffmpeg::${lib} STATIC IMPORTED)
        set_target_properties(ffmpeg::${lib} PROPERTIES
            IMPORTED_LOCATION "${SPLICE_FFMPEG_DIR}/lib/lib${lib}.a"
            INTERFACE_INCLUDE_DIRECTORIES "${SPLICE_FFMPEG_DIR}/include")
    endforeach()

    # libsnappy is what the Hap encoder compresses its texture chunks with. Ask
    # for the static archive by name: a plain `snappy` would resolve to the
    # Homebrew dylib and the bundle would then refuse to launch anywhere
    # Homebrew isn't installed. Static keeps the app self-contained and fully
    # offline.
    find_library(SNAPPY_LIBRARY NAMES libsnappy.a
        PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)

    if(NOT SNAPPY_LIBRARY)
        message(FATAL_ERROR
            "Static libsnappy.a not found. Install it with: brew install snappy")
    endif()

    # libx264 encodes the two .mp4s the splitter writes. Static, same reasoning
    # as libsnappy above. NOTE: x264 is GPL, so the built app as a whole is
    # GPL — see the licence note in README.md.
    find_library(X264_LIBRARY NAMES libx264.a
        PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)

    if(NOT X264_LIBRARY)
        message(FATAL_ERROR
            "Static libx264.a not found. Install it with: brew install x264")
    endif()
endif()

# ---------------------------------------------------------------------------
# The app
# ---------------------------------------------------------------------------
# macOS gets the squircle with its margin and shadow baked in; Windows gets the
# full-bleed one (same split artwork, see assets/*.svg).
if(WIN32)
    set(SPLICE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-windows.png")
else()
    set(SPLICE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-macos.png")
endif()

# DOCUMENT_EXTENSIONS writes CFBundleDocumentTypes into Info.plist, so the app
# appears in Finder's "Open With" and accepts files dropped on its Dock icon.
# (In-window drag-and-drop works without it — that's handled by the
# FileDragAndDropTarget in MainComponent.)
juce_add_gui_app(Splice
    VERSION "${SPLICE_APP_VERSION}"
    PRODUCT_NAME "Splice"
    COMPANY_NAME "Hybrid Constructs"
    BUNDLE_ID "com.hybridconstructs.splice"
    ICON_BIG "${SPLICE_ICON}"
    DOCUMENT_EXTENSIONS "mp4;mov;m4v;avi;mkv;webm;mpg;mpeg")

juce_generate_juce_header(Splice)

target_sources(Splice PRIVATE
    source/Main.cpp
    source/MainComponent.cpp
    source/HapEncoder.cpp
    source/HapSplitter.cpp
    source/JobScanner.cpp
    source/BatchRunner.cpp
    source/UpdateChecker.cpp)

target_include_directories(Splice PRIVATE source)

target_compile_definitions(Splice PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Splice,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Splice,JUCE_VERSION>")

target_link_libraries(Splice PRIVATE
    juce::juce_gui_extra
    # FFmpeg: static link order matters — higher-level libs first.
    ffmpeg::avformat
    ffmpeg::avcodec
    ffmpeg::swscale
    ffmpeg::swresample
    ffmpeg::avutil
    ${SNAPPY_LIBRARY}
    ${X264_LIBRARY}
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(APPLE)
    # System libs/frameworks the FFmpeg demuxers and hardware paths reference.
    target_link_libraries(Splice PRIVATE
        "-framework CoreFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework VideoToolbox"
        "-framework AudioToolbox"
        "-framework Security"
        z bz2 iconv lzma c++)

    # The FFmpeg static libs are single-arch (its configure can't emit a
    # universal binary in one pass), so match the host arch here.
    execute_process(COMMAND uname -m
        OUTPUT_VARIABLE SPLICE_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
    set_target_properties(Splice PROPERTIES OSX_ARCHITECTURES "${SPLICE_ARCH}")
elseif(WIN32)
    # System libs FFmpeg's Windows demuxers/network stubs and swscale/x264
    # reference even in this network-disabled, mostly-decoder build.
    target_link_libraries(Splice PRIVATE
        ws2_32 secur32 bcrypt ole32 strmiids)
endif()

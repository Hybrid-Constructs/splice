cmake_minimum_required(VERSION 3.22)

project(Splice VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# JUCE. By default we reuse the checkout that already exists in the sibling
# hybridffgl repo so this project doesn't clone a second copy; point
# SPLICE_JUCE_DIR somewhere else to override.
# ---------------------------------------------------------------------------
set(SPLICE_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../hybridffgl/submodules/JUCE"
    CACHE PATH "Path to a JUCE framework checkout")

if(NOT EXISTS "${SPLICE_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at ${SPLICE_JUCE_DIR}. "
        "Set -DSPLICE_JUCE_DIR=/path/to/JUCE.")
endif()

add_subdirectory("${SPLICE_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# FFmpeg. Static libs built by tools/build-ffmpeg.sh, which configures the Hap
# encoder and MOV muxer in (stock Homebrew FFmpeg has no Hap *encoder*).
# ---------------------------------------------------------------------------
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

# libsnappy is what the Hap encoder compresses its texture chunks with. Ask for
# the static archive by name: a plain `snappy` would resolve to the Homebrew
# dylib and the bundle would then refuse to launch anywhere Homebrew isn't
# installed. Static keeps the app self-contained and fully offline.
find_library(SNAPPY_LIBRARY NAMES libsnappy.a
    PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)

if(NOT SNAPPY_LIBRARY)
    message(FATAL_ERROR
        "Static libsnappy.a not found. Install it with: brew install snappy")
endif()

# libx264 encodes the two .mp4s the splitter writes. Static, same reasoning as
# libsnappy above. NOTE: x264 is GPL, so the built app as a whole is GPL —
# see the licence note in README.md.
find_library(X264_LIBRARY NAMES libx264.a
    PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)

if(NOT X264_LIBRARY)
    message(FATAL_ERROR
        "Static libx264.a not found. Install it with: brew install x264")
endif()

# ---------------------------------------------------------------------------
# The app
# ---------------------------------------------------------------------------
# DOCUMENT_EXTENSIONS writes CFBundleDocumentTypes into Info.plist, so the app
# appears in Finder's "Open With" and accepts files dropped on its Dock icon.
# (In-window drag-and-drop works without it — that's handled by the
# FileDragAndDropTarget in MainComponent.)
juce_add_gui_app(Splice
    VERSION "${PROJECT_VERSION}"
    PRODUCT_NAME "Splice"
    COMPANY_NAME "Hybrid Constructs"
    BUNDLE_ID "com.hybridconstructs.splice"
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
endif()

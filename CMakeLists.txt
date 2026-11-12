cmake_minimum_required(VERSION 3.22)

# Universal binary (Intel + Apple Silicon). Release builds (Ninja / scripts/make-dmg.sh) run on
# macOS 10.15 Catalina and newer, covering Intel Macs back to 2012; the Apple Silicon half is
# automatically 11.0+ (the first macOS for those chips). The app only uses C++ runtime features
# that exist on 10.15 (checked with nm; filesystem/shared_mutex arrived in 10.15).
# Xcode 27's build system itself refuses targets below 12.0, so the Xcode (debugging) project
# uses 12.0.
if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
endif()
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures")

file(READ VERSION ELSEWHERE_VERSION)
string(STRIP "${ELSEWHERE_VERSION}" ELSEWHERE_VERSION)
project(Elsewhere VERSION ${ELSEWHERE_VERSION} LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

# Windows: link the C++ runtime statically, so Elsewhere.exe runs without installing the
# Visual C++ Redistributable first
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS YES)

# Small local patches to submodules (see patches/: <submodule>-*.patch). Applied once; skipped if
# already applied.
foreach(lib tracktion_engine ptformat)
    file(GLOB ELSEWHERE_LIB_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/${lib}-*.patch)
    foreach(patch ${ELSEWHERE_LIB_PATCHES})
        execute_process(COMMAND git apply --reverse --check ${patch}
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libs/${lib}
                        RESULT_VARIABLE already_applied OUTPUT_QUIET ERROR_QUIET)
        if(NOT already_applied EQUAL 0)
            execute_process(COMMAND git apply ${patch}
                            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libs/${lib}
                            RESULT_VARIABLE patch_result)
            if(NOT patch_result EQUAL 0)
                message(FATAL_ERROR "Failed to apply ${patch}")
            endif()
            message(STATUS "Applied ${patch}")
        endif()
    endforeach()
endforeach()

#------------------------------------------------------------------------------
# Networking for live sessions: libdatachannel (WebRTC data channels: NAT traversal
# via ICE, encrypted with DTLS) using Mbed TLS, both built from source so the app has
# no external dependencies and builds universal.
include(FetchContent)

# Command-line builds: the same "empty object file" note comes from ranlib
if(APPLE AND NOT CMAKE_GENERATOR STREQUAL "Xcode")
    set(CMAKE_C_ARCHIVE_FINISH   "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
    set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
endif()
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2
    URL_HASH SHA256=ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(mbedtls)

# Everything that includes Mbed TLS headers must see the same configuration
set(ELSEWHERE_MBEDTLS_CONFIG "MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/config/mbedtls_user_config.h\"")
foreach(lib mbedtls mbedx509 mbedcrypto)
    target_compile_definitions(${lib} PUBLIC ${ELSEWHERE_MBEDTLS_CONFIG})
endforeach()

add_library(MbedTLS::MbedTLS INTERFACE IMPORTED)
target_link_libraries(MbedTLS::MbedTLS INTERFACE mbedtls mbedx509 mbedcrypto)

set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/libdatachannel EXCLUDE_FROM_ALL)

# Opus: low-latency audio codec for real-time live mode. CPU-specific optimisations are
# disabled because one build compiles for both Intel and Apple Silicon (plain C is plenty fast).
set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
    URL https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz
    URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(opus)

# Third-party code: don't show its warnings (Xcode's defaults are stricter than these projects
# use), and don't report the empty object files for features we switched off. Elsewhere's own
# code keeps the full JUCE warning set.
foreach(dep mbedtls mbedx509 mbedcrypto everest p256m datachannel-static juice-static usrsctp opus)
    if(TARGET ${dep})
        target_compile_options(${dep} PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
        set_target_properties(${dep} PROPERTIES
            XCODE_ATTRIBUTE_GCC_WARN_INHIBIT_ALL_WARNINGS YES
            XCODE_ATTRIBUTE_OTHER_LIBTOOLFLAGS "-no_warning_for_no_symbols")
    endif()
endforeach()

set(TE_ADD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/tracktion_engine/modules/juce)
add_subdirectory(libs/tracktion_engine/modules)

# Apple signing, for Xcode builds. Empty signs locally, which is all you need to build and run
# from source. Official builds pass the Fuzz LLC team (scripts/xcode.sh reads
# ELSEWHERE_DEVELOPMENT_TEAM from the environment; see docs/RELEASING.md).
set(ELSEWHERE_DEVELOPMENT_TEAM "" CACHE STRING "Apple Developer Team ID used for code signing")

# Where the source code lives: About Elsewhere links to the release tag of each version, as the
# AGPL asks. Forks point this at their own repository.
set(ELSEWHERE_SOURCE_URL "https://github.com/sigsound/elsewhere" CACHE STRING "Public source repository")

# Forks should use their own bundle ID, so their builds don't share settings or permissions with Elsewhere
set(ELSEWHERE_BUNDLE_ID "com.fuzzllc.elsewhere" CACHE STRING "macOS bundle identifier")

# Audio Units only exist on macOS; Windows hosts VST3
if(APPLE)
    set(ELSEWHERE_HOST_AU TRUE)
else()
    set(ELSEWHERE_HOST_AU FALSE)
endif()

juce_add_gui_app(Elsewhere
    PRODUCT_NAME "Elsewhere"
    COMPANY_NAME "Fuzz LLC"
    BUNDLE_ID "${ELSEWHERE_BUNDLE_ID}"
    # Finder shows a clear "requires macOS ..." message on older systems
    # Local network: Somewhere (iPhone) finds Elsewhere with Bonjour (src/SomewhereLink.h)
    PLIST_TO_MERGE "<plist><dict><key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string><key>NSLocalNetworkUsageDescription</key><string>Elsewhere lets Somewhere on your iPhone find it on your Wi-Fi to send samples and control tracks.</string><key>NSBonjourServices</key><array><string>_somewhere._tcp</string></array><key>CFBundleIconName</key><string>AppIcon</string></dict></plist>"
    # App icon: resources/AppIcon.icon on a Mac (compiled below), AppIcon.png on Windows and in the
    # About window (scripts/make-icon.py turns resources/Logo.png into AppIcon.png)
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/AppIcon.png"
    # Hardened Runtime is required for notarization. A DAW needs audio input, and to load
    # third-party plug-ins signed by other developers (some of which also need JIT).
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS
        com.apple.security.device.audio-input
        com.apple.security.cs.disable-library-validation
        com.apple.security.cs.allow-jit
        com.apple.security.cs.allow-unsigned-executable-memory
    VERSION ${ELSEWHERE_VERSION}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Elsewhere needs audio input access to record from your audio interface."
    DOCUMENT_EXTENSIONS tracktionedit
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
    PLUGINHOST_AU ${ELSEWHERE_HOST_AU})

juce_generate_juce_header(Elsewhere)

# Xcode: generate a scheme for the app only (scripts/xcode.sh turns it into Debug/Release schemes)
set_target_properties(Elsewhere PROPERTIES XCODE_GENERATE_SCHEME ON)

if(ELSEWHERE_DEVELOPMENT_TEAM)
    set_target_properties(Elsewhere PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${ELSEWHERE_DEVELOPMENT_TEAM}"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development")
else()
    set_target_properties(Elsewhere PROPERTIES XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-")
endif()

file(GLOB_RECURSE ELSEWHERE_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.mm src/*.h)

# Platform code: Objective-C++ on a Mac, PlatformOther.cpp everywhere else
if(APPLE)
    list(FILTER ELSEWHERE_SOURCES EXCLUDE REGEX "PlatformOther\\.cpp$")
else()
    list(FILTER ELSEWHERE_SOURCES EXCLUDE REGEX "\\.mm$")
endif()
target_sources(Elsewhere PRIVATE ${ELSEWHERE_SOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX Source FILES ${ELSEWHERE_SOURCES})

# Show the icon files in Xcode's navigator (under "Resources") so they're easy to find and
# replace. They're only listed here; JUCE builds AppIcon.icns from ICON_BIG above.
set(ELSEWHERE_ICON_FILES resources/AppIcon.png resources/Logo.png)
target_sources(Elsewhere PRIVATE ${ELSEWHERE_ICON_FILES})
set_source_files_properties(${ELSEWHERE_ICON_FILES} PROPERTIES HEADER_FILE_ONLY TRUE XCODE_EXPLICIT_FILE_TYPE image.png)
source_group(Resources FILES ${ELSEWHERE_ICON_FILES})

# The .icns JUCE generates from AppIcon.png: mark it as an image (CMake defaults to "source code",
# so Xcode showed it as garbled text) and keep it next to the PNGs
set(ELSEWHERE_GENERATED_ICON "${CMAKE_CURRENT_BINARY_DIR}/Elsewhere_artefacts/JuceLibraryCode/AppIcon.icns")
set_source_files_properties(${ELSEWHERE_GENERATED_ICON} PROPERTIES XCODE_EXPLICIT_FILE_TYPE image.icns)
source_group("Resources\\Generated" FILES ${ELSEWHERE_GENERATED_ICON})

# macOS 26 and later show the icon in light, dark and tinted versions, from resources/AppIcon.icon
# (an Icon Composer file). actool compiles it into the bundle's Assets.car, along with an
# AppIcon.icns for older macOS that replaces the one JUCE makes from AppIcon.png.
if(APPLE)
    set(ELSEWHERE_ICON_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT ELSEWHERE_ICON_TARGET)
        set(ELSEWHERE_ICON_TARGET 12.0)
    endif()
    add_custom_command(TARGET Elsewhere POST_BUILD
        COMMAND xcrun actool "${CMAKE_CURRENT_SOURCE_DIR}/resources/AppIcon.icon"
                --compile "$<TARGET_BUNDLE_CONTENT_DIR:Elsewhere>/Resources"
                --platform macosx --minimum-deployment-target ${ELSEWHERE_ICON_TARGET} --app-icon AppIcon
                --output-partial-info-plist "${CMAKE_CURRENT_BINARY_DIR}/AppIcon-partial.plist"
                --errors --warnings
        VERBATIM)
endif()

target_compile_definitions(Elsewhere PRIVATE
    JUCE_PLUGINHOST_AU=$<BOOL:${APPLE}>
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_LAME_AUDIO_FORMAT=1                # MP3 export through LAME, when it's installed
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=0   # LGPL: Signalsmith Stretch (MIT) does warp, transpose and Pitch Shift
    TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1
    TRACKTION_ENABLE_CONTROL_SURFACES=1         # Mackie Control surfaces
    ELSEWHERE_VERSION="${ELSEWHERE_VERSION}"
    ELSEWHERE_SOURCE_URL="${ELSEWHERE_SOURCE_URL}")

# Recorded drum kits (CC0 samples from Karoryfer; see resources/kits/README.md), built into the app
file(GLOB ELSEWHERE_KIT_SAMPLES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/kits/*.flac)
juce_add_binary_data(ElsewhereKitSamples HEADER_NAME KitSamples.h NAMESPACE KitSamples SOURCES ${ELSEWHERE_KIT_SAMPLES})

# The sound pack catalog, shared with Somewhere (sounds/catalog.json)
juce_add_binary_data(ElsewhereSoundCatalog HEADER_NAME SoundCatalog.h NAMESPACE SoundCatalog SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/sounds/catalog.json)

# The About window: the app icon and the third-party licence notices
juce_add_binary_data(ElsewhereAboutResources HEADER_NAME AboutResources.h NAMESPACE AboutResources SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/AppIcon.png
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/ThirdPartyNotices.txt)
set_target_properties(ElsewhereAboutResources PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(ElsewhereSoundCatalog PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(ElsewhereKitSamples PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 11.0)
    target_compile_options(ElsewhereKitSamples PRIVATE -Wno-\#warnings)
    target_compile_options(ElsewhereAboutResources PRIVATE -Wno-\#warnings)
endif()

target_link_libraries(Elsewhere PRIVATE
    ElsewhereKitSamples
    ElsewhereSoundCatalog
    ElsewhereAboutResources
    LibDataChannel::LibDataChannelStatic
    opus
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

set_target_properties(Elsewhere PROPERTIES C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)

#------------------------------------------------------------------------------
# Pro Tools sessions: libptformat (LGPL-2.1+) plus its C entry point (libs/ptformat-c), built as a
# separate shared library that ships inside the app and that Elsewhere loads at run time. Keeping it
# separate and replaceable is what lets closed-source Elsewhere use it: see docs/LICENSING.md.
add_library(ptformat SHARED libs/ptformat/ptformat.cc libs/ptformat-c/ptformat_c.cc)
target_include_directories(ptformat PRIVATE libs/ptformat libs/ptformat-c)
set_target_properties(ptformat PROPERTIES OUTPUT_NAME ptformat C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
if(MSVC)
    target_compile_definitions(ptformat PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_dependencies(Elsewhere ptformat)

# LGPL: its complete source (with our change and the C entry point) ships alongside it
set(ELSEWHERE_PTFORMAT_SOURCE_ZIP ${CMAKE_CURRENT_BINARY_DIR}/ptformat-source.zip)
set(ELSEWHERE_PTFORMAT_SOURCE_FILES
    libs/ptformat/ptformat.cc libs/ptformat/ptformat/ptformat.h libs/ptformat/ptformat/visibility.h
    libs/ptformat/COPYING.LESSER libs/ptformat/README.md
    libs/ptformat-c/ptformat_c.h libs/ptformat-c/ptformat_c.cc libs/ptformat-c/README.txt
    patches/ptformat-double-ratefactor.patch)
list(TRANSFORM ELSEWHERE_PTFORMAT_SOURCE_FILES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/ OUTPUT_VARIABLE ELSEWHERE_PTFORMAT_SOURCE_DEPENDS)
add_custom_command(OUTPUT ${ELSEWHERE_PTFORMAT_SOURCE_ZIP}
    COMMAND ${CMAKE_COMMAND} -E tar cf ${ELSEWHERE_PTFORMAT_SOURCE_ZIP} --format=zip ${ELSEWHERE_PTFORMAT_SOURCE_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${ELSEWHERE_PTFORMAT_SOURCE_DEPENDS}
    COMMENT "Packing the ptformat source (LGPL)")
add_custom_target(ptformat_source DEPENDS ${ELSEWHERE_PTFORMAT_SOURCE_ZIP})
add_dependencies(Elsewhere ptformat_source)

if(APPLE)
    set_target_properties(ptformat PROPERTIES INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)
    add_custom_command(TARGET Elsewhere POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:Elsewhere>/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ptformat>" "$<TARGET_BUNDLE_CONTENT_DIR:Elsewhere>/Frameworks/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:Elsewhere>/Resources/Licenses"
        COMMAND ${CMAKE_COMMAND} -E copy ${ELSEWHERE_PTFORMAT_SOURCE_ZIP} "$<TARGET_BUNDLE_CONTENT_DIR:Elsewhere>/Resources/Licenses/")
else()
    add_custom_command(TARGET Elsewhere POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ptformat>" "$<TARGET_FILE_DIR:Elsewhere>/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Elsewhere>/Licenses"
        COMMAND ${CMAKE_COMMAND} -E copy ${ELSEWHERE_PTFORMAT_SOURCE_ZIP} "$<TARGET_FILE_DIR:Elsewhere>/Licenses/")
endif()

if(APPLE)
    target_compile_options(Elsewhere PRIVATE -fno-aligned-allocation)

    # The current libc++ prints "platform no longer supported" for 10.15 targets; the symbols we
    # use are all present there, so keep the build output clean
    if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 11.0)
        target_compile_options(Elsewhere PRIVATE -Wno-\#warnings)
    endif()
endif()

if(MSVC)
    # Big translation units (Tracktion Engine, JUCE), UTF-8 source files, and no min/max macros
    target_compile_options(Elsewhere PRIVATE /bigobj /utf-8)
    target_compile_definitions(Elsewhere PRIVATE NOMINMAX)
endif()

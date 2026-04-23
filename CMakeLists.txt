cmake_minimum_required(VERSION 3.16)
project(tt CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)

set(TT_SOURCES
    src/main.cpp
    src/font.cpp
    src/renderer.cpp
    src/app.cpp
    src/data.cpp
)

set(HAVE_WAYLAND FALSE)
set(HAVE_X11 FALSE)
set(PROTO_GENERATED)

if(APPLE)
    # macOS: system OpenGL 3.3 core profile + Cocoa. FreeType + HarfBuzz
    # are built from source so we get a predictable version (and HarfBuzz
    # with CoreText support) regardless of what's on the system.
    find_package(OpenGL REQUIRED)

    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    FetchContent_Declare(freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG VER-2-13-3
        GIT_SHALLOW ON
    )

    set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_CORETEXT ON CACHE BOOL "" FORCE)
    FetchContent_Declare(harfbuzz
        GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
        GIT_TAG 10.4.0
        GIT_SHALLOW ON
    )

    FetchContent_MakeAvailable(freetype harfbuzz)

    # Generate tt.icns from tt.svg using built-in sips + iconutil.
    set(TT_ICNS ${CMAKE_BINARY_DIR}/tt.icns)
    add_custom_command(
        OUTPUT ${TT_ICNS}
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen-icns.sh
                ${CMAKE_SOURCE_DIR}/tt.svg ${TT_ICNS}
        DEPENDS ${CMAKE_SOURCE_DIR}/tt.svg ${CMAKE_SOURCE_DIR}/scripts/gen-icns.sh
        COMMENT "Generating tt.icns from tt.svg"
    )
    set_source_files_properties(${TT_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    list(APPEND TT_SOURCES src/main_macos.mm ${TT_ICNS})

    add_executable(tt MACOSX_BUNDLE ${TT_SOURCES})

    set_target_properties(tt PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/cmake/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "TT"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.tt.timetracker"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_ICON_FILE tt.icns
    )

    target_link_libraries(tt PRIVATE
        OpenGL::GL
        freetype
        harfbuzz
        nlohmann_json::nlohmann_json
        "-framework Cocoa"
        "-framework CoreText"
        "-framework CoreFoundation"
        "-framework CoreVideo"
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GL  REQUIRED IMPORTED_TARGET gl)
    pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)

    # Font rendering
    pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(HARFBUZZ  REQUIRED IMPORTED_TARGET harfbuzz)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

    # --- X11 backend (optional) ---
    pkg_check_modules(X11  IMPORTED_TARGET x11)
    pkg_check_modules(XEXT IMPORTED_TARGET xext)

    # --- Wayland backend (optional) ---
    pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
    pkg_check_modules(WAYLAND_EGL    IMPORTED_TARGET wayland-egl)
    pkg_check_modules(WAYLAND_CURSOR IMPORTED_TARGET wayland-cursor)
    pkg_check_modules(XKBCOMMON      IMPORTED_TARGET xkbcommon)

    if(WAYLAND_CLIENT_FOUND AND WAYLAND_EGL_FOUND AND WAYLAND_CURSOR_FOUND AND XKBCOMMON_FOUND)
        find_program(WAYLAND_SCANNER wayland-scanner)
        if(WAYLAND_SCANNER)
            set(HAVE_WAYLAND TRUE)

            pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
            if(NOT WAYLAND_PROTOCOLS_DIR OR NOT EXISTS "${WAYLAND_PROTOCOLS_DIR}")
                set(WAYLAND_PROTOCOLS_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols)
                file(MAKE_DIRECTORY ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell)
                file(MAKE_DIRECTORY ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration)
                set(XDG_SHELL_URL "https://gitlab.freedesktop.org/wayland/wayland-protocols/-/raw/main/stable/xdg-shell/xdg-shell.xml")
                set(XDG_DECO_URL "https://gitlab.freedesktop.org/wayland/wayland-protocols/-/raw/main/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
                if(NOT EXISTS ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
                    file(DOWNLOAD ${XDG_SHELL_URL} ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml SHOW_PROGRESS)
                endif()
                if(NOT EXISTS ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
                    file(DOWNLOAD ${XDG_DECO_URL} ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml SHOW_PROGRESS)
                endif()
            endif()
            set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
            file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

            set(PROTOCOLS
                unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
                stable/xdg-shell/xdg-shell.xml
            )

            foreach(PROTO ${PROTOCOLS})
                get_filename_component(BASE ${PROTO} NAME_WE)
                set(XML ${WAYLAND_PROTOCOLS_DIR}/${PROTO})
                set(HEADER ${PROTO_GEN_DIR}/${BASE}-client-protocol.h)
                set(SOURCE ${PROTO_GEN_DIR}/${BASE}-protocol.c)
                add_custom_command(OUTPUT ${HEADER}
                    COMMAND ${WAYLAND_SCANNER} client-header ${XML} ${HEADER}
                    DEPENDS ${XML} VERBATIM)
                add_custom_command(OUTPUT ${SOURCE}
                    COMMAND ${WAYLAND_SCANNER} private-code ${XML} ${SOURCE}
                    DEPENDS ${XML} VERBATIM)
                list(APPEND PROTO_GENERATED ${HEADER} ${SOURCE})
            endforeach()
        endif()
    endif()

    if(X11_FOUND AND XEXT_FOUND)
        set(HAVE_X11 TRUE)
    endif()

    if(NOT HAVE_WAYLAND AND NOT HAVE_X11)
        message(FATAL_ERROR "Need at least one of: Wayland or X11")
    endif()

    list(APPEND TT_SOURCES ${PROTO_GENERATED})
    add_executable(tt ${TT_SOURCES})

    target_link_libraries(tt PRIVATE
        PkgConfig::GL
        PkgConfig::EGL
        PkgConfig::FREETYPE
        PkgConfig::HARFBUZZ
        PkgConfig::FONTCONFIG
        nlohmann_json::nlohmann_json
    )

    if(HAVE_WAYLAND)
        target_compile_definitions(tt PRIVATE HAVE_WAYLAND=1)
        target_include_directories(tt PRIVATE ${PROTO_GEN_DIR})
        target_link_libraries(tt PRIVATE
            PkgConfig::WAYLAND_CLIENT
            PkgConfig::WAYLAND_EGL
            PkgConfig::WAYLAND_CURSOR
            PkgConfig::XKBCOMMON
        )
    endif()

    if(HAVE_X11)
        target_compile_definitions(tt PRIVATE HAVE_X11=1)
        target_link_libraries(tt PRIVATE PkgConfig::X11 PkgConfig::XEXT)
    endif()
endif()

target_compile_options(tt PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

message(STATUS "Wayland backend: ${HAVE_WAYLAND}")
message(STATUS "X11 backend:     ${HAVE_X11}")
message(STATUS "macOS backend:   ${APPLE}")

# Install
install(TARGETS tt DESTINATION bin)
if(LINUX OR CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    install(FILES tt.desktop DESTINATION share/applications)
    install(FILES tt.svg DESTINATION share/icons/hicolor/scalable/apps)
endif()

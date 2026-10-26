cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(tt LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- nlohmann/json (always FetchContent) ----
message(STATUS "Fetching nlohmann/json...")
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- wxWidgets: try system first, fall back to FetchContent ----
find_package(wxWidgets QUIET COMPONENTS core base)

if(NOT wxWidgets_FOUND)
    message(STATUS "wxWidgets not found in system — building from source (FetchContent)")

    # wxWidgets 3.2.6's CMakeLists declares cmake_minimum_required(VERSION 3.0),
    # which CMake 4 rejects. Override the policy floor for the fetched build.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

    set(wxBUILD_SHARED OFF)
    FetchContent_Declare(
        wxWidgets
        GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
        GIT_TAG v3.2.6
        GIT_SHALLOW ON
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(wxWidgets)

    set(WX_LIBS wxcore)
else()
    message(STATUS "Using system wxWidgets ${wxWidgets_VERSION_STRING}")
    include(${wxWidgets_USE_FILE})
    set(WX_LIBS ${wxWidgets_LIBRARIES})
endif()

# ---- Sources ----
set(SRCS
    src/main.cpp
    src/tt_frame.cpp
    src/data.cpp
    src/cli.cpp
    src/git_sync.cpp
    src/omarchy.cpp
)

if(APPLE)
    add_executable(tt MACOSX_BUNDLE ${SRCS})
elseif(WIN32)
    add_executable(tt WIN32 ${SRCS})
else()
    add_executable(tt ${SRCS})
endif()

target_link_libraries(tt PRIVATE
    ${WX_LIBS}
    nlohmann_json::nlohmann_json
)

target_compile_options(tt PRIVATE -Wall -Wextra -Wno-unused-parameter)

include(GNUInstallDirs)

# ---- Omarchy bar widget ----
# Not under share/tt: with a ~/.local prefix that is the data dir itself,
# which may be a git repo that tt commits wholesale.
set(TT_OMARCHY_PLUGIN_DIR ${CMAKE_INSTALL_DATADIR}/tt-omarchy-plugin)
file(RELATIVE_PATH TT_OMARCHY_PLUGIN_REL
    ${CMAKE_INSTALL_FULL_BINDIR}
    ${CMAKE_INSTALL_PREFIX}/${TT_OMARCHY_PLUGIN_DIR})
target_compile_definitions(tt PRIVATE
    TT_OMARCHY_PLUGIN_REL="${TT_OMARCHY_PLUGIN_REL}"
    TT_OMARCHY_PLUGIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/omarchy"
)

install(TARGETS tt
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .
)
if(LINUX OR CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    install(FILES tt.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES tt.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    install(DIRECTORY omarchy/ DESTINATION ${TT_OMARCHY_PLUGIN_DIR})
endif()

# ---- Xfce panel plugin ----
# Built when the Xfce panel headers are installed.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(XFCE_PANEL QUIET IMPORTED_TARGET libxfce4panel-2.0 libwnck-3.0)
endif()

if(XFCE_PANEL_FOUND)
    message(STATUS "Building the Xfce panel plugin")
    enable_language(C)

    add_library(tt-panel MODULE xfce/panel-plugin.cpp xfce/register.c)
    target_link_libraries(tt-panel PRIVATE PkgConfig::XFCE_PANEL nlohmann_json::nlohmann_json)
    # The icons get a directory of their own rather than going into hicolor,
    # whose icon cache, if stale, would hide them.
    set(TT_PANEL_ICON_DIR ${CMAKE_INSTALL_DATADIR}/tt-xfce-panel)
    target_compile_definitions(tt-panel PRIVATE
        TT_COMMAND="${CMAKE_INSTALL_FULL_BINDIR}/tt"
        TT_PANEL_ICON_DIR="${CMAKE_INSTALL_FULL_DATADIR}/tt-xfce-panel"
    )
    target_compile_options(tt-panel PRIVATE -Wall -Wextra -Wno-unused-parameter)

    # The button in a window of its own, to try it without the panel.
    add_executable(tt-panel-window EXCLUDE_FROM_ALL xfce/panel-plugin.cpp)
    target_link_libraries(tt-panel-window PRIVATE PkgConfig::XFCE_PANEL nlohmann_json::nlohmann_json)
    target_compile_definitions(tt-panel-window PRIVATE
        TT_PANEL_STANDALONE
        TT_COMMAND="${CMAKE_INSTALL_FULL_BINDIR}/tt"
        TT_PANEL_ICON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/xfce/icons"
    )

    # The panel looks for plugins under each prefix in the XDG data dirs,
    # ~/.local included: the .desktop file in <prefix>/share/xfce4/panel/
    # plugins, the module in its own lib dir relative to that prefix.
    pkg_get_variable(XFCE_PANEL_PREFIX libxfce4panel-2.0 prefix)
    pkg_get_variable(XFCE_PANEL_LIBDIR libxfce4panel-2.0 libdir)
    file(RELATIVE_PATH XFCE_PANEL_LIBDIR_REL ${XFCE_PANEL_PREFIX} ${XFCE_PANEL_LIBDIR})
    install(TARGETS tt-panel
        LIBRARY DESTINATION ${XFCE_PANEL_LIBDIR_REL}/xfce4/panel/plugins
        COMPONENT xfce-panel)
    install(FILES xfce/tt-panel.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/xfce4/panel/plugins
        COMPONENT xfce-panel)
    install(FILES xfce/icons/tt-idle-symbolic.svg xfce/icons/tt-running-symbolic.svg
        DESTINATION ${TT_PANEL_ICON_DIR}
        COMPONENT xfce-panel)
else()
    message(STATUS "Xfce panel headers not found — skipping the panel plugin")
endif()

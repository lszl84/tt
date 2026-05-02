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
install(TARGETS tt
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .
)
if(LINUX OR CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    install(FILES tt.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES tt.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

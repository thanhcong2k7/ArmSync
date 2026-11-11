cmake_minimum_required(VERSION 3.16)
project(ArmSync VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# On Ubuntu 22.04 (Jammy), the Ubuntu archive provides wxWidgets 3.0.x.
# Fetch wxWidgets 3.2 when a suitable system installation is unavailable.
option(ARMSYNC_FETCH_WX
       "Download and build wxWidgets 3.2 if not found on the system" ON)
set(ARMSYNC_WX_VERSION "3.2.12" CACHE STRING "wxWidgets 3.2.x version to fetch")

set(ARMSYNC_WX_FROM_SOURCE OFF)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)

find_package(wxWidgets 3.2 QUIET COMPONENTS core base)

if(wxWidgets_FOUND)
    message(STATUS "Using system wxWidgets ${wxWidgets_VERSION_STRING}")
    if(wxWidgets_USE_FILE)
        include(${wxWidgets_USE_FILE})
    endif()
elseif(ARMSYNC_FETCH_WX)
    message(STATUS "System wxWidgets >= 3.2 not found; fetching wxWidgets ${ARMSYNC_WX_VERSION}")
    include(FetchContent)

    # Build only the libraries ArmSync needs. These options must be set before
    # FetchContent_MakeAvailable() adds wxWidgets to this build.
    set(wxBUILD_SHARED  OFF CACHE BOOL   "Build static wxWidgets libraries" FORCE)
    set(wxBUILD_SAMPLES OFF CACHE STRING "" FORCE)
    set(wxBUILD_TESTS   OFF CACHE STRING "" FORCE)
    set(wxBUILD_DEMOS   OFF CACHE BOOL   "" FORCE)
    set(wxBUILD_PRECOMP OFF CACHE BOOL   "" FORCE)

    set(_wx_url
        "https://github.com/wxWidgets/wxWidgets/releases/download/v${ARMSYNC_WX_VERSION}/wxWidgets-${ARMSYNC_WX_VERSION}.tar.bz2")

    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        FetchContent_Declare(wxWidgets
            URL "${_wx_url}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
    else()
        FetchContent_Declare(wxWidgets URL "${_wx_url}")
    endif()

    FetchContent_MakeAvailable(wxWidgets)
    set(ARMSYNC_WX_FROM_SOURCE ON)
else()
    message(FATAL_ERROR
        "wxWidgets >= 3.2 was not found. Install wxWidgets 3.2 or configure "
        "with -DARMSYNC_FETCH_WX=ON. On Ubuntu 22.04, the default Jammy "
        "repository only provides wxWidgets 3.0.x.")
endif()

# libssh2: prefer pkg-config, then a CMake package, then explicit library lookup.
find_package(PkgConfig QUIET)
set(ARMSYNC_SSH2_TARGET "")

if(PkgConfig_FOUND)
    pkg_check_modules(LIBSSH2 QUIET IMPORTED_TARGET libssh2)
    if(TARGET PkgConfig::LIBSSH2)
        set(ARMSYNC_SSH2_TARGET PkgConfig::LIBSSH2)
    endif()
endif()

if(NOT ARMSYNC_SSH2_TARGET)
    find_package(libssh2 CONFIG QUIET)
    if(TARGET libssh2::libssh2)
        set(ARMSYNC_SSH2_TARGET libssh2::libssh2)
    endif()
    foreach(_candidate
            libssh2::libssh2
            Libssh2::libssh2
            Libssh2::Libssh2
            libssh2)
        if(TARGET ${_candidate})
            set(ARMSYNC_SSH2_TARGET ${_candidate})
            break()
        endif()
    endforeach()
endif()

if(NOT ARMSYNC_SSH2_TARGET)
    find_path(LIBSSH2_INCLUDE_DIR NAMES libssh2.h)
    find_library(LIBSSH2_LIBRARY NAMES ssh2 libssh2)

    if(NOT LIBSSH2_INCLUDE_DIR OR NOT LIBSSH2_LIBRARY)
        message(FATAL_ERROR
            "libssh2 was not found. On Ubuntu/Debian install libssh2-1-dev; "
            "on Windows, vcpkg install libssh2.")
    endif()

    add_library(armsync_libssh2 UNKNOWN IMPORTED)
    set_target_properties(armsync_libssh2 PROPERTIES
        IMPORTED_LOCATION "${LIBSSH2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBSSH2_INCLUDE_DIR}"
    )
    set(ARMSYNC_SSH2_TARGET armsync_libssh2)
endif()

find_package(Threads REQUIRED)

add_executable(ArmSync WIN32 MACOSX_BUNDLE
    src/App.h            src/App.cpp
    src/Events.h
    src/Settings.h       src/Settings.cpp
    src/SettingsDialog.h src/SettingsDialog.cpp
    src/SftpClient.h     src/SftpClient.cpp
    src/SyncWorker.h     src/SyncWorker.cpp
    src/MainFrame.h      src/MainFrame.cpp
)

target_include_directories(ArmSync PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(ARMSYNC_WX_FROM_SOURCE)
    # These names are the wxWidgets CMake targets created by its source build.
    target_link_libraries(ArmSync PRIVATE wx::core wx::base)
else()
    target_link_libraries(ArmSync PRIVATE ${wxWidgets_LIBRARIES})
endif()

target_link_libraries(ArmSync PRIVATE
    ${ARMSYNC_SSH2_TARGET}
    Threads::Threads
    OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(ArmSync PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(ArmSync PRIVATE /W3 /utf-8)
else()
    target_compile_options(ArmSync PRIVATE -Wall -Wextra -Wpedantic)
endif()
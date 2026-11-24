# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(isp-channel VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

include(GNUInstallDirs)
include(CTest)
find_package(Threads REQUIRED)

# ISP core: virtual sensor, software ISP stages, control algorithms.
# No dependency on libcamera or GStreamer, so it can be unit tested anywhere.
add_library(isp-core STATIC
    src/isp_control.cpp
    src/sensor_model.cpp
    src/soft_isp.cpp
    src/thread_pool.cpp)
target_include_directories(isp-core PUBLIC src)
target_link_libraries(isp-core PUBLIC Threads::Threads)

add_executable(isp-bench tools/isp-bench.cpp)
target_link_libraries(isp-bench PRIVATE isp-core)

add_executable(isp-ctl tools/isp-ctl.cpp)

if(BUILD_TESTING)
    add_executable(test-isp tests/test_isp.cpp)
    target_link_libraries(test-isp PRIVATE isp-core)
    add_test(NAME isp COMMAND test-isp)
endif()

# The channel application itself. Optional so that the core and its tests
# build on a host without libcamera.
option(ISP_BUILD_APP "Build the isp-channel application" ON)
if(ISP_BUILD_APP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
    pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)
    pkg_check_modules(LIBPNG REQUIRED IMPORTED_TARGET libpng)

    add_executable(isp-channel
        src/camera_source.cpp
        src/channel.cpp
        src/config.cpp
        src/control_socket.cpp
        src/encoder.cpp
        src/frame_stats.cpp
        src/log.cpp
        src/main.cpp
        src/snapshot.cpp)
    target_link_libraries(isp-channel PRIVATE
        isp-core PkgConfig::LIBCAMERA PkgConfig::GSTREAMER PkgConfig::LIBPNG)

    install(TARGETS isp-channel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(TARGETS isp-ctl isp-bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

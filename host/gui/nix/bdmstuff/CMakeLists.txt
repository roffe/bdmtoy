# bdmstuff, the bdmtoy GUI: Qt 6, libusb-1.0.
#   cmake -B build && cmake --build build
cmake_minimum_required(VERSION 3.21)
project(bdmstuff LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../core)

add_executable(bdmstuff
    main.cpp
    mainwindow.cpp
    worker.cpp
    debugwindow.cpp
    mainwindow.ui
    debugwindow.ui
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/toy_update.c
    ${CORE}/core.c
    ${CORE}/core_debug.c
    ${CORE}/core_requests.c
    ${CORE}/core_requests_CPU32.c
    ${CORE}/core_requests_HCS12.c
    ${CORE}/core_requests_JTAG.c
    ${CORE}/core_requests_MPC.c
    ${CORE}/core_worker.c
    ${CORE}/debug/dbg_hcs12.c
    ${CORE}/targets/BMW_cluster.c
    ${CORE}/targets/Trionic.c
    ${CORE}/targets/checksum/checksum.c
    ${CORE}/targets/checksum/edc16c39/csum_16c39.c
    ${CORE}/targets/checksum/edc16c39/csum_16c39_saab95.c
    ${CORE}/targets/generic.c
    ${CORE}/targets/hcs12.c
    ${CORE}/targets/mpc5566.c
    ${CORE}/targets/mpc562.c
    ${CORE}/targets/68hc08_uart.c
    ${CORE}/utils/utils.c
    ${CORE}/utils/utl_md5.c
)

target_include_directories(bdmstuff PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Fail on Qt API removed in Qt 6
target_compile_definitions(bdmstuff PRIVATE QT_DISABLE_DEPRECATED_UP_TO=0x060000)
target_link_libraries(bdmstuff PRIVATE Qt6::Widgets Threads::Threads PkgConfig::LIBUSB)
install(TARGETS bdmstuff)

cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{EMBER_QEMU})
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults" "sdkconfig.qemu")
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults" "sdkconfig.release")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ember-firmware)

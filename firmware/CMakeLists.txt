cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(DEFINED ENV{EMBER_QEMU})
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults" "sdkconfig.qemu")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults" "sdkconfig.release")
endif()

project(ember-firmware)

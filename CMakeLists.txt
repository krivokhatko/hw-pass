cmake_minimum_required(VERSION 3.16)

# The chip of the build. ESP-IDF takes it from the sdkconfig of the build,
# else from the project sdkconfig, else from the defaults files. With a
# build directory of its own (-B, -DSDKCONFIG), the project sdkconfig
# belongs to another build: skip it. The board file (boards/<name>.defaults)
# names the chip. Without a board file, the build is for the StickS3.
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
    if(SDKCONFIG)
        set(configs "${SDKCONFIG}")
    else()
        set(configs "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
    endif()
    if(SDKCONFIG_DEFAULTS)
        list(APPEND configs ${SDKCONFIG_DEFAULTS})
    elseif(DEFINED ENV{SDKCONFIG_DEFAULTS})
        list(APPEND configs $ENV{SDKCONFIG_DEFAULTS})
    endif()
    set(IDF_TARGET esp32s3)
    foreach(file ${configs})
        get_filename_component(file "${file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
        if(EXISTS "${file}")
            file(STRINGS "${file}" lines REGEX "^CONFIG_IDF_TARGET=\"[^\"]+\"$")
            if(lines)
                list(GET lines 0 line)
                string(REGEX REPLACE "CONFIG_IDF_TARGET=\"([^\"]+)\"" "\\1" IDF_TARGET "${line}")
                break()
            endif()
        endif()
    endforeach()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components that main depends on.
idf_build_set_property(MINIMAL_BUILD ON)
# The managed components differ by chip (esp_tinyusb is for the ESP32-S3
# only): one lock file for each chip.
idf_build_set_property(DEPENDENCIES_LOCK "dependencies.lock.${IDF_TARGET}")
# NimBLE: if the paired keypad has no bonding keys, do not pair again
# without the user. Else a device with the address of the keypad gets
# new keys and types. Kconfig has no option for this.
idf_build_set_property(COMPILE_DEFINITIONS "MYNEWT_VAL_BLE_RESTART_PAIR=0" APPEND)
project(hw_pass)

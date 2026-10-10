cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components that main depends on.
idf_build_set_property(MINIMAL_BUILD ON)
# NimBLE: if the paired keypad has no bonding keys, do not pair again
# without the user. Else a device with the address of the keypad gets
# new keys and types. Kconfig has no option for this.
idf_build_set_property(COMPILE_DEFINITIONS "MYNEWT_VAL_BLE_RESTART_PAIR=0" APPEND)
project(hw_pass)

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components that main depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(hw_pass)

cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS "../components")
set(TEST_COMPONENTS "bm1397 stratum" CACHE STRING "List of components to test")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/components/mbedtls/")
list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/tools/mocks/freertos/")
list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/tools/mocks/driver/")

project(esp-miner-host-test)

# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# (Not part of the boilerplate)
# This example uses an extra component for common functions such as Wi-Fi and Ethernet connection.
# set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-miner)

if(NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    add_custom_target(
      git-hooks ALL # ALL ensures this target is built by default
      COMMAND ${CMAKE_COMMAND} -E copy
              ${CMAKE_SOURCE_DIR}/ci/githooks/pre-commit
              ${CMAKE_SOURCE_DIR}/.git/hooks/pre-commit
      COMMAND chmod +x ${CMAKE_SOURCE_DIR}/.git/hooks/pre-commit
    )
endif()
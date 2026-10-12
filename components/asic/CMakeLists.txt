cmake_minimum_required(VERSION 3.16)
set(ASIC_SOURCES
    bm1373.c bm1370.c bm1368.c bm1366.c bm1397.c serial.c crc.c
    asic_common.c asic.c asic_submit.c asic_context.c asic_platform.c
    bm_hashrate.c bm_job_packet.c bm_job_midstate.c frequency_transition_bmXX.c pll.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ASIC_SOURCES} "platform/esp_idf.c"
        INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "private_include"
        REQUIRES mining_job pthread
        PRIV_REQUIRES esp_driver_uart esp_timer esp_rom freertos log)
    set(ASIC_TARGET ${COMPONENT_LIB})
else()
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        project(asic VERSION 1.0.0 LANGUAGES C)
        set(ASIC_STANDALONE ON)
    endif()
    include(TestBigEndian)
    test_big_endian(ASIC_BIG_ENDIAN)
    if(ASIC_BIG_ENDIAN)
        message(FATAL_ERROR "ASIC packet layouts currently require a little-endian target")
    endif()
    find_package(Threads REQUIRED)
    if(NOT TARGET mining_job)
        add_subdirectory(../mining_job "${CMAKE_CURRENT_BINARY_DIR}/mining_job")
    endif()
    add_library(asic STATIC ${ASIC_SOURCES} platform/host.c)
    set(ASIC_TARGET asic)
    target_include_directories(asic PUBLIC include PRIVATE private_include)
    target_link_libraries(asic PUBLIC mining_job Threads::Threads m)
    target_compile_options(asic PRIVATE -Wall -Wextra -Wno-unused-parameter)
    option(ASIC_BUILD_TESTS "Build native ASIC regression tests" ${ASIC_STANDALONE})
endif()
add_library(asic::asic ALIAS ${ASIC_TARGET})
target_compile_features(${ASIC_TARGET} PUBLIC c_std_11)
if(NOT ESP_PLATFORM AND ASIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(host_test)
endif()

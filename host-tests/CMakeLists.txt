cmake_minimum_required(VERSION 3.22)

project(esp_miner_host_tests LANGUAGES C)

include(CTest)
include(FetchContent)

# cJSON 1.7.19 predates CMake 4 and declares an older policy baseline.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

option(ESP_MINER_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

# Keep native dependencies exact and independent from packages installed on the host.
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG bf560290f6020737eafaa8b5cbd2177c3956c03f
    GIT_SHALLOW FALSE
)

set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
set(ENABLE_TARGET_EXPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG c859b25da02955fef659d658b8f324b5cde87be3
    GIT_SHALLOW FALSE
)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.0/mbedtls-4.1.0.tar.bz2
    URL_HASH SHA256=377a09cf8eb81b5fb2707045e5522d5489d3309fed5006c9874e60558fc81d10
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(unity cjson mbedtls)

target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE=1)

add_executable(
    esp_miner_host_tests
    src/host_test_runner.c
    ../components/stratum/test/test_stratum_json.c
    ../components/stratum/test/test_coinbase_decoder.c
    ../components/stratum/test/test_base58.c
    ../components/stratum/test/test_bech32.c
    ../components/stratum/test/test_mining.c
    ../components/stratum/test/test_utils.c
    ../components/stratum_v2/test/test_sv2.c
    ../components/asic/test/test_pll.c
    ../components/asic/test/test_timeout.c
    ../components/asic/test/test_asic_backend.c
    ../components/stratum/stratum_api.c
    ../components/stratum/coinbase_decoder.c
    ../components/stratum/base58.c
    ../components/stratum/mining.c
    ../components/stratum/utils.c
    ../components/stratum/segwit_addr.c
    ../components/stratum_v2/sv2_protocol.c
    ../components/asic/pll.c
    ../components/asic/asic_common.c
    ../components/asic/asic_backend.c
    src/asic_bitmain_stub.c
)

target_compile_features(esp_miner_host_tests PRIVATE c_std_11)
target_compile_definitions(
    esp_miner_host_tests
    PRIVATE
        ESP_MINER_PARSER_ONLY=1
        _POSIX_C_SOURCE=200809L
)
target_compile_options(
    esp_miner_host_tests
    PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/include/host_test_compat.h
        -ffunction-sections
        -fdata-sections
        -Wall
        -Wextra
        -Werror
)
target_link_options(esp_miner_host_tests PRIVATE -Wl,--gc-sections)

target_include_directories(
    esp_miner_host_tests
    PRIVATE
        include
        ${cjson_SOURCE_DIR}
        ../components/stratum/include
        ../components/stratum_v2/include
        ../components/asic/include
)

target_link_libraries(esp_miner_host_tests PRIVATE unity cjson TF-PSA-Crypto::tfpsacrypto m)

if(ESP_MINER_ENABLE_SANITIZERS)
    target_compile_options(esp_miner_host_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(esp_miner_host_tests PRIVATE -fsanitize=address,undefined)
endif()

add_test(NAME host.unit COMMAND esp_miner_host_tests)
set_tests_properties(host.unit PROPERTIES LABELS "host;unit")

# Native host tests

These tests compile production C sources for Linux. They do not need ESP-IDF,
an ESP32 image, or QEMU.

Dependencies are fetched at exact Git revisions during CMake configuration.
The revisions match ESP-IDF 6.0.2 for Unity, cJSON 1.7.19, and the mbedTLS
4.1.0 release used by ESP-IDF 6.0.2.

```sh
cmake -S host-tests -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --target esp_miner_host_tests
ctest --test-dir build/host --output-on-failure
```

Enable AddressSanitizer and UndefinedBehaviorSanitizer with:

```sh
cmake -S host-tests -B build/host-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DESP_MINER_ENABLE_SANITIZERS=ON
cmake --build build/host-sanitize --target esp_miner_host_tests
ctest --test-dir build/host-sanitize --output-on-failure
```

Pass a name or tag substring to run a smaller set directly:

```sh
./build/host/esp_miner_host_tests '[sv2]'
```

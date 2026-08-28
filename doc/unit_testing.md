## Unit testing

Run portable C tests on Linux first. QEMU is reserved for focused ESP32-S3
integration tests. See [the test plan](test-plan.md) for ownership rules and
hardware-only coverage.

Tests remain in each component's `test` directory. The native runner reuses
the same Unity test bodies and production C sources.

### Linux host

```sh
cmake -S host-tests -B build/host -DCMAKE_BUILD_TYPE=Debug \
  -DESP_MINER_ENABLE_SANITIZERS=ON
cmake --build build/host --target esp_miner_host_tests --parallel
ctest --test-dir build/host --output-on-failure
```

Run one name or tag directly:

```sh
./build/host/esp_miner_host_tests '[mining]'
```

The first configure downloads pinned Unity, cJSON, and mbedTLS sources.

### ESP32-S3 and QEMU

Use ESP-IDF 6.0.2 to build the focused integration image:

```
idf.py -C test-ci build
./tools/run_qemu_tests.sh
```

The QEMU runner executes only tests tagged `[qemu-integration]`. It does not
repeat the portable parser, hash, and wire-format suite.

See the [ESP-IDF unit test guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/unit-tests.html)
for target-based Unity details.

### Flashing a target test image
**NOTE: Flashing the unit test binary will replace the existing firmware on the ESP32. For example, you will no longer have access to the AxeOS web UI and must flash a release or build and flash a non-test binary to recover. Do not attempt to do this unless you are willing to spend time recovering (or have dedicated test devices)**

At the conclusion of the build, instructions are provided to flash it. To ensure esptool uses the correct serial port and has permission to do so, ensure the serial device of the esp32 is known (e.g. from `dmesg` output on connection) and ensure the user is in the `dialout` group.

```
sudo usermod -aG dialout $USER
```

For example, if the serial device is `/dev/ttyACM0`:
```
python -m esptool -p /dev/ttyACM0 --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 2MB --flash_freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/unit_test_stratum.bin
```

To view test output, the serial monitor can be used:
```
idf.py -p /dev/ttyACM0 monitor
```
(`CTRL-]` can be used to stop the monitor)

### Adding a unit test
In the following example, a unit test is added to the `foo` component.

```
$ ls -R components/foo
components/foo:
CMakeLists.txt  foo.c  include

components/foo/include:
foo.h
```

components/foo/CMakeLists.txt:
```
idf_component_register(
SRCS
    "foo.c"

INCLUDE_DIRS
    "include"

REQUIRES
)
```

components/foo/include/foo.h:
```c
#ifndef FOO_H
#define FOO_H

int foo(int i);

#endif // FOO_H
```

components/foo/foo.c:
```c
#include "foo.h"

int foo(int i) {
    return i;
}
```

A directory `components/foo/test` is created, with its test file and `CMakeLists.txt`

components/foo/test/CMakeLists.txt:
```
idf_component_register(SRC_DIRS "."
                    INCLUDE_DIRS "."
                    REQUIRES unity foo)

```

components/foo/test/test_foo.c:
```c
#include "unity.h"
#include "foo.h"

TEST_CASE("Foo returns what is provided", "[foo]")
{
    TEST_ASSERT_EQUAL_INT(42, foo(42));
}

```

The unit test application's `test/CMakeLists.txt` is modified to include `foo` in the test binary:
```diff
-set(TEST_COMPONENTS "bm1397 stratum" CACHE STRING "List of components to test")
+set(TEST_COMPONENTS "bm1397 stratum foo" CACHE STRING "List of components to test")
```

Build, flash, and monitor the test binary. Output from the new test should be present.
```
#### Running all the registered tests #####

Running Foo returns what is provided...
/home/dev/myrepos/ESP-Miner/components/foo/test/test_foo.c:4:Foo returns what is provided:PASS
...
```


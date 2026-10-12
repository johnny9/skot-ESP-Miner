# Portable ASIC component

The upstream BM1397, BM1366, BM1368, BM1370 and BM1373 drivers build as an
ESP-IDF component or a C11 static library. The component depends on the sibling
`mining_job` component, pthreads and libm. It does not depend on `main`,
`GlobalState`, Stratum, networking or ESP-IDF headers on a host.

## Native build

A little-endian POSIX host, C11 compiler and CMake 3.16+ are required. The default
build downloads no dependencies:

```sh
cmake -S components/asic -B build/asic-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/asic-host --parallel
ctest --test-dir build/asic-host --output-on-failure
```

An independent project uses `add_subdirectory(path/to/components/asic asic)` and
links `asic::asic`. Keep `components/mining_job` beside `components/asic`.
Tests default to off when the library is included by another project.

```sh
cmake -S examples/asic_host -B build/asic-example
cmake --build build/asic-example
./build/asic-example/asic_host
```

The native tests exercise all five production drivers with scripted transport:
fragmented chip detection and results, job metadata ownership, invalid/stale
work, exclusive hardware ownership, teardown, read/write failures and baud
drain failures. They also check a fixed BM1397 midstate vector. Existing Unity
tests for PLLs, timeouts and common job headers can run on a host by adding
`-DASIC_UNITY_SOURCE_DIR="$IDF_PATH/components/unity/unity/src"` at configuration.
The complete firmware regression suite remains available through
`tools/run_qemu_tests.sh`.

## Firmware integration

ESP-IDF consumers declare `REQUIRES asic`; the same driver source list is used
for both targets. The independent `examples/asic_component` project includes
only the ASIC and mining-job components:

```sh
cd examples/asic_component
idf.py set-target esp32s3
idf.py build
```

`main/miner_asic.c` translates firmware board configuration and frequency state
into the portable API. Hashrate monitor measurements stay in the application.
The default ESP backend retains upstream UART1 TX17/RX18 and 2048-byte RX/TX
buffers. Call `asic_esp_idf_configure` before I/O to select different ports/pins.

## Context and transport

Zero-initialize a caller-owned `asic_context_t`, then call `asic_context_init`
with an `asic_config_t`. Configuration includes chip model/count, core geometry,
difficulty, frequency, version mask, software midstates and voltage domains.
Keep the context at a stable address and never copy its mutex after init.

Install `asic_platform_ops_t` with `asic_platform_install(&ops, user)` before
opening channels or initializing hardware. Callbacks supply open/close, read,
write, baud changes, RX flush, TX drain, monotonic microseconds, delays and
optional logging. The table is copied; callback code and user data must outlive
all users. Reads may return fragments; zero means timeout and -1 means failure.
Writes must consume the whole buffer or fail. Read timeouts are milliseconds;
zero is a nonblocking poll. Time must advance while waiting on a read.

The host default supplies timing and logging, with hardware open returning
`ASIC_ERR_NOT_SUPPORTED`. A serial/USB or Bitaxe Raw adapter is supplied by the
application. Raw framing/leases and board power/reset supervision belong there.
`ASIC_IO_DATA` carries ASIC traffic; the callback contract reserves
`ASIC_IO_CONTROL` for an adapter's separate control channel. This upstream port
does not add the Bonanza drivers or a Raw client.

Prepare board power/reset, open the data channel with `SERIAL_init` or
`SERIAL_prepare_session`, and call `ASIC_init`. Check the detected chip count.
`ASIC_send_job` retains its own copy; callers may reuse or release their job.
`ASIC_process_work` returns borrowed driver storage containing a matched job
snapshot; copy it before the next receive. `ASIC_set_frequency(context, mhz)`
returns the applied frequency.

Legacy chip runtimes support one active hardware chain per process. A second
context is rejected before I/O, and a failed init retains ownership for retry
until destruction. Software-only contexts are independent. Serialize driver
operations as required; platform installation and context destruction require
all users to have stopped. Power down/reset the board before
`asic_context_destroy`, which frees retained jobs and closes owned channels.
The library does not perform board power sequencing.

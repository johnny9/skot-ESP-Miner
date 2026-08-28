# Test plan

This branch implements the host-first test foundation on PR 1897. The exact
base is `22fe7b54e6d63dde7474005ccd8f0a5225073b08` from the PR head named
`Instant pool switching`.

Work is isolated in branch `test/pr1897-host-qemu` and worktree
`esp-miner-pr1897-host-qemu`. The unrelated Bonanza worktree is not an input to
this branch.

## Test layers

| Layer | Owns | Current state |
| --- | --- | --- |
| Linux host | Parsers, wire formats, hashes, job math, validation, backend contracts, and fakes | 91 Unity tests under GCC, Clang, ASan, and UBSan |
| ESP32-S3 build | ESP-IDF component graph, target ABI, and production link | Test image and production firmware build with ESP-IDF 6.0.2 |
| QEMU | Behavior that needs the ESP32-S3 runtime or several firmware tasks | One focused FreeRTOS queue/task integration test |
| Hardware | Electrical, timing, peripheral, and real ASIC behavior | Manual or future hardware runner |
| AxeOS | Browser components and services | Existing frontend lane |

Run `python3 tools/test_inventory.py` to emit the active C test inventory as
JSON. Run it with `--check` to reject duplicate ownership or a portable test
that is missing from the host suite.

## Host tests first

The host suite compiles the production C files directly. It builds pinned
Unity, cJSON, and mbedTLS sources, so it does not replace parsing or hashing
with fakes.

The required order for new coverage is:

1. Safety policy, state transitions, and failure handling.
2. Untrusted input and bounds checks.
3. Job identity, clean-job behavior, stale results, and nonce validation.
4. Encoding, hashing, numeric helpers, and lower-risk utilities.

All four groups belong on the host when they can be tested with values and
injected callbacks. QEMU is not needed for cJSON, wire encoding, SHA-256,
coinbase, Base58, Bech32, Merkle, nonce, timeout, or PLL vectors.

## Focused QEMU scope

The current QEMU test uses real FreeRTOS tasks and an ESP-IDF queue. One task
drives the board fake and CPU ASIC simulator. A second task receives the
scripted ASIC result through the public backend. This checks target scheduling,
queue transfer, target pthread locks, and the integration of both device
interfaces.

Keep QEMU focused as the application becomes easier to start with simulated
devices. Add scenarios only when they cross production component or runtime
boundaries:

- application boot and task readiness;
- pool input through job creation, ASIC dispatch, result handling, and share
  submission;
- clean-job turnover followed by a stale simulated result;
- temperature, fan, or power failure causing the complete safe-state response;
- bounded ASIC timeout and restart behavior;
- ESP-IDF transport cleanup and persisted configuration across restart.

Do not move value-only assertions back into QEMU. A small assertion may be
repeated only when it proves an integration boundary.

## Simulation boundaries

`asic_backend_t` selects the production Bitmain backend or the deterministic
CPU simulator. The simulator records configuration and work, then returns
scripted results from a bounded queue.

`board_io_backend_t` covers display, power, core voltage, temperature, and fan
operations. Production binds the hardware adapter at application entry. Tests
bind a thread-safe fake with measurements, failure injection, and output
recording.

Future QEMU scenarios should extend these narrow models with scripted latency
and named fault points. Production policy must remain in production modules,
not in the fakes.

## Hardware-only coverage

Use a physical board for:

- safe GPIO and rail polarity at boot;
- UART pins, baud rate, buffering, signal integrity, and sustained traffic;
- regulator sequence, voltage accuracy, faults, and cutoff latency;
- sensor conversion and thermal response;
- fan PWM, tachometer, stall handling, and cooling performance;
- display bus and panel output;
- ASIC discovery, register access, PLL behavior, nonce delivery, and sustained
  mining.

QEMU results do not qualify these behaviors.

## Commands

```sh
python3 tools/test_inventory.py --check
cmake -S host-tests -B build/host -DCMAKE_BUILD_TYPE=Debug \
  -DESP_MINER_ENABLE_SANITIZERS=ON
cmake --build build/host --target esp_miner_host_tests --parallel
ctest --test-dir build/host --output-on-failure
```

Build the ESP32-S3 test image with ESP-IDF 6.0.2:

```sh
idf.py -C test-ci build
```

Run the focused integration image with `tools/run_qemu_tests.sh`, or use the
same `bitaxeorg/esp32-qemu-test-action@v6` action as CI.

## Next implementation steps

1. Extract one-iteration application task logic for direct host tests.
2. Add test-only boot selection for the CPU ASIC and board fake.
3. Add a machine-readable control channel for changing sensor values and ASIC
   result scripts while QEMU is running.
4. Add a fake pool and the full job-to-share QEMU scenario.
5. Add safe-state fault scenarios, then hardware runners for physical checks.

Each step must keep the host suite, focused QEMU suite, and production firmware
build green. Simulation code must never become the production default.

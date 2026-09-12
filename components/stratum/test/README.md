# Stratum test support

These modules test how pool jobs become ASIC jobs. They use the real mining and
job-task code with controlled inputs and recorded outputs.

## How the modules fit together

1. A test case creates a miner job or a list of task events.
2. The job pipeline harness runs the real job task with that input.
3. The mining test instance builds the real mining code with private test
   names.
4. Test doubles provide task events and record jobs sent to the ASIC boundary.
5. The test checks the job data, task behavior, and memory ownership.

## Support modules

| Module | Purpose |
| --- | --- |
| `job_pipeline_test_harness.*` | Runs the real `create_jobs_task()` with a short event script. It records generated jobs, version masks, delays, and coinbase decode calls. It also stops the task after the script ends. |
| `mining_test_bindings.h` | Gives the mining functions private test names and sends selected allocations through the fault injector. This keeps allocation tests separate from other test tasks. |
| `mining_test_instance.c` | Builds an isolated instance of the real mining source. |
| `mining_allocator_fault_injector.*` | Fails one selected allocation and records allocation calls. A test can then check error handling and recovery. |
| `stubs/` | Provides small replacement headers with only the platform types and state needed by these tests. |

## Tests that use these support modules

| File | Main coverage |
| --- | --- |
| `test_job_building.c` | Coinbase hashing at stack and heap limits, allocation failure recovery, ASIC job defaults, copied metadata, and software midstates. |
| `test_mining_pipeline.c` | SV1 and SV2 job conversion, exact ASIC job data, idle and staged task events, invalid and maximum metadata, job ownership, allocation recovery, empty extranonce data, and large coinbase data. |

## Other tests in this directory

| File | Main coverage |
| --- | --- |
| `test_base58.c` | Base58 P2PKH and P2SH address encoding, including a small output buffer. |
| `test_bech32.c` | Bech32 and Bech32m address encoding for several witness types and networks, including invalid inputs. |
| `test_coinbase_decoder.c` | Varint bounds, payout address decoding, network formats, BIP-110 signaling, job input, and transaction locktime checks. |
| `test_miner_job.c` | Miner-job pool slots, buffer ownership, index wraparound, and rollable-job checks. |
| `test_mining.c` | Coinbase hashes, Merkle roots, midstates, version-mask changes, and nonce difficulty. |
| `test_stratum_json.c` | SV1 JSON-RPC parsing, job fields, server messages, malformed input, line buffering, and size limits. |
| `test_utils.c` | Hashing, hex conversion, URL decoding, byte order, network difficulty, and difficulty conversion safety. |

## Test double names

- A **fake** returns scripted input, such as a task notification.
- A **spy** records a call or its data so a test can check it.
- A **stub** provides a fixed or empty implementation for behavior outside the
  test.
- A **harness** owns shared test state and connects the test doubles.
- A **test instance** builds real production source for use by the tests.
- A **fault injector** creates one requested failure for an error-path test.

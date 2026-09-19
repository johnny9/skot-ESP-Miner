# BZM and Bonanza driver port onto PR #1972

Base: upstream PR #1972, `492392e677d7d208d85f8898ad3aad00f79d9e94`.
Driver source: the local Bonanza power integration at
`474c8bcb` (`merge/bzm-power-master-20260918`).

The series separates the BZM ASIC driver and Bonanza board power/safety.
It retains PR #1972's `asic_job_t`, `task_result`, public ASIC entry points,
global-state layout, and existing power/reset/regulator
signatures. ASIC and family identities are appended to the existing enums;
board 1002 uses the existing device configuration fields.

BZM's generated work handles, retained assignments, sequence barriers,
deduplication, telemetry and runtime safety checks remain driver-specific. Results resolve
rolled time into their owned job snapshot before returning `task_result`.
The existing job producer uses four software midstates and the negotiated
version mask; a zero mask remains zero for BZM. Invalidated work cancels driver
backpressure retries. Bitmain packet encoders, drivers, and serial transport
remain unchanged.

Bonanza owns its staged bring-up, bridge lease, fan interlocks, verified
safe-off, cooling and live clock/rail transitions. Its extended TPS546 binding
is deliberately separate from the upstream TPS546 configuration structure.
This duplicates regulator implementation code to avoid widening that shared
interface. UART buffering is likewise isolated in `bzm_serial`.

Production timing, filter and safety limits are fixed constants in the owning
implementation files, preserving the original production defaults. The board
and driver share only the telemetry freshness limit. No BZM/Bonanza Kconfig
settings are added; local nonce difficulty derives from the ASIC result filter.

Existing pause, resume, firmware OTA and restart routes delegate to the board
owner on Bonanza. Self-test waits for board startup and delegates shutdown.
Bonanza uses the existing headless display mode; external display support is
outside this port. This series does not import the fork's HTTP schema, health
dashboard, new bridge-update HTTP route, generalized driver framework, or asynchronous
Stratum submission subsystem. Bridge flashing, its update API, SWD transport,
and blank-bridge recovery are excluded; normal bridge control remains.
Unused health-reporting APIs, submission-context and generic event wrappers,
legacy initialization, frequency qualification, the superseded cooling helper,
and unused regulator profiles are also excluded. Task and regulator helpers
used only within their implementation stay private.

## Validation

- ESP-IDF 6.0.2 application build passes, with 33% application partition free.
  The unchanged AxeOS bundle was reused from the PR #1972 baseline worktree.
- The complete QEMU suite reports **400 tests, 0 failures**. This includes
  existing Bitmain characterization and imported BZM protocol, parser,
  topology, work-store, deduplication, power-policy and safety tests.
  Added checks cover owned upstream result snapshots, timestamp overflow,
  and zero-mask/four-midstate job production.
- `git diff --check` passes. The common job implementation and headers,
  `asic.h`, `asic_common.h`, `global_state.h`, and public power, reset,
  initialization, thermal and regulator headers match the base.
- Local build and QEMU logs are under `.cache/driver-port/`.

No hardware was flashed or exercised for this port. Startup and self-test,
live tuning, pause/resume, firmware update/restart, and fresh accepted shares
across SV1/SV2/fallback still require validation on a board with the matching
protocol-1.0 RP2040 bridge firmware. In particular, this branch retains
upstream's synchronous pool submission path; the fork's later nonblocking
network-write behavior is outside this driver-only port.

# Solar Energy Click Controls And Indicators For nRF9160

Purpose: define what the nRF9160 can read/control from the Solar Energy Click setup, and provide a code-facing plan for integrating those signals.

Date baseline: 2026-05-12

## Source Documents Used

- docs/solar_energy_click/solar-energy-click-schematic-v100.pdf
- docs/solar_energy_click/SCH PDF.pdf (Stratus Shield V2)
- docs/solar_energy_click/conexio-stratus_v3.pdf
- https://www.mikroe.com/solar-energy-click
- conexio_board_root_v3/boards/conexio/conexio_stratus_pro/conexio_stratus_pro_common.dtsi
- conexio_board_root_v3/boards/conexio/conexio_stratus_pro/conexio_stratus_pro_common-pinctrl.dtsi

## Pin-By-Pin Trace: Solar Click -> Stratus Shield -> nRF9160

This section follows the exact three control/status pins called out in the schematics.

### 1) VOUT_EN path (Solar Click AN pin)

- Solar Energy Click schematic (`solar-energy-click-schematic-v100.pdf`):
  - Signal `VOUT_EN` is connected to mikroBUS `AN`.
- Stratus Shield schematic (`SCH PDF.pdf`, lower connector mapping):
  - mikroBUS `AN -> P0.17/A4`.
- Stratus Shield routing (`SCH PDF.pdf`, upper interconnect to module):
  - That net routes onward to the Conexio Stratus 9160.
- Conexio Stratus V3/module side (`conexio-stratus_v3.pdf`):
  - Final MCU pin is nRF9160 `P0.17`.

Firmware meaning:

- `VOUT_EN` is a control line from MCU to the BQ25570 output-enable function.
- Treat as GPIO output:
  - asserted state enables regulated output path,
  - deasserted state disables output path.

### 2) EN path (Solar Click CS pin)

- Solar Energy Click schematic (`solar-energy-click-schematic-v100.pdf`):
  - Signal `EN` is connected to mikroBUS `CS`.
- Stratus Shield schematic (`SCH PDF.pdf`, lower connector mapping):
  - mikroBUS `CS -> P0.18/A5`.
- Stratus Shield routing (`SCH PDF.pdf`, upper interconnect to module):
  - That net routes onward to the Conexio Stratus 9160.
- Conexio Stratus V3/module side (`conexio-stratus_v3.pdf`):
  - Final MCU pin is nRF9160 `P0.18`.

Firmware meaning:

- `EN` is a second digital control input for the click power-management path.
- Treat as GPIO output and initialize to a known-safe boot state before enabling harvesting/output behavior.

### 3) VBAT_OK path (Solar Click INT pin)

- Solar Energy Click schematic (`solar-energy-click-schematic-v100.pdf`):
  - Signal `VBAT_OK` is connected to mikroBUS `INT`.
- Stratus Shield schematic (`SCH PDF.pdf`, lower connector mapping):
  - mikroBUS `INT -> P0.16/A3`.
- Stratus Shield routing (`SCH PDF.pdf`, upper interconnect to module):
  - That net routes onward to the Conexio Stratus 9160.
- Conexio Stratus V3/module side (`conexio-stratus_v3.pdf`):
  - Final MCU pin is nRF9160 `P0.16`.

Firmware meaning:

- `VBAT_OK` is a status indicator from the energy-harvesting IC to MCU.
- Treat as GPIO input, ideally interrupt-driven (both edges), so firmware can react quickly to threshold crossings.
- MikroE page behavior reference:
  - low state indicates battery/storage has dropped below the lower threshold,
  - return transition indicates recovery above the upper hysteresis threshold.

## Consolidated Mapping Table

| Solar Click internal signal | mikroBUS pin | Stratus Shield mapping | nRF9160 pin | Direction (MCU view) | Typical firmware role |
| --- | --- | --- | --- | --- | --- |
| `VOUT_EN` | `AN` | `AN -> P0.17/A4` | `P0.17` | Output | Enable/disable regulated output path |
| `EN` | `CS` | `CS -> P0.18/A5` | `P0.18` | Output | Secondary enable/control line |
| `VBAT_OK` | `INT` | `INT -> P0.16/A3` | `P0.16` | Input | Battery/storage threshold status |

Status: PIN CONTINUITY FOR THESE THREE LINES IS NOW TREATED AS THE PRIMARY IMPLEMENTATION PATH.

## BQ25570 Section 7.4 Functional Modes (Summary + Firmware Use)

Source: `docs/solar_energy_click/bq25570.pdf`, Section 7.4 (Device Functional Modes), including 7.4.1 to 7.4.4.

Section 7.4 defines five functional modes:

1. Cold-start operation.
2. Main boost charger disabled (ship mode).
3. Main boost charger enabled.
4. Buck converter enabled mode.
5. Thermal shutdown.

### Enable-pin mode table (when `VSTOR > VSTOR_CHGEN`)

From the datasheet table in 7.4:

| EN | VOUT_EN | Datasheet behavior | Firmware interpretation |
| --- | --- | --- | --- |
| 0 | 0 | Buck standby mode: boost charger and VBAT_OK enabled, buck disabled | Harvest and battery management active; keep system rail off/idle |
| 0 | 1 | Boost charger, buck converter, and VBAT_OK enabled | Normal run mode: harvested energy available to regulated output |
| 1 | x | Ship mode: lowest leakage, boost off, VBAT-VSTOR PFET off, buck off, VBAT_OK disabled | Deep sleep / shipping-storage state |

`x` means don't care.

### Mode details and how code should use them

#### 1) Ship mode (`EN = 1`, `VSTOR > VSTOR_CHGEN`)

- Datasheet effect: disables boost, buck, battery management indication, and disconnects VBAT from VSTOR.
- Intended use: lowest leakage for storage/transport.
- Code use:
  - Assert `EN` only for long-duration deep-sleep or shipment.
  - Before asserting `EN`, flush telemetry and quiesce high-load peripherals.
  - On wake, deassert `EN` and allow startup settling before enabling heavy loads.

#### 2) Cold-start mode (`VSTOR < VSTOR_CHGEN`, sufficient `VIN_DC` and input power)

- Datasheet effect: cold-start converter charges VSTOR until main charger can take over; EN control does not function during cold start.
- Intended use: bootstraps from a depleted storage element.
- Code use:
  - Do not assume immediate controllability of power rails on first boot from empty storage.
  - Gate high-current app init until `VBAT_OK` indicates sufficient stored energy.
  - Expect possible staircase-like startup behavior if load is too high during transition.

#### 3) Main boost charger enabled (`EN = 0`, `VSTOR > VSTOR_CHGEN`)

- Datasheet effect: primary harvesting mode, MPPT-regulated boost charging into storage.
- Intended use: continuous energy harvesting operation.
- Code use:
  - Keep `EN` low for normal harvesting.
  - Use `VBAT_OK` interrupts to adapt workload to available energy.
  - Prefer duty-cycled sensor/radio activity to keep average load below harvested power.

#### 4) Buck converter enabled (`EN = 0`, `VOUT_EN = 1`, `VSTOR > VBAT_UV`)

- Datasheet effect: regulated output enabled from VSTOR.
- Intended use: run MCU/sensors from regulated rail when energy budget allows.
- Code use:
  - Use `VOUT_EN` as a software power gate for downstream rails.
  - Turn `VOUT_EN` off during low-energy periods (`VBAT_OK` low) to extend survival.
  - Re-enable with staged bring-up (sensors first, modem later) to avoid large transients.

#### 5) Thermal shutdown

- Datasheet effect: boost and buck are disabled at high die temperature and resume after cooling.
- Intended use: self-protection.
- Code use:
  - Treat sudden rail/charge loss as a possible thermal event.
  - Add retry logic and backoff delays after repeated power-good loss events.
  - Reduce peak load profile when thermal or repeated brownout patterns are detected.

### Practical firmware state machine for this board

Using your mapped pins (`EN -> P0.18`, `VOUT_EN -> P0.17`, `VBAT_OK -> P0.16`):

1. Boot default:
   - `EN = 0` (allow harvesting path).
   - `VOUT_EN = 0` until early checks complete.
2. If `VBAT_OK` indicates enough energy:
   - set `VOUT_EN = 1` and enter normal application run profile.
3. If `VBAT_OK` drops:
   - shed optional loads,
   - optionally set `VOUT_EN = 0` to preserve storage,
   - remain in low-duty telemetry mode.
4. Optional ultra-low leakage window:
   - set `EN = 1` (ship/deep-storage mode),
   - on scheduled wake, set `EN = 0`, wait for recharge, then re-evaluate `VBAT_OK`.

This maps datasheet mode behavior directly to software control policy so the 9160 can make deterministic energy-aware decisions.

#### Why INT Matters For Firmware

- INT can be treated as a low-cost event signal instead of polling analog values continuously.
- If INT reflects VBAT_OK threshold crossing, firmware can react immediately to harvest/storage state changes.

#### Proposed 9160 Handling Pattern (After Pin Verification)

1. Configure INT as GPIO input with interrupt on both edges.
2. Debounce in software (for example 10-50 ms guard window).
3. On each INT event:
  - capture timestamp,
  - read VBAT_OK level,
  - update state machine (storage good vs storage low),
  - emit telemetry event.
4. Add a periodic status poll as fallback in case an interrupt edge is missed.

Suggested event semantics:

- Falling edge (INT -> low): low-battery/low-storage event (below ~2.85 V).
- Rising edge (INT -> high): recovery event (around ~3.25 V, with hysteresis).

#### Devicetree Representation Suggestion

Add a named GPIO binding (example):

- solar_vbat_ok_gpios = <...P0.16...>

Then consume this label from application code rather than hardcoding pin numbers.

## Additional Solar Click Signals (Still Secondary)

The Solar Energy Click sheet shows BQ25570 nets including `VBAT_OV`, `VRDIV`, `OK_HYST`, `OK_PROG`, and `VOUT_SET`. The three key runtime pins (`VOUT_EN`, `EN`, `VBAT_OK`) are covered in the mapping above.

Treat these as integration candidates, pending explicit pin verification on the shield design or continuity test.

### Candidate Readable Indicators

- `VBAT_OK` (digital status from BQ25570 via `INT -> P0.16`)
  - Behavior: storage/output threshold condition.
  - Firmware mode: GPIO interrupt input (edge-triggered).

- Solar/output analog nodes (if routed): VSOLAR, VSTOR, VSYS, VBAT (through divider)
  - Firmware mode after verification: ADC channels with per-net conversion formulas.

### Candidate Controllable Inputs

- `VOUT_EN` via `AN -> P0.17`
  - Expected behavior: enable/disable regulated output path.
  - Firmware mode: GPIO output control.

- `EN` via `CS -> P0.18`
  - Expected behavior: control/enable signal for the click power path.
  - Firmware mode: GPIO output control.

- VBAT_OV, VOUT_SET, OK_PROG, OK_HYST, VRDIV
  - These are configuration/programming nets around BQ25570; likely strap/resistor configured on board.
  - Usually not runtime controlled unless explicitly routed to MCU GPIO/PWM/DAC path.

Status: `VOUT_EN`, `EN`, and `VBAT_OK` are now first-class mapped signals. Other listed nets remain optional/secondary unless separately routed.

## Board-Level Pin Context Relevant To Solar Click Integration

Primary Solar Click control/status GPIOs to use on nRF9160:

- `P0.17`: `VOUT_EN` (from mikroBUS `AN` / shield `A4`).
- `P0.18`: `EN` (from mikroBUS `CS` / shield `A5`).
- `P0.16`: `VBAT_OK` (from mikroBUS `INT` / shield `A3`).

These three pins should be reserved and named in DTS/application code for Solar Click control/status handling.

## Mode-Based Implementation Plan (Aligned To BQ25570 Section 7.4)

### Milestone 1: Pin + DTS Foundation (Prerequisite for all modes)

1. Add named GPIO bindings:
  - `solar_vout_en_gpios` -> `P0.17`
  - `solar_en_gpios` -> `P0.18`
  - `solar_vbat_ok_gpios` -> `P0.16`
2. Create a small driver/module that owns all three lines and exports:
  - init,
  - set/get for `EN` and `VOUT_EN`,
  - `VBAT_OK` read and callback registration.
3. Define a boot-safe default policy:
  - `EN = 0` (harvesting allowed),
  - `VOUT_EN = 0` until energy-good is confirmed.

### Milestone 2: Ship Mode Control (Section 7.4.1)

1. Implement `solar_enter_ship_mode()`:
  - quiesce modem/peripherals,
  - flush pending logs/telemetry,
  - set `EN = 1`.
2. Implement `solar_exit_ship_mode()`:
  - set `EN = 0`,
  - wait a configurable settle window,
  - re-check `VBAT_OK` before enabling loads.
3. Add persistent reason tracking for ship transitions (manual, low-energy policy, scheduled storage).

### Milestone 3: Main Boost + Buck Runtime Policy (Sections 7.4.3 and 7.4.3.1)

1. Use `VBAT_OK` interrupt on both edges as the primary state trigger.
2. On `VBAT_OK = good`:
  - enable `VOUT_EN`,
  - bring up subsystems in stages (sensors first, modem later).
3. On `VBAT_OK = low`:
  - shed optional loads,
  - optionally clear `VOUT_EN` if protection policy requires.
4. Keep `EN = 0` in normal operation so the main boost charger remains available.

### Milestone 4: Cold-Start Aware Bring-Up (Section 7.4.2)

1. Add a startup guard state that delays high-current operations until either:
  - `VBAT_OK` asserts, or
  - a conservative timeout plus repeated good samples has passed.
2. Add brownout/restart counters so repeated early resets force a reduced startup profile.
3. Add a low-duty fallback mode for poor-harvest conditions.

### Milestone 5: Thermal-Shutdown Resilience (Section 7.4.4)

1. Treat unexpected loss of charging/output availability as a potential thermal event.
2. Add retry with exponential backoff before re-enabling heavier loads.
3. Emit telemetry events for suspected thermal shutdown/recovery sequences.

### Milestone 6: Validation And Tuning

1. Verify real active polarity for `EN` and `VOUT_EN` on hardware.
2. Verify `VBAT_OK` electrical behavior (push-pull/open-drain and pull requirements).
3. Tune debounce/guard windows for `VBAT_OK` transitions.
4. Tune thresholds and timing for load-shed, buck enable, and ship entry/exit.
5. Run energy-profile tests:
  - strong light,
  - weak/intermittent light,
  - cold-start from depleted storage.

## Suggested Zephyr Devicetree Additions

After confirming routing, represent signals in DTS as named gpios/channels, for example:

- solar_vbat_ok_gpios = <...P0.16...>
- solar_vout_en_gpios = <...P0.17...>
- solar_en_gpios = <...P0.18...>

This keeps application code board-agnostic and avoids hardcoded pin numbers.

## Suggested Software Interface

Create one module (for example solar_click.c/.h) exposing:

- solar_click_init()
- solar_click_sample(struct solar_click_sample *s)
- solar_click_set_vout_enabled(bool en)
- solar_click_set_en(bool en)
- solar_click_get_status_flags()

Sample struct recommendation:

- bool vbat_ok;
- bool vout_en;
- bool en;

## Open Verification Checklist

- Confirm active polarity for `VOUT_EN` on real hardware behavior.
- Confirm active polarity for `EN` on real hardware behavior.
- Confirm `VBAT_OK` polarity/drive mode at the MCU pin and required pull configuration.
- Confirm voltage domains for all three digital lines (3V3-safe at nRF9160 GPIO).
- Confirm ship-mode entry/exit latency is acceptable for field operation.
- Confirm cold-start recovery under minimum expected harvest power.
- Confirm behavior under repeated thermal or brownout-like events.

## Bring-Up Smoke Test Checklist (Post-Flash)

Use this checklist immediately after flashing to validate the implemented Solar Click control/status path.

### 1) Boot And Pin Init

- Flash firmware and open serial logs.
- Confirm module init logs appear for:
  - `EN` control pin,
  - `VOUT_EN` control pin,
  - `INT/VBAT_OK` monitoring.
- Confirm default startup policy is applied:
  - `EN = 0`
  - `VOUT_EN = 0`

Pass criteria:

- No GPIO init error lines.
- Defaults are reported/applied exactly once at boot.

### 2) VBAT_OK Interrupt Path

- With stable power available, observe initial `VBAT_OK`/INT level event log.
- Force a low-energy condition (for example by reducing input harvest power) until threshold crossing occurs.
- Restore energy input and confirm recovery event occurs.

Pass criteria:

- Exactly one low-battery event per threshold crossing (no event storm).
- Exactly one recovery event when storage rises back above hysteresis threshold.

### 3) VOUT_EN Control Behavior

- Toggle `VOUT_EN` through firmware path (or temporary debug hook) from 0 -> 1 -> 0.
- Verify regulated output rail behavior follows command state.

Pass criteria:

- `VOUT_EN=1` enables expected downstream rail/load behavior.
- `VOUT_EN=0` disables the same behavior consistently.

### 4) EN (Ship-Control Line) Behavior

- Toggle `EN` through firmware path (or temporary debug hook) from 0 -> 1 -> 0.
- Verify system enters/leaves low-leakage behavior as expected by board design.

Pass criteria:

- `EN=1` corresponds to ship/disable behavior expected from BQ25570 mode table.
- `EN=0` restores normal harvest/runtime path.

### 5) Basic Stability Check

- Run for at least 10 minutes under normal light/power input.
- Confirm no repeated GPIO re-init, interrupt chatter loops, or unexpected resets.

Pass criteria:

- Stable logging cadence.
- No watchdog reset or brownout reset symptoms during this short run.

## Validation Run Results (2026-05-14)

Source: live UART log after flashing the sysbuild image and running `solar` CLI commands.

### Checklist Status Summary

1. Boot And Pin Init: PASS
   - Observed:
     - `[solar] EN control enabled on P0.18 (default=0)`
     - `[solar] VOUT_EN control enabled on P0.17 (default=0)`
     - `[solar] INT monitoring enabled on P0.16`
     - `[solar] initialization complete (EN=0, VOUT_EN=0)`

2. VBAT_OK Interrupt Path: PARTIAL PASS
   - Observed:
     - Initial/recovery path works (`INT level=1 -> battery-recovered`, callback fired).
     - CLI read confirms current level (`VBAT_OK level=1`).
   - Still needed:
     - Force a true low-energy threshold crossing and capture the low event transition.

3. VOUT_EN Control Behavior: PASS (software control path)
   - Observed:
     - `solar vout 1` -> `[solar-cli] VOUT_EN set to 1`
     - `solar vout 0` -> `[solar-cli] VOUT_EN set to 0`
   - Still recommended:
     - Correlate with measured rail/load effect on hardware instrument.

4. EN (Ship-Control Line) Behavior: PASS (software control path)
   - Observed:
     - `solar en 1` -> `[solar-cli] EN set to 1`
     - `solar en 0` -> `[solar-cli] EN set to 0`
   - Still recommended:
     - Correlate with measured low-leakage or expected board-level behavior.

5. Basic Stability Check: PASS
   - Observed:
     - Continuous run beyond 90 seconds with no reset loop, no watchdog reset, and no repeated GPIO init failures.

### Additional Runtime Finding (Not Solar GPIO Logic)

- Repeated Golioth CoAP errors were observed:
  - `Failed to allocate packet buffer`
  - `Failed to create new CoAP GET request: -12`
  - followed by reconnect attempts.

Interpretation:

- This indicates runtime memory pressure in the networking/Golioth path (consistent with high RAM utilization in build output), not a direct failure of Solar Click pin control.

Recommended follow-up:

1. Reduce telemetry publish pressure/frequency during GNSS acquisition windows.
2. Audit Golioth buffer/context counts versus available RAM.
3. Re-check memory headroom after any feature additions.

## Practical Bottom Line

What can be used with confidence now:

- `VOUT_EN`: `AN -> P0.17` (GPIO output).
- `EN`: `CS -> P0.18` (GPIO output).
- `VBAT_OK`: `INT -> P0.16` (GPIO input/interrupt).

This enables immediate firmware work around explicit control and status signaling for the Solar Energy Click power path.
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

## High-Confidence Mapping (Safe To Implement Now)

### 1) Solar Analog Measurement (AN)

- Click side: mikroBUS AN pin is present on Solar Energy Click schematic.
- Shield side: net name SOL is present and tied to P0.13/A0 in the shield schematic text.
- 9160 side: board DTS maps D13/A0 to gpio0 pin 13.
- Firmware use:
  - Primary: ADC sampling on channel connected to P0.13 (A0).
  - Meaning: solar-harvest related analog level (use scaling/calibration in software).

Status: CONFIRMED (best available evidence from current schematics + board DTS).

### 2) INT Pin (Indicator Candidate)

- Click side: Solar Energy Click exposes mikroBUS INT.
- Device behavior context: BQ25570 provides VBAT_OK status signaling and is commonly used to indicate when stored energy crosses configured thresholds.
- Schematic evidence in this repo:
  - Solar Energy Click sheet shows VBAT_OK signal present at the click-board level.
  - Stratus Shield schematic text shows the mikroBUS INT pin in the routed signal set.
- Working assumption for firmware planning: INT is likely used as a digital status indicator path (most likely tied to VBAT_OK behavior).

Website-confirmed behavior (MikroE product page):

- The interrupt pin is routed to mikroBUS INT.
- INT is driven LOW when battery voltage drops below 2.85 V.
- INT returns when battery rises to about 3.25 V (hysteresis behavior stated on page).

Firmware interpretation from the above:

- Treat INT as active-low low-battery/storage-warning indicator.
- Rising edge likely indicates recovery above the hysteresis upper threshold.

Status: HIGH CONFIDENCE FOR LOGIC BEHAVIOR; FINAL MCU PIN CONTINUITY STILL TO VERIFY.

#### Why INT Matters For Firmware

- INT can be treated as a low-cost event signal instead of polling analog values continuously.
- If INT reflects VBAT_OK threshold crossing, firmware can react immediately to harvest/storage state changes.

#### Proposed 9160 Handling Pattern (After Pin Verification)

1. Configure INT as GPIO input with interrupt on both edges.
2. Debounce in software (for example 10-50 ms guard window).
3. On each INT event:
   - capture timestamp,
   - sample solar analog (AN/A0),
   - update state machine (harvest available, storage good, storage low),
   - emit telemetry event.
4. Keep periodic analog sampling as fallback in case INT is missed.

Suggested event semantics:

- Falling edge (INT -> low): low-battery/low-storage event (below ~2.85 V).
- Rising edge (INT -> high): recovery event (around ~3.25 V, with hysteresis).

#### Devicetree Representation Suggestion

After routing is confirmed, add a named GPIO binding (example):

- solar_int_gpios = <...>

Then consume this label from application code rather than hardcoding pin numbers.

## Additional Solar Click Signals (Present On Click, Routing Must Be Verified)

The Solar Energy Click sheet shows BQ25570 nets including VOUT_EN, VBAT_OK, VBAT_OV, VRDIV, OK_HYST, OK_PROG, and VOUT_SET. However, PDF text extraction does not unambiguously prove each one reaches a specific 9160 pin through the shield.

Treat these as integration candidates, pending explicit pin verification on the shield design or continuity test.

### Candidate Readable Indicators

- VBAT_OK (digital status from BQ25570)
  - Expected behavior: indicates storage/output threshold condition.
  - Firmware mode after verification: GPIO input, optional interrupt.

- INT (mikroBUS interrupt/status line from Solar Energy Click path)
  - Expected behavior: digital indicator transition tied to energy/storage status.
  - Firmware mode after verification: GPIO interrupt input (edge-triggered).

- Solar/output analog nodes (if routed): VSOLAR, VSTOR, VSYS, VBAT (through divider)
  - Firmware mode after verification: ADC channels with per-net conversion formulas.

### Candidate Controllable Inputs

- VOUT_EN / EN
  - Expected behavior: enable/disable regulated output path.
  - Firmware mode after verification: GPIO output control.

- VBAT_OV, VOUT_SET, OK_PROG, OK_HYST, VRDIV
  - These are configuration/programming nets around BQ25570; likely strap/resistor configured on board.
  - Usually not runtime controlled unless explicitly routed to MCU GPIO/PWM/DAC path.

Status: PRESENT ON CLICK; ROUTING TO 9160 NOT YET CONFIRMED.

## Board-Level Pin Context Relevant To Solar Click Integration

From current Stratus board definitions:

- D13/A0 -> P0.13 (usable for SOL analog input).
- SPI bus available in pinctrl:
  - SCK P0.19
  - MISO P0.20
  - MOSI P0.21
- Sensor I2C already allocated:
  - SDA P0.26
  - SCL P0.27

Implication: if any Solar Click control/status lines are wired to mikroBUS SPI/UART/I2C/GPIO pins, software support can be added, but exact net-to-pin mapping must be locked first.

## Code Modification Plan

## Phase 1 (Implement Immediately)

1. Add Solar ADC acquisition path on A0/P0.13.
2. Publish/report the measured value in telemetry.
3. Add filtering + calibration constants (raw ADC -> volts).
4. Add thresholds for basic indicators in software:
   - solar_present
   - solar_low
   - solar_good

## Phase 2 (After Hardware Routing Verification)

1. Add GPIO input for VBAT_OK (if connected).
2. Add GPIO output for VOUT_EN/EN (if connected).
3. Add optional interrupt-driven status changes.
4. Add telemetry fields:
   - vbat_ok
   - vout_enabled
   - harvest_state

## Suggested Zephyr Devicetree Additions

After confirming routing, represent signals in DTS as named gpios/channels, for example:

- solar_an = adc channel on P0.13
- solar_vbat_ok_gpios = <...>
- solar_vout_en_gpios = <...>

This keeps application code board-agnostic and avoids hardcoded pin numbers.

## Suggested Software Interface

Create one module (for example solar_click.c/.h) exposing:

- solar_click_init()
- solar_click_sample(struct solar_click_sample *s)
- solar_click_set_output_enabled(bool en)   // only if VOUT_EN is confirmed
- solar_click_get_status_flags()

Sample struct recommendation:

- float solar_input_v;
- float storage_v;        // optional, if available
- float system_v;         // optional, if available
- bool vbat_ok;           // optional, if wired
- bool output_enabled;    // optional, if controllable

## Open Verification Checklist (Do Before Phase 2)

- Verify exact mikroBUS net mapping on Stratus Shield V2 for:
  - RST, CS, PWM, INT, RX, TX, SCL, SDA
- Verify which of these nets are actually connected to Solar Click BQ25570 control/status pins.
- Verify INT electrical polarity and output type at the MCU pin (active-high vs active-low, push-pull vs open-drain).
- Confirm voltage domains for any digital status/control line (3V3-safe at nRF9160 GPIO).
- Confirm whether onboard pull-ups/pull-downs exist for control/status lines.

## Practical Bottom Line

What can be used with confidence now:

- Solar analog read path via AN -> SOL -> A0/P0.13.

What likely can be added next, pending pin proof:

- VBAT_OK digital status read.
- VOUT_EN (or EN) digital control.

This split lets firmware work begin immediately while preventing incorrect GPIO assumptions.
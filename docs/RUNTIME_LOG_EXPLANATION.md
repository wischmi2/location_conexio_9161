# Runtime Log Explanation

Date: 2026-05-06

## Executive Summary

This log shows a healthy boot and connection sequence for an nRF91-based app using LTE + Golioth CoAP.

Main points:
- Device boots correctly through TF-M and Zephyr.
- LTE registration completes successfully.
- DNS and UDP path to Golioth works.
- Golioth client connects and stays connected.
- Time sync succeeds.
- Location library initializes and starts GNSS request.
- Ongoing periodic CoAP activity appears normal keepalive/maintenance traffic.

No clear fatal error is visible in this snippet.

## What Happened, In Order

### 1) Early modem/network events before reboot banner

- You see lines like `+CSCON` and `[lte] event type: 3` before the boot banner.
- Then boot messages appear:
  - `All pins have been configured as non-secure`
  - `Booting TF-M v2.2.0`
  - `*** Booting nRF Connect SDK ...`

Interpretation:
- The snippet likely contains tail from a previous runtime session, followed by a new reboot/start.

### 2) Secure/non-secure boot path is good

- TF-M starts: secure image init and hard-float ABI shown.
- Zephyr + NCS versions print.
- Golioth SDK version prints.
- Application banner prints: `Location sample started`.

Interpretation:
- Secure boot chain and app startup are successful.

### 3) LTE startup and registration

- App says `Connecting to LTE...` and registers LTE handler.
- Registration moves through temporary searching state to registered:
  - `+CEREG: 2 ...` (searching/trying)
  - `+CEREG: 5 ...` (registered roaming; commonly normal depending SIM/operator)
- App reports `Connected to LTE`.

Interpretation:
- LTE attach worked.
- Network registration is stable enough for data.

### 4) Time synchronization

- Date/time callback fires and returns success:
  - `[time] date_time event: 0`
  - `[time] wait returned: 0`
  - `[time] current time is valid`

Interpretation:
- System time became valid, which is important for TLS/DTLS certificate-time checks and general cloud operation.

### 5) Network readiness + DNS + socket probe

- App waits for usable interface and checks `net_if_up`.
- `net_if_up returned: -120` is printed, but app then reports LTE interface ready.
- DNS resolution for `coap.golioth.io:5684` succeeds on first attempt.
- Raw UDP connect probe returns success (`0 errno=0`).

Interpretation:
- Despite the `net_if_up` return code, the practical data path is ready.
- DNS and UDP path are confirmed working before client creation.

### 6) Golioth client creation and connect

- Client is created successfully.
- Connect attempt starts and targets an IP for Golioth.
- Then:
  - `Golioth CoAP client connected`
  - `Connected to Golioth`
  - wait for connection returns `0`

Interpretation:
- Cloud session establishment is successful.

### 7) Location subsystem startup

- Location library initializes successfully.
- App requests high-accuracy GNSS location.

Interpretation:
- Positioning flow has begun after cloud and time readiness, which is a sensible order.

### 8) Repeating CoAP + LTE control-plane activity (steady state)

Recurring pattern every ~8-10 seconds:
- `coap_io_loop_once: Handle EMPTY`
- `RX Non-empty`
- `Next timeout: ...`
- `+CSCON: 1` then `+CSCON: 0`
- occasional `+CEREG: 5 ...`

Interpretation:
- This is normal for an active LTE + CoAP client:
  - `Handle EMPTY` and `RX Non-empty` are expected CoAP loop/keepalive behavior.
  - `+CSCON` toggling indicates RRC connected/idle transitions (power-save related radio state changes).
  - Repeated `CEREG: 5` updates can occur as periodic network status URCs.

## Meaning of the Most Important Markers

- `CEREG: 5`:
  - Registered to LTE network (roaming status); often valid/expected.
- `CSCON: 1/0`:
  - 1 means signaling connection active, 0 means idle; normal toggling under traffic bursts.
- `Golioth CoAP client connected`:
  - Cloud link is up.
- `coap_io_loop_once: Handle EMPTY`:
  - CoAP maintenance cycle, typically not an error.
- `RX Non-empty`:
  - Received CoAP data/ack/response.

## Is Anything Wrong Here?

From this snippet alone, no critical failure is evident.

Potential watch items (not immediate failures):
- No GNSS fix/result lines are shown yet in the snippet.
- `net_if_up returned: -120` may be worth keeping for diagnostics, but subsequent successful DNS/UDP/cloud connect strongly suggests runtime recovered/continued correctly.

## Suggested Next Checks

1. Confirm downstream location events appear (latitude/longitude + accuracy).
2. Confirm cloud publish lines for location payload are present (stream set success).
3. Track reconnect behavior over longer runtime (10-30 minutes) to verify session stability.
4. If power is a concern, profile current consumption during `CSCON` 1/0 cycles to tune reporting/keepalive cadence.

## LTE Event Type Cheat Sheet (from your NCS version)

Source used: `C:/ncs/v3.2.0/nrf/include/modem/lte_lc.h`.

Important: numeric values can differ across SDK versions. These mappings are correct for your current NCS v3.2.0 header.

### Event IDs seen in your log

- `event type: 1` -> `LTE_LC_EVT_PSM_UPDATE`
  - Power Saving Mode parameter update.
- `event type: 3` -> `LTE_LC_EVT_RRC_UPDATE`
  - RRC state change; aligns with `+CSCON: 1/0` transitions.
- `event type: 4` -> `LTE_LC_EVT_CELL_UPDATE`
  - Serving cell update; often appears near `+CEREG` URCs.
- `event type: 5` -> `LTE_LC_EVT_LTE_MODE_UPDATE`
  - LTE mode update event.
- `event type: 14` -> `LTE_LC_EVT_PDN`
  - PDN event; matches lines like `+CGEV: ME PDN ACT 0`.

### Full baseline mapping (for your header)

- `0` -> `LTE_LC_EVT_NW_REG_STATUS`
- `1` -> `LTE_LC_EVT_PSM_UPDATE`
- `2` -> `LTE_LC_EVT_EDRX_UPDATE`
- `3` -> `LTE_LC_EVT_RRC_UPDATE`
- `4` -> `LTE_LC_EVT_CELL_UPDATE`
- `5` -> `LTE_LC_EVT_LTE_MODE_UPDATE`
- `6` -> `LTE_LC_EVT_TAU_PRE_WARNING`
- `7` -> `LTE_LC_EVT_NEIGHBOR_CELL_MEAS`
- `8` -> `LTE_LC_EVT_MODEM_SLEEP_EXIT_PRE_WARNING`
- `9` -> `LTE_LC_EVT_MODEM_SLEEP_EXIT`
- `10` -> `LTE_LC_EVT_MODEM_SLEEP_ENTER`
- `11` -> `LTE_LC_EVT_MODEM_EVENT`
- `12` -> `LTE_LC_EVT_RAI_UPDATE`
- `13` -> `LTE_LC_EVT_ENV_EVAL_RESULT`
- `14` -> `LTE_LC_EVT_PDN`
- `15` -> `LTE_LC_EVT_CELLULAR_PROFILE_ACTIVE`

### Why your log prints only numbers

Your handler currently logs unknown/non-special cases as:

`[lte] event type: %d`

and only handles `LTE_LC_EVT_NW_REG_STATUS` explicitly in its own `case`. So all other event enums are shown as raw integers.

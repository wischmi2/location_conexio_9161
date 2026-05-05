# Conexio Stratus Pro 9161 -> Golioth Location Relay Plan

## Goal
Update this Nordic Location sample so your Conexio Stratus Pro 9161 publishes each location fix (GNSS/cellular/Wi-Fi) to Golioth.

## Current App Baseline
- The app already acquires location and prints latitude/longitude in the location callback.
- Best integration point: `LOCATION_EVT_LOCATION` in `src/main.c`.
- LTE attach and modem init are already handled in `main()`.

## Target Data Flow
1. Device acquires location via existing Location library flow.
2. On each successful fix, app builds a compact JSON payload.
3. App publishes payload to Golioth LightDB Stream.
4. Golioth stores stream entries and can route them to dashboards/integrations.

Suggested stream path:
- `location/fix`

Suggested payload:
~~~json
{
  "lat": 12.893755,
  "lon": 55.575879,
  "acc_m": 2.8,
  "method": "GNSS",
  "ts": "2026-04-30T12:34:56Z"
}
~~~

## Implementation Plan

### Phase 1: Golioth project and credentials ✅ COMPLETE
1. Logged in via `goliothctl login` (OAuth browser flow).
2. Project: `emerald-mature-damselfly` (set as active project).
3. Device provisioned via `goliothctl provision`.
4. Recorded credentials:
   - **Host:** `coap.golioth.io`
   - **PSK-ID:** `stratus9161-psk@emerald-mature-damselfly`
   - **PSK:** *(set during provisioning — store securely, do not commit to git)*
   - **Device name in console:** `stratus9161`

Deliverable:
- ✅ Verified credentials ready for firmware configuration.

### Phase 2: Add Golioth SDK/module to the build
Use the approach your repo/workspace already uses for external modules:

Option A (recommended if you use West manifest):
1. Add Golioth firmware SDK as a West project/module.
2. Run `west update`.

Option B (if your workspace uses direct modules):
1. Add Golioth SDK under a modules folder accepted by your Zephyr setup.
2. Ensure Zephyr discovers the module on configure.

Deliverable:
- Build system can resolve Golioth headers/libraries.

### Phase 3: Kconfig integration
Create a dedicated overlay config for Golioth so base behavior remains clean.

Add a new file:
- `overlay-golioth.conf`

Add/enable (names may vary slightly by SDK version; verify in your installed Golioth module Kconfig):
- `CONFIG_GOLIOTH_SYSTEM_CLIENT=y`
- `CONFIG_GOLIOTH_LIGHTDB_STREAM=y`
- `CONFIG_GOLIOTH_SYSTEM_CLIENT_PSK_ID="<your-psk-id>"`
- `CONFIG_GOLIOTH_SYSTEM_CLIENT_PSK="<your-psk>"`
- `CONFIG_GOLIOTH_SYSTEM_CLIENT_HOST="coap.golioth.io"`

Useful support options:
- `CONFIG_JSON_LIBRARY=y`
- `CONFIG_SNTP=y` (if you need wall-clock timestamps and don’t rely only on modem/date_time)
- Logging buffer/stack increases if needed

Security note:
- Prefer injecting PSK values at build time via fragment, sysbuild/Kconfig overlay, or CI secrets instead of hardcoding in tracked files.

Deliverable:
- Firmware config contains all required Golioth capabilities.

### Phase 4: Application code changes
Primary file:
- `src/main.c`

Edits:
1. Add a small Golioth init function called after LTE attach.
2. Add a publish helper that accepts location data and serializes JSON.
3. In `LOCATION_EVT_LOCATION`, call the publish helper after existing prints.
4. Add lightweight retry/backoff behavior if publish fails.

Recommended structure:
- `golioth_client_init()`
- `golioth_publish_location(const struct location_data *loc, enum location_method method)`

Behavior guidance:
- Do not block location callback for long periods.
- If publish API is synchronous in your SDK version, queue data to a worker thread/work item.
- Publish only on successful fixes, not on timeout/error events.

Deliverable:
- Every successful location fix is sent to Golioth.

### Phase 5: Build + flash for Stratus Pro 9161
If you are unsure of board target name, list and filter boards first:
~~~powershell
west boards | findstr /I "9161 stratus conexio"
~~~

Build with Golioth overlay (replace `<board_target>`):
~~~powershell
west build -p -b <board_target> -- -DEXTRA_CONF_FILE=overlay-golioth.conf
~~~

Flash:
~~~powershell
west flash
~~~

Deliverable:
- Device running firmware that includes Golioth relay path.

### Phase 6: Verification and acceptance tests
1. Open device console logs and confirm:
   - LTE connected
   - location fixes continue to appear
   - publish success lines appear
2. In Golioth Console, confirm stream entries under `location/fix`.
3. Verify at least these scenarios:
   - GNSS fix publish
   - cellular fallback publish
   - network drop and reconnect recovery

Acceptance criteria:
- At least 5 consecutive fixes received in Golioth.
- No crash/resets during a 30+ minute run.
- Publish failures recover automatically after network returns.

## Minimal Code Change Map
- `src/main.c`
  - Add Golioth includes and globals
  - Initialize client after LTE registration success path
  - Publish in `LOCATION_EVT_LOCATION`
- `prj.conf`
  - Keep existing location/LTE config
  - Move Golioth-specific options into overlay if possible
- `overlay-golioth.conf` (new)
  - All Golioth and credential config
- `README.rst` (optional)
  - Add a short “Build with Golioth” section

## Rollout Strategy
1. First boot with publish disabled but client connected (connectivity validation).
2. Enable publish at low rate (for example periodic only).
3. Enable publish for all successful fixes.
4. Add production safeguards (rate limiting, retry jitter, watchdog-friendly behavior).

## Common Pitfalls
- Wrong board target or secure/non-secure variant mismatch.
- PSK formatting mistakes (hex/text mismatch per SDK expectations).
- Blocking publish in callback causing timing issues.
- Insufficient heap/stack after adding cloud client.
- Time not valid when generating ISO timestamps.

## Optional Enhancements
- Add a message sequence number (`seq`) to detect drops.
- Include LTE metadata (cell ID, RSRP) with each fix.
- Add configurable publish interval via Kconfig.
- Add remote control path (RPC/Settings) to enable/disable reporting.

## Execution Checklist
- [ ] Create Golioth device + PSK credentials
- [ ] Add Golioth module to workspace build
- [ ] Add `overlay-golioth.conf`
- [ ] Implement Golioth init in `src/main.c`
- [ ] Publish each `LOCATION_EVT_LOCATION` event
- [ ] Build/flash on Stratus Pro 9161
- [ ] Confirm data in Golioth console
- [ ] Run 30+ minute stability test

## Notes for This Repository
- Existing code location callback: `src/main.c`
- Existing sample config: `prj.conf`
- Existing app docs: `README.rst`

If you want, the next step is to implement these changes directly in code (overlay + `src/main.c`) and keep credentials in a local, non-committed conf fragment.

# Build Instructions (Conexio Stratus Pro)

This file is the source of truth for building this firmware in this repository.

The goal is: run these steps and get a successful build on the first try.

## One-Time Facts For This Repo

- Build system: Zephyr + NCS sysbuild (application + MCUboot + TF-M).
- This board target is valid in this repo today: `conexio_stratus_pro/nrf9160/ns`.
- This board target is not valid in this repo today: `conexio_stratus_pro/nrf9161/ns`.
- `BOARD_ROOT` is required so Zephyr can find the custom Conexio board definitions.
- `credentials.conf` must exist locally in repo root.

## Prerequisites

Before building, verify these paths exist on your machine:

- `C:/ncs/v3.2.0/zephyr`
- `C:/ncs/toolchains/66cdf9b75e/opt/zephyr-sdk`
- `C:/ncs/v3.2.0/modules/lib/golioth-firmware-sdk`
- `C:/Users/Brian/location/conexio_board_root_v3`
- `C:/Users/Brian/location/credentials.conf`

If any are missing, fix that first.

## Daily Build Steps (Use Exactly)

Run from repo root:

- `C:/Users/Brian/location`

### Step 1: Set NCS Environment In Current PowerShell

```powershell
$env:ZEPHYR_BASE="C:/ncs/v3.2.0/zephyr"
$env:ZEPHYR_TOOLCHAIN_VARIANT="zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR="C:/ncs/toolchains/66cdf9b75e/opt/zephyr-sdk"
```

### Step 2: Run Build Command

```powershell
west build --sysbuild -p always -d build -b conexio_stratus_pro/nrf9160/ns -- "-DBOARD_ROOT=C:/Users/Brian/location/conexio_board_root_v3" "-DZEPHYR_EXTRA_MODULES=C:/ncs/v3.2.0/modules/lib/golioth-firmware-sdk" "-DEXTRA_CONF_FILE=overlay-golioth.conf;credentials.conf"
```

## Cross-Machine Build Checks (If Building On Another Computer)

If another AI or developer builds this project on a different machine, check these items before the first build.

### 1) NCS Version Must Match Project Expectations

- This guide is validated with NCS v3.2.0.
- Different NCS versions can change Kconfig symbols, board support, sysbuild behavior, TF-M integration, and module APIs.
- If NCS version differs, update all related paths and expect potential config drift.

### 2) Absolute Paths Must Be Updated For That Machine

Update these to real paths on the new computer:

- `ZEPHYR_BASE`
- `ZEPHYR_SDK_INSTALL_DIR`
- `BOARD_ROOT`
- `ZEPHYR_EXTRA_MODULES` (Golioth SDK location)
- repository root path in `Set-Location`

If any one path is wrong, build typically fails during CMake board discovery or module loading.

### 3) Board Definitions Must Exist And Match The Selected Target

- Confirm board files exist under `conexio_board_root_v3/boards/conexio/conexio_stratus_pro`.
- Confirm target in this repo is still `conexio_stratus_pro/nrf9160/ns`.
- Do not assume `nrf9161/ns` works unless board files were explicitly updated to support it.

### 4) Local Secrets/Overlay Files Must Be Present

- `credentials.conf` must exist in repo root and include required credentials.
- `overlay-golioth.conf` must exist and be compatible with the installed NCS/module versions.
- Missing or malformed credential/config files may fail configure or runtime behavior.

### 5) Toolchain Components Must Be Installed And On PATH

Confirm these tools are available:

- `west`
- `cmake`
- `ninja`
- `python`
- Zephyr SDK toolchain binaries

If tool versions are very different from the validated setup, build behavior may change.

### 6) Shell Environment Must Be NCS-Oriented For That Session

- Build in a shell where `ZEPHYR_BASE` points to NCS Zephyr, not a standalone Zephyr checkout.
- If the shell has stale env vars from previous projects, clear/reset them before setting the values in this document.
- This avoids undefined Kconfig symbol floods and configure aborts.

### 7) Clean Build Directory On Environment Changes

- If changing NCS version, board files, or overlay set, use a pristine build (`-p always`) exactly as shown.
- Reusing stale CMake cache from another environment can produce misleading errors.

### 8) Windows-Specific Path/Quoting Considerations

- Keep the quoted CMake args exactly as shown for PowerShell.
- If paths contain spaces on another machine, keep them quoted.
- Use forward slashes in paths as shown to avoid escaping issues.

### 9) Quick Preflight Command Block For A New Machine

Run this first and verify output before building:

```powershell
west --version
cmake --version
ninja --version
python --version
Test-Path C:/ncs/v3.2.0/zephyr
Test-Path C:/ncs/toolchains/66cdf9b75e/opt/zephyr-sdk
Test-Path C:/Users/Brian/location/conexio_board_root_v3
Test-Path C:/ncs/v3.2.0/modules/lib/golioth-firmware-sdk
Test-Path C:/Users/Brian/location/credentials.conf
```

On a different machine, replace those paths with that machine's actual locations.

### 10) Minimal Data Another AI Needs To Build Successfully

When handing this to another AI, provide:

- NCS version
- exact filesystem paths for Zephyr, Zephyr SDK, board root, and Golioth module
- confirmed board target
- whether `credentials.conf` is present
- exact command block from this file with path substitutions

If those five inputs are correct, first-attempt build success is highly likely.

That is the known-good command sequence from the successful build on May 12, 2026.

## Expected Success Outputs

Primary artifacts:

- `build/location/zephyr/zephyr.elf`
- `build/location/zephyr/zephyr.signed.bin`
- `build/merged.hex`

Other commonly used outputs:

- `build/location/zephyr/zephyr.hex`
- `build/location/zephyr/zephyr.bin`
- `build/dfu_application.zip`

## Fast Validation Commands

```powershell
Test-Path build/location/zephyr/zephyr.elf
Test-Path build/location/zephyr/zephyr.signed.bin
Test-Path build/merged.hex
```

Each should return `True`.

## Issues Found During Real Build Attempts (And Exact Fixes)

These are the actual failure modes hit in this repo and how to fix them immediately.

### Issue 1: Board Not Found

Symptom:

- `No board named 'conexio_stratus_pro' found.`

Root cause:

- Missing `-DBOARD_ROOT=...` for custom board definitions.

Fix:

- Add `"-DBOARD_ROOT=C:/Users/Brian/location/conexio_board_root_v3"` to west build command.

### Issue 2: Invalid nrf9161 Qualifier

Symptom:

- `Board qualifiers '/nrf9161/ns' for board 'conexio_stratus_pro' not found.`
- Valid targets shown were only `conexio_stratus_pro/nrf9160` and `conexio_stratus_pro/nrf9160/ns`.

Root cause:

- Current board definition in this repo only provides nrf9160 targets.

Fix:

- Use `-b conexio_stratus_pro/nrf9160/ns`.

### Issue 3: Kconfig Warnings Escalate To Abort

Symptom:

- Many symbols shown as undefined (for example `NRF_MODEM_LIB`, `LOCATION`, `NRF_CLOUD`, etc.).
- Build stops with: `error: Aborting due to Kconfig warnings`.

Root cause:

- Shell was using wrong Zephyr tree (upstream Zephyr at `C:/Users/Brian/zephyr`) instead of NCS Zephyr.

Fix:

- Set these before build in current shell:
	- `ZEPHYR_BASE=C:/ncs/v3.2.0/zephyr`
	- `ZEPHYR_TOOLCHAIN_VARIANT=zephyr`
	- `ZEPHYR_SDK_INSTALL_DIR=C:/ncs/toolchains/66cdf9b75e/opt/zephyr-sdk`

### Issue 4: Conflicting/Outdated Build Command In Older Notes

Symptom:

- Build command appears reasonable but fails repeatedly depending on shell state.

Root cause:

- Command missing `BOARD_ROOT`, and board target set to nrf9161 qualifier not present in current board definitions.

Fix:

- Use the exact command in this file; do not substitute board target unless board files are updated.

## Known Warnings That Did Not Block Successful Build

The following were seen in successful builds and are informational for now:

- Deprecated symbol warnings (for example `NRF_CLOUD_REST`, `MBEDTLS_LEGACY_CRYPTO_C`).
- MCUboot default signing key warning (debug/non-production key warning).
- Partition Manager warning about no `pm_static.yml`.

These do not prevent build completion, but they should be reviewed for production readiness.

## Copy/Paste Daily Build Block

Use this block as-is in a fresh PowerShell:

```powershell
Set-Location C:/Users/Brian/location
$env:ZEPHYR_BASE="C:/ncs/v3.2.0/zephyr"
$env:ZEPHYR_TOOLCHAIN_VARIANT="zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR="C:/ncs/toolchains/66cdf9b75e/opt/zephyr-sdk"
west build --sysbuild -p always -d build -b conexio_stratus_pro/nrf9160/ns -- "-DBOARD_ROOT=C:/Users/Brian/location/conexio_board_root_v3" "-DZEPHYR_EXTRA_MODULES=C:/ncs/v3.2.0/modules/lib/golioth-firmware-sdk" "-DEXTRA_CONF_FILE=overlay-golioth.conf;credentials.conf"
```

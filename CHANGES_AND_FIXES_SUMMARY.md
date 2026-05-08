# Changes and Fixes Attempted So Far

This summary is based on the current workspace files and captured build logs.

## 1. Planning and integration setup

- Added a full integration plan in GOLIOTH_LOCATION_INTEGRATION_PLAN.md.
- Captured target architecture, phases, build commands, and verification criteria.
- Recorded Golioth project/device credential context and intended stream path for location data.

## 2. New Golioth overlay and configuration hardening

A new overlay file was added: overlay-golioth.conf.

Key changes attempted in this overlay:

- Enabled Golioth Firmware SDK and streaming:
  - CONFIG_GOLIOTH_FIRMWARE_SDK=y
  - CONFIG_GOLIOTH_STREAM=y
  - CONFIG_GOLIOTH_AUTH_METHOD_PSK=y
  - CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG=42
- Forced modem networking path for nRF91 offloaded sockets:
  - CONFIG_NRF_MODEM_LIB_NET_IF=y
  - CONFIG_NRF_MODEM_LIB_NET_IF_AUTO_START=y
  - CONFIG_NET_CONNECTION_MANAGER=y
- Forced IPv4-only LTE/network path to address observed modem/network behavior:
  - CONFIG_LTE_LC_PDN_DEFAULTS_OVERRIDE=y
  - CONFIG_LTE_LC_PDN_DEFAULT_FAM_IPV4=y
  - CONFIG_NET_IPV4=y
  - CONFIG_NET_IPV6=n
- Added extra diagnostics for Golioth and network stack:
  - CONFIG_GOLIOTH_LOG_LEVEL_DBG=y
  - CONFIG_NET_LOG=y
  - CONFIG_NET_SOCKETS_LOG_LEVEL_DBG=y
- Enabled eventfd support required by Golioth internals:
  - CONFIG_ZVFS=y
  - CONFIG_ZVFS_EVENTFD=y
- Adjusted TLS feature mix to avoid invalid DTLS option combinations reported in comments:
  - Disabled DHE key exchange variants
  - Disabled EC extended parsing
  - Disabled Nordic security backend in this overlay
  - Kept MBEDTLS legacy crypto compatibility enabled
- Increased runtime memory sizes:
  - CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048
  - CONFIG_HEAP_MEM_POOL_SIZE=16384
  - CONFIG_MAIN_STACK_SIZE=4096

## 3. Application code changes in src/main.c

Golioth integration code was added behind CONFIG_GOLIOTH_FIRMWARE_SDK guards.

Main code-level fixes attempted:

- Added Golioth headers and client globals.
- Added event callback and connection semaphore handling.
- Added CBOR encoding of location payload (lat/lon/acc) and publish via golioth_stream_set.
- Hooked publish logic into LOCATION_EVT_LOCATION so each successful fix is sent.
- Added network readiness checks before client creation:
  - net_if_up and L4 connected event wait
  - DNS retry loop for coap.golioth.io:5684
  - Raw UDP connect probe logging
- Added extensive debug logging for LTE state, DNS readiness, client creation, and publish status.

## 4. Sysbuild and MCUboot related changes

- Added sysbuild.conf with:
  - SB_CONFIG_BOOTLOADER_MCUBOOT=y
- Added sysbuild/mcuboot.conf with conservative MCUboot options:
  - Manual image sector settings
  - Removal/avoidance of invalid or conflicting symbols
  - Disabled unused peripheral features for bootloader footprint simplification

## 5. Build attempts and outcomes from logs

### Earlier failed attempt (build.log)

Observed failure:

- Trusted Firmware-M compile failed with:
  - fatal error: pm_config.h: No such file or directory
- Failure occurred while compiling TF-M platform source (spu.c), and build stopped.

### Later successful attempts

build_stratus.log tail indicates successful completion:

- Final link completed for zephyr.elf
- Memory usage summary printed
- "Completed 'location'"
- merged.hex generated

build_stratus_mcuboot.log tail indicates successful MCUboot/sysbuild completion:

- Final link completed for zephyr.elf
- Image signing steps ran
- dfu_application.zip generated
- merged.hex generated

## 6. Warnings and non-fatal issues still seen

Across logs, warnings still appear, including:

- Deprecated symbol warnings:
  - NRF_CLOUD_REST
  - MBEDTLS_LEGACY_CRYPTO_C
- CMake and policy warnings (developer-level)
- MCUboot warning about default signing key (debug-use key)
- Partition manager warning about missing pm_static.yml when using bootloader

These did not block the later successful builds but should be cleaned up for production readiness.

## 7. Current changed/untracked workspace state (high level)

Tracked files modified:

- Kconfig.sysbuild
- prj.conf
- src/main.c

Untracked files relevant to this effort include:

- overlay-golioth.conf
- GOLIOTH_LOCATION_INTEGRATION_PLAN.md
- sysbuild.conf
- sysbuild/mcuboot.conf
- build.log
- build_stratus.log
- build_stratus_mcuboot.log

## 8. Practical status

- Build path has progressed from an earlier TF-M failure to successful full image generation.
- Golioth integration code and overlay are now present.
- The remaining work is mainly runtime verification on hardware/network and cleanup of warnings/security hardening (especially credentials and signing key handling).

## 9. Latest updates (May 6, 2026)

The following reflects the most recent stabilization pass and build verification.

### What was confirmed as fixed

- Sysbuild + MCUboot flow now completes end-to-end and produces signed artifacts.
- Application image links successfully after prior TF-M include/path issues.
- The location app build and the MCUboot build both complete in the same run.
- Final artifacts are generated, including merged image outputs and DFU package files.

### Configuration and integration state now in place

- Golioth enablement remains active through overlay-golioth.conf.
- Network path remains pinned to an LTE-friendly IPv4 setup for modem offload behavior.
- Golioth publish path in src/main.c remains wired to LOCATION_EVT_LOCATION with CBOR payload stream publishing.
- Sysbuild/MCUboot settings remain in place to keep bootloader integration stable.

### Current non-blocking warnings still present

- Deprecated symbols still reported:
  - NRF_CLOUD_REST
  - MBEDTLS_LEGACY_CRYPTO_C
- MCUboot warning about use of default debug signing key.
- Partition manager warning about running bootloader flow without pm_static.yml.

These do not block builds but should be treated as production hardening items.

### Immediate next hardening actions

- Replace default MCUboot signing key with project-owned key material.
- Add a fixed pm_static.yml partition map for upgrade-safe releases.
- Remove or replace deprecated Kconfig symbols where possible.
- Perform on-device runtime verification of LTE attach, DNS, Golioth connect, and stream upload.

## 10. Latest build recovery updates (May 8, 2026)

This section captures the full set of changes needed to get this repository building on the Stratus 9160 path in this environment.

### 10.1 Board migration to nRF9160 (from nRF9161)

The active custom board root was migrated to 9160 naming and symbols:

- Updated:
  - `conexio_board_root_v3/boards/conexio/conexio_stratus_pro/board.yml`
  - `conexio_board_root_v3/boards/conexio/conexio_stratus_pro/board.cmake`
  - `conexio_board_root_v3/boards/conexio/conexio_stratus_pro/Kconfig.conexio_stratus_pro`
  - `conexio_board_root_v3/boards/conexio/conexio_stratus_pro/Kconfig.defconfig`
  - `conexio_board_root_v3/boards/conexio/conexio_stratus_pro/conexio_stratus_pro_partition_conf.dtsi`
- Renamed/created board files for 9160:
  - `conexio_stratus_pro_nrf9160.dts`
  - `conexio_stratus_pro_nrf9160_ns.dts`
  - `conexio_stratus_pro_nrf9160_defconfig`
  - `conexio_stratus_pro_nrf9160_ns_defconfig`
- Removed old `nrf9161` board file variants from the v3 board folder.

### 10.2 Repository cleanup requested

Per cleanup request, removed:

- all current build directories at the time
- `conexio_board_root`
- `conexio_stratus_pro_devicetree`

This left `conexio_board_root_v3` as the active custom board source.

### 10.3 Build environment diagnosis

Builds initially failed with many "undefined symbol" errors in `prj.conf` while using a non-matching SDK/workspace context.

Root cause findings:

- The command in this repo referenced `C:/ncs/v3.2.0/...`.
- Local machine has `C:/ncs/v3.2.3`, not `v3.2.0`.
- `golioth-firmware-sdk` was missing from `C:/ncs/v3.2.3/modules/lib`.

### 10.4 Golioth module installation in NCS v3.2.3

Installed missing module:

- cloned `https://github.com/golioth/golioth-firmware-sdk.git` to
  - `C:/ncs/v3.2.3/modules/lib/golioth-firmware-sdk`
- verified module metadata exists:
  - `C:/ncs/v3.2.3/modules/lib/golioth-firmware-sdk/zephyr/module.yml`

### 10.5 Config symbol compatibility fixes for current NCS

`NPM13XX_CHARGER` is not a valid symbol in this build path and caused hard Kconfig failures.

Actions taken:

- Removed/updated charger symbol usage in:
  - `conexio_board_root_v3/boards/conexio/conexio_stratus_pro/conexio_stratus_pro_nrf9160_defconfig`
  - `conexio_board_root_v3/boards/conexio/conexio_stratus_pro/conexio_stratus_pro_nrf9160_ns_defconfig`
  - `sysbuild/mcuboot.conf`

### 10.6 Sysbuild/mcuboot board qualification fix

A TF-M target mismatch appeared when MCUboot was being built using the non-secure board qualifier.

Fix:

- Explicitly set MCUboot image board to secure qualifier via build arg:
  - `-Dmcuboot_BOARD=conexio_stratus_pro/nrf9160`

### 10.7 Windows path translation failure and final fix

After configuration passed, build failed at Ninja stage with:

- missing dependency path in POSIX style:
  - `/c/ncs/v3.2.3/nrf/.git`

On this Windows setup, that path did not resolve during the build step, even though `C:\ncs\v3.2.3\nrf\.git` existed.

Fix applied:

- created junction:
  - `C:\c -> C:\`

This made `/c/...` paths resolvable for the generated dependency chain and unblocked the build.

### 10.8 Final working build command (validated)

Run from NCS 3.2.3 workspace context:

```powershell
west build --sysbuild -p always -d C:/Users/Brian/location_conexio_9161/build -b conexio_stratus_pro/nrf9160/ns C:/Users/Brian/location_conexio_9161 -- "-DBOARD_ROOT=C:/Users/Brian/location_conexio_9161/conexio_board_root_v3" "-Dmcuboot_BOARD=conexio_stratus_pro/nrf9160" "-DZEPHYR_EXTRA_MODULES=C:/ncs/v3.2.3/modules/lib/golioth-firmware-sdk" "-DEXTRA_CONF_FILE=overlay-golioth.conf"
```

### 10.9 Final successful build indicators

Successful run produced:

- full compile/link completion for app and MCUboot
- `dfu_application.zip`
- `merged.hex`

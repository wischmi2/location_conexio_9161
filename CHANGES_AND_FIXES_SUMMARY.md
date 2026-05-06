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

# Changes and fixes — Golioth DTLS debugging (nRF9160 / Conexio Stratus Pro)

This document lists configuration and code changes made while debugging **LTE → DNS → Golioth DTLS** on **nRF Connect SDK v3.2.x**, **Zephyr 4.2.x**, **Golioth Firmware SDK v0.22.x**, board **`conexio_stratus_pro/nrf9160/ns`**.

---

## 1. Symptoms observed on the serial console

| Observation | Notes |
|---------------|--------|
| `modem_key_mgmt` / sec-tag write failures while LTE active | Nordic modem rejects credential writes when data call is already up. |
| `Failed to connect to socket: -109` (`ENOTSUP`) | Modem-offloaded TLS path does not support some `setsockopt` operations Golioth’s Zephyr port uses for DTLS. |
| `Failed to connect` / handshake with errno **2** (`ENOENT`) | PSK not visible to **native** mbedTLS via Zephyr’s credential lookup (modem TLS backend does not register PSK types for `credential_next_get`). |
| `connect()` **-22** (`EINVAL`) | Misconfiguration during TLS setup (PSK length, cipher options, or Golioth DTLS handshake timeout overrides interacting with half-initialized mbedTLS config). |
| `Failed to connect to socket: -12` (`ENOMEM`) | mbedTLS heap and/or system heap too small during DTLS handshake; Zephyr maps allocation failures to `-12`. |
| `TLS handshake error: -0x7780` | **`MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE`** (mbedTLS `ssl.h`): the **Golioth server sent a TLS fatal alert** — wrong PSK/identity, cipher mismatch, or other handshake rejection (not a RAM failure). |
| `Failed to connect to socket: -113` | **`ECONNABORTED` (113)** in Zephyr errno — `sockets_tls.c` maps non-recoverable mbedTLS handshake failures (including fatal-alert errors) to **`ECONNABORTED`** after reset; same underlying issue as `-0x7780`, not a separate transport bug. |
| `Registration rejected, EMM cause: 11` | Network/SIM/plan issue; registration later succeeded in traces — separate from Golioth socket errors. |
| `nrf_modem_lib_netif: MTU query failed` | Warning seen around iface bring-up; DNS and UDP probes still succeeded afterward. |

---

## 2. `overlay-golioth.conf` — Golioth, modem net IF, TLS, heaps

Applied with **`-DEXTRA_CONF_FILE=overlay-golioth.conf`** (often combined with `credentials.conf`).

**Golioth**

- `CONFIG_GOLIOTH_FIRMWARE_SDK=y`, `CONFIG_GOLIOTH_STREAM=y`, `CONFIG_GOLIOTH_AUTH_METHOD_PSK=y`
- `CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG=42` — fixed sec tag for PSK/TLS auth.

**Modem network interface / boot order**

- `CONFIG_NRF_MODEM_LIB_NET_IF=y`, `CONFIG_NRF_MODEM_LIB_NET_IF_AUTO_START=y`
- **`CONFIG_NRF_MODEM_LIB_NET_IF_AUTO_CONNECT=n`** — prevents LTE from attaching before the application can provision credentials (avoids “not allowed when LTE connection is active” for `modem_key_mgmt`).
- `CONFIG_MODEM_KEY_MGMT=y`, `CONFIG_NET_CONNECTION_MANAGER=y`, `CONFIG_NET_CONNECTION_MANAGER_MONITOR_STACK_SIZE=1024`

**PSK visible to native mbedTLS**

- **`CONFIG_TLS_CREDENTIALS_BACKEND_VOLATILE=y`** — use Zephyr’s volatile credential store so `tls_credential_add(PSK_ID/PSK)` is visible to native mbedTLS (fixes **ENOENT** when not using modem TLS backend for PSK).

**LTE / IP stack**

- IPv4-only PDN (observed ESM / dual-stack issues): `CONFIG_LTE_LC_PDN_DEFAULTS_OVERRIDE=y`, `CONFIG_LTE_LC_PDN_DEFAULT_FAM_IPV4=y`
- **`CONFIG_NET_IPV6=y`** with neighbor discovery/MLD off (`CONFIG_NET_IPV6_NBR_CACHE=n`, `CONFIG_NET_IPV6_MLD=n`) — “hello_nrf91_offloaded” style so **`getaddrinfo` / DTLS** are not broken by fully disabling the IPv6 node; still IPv4 data path.

**Force native TLS over modem offload**

- **`CONFIG_NET_SOCKETS_TLS_PRIORITY=35`** — Zephyr native mbedTLS DTLS wins over modem-offloaded TLS, avoiding **ENOTSUP (-109)** on DTLS `setsockopt` paths.

**Logging**

- `CONFIG_GOLIOTH_LOG_LEVEL_DBG=y`, `CONFIG_NET_LOG=y`, `CONFIG_NET_SOCKETS_LOG_LEVEL_INF=y` — verbose Golioth, socket layer left less chatty to reduce UART interleaving.

**ZVFS / eventfd (Golioth internal)**

- `CONFIG_ZVFS=y`, `CONFIG_ZVFS_EVENTFD=y`, limits raised (`EVENTFD_MAX`, `OPEN_MAX`).

**mbedTLS / ciphers**

- PSK-focused key exchange, DHE variants off, `CONFIG_NORDIC_SECURITY_BACKEND=n`, `CONFIG_MBEDTLS_LEGACY_CRYPTO_C=y`
- **`CONFIG_GOLIOTH_CIPHERSUITES="TLS_PSK_WITH_AES_128_GCM_SHA256 TLS_PSK_WITH_AES_128_CCM"`** — **GCM first, then CCM**. Golioth’s own `port/zephyr/Kconfig` defaults **`TLS_PSK_WITH_AES_128_GCM_SHA256`** for NCS+nRF when GCM is available. An earlier attempt used **CCM-only** to shrink ClientHello / avoid glue issues; runtime logs then showed **`TLS handshake error: -0x7780`** (fatal server alert), often consistent with **cipher negotiation mismatch**. Offering **GCM and CCM** matches cloud + upstream hello samples and avoids advertising only CCM.
- AES, CCM, GCM, SHA256 enabled (`CONFIG_MBEDTLS_CIPHER_*`, `CONFIG_MBEDTLS_SHA256=y`).
- **`CONFIG_MBEDTLS_PSK_MAX_LEN=64`** — Golioth PSKs must fit mbedTLS `mbedtls_ssl_conf_psk` limits.

**Heaps (DTLS / allocation failures)**

- `CONFIG_MBEDTLS_ENABLE_HEAP=y`
- **`CONFIG_MBEDTLS_HEAP_SIZE=86016`**
- **`CONFIG_HEAP_MEM_POOL_SIZE=57344`**

Larger pairs (e.g. **98304 + 65536**) were tried to kill **ENOMEM (-12)** but **linked firmware overflowed nRF9160 non-secure RAM by ~11 KiB**; the **86016 / 57344** pair was chosen to **fit the linker map** while keeping substantial headroom for handshake allocations.

**Golioth DTLS handshake timeouts**

- **`CONFIG_GOLIOTH_ZEPHYR_DTLS_HANDSHAKE_TIMEOUT_MIN_MS=-1`** and **`MAX_MS=-1`** — leave defaults so Golioth does not call `mbedtls_ssl_conf_handshake_timeout()` in a way that breaks config after non-default `setsockopt` ordering (**EINVAL -22** mitigation).

**Stacks**

- `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048`, `CONFIG_MAIN_STACK_SIZE=4096` (overlay overrides smaller base `prj.conf` values where applicable).

---

## 3. `prj.conf` — base application + networking

**Heap/stacks (base; overlay overrides for Golioth build)**

- Default `CONFIG_MAIN_STACK_SIZE=2048`, `CONFIG_HEAP_MEM_POOL_SIZE=8192`, `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=1536`, `CONFIG_AT_MONITOR_HEAP_SIZE=1024`

**Networking (required with overlay)**

- `CONFIG_NETWORKING=y`, `CONFIG_NET_NATIVE=y`, `CONFIG_NET_SOCKETS=y`, **`CONFIG_NET_SOCKETS_OFFLOAD=y`** — modem provides L2; Zephyr IP stack + sockets used for native DTLS to Golioth.

**LTE / GNSS / sensors / logging**

- Unchanged theme: LTE-M + GNSS, PSM/eDRX tweaks for GNSS windows, `DATE_TIME`, onboard SHT40 + LIS2DH, UART logging with `CONFIG_LOG_PRINTK=y`, nRF Cloud disabled for standalone GNSS validation.

---

## 4. `src/main.c` — provisioning order, PSK handling, readiness, diagnostics

**Boot order (credential safety)**

1. Optional modem/AT diagnostics as implemented in app.
2. **`golioth_provision_psk_credentials()`** runs **before** `lte_lc_register_handler()` / **`lte_lc_connect()`** so PSK is in volatile TLS storage **before** LTE attach competes with modem rules.

**PSK material**

- Credentials from **`CONFIG_GOLIOTH_SAMPLE_PSK_ID`** / **`CONFIG_GOLIOTH_SAMPLE_PSK`** (typically via `credentials.conf`).
- **Hex decode**: if the PSK string is exactly **64 or 128** characters and all hex, firmware decodes to **raw key bytes** (Golioth console often shows hex; mbedTLS needs bytes, not ASCII hex — avoids **`mbedtls_ssl_conf_psk` / EINVAL**).
- **`tls_credential_delete` / `tls_credential_add`** for `TLS_CREDENTIAL_PSK_ID` and `TLS_CREDENTIAL_PSK` on the Golioth sec tag.

**mbedTLS self-test**

- **`golioth_mbedtls_psk_selftest()`** — minimal `mbedtls_ssl_config_defaults` + **`mbedtls_ssl_conf_psk`** to confirm PSK bytes are accepted (**“ssl_conf_psk OK”** in logs separates mbedTLS config failures from **socket-layer -12 / -22**).

**Network readiness before Golioth client**

- **`wait_for_network_ready()`**: interface up / connected semantics, IPv4 check, then **`wait_for_golioth_dns()`** with retries for **`coap.golioth.io:5684`**, plus UDP probe logging — client creation happens **after** LTE + DNS succeed.

**Golioth client**

- `golioth_client` with **`GOLIOTH_TLS_AUTH_TYPE_TAG`** and tag **42**.
- Stream publish on location events / status helpers as implemented (`golioth_stream_set`, JSON payloads).

**LED / heartbeat**

- **`boot_mark()`** stages and optional LED blink patterns to show where boot hangs (e.g. `golioth_wait_connected`).

---

## 5. Credentials and build invocation

**`credentials.conf.sample`**

- Documents copying to **`credentials.conf`** (gitignored) and **`CONFIG_GOLIOTH_SAMPLE_PSK_ID` / `CONFIG_GOLIOTH_SAMPLE_PSK`**.
- Notes **full hex paste** when the console shows 64/128 hex nibbles.

**Typical extra config**

```text
-DEXTRA_CONF_FILE="overlay-golioth.conf;credentials.conf"
-DZEPHYR_EXTRA_MODULES=<path-to>/golioth-firmware-sdk
-DBOARD_ROOT=<path-to>/conexio_board_root_v3
```

MCUboot sysbuild may use **`-Dmcuboot_BOARD=conexio_stratus_pro/nrf9160`** (secure board for bootloader image).

**Exact command used for the successful 9160 build (copy/paste):**

```powershell
$env:ZEPHYR_BASE="C:/ncs/v3.2.0/zephyr"
$env:ZEPHYR_TOOLCHAIN_VARIANT="zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR="C:/ncs/toolchains/66cdf9b75e/opt/zephyr-sdk"
& "C:/ncs/toolchains/66cdf9b75e/opt/bin/Scripts/west" build --sysbuild -p always -d build_9160_check -b conexio_stratus_pro/nrf9160/ns -- "-DBOARD_ROOT=C:/Users/Brian/location/conexio_board_root_v3" "-DZEPHYR_EXTRA_MODULES=C:/ncs/v3.2.0/modules/lib/golioth-firmware-sdk" "-DEXTRA_CONF_FILE=overlay-golioth.conf;credentials.conf"
```

### Pre-flight checklist (before building in a new place)

- Confirm branch is correct and clean (`git branch --show-current`, `git status --short`).
- Confirm board target is **`conexio_stratus_pro/nrf9160/ns`**.
- Confirm local credentials file exists and has the intended device's PSK/PSK-ID (`credentials.conf`).
- Confirm `overlay-golioth.conf` includes CoAP path-length support:
	- `CONFIG_COAP_EXTENDED_OPTIONS_LEN=y`
	- `CONFIG_COAP_EXTENDED_OPTIONS_LEN_VALUE=32`
- Confirm Golioth module path exists: `C:/ncs/v3.2.0/modules/lib/golioth-firmware-sdk`.
- Confirm build env vars are set in the active shell:
	- `ZEPHYR_BASE=C:/ncs/v3.2.0/zephyr`
	- `ZEPHYR_TOOLCHAIN_VARIANT=zephyr`
	- `ZEPHYR_SDK_INSTALL_DIR=C:/ncs/toolchains/66cdf9b75e/opt/zephyr-sdk`
- Rebuild with `-p always` to avoid stale artifacts.
- Flash/upload from the same build directory you just produced (`build_9160_check`).

---

## 6. Items tried or rejected during debugging

| Item | Result |
|------|--------|
| **`CONFIG_MBEDTLS_DEBUG=y`** | Link failure: `mbedtls_debug_set_threshold` not resolved with current **nrf_security** wiring — not left enabled. |
| Very large **`MBEDTLS_HEAP_SIZE` + `HEAP_MEM_POOL_SIZE`** | Fixed **ENOMEM** risk but **RAM region overflow** at link time; reduced to **86016 / 57344** so the image fits. |
| **`CONFIG_GOLIOTH_CIPHERSUITES` CCM-only** | After **ENOMEM (-12)** was addressed, logs showed **`-0x7780` / -113**; ciphers updated to **GCM + CCM** (see §2). |

---

## 7. Board repository and build environment (condensed)

- Custom board under **`conexio_board_root_v3`** for **Stratus Pro nRF9160** (`*_nrf9160*.dts`, `*_ns`, defconfigs). Older nRF9161 filenames were migrated away where applicable.
- **NCS v3.2.3** (paths in docs may say v3.2.0 — use the toolchain actually installed).
- **`golioth-firmware-sdk`** must exist under **`modules/lib/golioth-firmware-sdk`** (or passed via **`ZEPHYR_EXTRA_MODULES`**).
- Invalid **`NPM13XX_CHARGER`** Kconfig symbols were removed from board/mcuboot configs where they broke Kconfig on this SDK.
- On **Windows**, some builds needed a **`C:\c` → `C:\`** junction so **`/c/ncs/...`** dependency paths resolve during Ninja (environment-specific).

---

## 8. Current status and suggested next steps

- **Build**: With **86016 / 57344** heaps and **GCM+CCM** ciphers, **`west build --sysbuild`** completes for **`conexio_stratus_pro/nrf9160/ns`** when RAM fits.
- **Runtime progression observed**: **`ssl_conf_psk OK`** → LTE/DNS OK → earlier **`Failed to connect: -12`** addressed via heaps → then **`TLS handshake error: -0x7780`** and **`Failed to connect: -113`** (fatal server alert + **`ECONNABORTED`**), addressed in config by **not** restricting ClientHello to **CCM-only**.
- **Next on hardware**: Flash the build with **`TLS_PSK_WITH_AES_128_GCM_SHA256 TLS_PSK_WITH_AES_128_CCM`** and confirm **Connected to Golioth**. If **`-0x7780`** persists, treat as **credentials / project pairing**: exact **`CONFIG_GOLIOTH_SAMPLE_PSK_ID`**, full PSK (including **64-hex** decode path), device registered in the correct Golioth project — not heap size.
- If **ENOMEM (-12)** reappears after further changes: increase heaps only within linker margin, or raise **`CONFIG_GOLIOTH_COAP_THREAD_STACK_SIZE`**, or trim other RAM consumers.
- If connect succeeds: validate **LightDB stream** payloads and consider removing or `#ifdef`-gating **`golioth_mbedtls_psk_selftest()`** for production boot time and log noise.
- **Production**: replace MCUboot default signing key, add **`pm_static.yml`** if required by release policy, and keep **`credentials.conf`** out of version control.

---

## 9. Golioth telemetry publish failure — `Path too long: 18 > 12` ✅ FIXED

### Symptom

After Golioth connected successfully on `conexio_stratus_pro/nrf9160/ns`, the serial log showed:

```
[00:00:21.014,770] <err> golioth_coap_client: Path too long: 18 > 12
[golioth] telemetry/snapshot publish failed: 6
```

Every periodic telemetry publish was silently dropped. Location publishes to short paths (`loc/fix`, `loc/stat`) still worked because those are ≤ 7 characters.

### Root cause

`CONFIG_GOLIOTH_COAP_CLIENT_MAX_PATH_LEN` defaults to **12** in the Golioth Firmware SDK. The application publishes sensor + accelerometer + location snapshots to the path `telemetry/snapshot`, which is **18 characters** — 6 over the limit. The SDK rejects the publish with error code 6 (`GOLIOTH_ERR_INVALID_FORMAT`) and logs "Path too long".

This is a compile-time Kconfig that caps the internal CoAP path buffer; it is not enforced at the Golioth cloud side. The fix is simply to raise it in the overlay.

### Fix applied — `overlay-golioth.conf`

```kconfig
# Raise max CoAP path length to accommodate paths like "telemetry/snapshot" (18 chars).
# GOLIOTH_COAP_MAX_PATH_LEN is a derived (no-prompt) symbol driven by these two Zephyr CoAP symbols.
CONFIG_COAP_EXTENDED_OPTIONS_LEN=y
CONFIG_COAP_EXTENDED_OPTIONS_LEN_VALUE=32
```

Set to **32** to give comfortable headroom for any future longer paths without meaningfully increasing RAM usage (the buffer is stack-allocated per request, not a global pool).

### How to avoid this in future

Any LightDB Stream path used in `golioth_stream_set()` or `golioth_stream_set_async()` must fit within `CONFIG_GOLIOTH_COAP_CLIENT_MAX_PATH_LEN`. Count the full path string including slashes (e.g. `telemetry/snapshot` = 18). Set the config ≥ your longest path. The default of 12 only fits very short single-segment paths.

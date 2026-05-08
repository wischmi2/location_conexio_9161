# Build Instructions (Conexio Stratus Pro nRF9161)

This file is the source of truth for building this firmware in this repository.

## Prerequisites

This project requires the NCS 3.2.0 Zephyr environment and a custom board root.

If you are not already in an NCS shell, set these variables first:

```powershell
$env:ZEPHYR_BASE="C:/ncs/v3.2.0/zephyr"
$env:ZEPHYR_TOOLCHAIN_VARIANT="zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR="C:/ncs/toolchains/66cdf9b75e/opt/zephyr-sdk"
```

You can then use either `west` from PATH, or the NCS west executable:

```powershell
& "C:/ncs/toolchains/66cdf9b75e/opt/bin/Scripts/west" --version
```

## Working Build Command

Run this from the repository root:

`C:\Users\Brian\location`

```powershell
west build --sysbuild -p always -d build -b conexio_stratus_pro/nrf9161/ns -- "-DBOARD_ROOT=C:/Users/Brian/location/conexio_board_root_v3" "-DZEPHYR_EXTRA_MODULES=C:/ncs/v3.2.0/modules/lib/golioth-firmware-sdk" "-DEXTRA_CONF_FILE=overlay-golioth.conf;credentials.conf"
```

If your shell resolves to a non-NCS west/Zephyr install, run the same command with explicit NCS west:

```powershell
& "C:/ncs/toolchains/66cdf9b75e/opt/bin/Scripts/west" build --sysbuild -p always -d build -b conexio_stratus_pro/nrf9161/ns -- "-DBOARD_ROOT=C:/Users/Brian/location/conexio_board_root_v3" "-DZEPHYR_EXTRA_MODULES=C:/ncs/v3.2.0/modules/lib/golioth-firmware-sdk" "-DEXTRA_CONF_FILE=overlay-golioth.conf;credentials.conf"
```

## Output Artifacts

After a successful build, the main artifacts are:

- `build/location/zephyr/zephyr.elf`
- `build/location/zephyr/zephyr.signed.bin`
- `build/merged.hex`

## Notes

- This repository uses `sysbuild` (application + MCUboot).
- `credentials.conf` is expected to exist locally (it is intentionally not committed).
- The build requires the custom board root: `-DBOARD_ROOT=C:/Users/Brian/location/conexio_board_root_v3`.
- If you see "Invalid BOARD" or many "undefined symbol" Kconfig warnings, your shell is not using NCS Zephyr/toolchain.

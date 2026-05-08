# Build Instructions (Conexio Stratus Pro nRF9161)

This file is the source of truth for building this firmware in this repository.

## Working Build Command

Run this from the repository root:

`C:\Users\Brian\location`

```powershell
west build --sysbuild -p always -d build -b conexio_stratus_pro/nrf9161/ns -- "-DZEPHYR_EXTRA_MODULES=C:/ncs/v3.2.0/modules/lib/golioth-firmware-sdk" "-DEXTRA_CONF_FILE=overlay-golioth.conf;credentials.conf"
```

## Output Artifacts

After a successful build, the main artifacts are:

- `build/location/zephyr/zephyr.elf`
- `build/location/zephyr/zephyr.signed.bin`
- `build/merged.hex`

## Notes

- This repository uses `sysbuild` (application + MCUboot).
- `credentials.conf` is expected to exist locally (it is intentionally not committed).
- Use this `west build` command as the default build path for this project.

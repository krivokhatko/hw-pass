# hw-pass

Firmware for the M5Stack StickS3 (ESP32-S3).

## Target

| Item | Value |
|---|---|
| Board | M5Stack StickS3 |
| SoC | ESP32-S3-PICO-1, revision v0.2 |
| Flash | 8 MB, quad |
| PSRAM | 8 MB, not enabled yet |
| Console | USB Serial/JTAG. UART0 is not connected to USB. |

## Toolchain

- ESP-IDF v5.5.5, installed in `~/esp/esp-idf`.
- Do not use ESP-IDF v6.x yet. M5GFX has open display bugs on v6.x.

For the WSL setup, refer to [docs/dev-setup.md](docs/dev-setup.md).

## Layout

```
CMakeLists.txt        Project file.
sdkconfig.defaults    Project configuration. Edit this file, not sdkconfig.
partitions.csv        Flash partition table for 8 MB flash.
main/                 Application entry point (app_main). Keep it small.
components/           Project modules. One directory for each module.
docs/                 Developer documentation.
```

## Build and flash

```
get_idf
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

To stop the monitor, push `Ctrl+]`.

## Configuration

`sdkconfig` is a generated file. Git ignores it. `sdkconfig.defaults` is the source of the configuration.

To change the configuration:

1. Add or change the option in `sdkconfig.defaults`.
2. Delete `sdkconfig`.
3. Run `idf.py build`.

## Add a module

1. Make the directory `components/<name>/`.
2. Put the public headers in `components/<name>/include/`.
3. Register the module with `idf_component_register` in `components/<name>/CMakeLists.txt`.
4. Add `<name>` to `PRIV_REQUIRES` of each component that uses it.

The build includes only the components that `main` needs. A module that no component requires is not built.

For an example, refer to `components/sysinfo/`.

## External components

1. Add the component with `idf.py add-dependency "<namespace>/<name>^<version>"`.
2. Commit `main/idf_component.yml` and `dependencies.lock`.

Git ignores `managed_components/`.

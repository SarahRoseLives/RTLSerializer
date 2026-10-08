# RTLSerializer

A small Windows GUI for reading and writing the EEPROM of RTL-SDR (RTL2832U)
dongles - in particular the USB **serial number**, manufacturer and product
strings.

It is a friendly front end for the same on-dongle configuration that the
osmocom `rtl_eeprom` tool edits, with the string-descriptor parsing/generation
logic ported from `rtl_eeprom.c`.

## Features

- Lists all connected RTL-SDR devices (index, vendor/product, current serial).
- Reads and displays the current EEPROM configuration.
- Edits manufacturer, product and serial number, plus the serial-enabled,
  IR-endpoint and remote-wakeup flags.
- Backup and full restore of the 256-byte EEPROM image.

## Safety

Writing the EEPROM is deliberately conservative so a dongle is hard to brick:

- The full 256-byte EEPROM is read first; only the 128-byte configuration area
  is written for normal edits.
- The configuration is regenerated from the **existing** EEPROM, so untouched
  fields, vendor/product IDs and unknown OEM bytes are preserved.
- `rtlsdr_write_eeprom()` only programs bytes that actually differ, and a
  no-edit write is a true zero-byte no-op.
- A backup is written to `backups/` before every write, and every write is
  verified by reading the EEPROM back.
- Input is sanitised: the serial accepts digits `0-9` only, while manufacturer
  and product accept letters, digits, space and `- _ . +`. Values that do not
  fit the descriptor area disable the Write button.

Use at your own risk. Unplug/replug the dongle after a successful write for the
new descriptors to take effect.

## Building

Requires a 64-bit MinGW-w64 toolchain (MSYS2 is easiest) plus CMake and Ninja.
The librtlsdr headers, import libraries and runtime DLLs are vendored under
`third_party/rtlsdr`, so no extra SDK is needed.

From a shell with `C:\msys64\mingw64\bin` on `PATH`:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable is produced at `build/RTLSerializer.exe`, with `rtlsdr.dll` and
`libusb-1.0.dll` copied next to it automatically.

## Layout

```
src/main.cpp      Win32 GUI
src/eeprom.*      EEPROM string-descriptor parsing / generation
src/app.rc        Manifest resource
third_party/rtlsdr  Vendored librtlsdr SDK (GPLv2)
```

## Credits

- Reference implementation: osmocom `rtl-sdr` (`rtl_eeprom.c`, `librtlsdr`).
- `third_party/rtlsdr` is the GPLv2 librtlsdr library and its public header.

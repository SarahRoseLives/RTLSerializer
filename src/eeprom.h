// RTLSerializer - EEPROM configuration model.
//
// This is a C++ port of the EEPROM parsing/generation logic from the
// osmocom rtl-sdr project's rtl_eeprom.c (src/rtl_eeprom.c), which is the
// reference tool for reading and writing the RTL2832 EEPROM.
//
// The RTL2832 EEPROM layout used here:
//   0x00 - 0x01  magic        0x28 0x32
//   0x02 - 0x03  vendor id    (little endian)
//   0x04 - 0x05  product id   (little endian)
//   0x06         have_serial  0xA5 when the serial descriptor is valid
//   0x07         flags        bit0 remote wakeup, bit1 IR endpoint, 0x14 base
//   0x08         power        0x02
//   0x09 ...     USB string descriptors (manufacturer, product, serial)
//   0x4E (78)    length of IR config
//
// Each USB string descriptor is: [len][0x03][UTF-16LE chars...].

#ifndef RTL_SERIALIZER_EEPROM_H
#define RTL_SERIALIZER_EEPROM_H

#include <cstdint>
#include <string>

namespace rtlsdr_eeprom {

// Total EEPROM size in bytes (matches rtl_eeprom's EEPROM_SIZE).
constexpr int kEepromSize = 256;
// Number of bytes rtl_eeprom writes for a normal configuration change.
constexpr int kConfigSize = 128;
// Offset of the first USB string descriptor.
constexpr int kStrOffset = 0x09;
// Offset at which the IR configuration length is stored.
constexpr int kIrConfigOffset = 78;

// Decoded EEPROM configuration.
struct Config {
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    std::string manufacturer;
    std::string product;
    std::string serial;
    bool have_serial = false;
    bool enable_ir = false;
    bool remote_wakeup = false;
};

// Parse a 256-byte EEPROM image into `out`.
// Returns false when the image does not carry a valid RTL2832 header.
bool ParseEeprom(const uint8_t* image, Config& out);

// Regenerate the configuration region of `image` (which must be a 256-byte
// buffer previously read from the device) from `conf`. Bytes outside the
// string descriptor area are left untouched so that OEM/IR data is preserved.
// Returns false if one of the strings does not fit before the IR config offset.
bool GenerateEeprom(const Config& conf, uint8_t* image);

// Convenience: number of bytes that differ between two images over [0, len).
int CountDifferences(const uint8_t* a, const uint8_t* b, int len);

// Offset just past the last string descriptor for `conf`, or -1 if the three
// strings cannot fit before the IR config offset. Used for input validation.
int DescriptorEnd(const Config& conf);

}  // namespace rtlsdr_eeprom

#endif  // RTL_SERIALIZER_EEPROM_H

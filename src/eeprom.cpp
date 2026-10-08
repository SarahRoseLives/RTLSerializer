#include "eeprom.h"

#include <cstring>

namespace rtlsdr_eeprom {

namespace {

// Read a USB string descriptor located at `pos`. Mirrors get_string_descriptor()
// from rtl_eeprom.c but with bounds checking against the full EEPROM image.
bool GetStringDescriptor(const uint8_t* image, int pos, int& next, std::string& out) {
    if (pos < 2 || pos + 2 > kEepromSize) {
        return false;
    }

    const int len = image[pos];
    if (len < 2 || pos + len > kEepromSize) {
        return false;
    }

    out.clear();
    for (int i = 2; i < len; i += 2) {
        out.push_back(static_cast<char>(image[pos + i]));
    }

    // rtl_eeprom advances by the descriptor length (i ends at len for the even
    // lengths that USB descriptors always use).
    next = pos + (len & ~1);
    return true;
}

// Write a USB string descriptor at `pos`. Mirrors set_string_descriptor() from
// rtl_eeprom.c. Returns false when the descriptor would overrun the IR config
// area, in which case the caller must abort the whole generation.
bool SetStringDescriptor(uint8_t* image, int pos, int& next, const std::string& str) {
    if (pos < 0) {
        return false;
    }

    int j = 2;
    image[pos + 1] = 0x03;

    for (std::size_t i = 0; i < str.size(); ++i) {
        if ((pos + j) >= kIrConfigOffset) {
            return false;
        }
        image[pos + j++] = static_cast<uint8_t>(str[i]);
        image[pos + j++] = 0x00;
    }

    image[pos] = static_cast<uint8_t>(j);
    next = pos + j;
    return true;
}

}  // namespace

bool ParseEeprom(const uint8_t* image, Config& out) {
    if (!image) {
        return false;
    }

    // rtl_eeprom only warns on a bad header; we treat it as an error so the UI
    // can refuse to operate on a device whose EEPROM we do not understand.
    if (image[0] != 0x28 || image[1] != 0x32) {
        return false;
    }

    out.vendor_id = static_cast<uint16_t>(image[2] | (image[3] << 8));
    out.product_id = static_cast<uint16_t>(image[4] | (image[5] << 8));
    out.have_serial = (image[6] == 0xa5);
    out.remote_wakeup = (image[7] & 0x01) != 0;
    out.enable_ir = (image[7] & 0x02) != 0;
    out.manufacturer.clear();
    out.product.clear();
    out.serial.clear();

    int pos = kStrOffset;
    int next = pos;
    if (GetStringDescriptor(image, pos, next, out.manufacturer)) {
        pos = next;
        if (GetStringDescriptor(image, pos, next, out.product)) {
            pos = next;
            GetStringDescriptor(image, pos, next, out.serial);
        }
    }

    return true;
}

bool GenerateEeprom(const Config& conf, uint8_t* image) {
    if (!image) {
        return false;
    }

    // rtl_eeprom unconditionally forces `dat[8] = 0x02`. That byte is not part
    // of any USB string descriptor and real OEM EEPROMs sometimes carry a
    // different factory value (our test unit has 0x12). Overwriting it would
    // make even a no-op write program bytes that have nothing to do with the
    // serial number, so preserve the factory value and only fall back to 0x02
    // when the buffer did not contain a valid RTL2832 header to begin with.
    const bool had_header = (image[0] == 0x28 && image[1] == 0x32);

    image[0] = 0x28;
    image[1] = 0x32;
    image[2] = static_cast<uint8_t>(conf.vendor_id & 0xff);
    image[3] = static_cast<uint8_t>((conf.vendor_id >> 8) & 0xff);
    image[4] = static_cast<uint8_t>(conf.product_id & 0xff);
    image[5] = static_cast<uint8_t>((conf.product_id >> 8) & 0xff);
    image[6] = conf.have_serial ? 0xa5 : 0x00;
    image[7] = 0x14;
    image[7] |= conf.remote_wakeup ? 0x01 : 0x00;
    image[7] |= conf.enable_ir ? 0x02 : 0x00;
    if (!had_header) {
        image[8] = 0x02;
    }

    int pos = kStrOffset;
    int next = pos;
    if (!SetStringDescriptor(image, pos, next, conf.manufacturer)) {
        return false;
    }
    pos = next;
    if (!SetStringDescriptor(image, pos, next, conf.product)) {
        return false;
    }
    pos = next;
    if (!SetStringDescriptor(image, pos, next, conf.serial)) {
        return false;
    }
    pos = next;

    image[kIrConfigOffset] = 0x00;  // length of IR config

    // Note: like rtl_eeprom we deliberately do not clear the gap between the
    // end of the serial descriptor and the IR config offset. Those bytes are
    // not referenced by any USB descriptor and leaving them alone means the
    // library's read-modify-write only touches bytes that actually changed.
    (void)pos;

    return true;
}

int CountDifferences(const uint8_t* a, const uint8_t* b, int len) {
    int count = 0;
    for (int i = 0; i < len; ++i) {
        if (a[i] != b[i]) {
            ++count;
        }
    }
    return count;
}

int DescriptorEnd(const Config& conf) {
    uint8_t scratch[kEepromSize];
    std::memset(scratch, 0, sizeof(scratch));
    if (!GenerateEeprom(conf, scratch)) {
        return -1;
    }
    return kStrOffset + (2 + 2 * static_cast<int>(conf.manufacturer.size())) +
           (2 + 2 * static_cast<int>(conf.product.size())) +
           (2 + 2 * static_cast<int>(conf.serial.size()));
}

}  // namespace rtlsdr_eeprom

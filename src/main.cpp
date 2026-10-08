// RTLSerializer - a small Windows GUI to read/write RTL-SDR EEPROM settings.
//
// Safety model (mirrors osmocom rtl_eeprom):
//   * The full 256-byte EEPROM is read into memory before anything is changed.
//   * Only the 128-byte configuration area is ever written for normal edits.
//   * The EEPROM is regenerated from the *existing* configuration, so fields
//     the user did not touch keep their original bytes.
//   * rtlsdr_write_eeprom() performs a per-byte read-modify-write and only
//     programs bytes that actually differ.
//   * A backup of the current EEPROM is written to disk before every write,
//     and the result is verified by reading the EEPROM back.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "eeprom.h"
#include "rtl-sdr.h"

namespace {

enum ControlId {
    IDC_DEVCOMBO = 1001,
    IDC_REFRESH,
    IDC_READ,
    IDC_MANUF,
    IDC_PRODUCT,
    IDC_SERIAL,
    IDC_HAVESERIAL,
    IDC_IR,
    IDC_WAKEUP,
    IDC_CLEARSERIAL,
    IDC_VIDPID,
    IDC_WRITE,
    IDC_BACKUP,
    IDC_RESTORE,
    IDC_LOG,
};

const wchar_t kWindowClass[] = L"RTLSerializerWindow";

HINSTANCE g_instance = nullptr;
HWND g_main = nullptr;
HFONT g_font = nullptr;

HWND g_combo = nullptr;
HWND g_deviceStatus = nullptr;
HWND g_manuf = nullptr;
HWND g_product = nullptr;
HWND g_serial = nullptr;
HWND g_haveSerial = nullptr;
HWND g_ir = nullptr;
HWND g_wakeup = nullptr;
HWND g_vidpid = nullptr;
HWND g_validation = nullptr;
HWND g_write = nullptr;
HWND g_log = nullptr;

std::vector<std::string> g_deviceLabels;
uint8_t g_original[rtlsdr_eeprom::kEepromSize];
rtlsdr_eeprom::Config g_conf;
bool g_loaded = false;
bool g_populating = false;

// Forward declarations.
void UpdateValidation();
void BuildConfigFromUi(rtlsdr_eeprom::Config& out);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void AppendLog(const std::string& text) {
    if (!g_log) {
        return;
    }
    std::string line = text + "\r\n";
    int len = GetWindowTextLengthA(g_log);
    SendMessageA(g_log, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    SendMessageA(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void SetText(HWND h, const std::string& s) {
    if (h) {
        SetWindowTextA(h, s.c_str());
    }
}

std::string GetText(HWND h) {
    if (!h) {
        return std::string();
    }
    int len = GetWindowTextLengthA(h);
    std::string buf(static_cast<std::size_t>(len) + 1, '\0');
    GetWindowTextA(h, &buf[0], len + 1);
    buf.resize(static_cast<std::size_t>(len));
    return buf;
}

std::wstring GetTextW(HWND h) {
    if (!h) {
        return std::wstring();
    }
    int len = GetWindowTextLengthW(h);
    std::wstring buf(static_cast<std::size_t>(len) + 1, L'\0');
    GetWindowTextW(h, &buf[0], len + 1);
    buf.resize(static_cast<std::size_t>(len));
    return buf;
}

std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    std::size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) {
        return std::string();
    }
    std::size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

// Character policy. Only plain ASCII is ever written to the EEPROM, which
// avoids encoding surprises in the USB string descriptors.
//   * serial              -> digits 0-9 only
//   * manufacturer/product -> letters, digits, space and - _ . +
bool IsAllowedChar(wchar_t c, bool digits_only) {
    if (c >= L'0' && c <= L'9') {
        return true;
    }
    if (digits_only) {
        return false;
    }
    if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) {
        return true;
    }
    return c == L' ' || c == L'-' || c == L'_' || c == L'.' || c == L'+';
}

// Remove any disallowed characters from an edit control in place. Called on
// every EN_CHANGE so typed and pasted text is filtered immediately.
void SanitizeEdit(HWND h, bool digits_only) {
    if (!h) {
        return;
    }
    std::wstring in = GetTextW(h);
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t c : in) {
        if (IsAllowedChar(c, digits_only)) {
            out.push_back(c);
        }
    }
    if (out == in) {
        return;
    }

    LRESULT sel = SendMessageW(h, EM_GETSEL, 0, 0);
    int caret = LOWORD(sel);
    SetWindowTextW(h, out.c_str());
    if (caret > static_cast<int>(out.size())) {
        caret = static_cast<int>(out.size());
    }
    SendMessageW(h, EM_SETSEL, caret, caret);
}

void BuildConfigFromUi(rtlsdr_eeprom::Config& out) {
    out = g_conf;
    out.manufacturer = Trim(GetText(g_manuf));
    out.product = Trim(GetText(g_product));
    out.serial = Trim(GetText(g_serial));
    out.have_serial = IsDlgButtonChecked(g_main, IDC_HAVESERIAL) == BST_CHECKED;
    out.enable_ir = IsDlgButtonChecked(g_main, IDC_IR) == BST_CHECKED;
    out.remote_wakeup = IsDlgButtonChecked(g_main, IDC_WAKEUP) == BST_CHECKED;
}

// Re-evaluates whether the current field contents can be written; enables or
// disables the Write button and explains any problem.
void UpdateValidation() {
    if (!g_write) {
        return;
    }
    if (!g_loaded) {
        EnableWindow(g_write, FALSE);
        if (g_validation) {
            SetText(g_validation, "");
        }
        return;
    }

    rtlsdr_eeprom::Config conf;
    BuildConfigFromUi(conf);

    uint8_t scratch[rtlsdr_eeprom::kEepromSize];
    std::memcpy(scratch, g_original, sizeof(scratch));

    if (!rtlsdr_eeprom::GenerateEeprom(conf, scratch)) {
        SetText(g_validation,
                "Too long! The three strings must fit the descriptor area (max 31 characters "
                "total).");
        EnableWindow(g_write, FALSE);
        return;
    }

    if (conf.have_serial && conf.serial.empty()) {
        SetText(g_validation,
                "Serial is enabled but empty. Type a number or untick 'Serial number enabled'.");
        EnableWindow(g_write, FALSE);
        return;
    }

    const int cap = rtlsdr_eeprom::kIrConfigOffset - rtlsdr_eeprom::kStrOffset;
    const int used = rtlsdr_eeprom::DescriptorEnd(conf) - rtlsdr_eeprom::kStrOffset;
    char info[160];
    std::snprintf(info, sizeof(info), "OK: descriptor space %d/%d bytes.  Serial: 0-9 only.",
                  used, cap);
    SetText(g_validation, info);
    EnableWindow(g_write, TRUE);
}

std::string ExeDir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    std::size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) {
        s = s.substr(0, p);
    }
    return s;
}

bool WriteFileBytes(const std::string& path, const uint8_t* data, int len) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    std::size_t written = fwrite(data, 1, static_cast<std::size_t>(len), f);
    fclose(f);
    return written == static_cast<std::size_t>(len);
}

bool ReadFileBytes(const std::string& path, uint8_t* data, int len, int& got) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    got = static_cast<int>(fread(data, 1, static_cast<std::size_t>(len), f));
    fclose(f);
    return true;
}

int SelectedDevice() {
    LRESULT r = SendMessageA(g_combo, CB_GETCURSEL, 0, 0);
    return (r == CB_ERR) ? -1 : static_cast<int>(r);
}

void EnableEditing(bool enabled) {
    HWND controls[] = {g_manuf, g_product, g_serial, g_haveSerial, g_ir, g_wakeup};
    for (HWND h : controls) {
        if (h) {
            EnableWindow(h, enabled ? TRUE : FALSE);
        }
    }
}

// ---------------------------------------------------------------------------
// EEPROM access
// ---------------------------------------------------------------------------

bool OpenDevice(int index, rtlsdr_dev_t** dev, std::string& err) {
    int r = rtlsdr_open(dev, static_cast<uint32_t>(index));
    if (r < 0) {
        err = "rtlsdr_open() failed (" + std::to_string(r) + ")";
        return false;
    }
    return true;
}

bool ReadEepromRaw(int index, uint8_t* buffer, std::string& err) {
    rtlsdr_dev_t* dev = nullptr;
    if (!OpenDevice(index, &dev, err)) {
        return false;
    }
    std::memset(buffer, 0, rtlsdr_eeprom::kEepromSize);
    int r = rtlsdr_read_eeprom(dev, buffer, 0, rtlsdr_eeprom::kEepromSize);
    rtlsdr_close(dev);
    if (r < 0) {
        if (r == -3) {
            err = "no EEPROM was found on this device";
        } else {
            err = "rtlsdr_read_eeprom() failed (" + std::to_string(r) + ")";
        }
        return false;
    }
    return true;
}

bool WriteEepromRaw(int index, uint8_t* buffer, int len, std::string& err) {
    rtlsdr_dev_t* dev = nullptr;
    if (!OpenDevice(index, &dev, err)) {
        return false;
    }
    int r = rtlsdr_write_eeprom(dev, buffer, 0, static_cast<uint16_t>(len));
    rtlsdr_close(dev);
    if (r < 0) {
        err = "rtlsdr_write_eeprom() failed (" + std::to_string(r) + ")";
        return false;
    }
    return true;
}

std::string MakeBackupPath(int index) {
    std::string dir = ExeDir() + "\\backups";
    CreateDirectoryA(dir.c_str(), nullptr);

    std::string serial = g_conf.serial;
    if (serial.empty()) {
        serial = "nodevice";
    }
    for (char& c : serial) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[512];
    std::snprintf(name, sizeof(name), "\\eeprom_dev%d_%s_%04d%02d%02d_%02d%02d%02d.bin",
                  index, serial.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return dir + name;
}

bool BackupCurrent(int index, std::string& outPath) {
    outPath = MakeBackupPath(index);
    return WriteFileBytes(outPath, g_original, rtlsdr_eeprom::kEepromSize);
}

// ---------------------------------------------------------------------------
// UI population
// ---------------------------------------------------------------------------

void PopulateFields() {
    g_populating = true;
    SetText(g_manuf, g_conf.manufacturer);
    SetText(g_product, g_conf.product);
    SetText(g_serial, g_conf.serial);
    CheckDlgButton(g_main, IDC_HAVESERIAL, g_conf.have_serial ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_IR, g_conf.enable_ir ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_WAKEUP, g_conf.remote_wakeup ? BST_CHECKED : BST_UNCHECKED);
    g_populating = false;

    char vp[128];
    std::snprintf(vp, sizeof(vp), "Vendor ID: 0x%04x     Product ID: 0x%04x",
                  g_conf.vendor_id, g_conf.product_id);
    SetText(g_vidpid, vp);

    UpdateValidation();
}

void RefreshDeviceList() {
    SendMessageA(g_combo, CB_RESETCONTENT, 0, 0);
    g_deviceLabels.clear();

    uint32_t count = rtlsdr_get_device_count();
    for (uint32_t i = 0; i < count; ++i) {
        char manuf[256] = {0};
        char product[256] = {0};
        char serial[256] = {0};
        rtlsdr_get_device_usb_strings(i, manuf, product, serial);

        char line[768];
        std::snprintf(line, sizeof(line), "%u:  %s %s  [SN: %s]", i,
                      manuf[0] ? manuf : "?", product[0] ? product : "?",
                      serial[0] ? serial : "(none)");
        g_deviceLabels.push_back(line);
        SendMessageA(g_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
    }

    if (count > 0) {
        SendMessageA(g_combo, CB_SETCURSEL, 0, 0);
        SetText(g_deviceStatus, "Ready.");
        AppendLog("Found " + std::to_string(count) + " device(s).");
    } else {
        SetText(g_deviceStatus, "No RTL-SDR devices found.");
        AppendLog("No RTL-SDR devices found. Plug in a device and press Refresh.");
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void DoRead() {
    int index = SelectedDevice();
    if (index < 0) {
        AppendLog("Select a device first.");
        return;
    }

    std::string err;
    if (!ReadEepromRaw(index, g_original, err)) {
        g_loaded = false;
        EnableEditing(false);
        UpdateValidation();
        SetText(g_deviceStatus, "Read failed: " + err);
        AppendLog("Read failed: " + err);
        MessageBoxA(g_main, err.c_str(), "Read EEPROM", MB_ICONERROR);
        return;
    }

    if (!rtlsdr_eeprom::ParseEeprom(g_original, g_conf)) {
        g_loaded = true;
        EnableEditing(true);
        char header[128];
        std::snprintf(header, sizeof(header),
                      "Vendor ID: -     Product ID: -     (bad header %02x %02x)", g_original[0],
                      g_original[1]);
        AppendLog("Warning: EEPROM header is not 0x28 0x32; fields shown may be wrong.");
        g_conf = rtlsdr_eeprom::Config{};
        PopulateFields();
        SetText(g_vidpid, header);
        SetText(g_validation,
                "EEPROM header not recognised; writing is disabled. Use Restore with a good "
                "dump.");
        EnableWindow(g_write, FALSE);
        return;
    }

    g_loaded = true;
    EnableEditing(true);
    PopulateFields();
    SetText(g_deviceStatus, "EEPROM loaded.");
    char ids[128];
    std::snprintf(ids, sizeof(ids), "Read OK: Vendor ID 0x%04x, Product ID 0x%04x",
                  g_conf.vendor_id, g_conf.product_id);
    AppendLog(ids + std::string(" (device ") + std::to_string(index) + ").");
    AppendLog("  Manufacturer: \"" + g_conf.manufacturer + "\"");
    AppendLog("  Product     : \"" + g_conf.product + "\"");
    AppendLog("  Serial      : \"" + g_conf.serial + "\"");
}

void DoWrite() {
    if (!g_loaded) {
        AppendLog("Read the EEPROM before writing.");
        return;
    }
    int index = SelectedDevice();
    if (index < 0) {
        AppendLog("Select a device first.");
        return;
    }

    rtlsdr_eeprom::Config conf;
    BuildConfigFromUi(conf);  // preserves vendor/product ids and flags

    uint8_t image[rtlsdr_eeprom::kEepromSize];
    std::memcpy(image, g_original, rtlsdr_eeprom::kEepromSize);
    if (!rtlsdr_eeprom::GenerateEeprom(conf, image)) {
        MessageBoxA(g_main,
                    "The manufacturer / product / serial strings are too long to fit in the "
                    "EEPROM's descriptor area.\n\nShorten them and try again.",
                    "Strings too long", MB_ICONWARNING);
        return;
    }
    if (conf.have_serial && conf.serial.empty()) {
        MessageBoxA(g_main,
                    "The serial number is enabled but empty. Type a number, or untick "
                    "'Serial number enabled'.",
                    "Empty serial", MB_ICONWARNING);
        return;
    }

    int diffs = rtlsdr_eeprom::CountDifferences(g_original, image, rtlsdr_eeprom::kConfigSize);
    if (diffs == 0) {
        AppendLog("No changes: the EEPROM already matches the fields shown.");
        MessageBoxA(g_main, "Nothing to write - the EEPROM already matches the fields shown.",
                    "No changes", MB_ICONINFORMATION);
        return;
    }

    char msg[2048];
    std::snprintf(msg, sizeof(msg),
                  "Write EEPROM configuration to device %d?\n\n"
                  "  Manufacturer : \"%s\"\n"
                  "  Product      : \"%s\"\n"
                  "  Serial       : \"%s\"  (enabled: %s)\n"
                  "  IR endpoint  : %s\n"
                  "  Remote wakeup: %s\n\n"
                  "%d byte(s) in the 128-byte configuration area will change.\n"
                  "A backup of the current EEPROM is written to the 'backups' folder first.\n\n"
                  "Continue?",
                  index, conf.manufacturer.c_str(), conf.product.c_str(), conf.serial.c_str(),
                  conf.have_serial ? "yes" : "no", conf.enable_ir ? "yes" : "no",
                  conf.remote_wakeup ? "yes" : "no", diffs);

    if (MessageBoxA(g_main, msg, "Confirm EEPROM write",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        AppendLog("Write cancelled by user.");
        return;
    }

    std::string backupPath;
    if (BackupCurrent(index, backupPath)) {
        AppendLog("Backup saved: " + backupPath);
    } else {
        if (MessageBoxA(g_main,
                        "Could not create a backup file.\n\n"
                        "Proceed with the write anyway?",
                        "Backup failed", MB_YESNO | MB_ICONERROR | MB_DEFBUTTON2) != IDYES) {
            AppendLog("Write aborted: no backup could be created.");
            return;
        }
        AppendLog("Warning: proceeding without a backup.");
    }

    std::string err;
    if (!WriteEepromRaw(index, image, rtlsdr_eeprom::kConfigSize, err)) {
        AppendLog("Write failed: " + err);
        MessageBoxA(g_main, err.c_str(), "Write EEPROM", MB_ICONERROR);
        return;
    }

    uint8_t verify[rtlsdr_eeprom::kEepromSize];
    std::string verifyErr;
    if (ReadEepromRaw(index, verify, verifyErr)) {
        int mismatch =
            rtlsdr_eeprom::CountDifferences(verify, image, rtlsdr_eeprom::kConfigSize);
        if (mismatch == 0) {
            AppendLog("Verification: EEPROM matches what was written.");
        } else {
            AppendLog("WARNING: verification found " + std::to_string(mismatch) +
                      " mismatched byte(s); consider restoring the backup.");
        }
    } else {
        AppendLog("Could not re-read the EEPROM for verification: " + verifyErr);
    }

    std::memcpy(g_original, image, rtlsdr_eeprom::kEepromSize);
    g_conf = conf;
    PopulateFields();
    SetText(g_deviceStatus, "Write complete - replug the device.");

    MessageBoxA(g_main,
                "Configuration written successfully.\n\n"
                "Unplug and replug the device for the new descriptors to take effect.",
                "Success", MB_ICONINFORMATION);
}

void DoBackup() {
    if (!g_loaded) {
        AppendLog("Read the EEPROM first.");
        return;
    }
    std::string path;
    if (BackupCurrent(SelectedDevice(), path)) {
        AppendLog("Backup saved: " + path);
        MessageBoxA(g_main, ("Backup saved to:\n\n" + path).c_str(), "Backup",
                    MB_ICONINFORMATION);
    } else {
        AppendLog("Backup failed.");
        MessageBoxA(g_main, "Could not write the backup file.", "Backup", MB_ICONERROR);
    }
}

void DoRestore() {
    int index = SelectedDevice();
    if (index < 0) {
        AppendLog("Select a device first.");
        return;
    }

    char file[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    std::memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = "EEPROM dump (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Restore EEPROM from file";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameA(&ofn)) {
        return;
    }

    uint8_t buffer[rtlsdr_eeprom::kEepromSize];
    int got = 0;
    if (!ReadFileBytes(file, buffer, rtlsdr_eeprom::kEepromSize, got) ||
        got != rtlsdr_eeprom::kEepromSize) {
        MessageBoxA(g_main, "The selected file must be exactly 256 bytes.", "Invalid file",
                    MB_ICONERROR);
        return;
    }

    if (MessageBoxA(g_main,
                    "Overwrite the entire 256-byte EEPROM with this file?\n\n"
                    "Only do this with a dump taken from a known-good RTL-SDR.",
                    "Confirm full restore", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    if (g_loaded) {
        std::string backupPath;
        if (BackupCurrent(index, backupPath)) {
            AppendLog("Backup before restore: " + backupPath);
        }
    }

    std::string err;
    if (!WriteEepromRaw(index, buffer, rtlsdr_eeprom::kEepromSize, err)) {
        AppendLog("Restore failed: " + err);
        MessageBoxA(g_main, err.c_str(), "Restore EEPROM", MB_ICONERROR);
        return;
    }

    std::memcpy(g_original, buffer, rtlsdr_eeprom::kEepromSize);
    g_loaded = true;
    EnableEditing(true);
    if (rtlsdr_eeprom::ParseEeprom(g_original, g_conf)) {
        PopulateFields();
    }
    AppendLog("Full EEPROM restore complete. Replug the device.");
    MessageBoxA(g_main,
                "EEPROM restored.\n\nUnplug and replug the device for changes to take effect.",
                "Restore complete", MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// Window creation
// ---------------------------------------------------------------------------

HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle, int x,
                 int y, int w, int h, int id) {
    HWND hwnd = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                g_main, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                g_instance, nullptr);
    if (hwnd && g_font) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
    return hwnd;
}

void CreateControls() {
    const DWORD editStyle = WS_TABSTOP | ES_AUTOHSCROLL;

    MakeControl(L"BUTTON", L"Device", BS_GROUPBOX, 0, 10, 10, 620, 90, -1);
    MakeControl(L"STATIC", L"RTL-SDR device:", SS_LEFT, 0, 24, 40, 100, 18, -1);
    g_combo = MakeControl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 126,
                          36, 340, 220, IDC_DEVCOMBO);
    MakeControl(L"BUTTON", L"Refresh", WS_TABSTOP | BS_PUSHBUTTON, 0, 476, 33, 140, 24,
                IDC_REFRESH);
    MakeControl(L"BUTTON", L"Read EEPROM", WS_TABSTOP | BS_PUSHBUTTON, 0, 476, 61, 140, 24,
                IDC_READ);
    g_deviceStatus = MakeControl(L"STATIC", L"Press Refresh to scan for devices.", SS_LEFT, 0, 24,
                                 68, 440, 18, -1);

    MakeControl(L"BUTTON", L"EEPROM contents", BS_GROUPBOX, 0, 10, 108, 620, 210, -1);
    MakeControl(L"STATIC", L"Manufacturer", SS_LEFT, 0, 24, 138, 100, 18, -1);
    g_manuf = MakeControl(L"EDIT", L"", editStyle | WS_TABSTOP, WS_EX_CLIENTEDGE, 128, 134, 490,
                          22, IDC_MANUF);
    MakeControl(L"STATIC", L"Product", SS_LEFT, 0, 24, 168, 100, 18, -1);
    g_product = MakeControl(L"EDIT", L"", editStyle | WS_TABSTOP, WS_EX_CLIENTEDGE, 128, 164, 490,
                            22, IDC_PRODUCT);
    MakeControl(L"STATIC", L"Serial number (0-9)", SS_LEFT, 0, 24, 198, 110, 18, -1);
    g_serial = MakeControl(L"EDIT", L"", editStyle | WS_TABSTOP, WS_EX_CLIENTEDGE, 128, 194, 490,
                           22, IDC_SERIAL);

    g_haveSerial = MakeControl(L"BUTTON", L"Serial number enabled", BS_AUTOCHECKBOX | WS_TABSTOP,
                               0, 128, 224, 170, 20, IDC_HAVESERIAL);
    g_ir = MakeControl(L"BUTTON", L"IR endpoint enabled", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 306,
                       224, 170, 20, IDC_IR);
    g_wakeup = MakeControl(L"BUTTON", L"Remote wakeup enabled", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                           128, 246, 170, 20, IDC_WAKEUP);
    MakeControl(L"BUTTON", L"Clear serial number", WS_TABSTOP | BS_PUSHBUTTON, 0, 306, 244, 150,
                24, IDC_CLEARSERIAL);
    g_vidpid = MakeControl(L"STATIC", L"Vendor ID: -     Product ID: -", SS_LEFT, 0, 128, 276, 490,
                           18, IDC_VIDPID);
    g_validation = MakeControl(L"STATIC", L"", SS_LEFT, 0, 24, 296, 596, 16, -1);

    g_write = MakeControl(L"BUTTON", L"Write to device...", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 128,
                          328, 170, 30, IDC_WRITE);
    MakeControl(L"BUTTON", L"Backup EEPROM...", WS_TABSTOP | BS_PUSHBUTTON, 0, 308, 328, 150, 30,
                IDC_BACKUP);
    MakeControl(L"BUTTON", L"Restore from file...", WS_TABSTOP | BS_PUSHBUTTON, 0, 468, 328, 150,
                30, IDC_RESTORE);

    MakeControl(L"BUTTON", L"Log", BS_GROUPBOX, 0, 10, 370, 620, 175, -1);
    g_log = MakeControl(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                                        WS_VSCROLL,
                        WS_EX_CLIENTEDGE, 22, 392, 596, 143, IDC_LOG);

    // Hard cap on typed length; the combined descriptor fit is checked live.
    SendMessageW(g_manuf, EM_SETLIMITTEXT, 31, 0);
    SendMessageW(g_product, EM_SETLIMITTEXT, 31, 0);
    SendMessageW(g_serial, EM_SETLIMITTEXT, 31, 0);

    EnableEditing(false);
    UpdateValidation();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_main = hwnd;
            CreateControls();
            AppendLog("RTLSerializer - read and write RTL-SDR serial numbers.");
            AppendLog("Always keep a backup before writing. Use at your own risk.");
            RefreshDeviceList();
            return 0;

        case WM_COMMAND:
            {
                const int id = LOWORD(wParam);
                const int code = HIWORD(wParam);

                if (code == EN_CHANGE &&
                    (id == IDC_MANUF || id == IDC_PRODUCT || id == IDC_SERIAL)) {
                    if (!g_populating) {
                        SanitizeEdit(reinterpret_cast<HWND>(lParam), id == IDC_SERIAL);
                        UpdateValidation();
                    }
                    return 0;
                }

                if (code == BN_CLICKED &&
                    (id == IDC_HAVESERIAL || id == IDC_IR || id == IDC_WAKEUP)) {
                    UpdateValidation();
                    return 0;
                }

                switch (id) {
                    case IDC_REFRESH:
                        RefreshDeviceList();
                        return 0;
                    case IDC_READ:
                        DoRead();
                        return 0;
                    case IDC_WRITE:
                        DoWrite();
                        return 0;
                    case IDC_BACKUP:
                        DoBackup();
                        return 0;
                    case IDC_RESTORE:
                        DoRestore();
                        return 0;
                    case IDC_CLEARSERIAL:
                        SetText(g_serial, "");
                        CheckDlgButton(g_main, IDC_HAVESERIAL, BST_UNCHECKED);
                        UpdateValidation();
                        return 0;
                    default:
                        break;
                }
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_font) {
                DeleteObject(g_font);
                g_font = nullptr;
            }
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void InitFont() {
    NONCLIENTMETRICSA ncm;
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_font = CreateFontIndirectA(&ncm.lfMessageFont);
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    g_instance = instance;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    InitFont();

    WNDCLASSEXW wc;
    std::memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    RECT rect = {0, 0, 640, 552};
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    HWND hwnd = CreateWindowExW(
        0, kWindowClass, L"RTLSerializer - RTL-SDR EEPROM serial tool",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

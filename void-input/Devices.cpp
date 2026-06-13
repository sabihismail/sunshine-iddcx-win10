#include "Devices.h"

// ---------------------------------------------------------------------------
// Mouse
//
// One HID device exposing two collections, selected by report id:
//   ID 1 - relative pointer: signed 16-bit X/Y deltas
//   ID 2 - absolute pointer: unsigned 16-bit X/Y over a 0..32767 range
// Both carry a 5-button bitmask (L=0x01 R=0x02 M=0x04 X1=0x08 X2=0x10), a
// vertical wheel, and a horizontal wheel (AC Pan). Report bytes after the
// report-id byte:  buttons(1) + X(2) + Y(2) + wheel(1) + hwheel(1) = 7,
// so each report is 8 bytes including the id.
// ---------------------------------------------------------------------------
static const UCHAR k_MouseReportDescriptor[] = {
    // ---- Relative pointer (Report ID 1) ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x05,        //     Usage Maximum (Button 5)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x05,        //     Report Count (5)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)        ; 5 buttons
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x03,        //     Report Size (3)
    0x81, 0x03,        //     Input (Const,Var,Abs)       ; 3-bit pad
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x16, 0x01, 0x80,  //     Logical Minimum (-32767)
    0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data,Var,Rel)        ; X, Y relative
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data,Var,Rel)        ; wheel
    0x05, 0x0C,        //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,  //     Usage (AC Pan)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data,Var,Rel)        ; horizontal wheel
    0xC0,              //   End Collection (Physical)
    0xC0,              // End Collection (Application)

    // ---- Absolute pointer (Report ID 2) ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x05,        //     Usage Maximum (Button 5)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x05,        //     Report Count (5)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)        ; 5 buttons
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x03,        //     Report Size (3)
    0x81, 0x03,        //     Input (Const,Var,Abs)       ; 3-bit pad
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x02,        //     Input (Data,Var,Abs)        ; X, Y absolute
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data,Var,Rel)        ; wheel
    0x05, 0x0C,        //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,  //     Usage (AC Pan)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data,Var,Rel)        ; horizontal wheel
    0xC0,              //   End Collection (Physical)
    0xC0,              // End Collection (Application)
};

// ---------------------------------------------------------------------------
// Keyboard
//
// Two collections selected by report id:
//   ID 1 - boot keyboard: 8 modifier bits (LCtrl..RGui), a reserved byte, and
//          six key-code slots (6-key rollover, HID Keyboard/Keypad usages).
//   ID 2 - consumer control: one 16-bit consumer usage (media/volume keys), 0 = none.
// Input-only for now (no lock-LED output report); LED feedback arrives with the
// output-event channel in a later milestone. Report bytes after the id:
//   keyboard - modifiers(1) + reserved(1) + keys(6) = 8  (9 with the id)
//   consumer - usage(2)                                  (3 with the id)
// ---------------------------------------------------------------------------
static const UCHAR k_KeyboardReportDescriptor[] = {
    // ---- Boot keyboard (Report ID 1) ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)        ; 8 modifier bits
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Const,Var,Abs)       ; reserved byte
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0xFF,        //   Usage Maximum (255)
    0x81, 0x00,        //   Input (Data,Array)          ; 6 key-code slots
    0xC0,              // End Collection

    // ---- Consumer control (Report ID 2) ----
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0x3C, 0x02,  //   Usage Maximum (0x023C)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0x3C, 0x02,  //   Logical Maximum (0x023C)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x10,        //   Report Size (16)
    0x81, 0x00,        //   Input (Data,Array)          ; one consumer usage (0 = none)
    0xC0,              // End Collection
};

// ---------------------------------------------------------------------------
// Xbox One game pad (HID form)
//
// A single (un-numbered) input report and one force-feedback output report:
//   input  (17 bytes): X/Y/Rx/Ry as 16-bit sticks (0x8000 centered), Z/Rz as
//           10-bit triggers, 10 buttons, a 4-bit hat, a system-menu bit, and a
//           battery byte.
//   output (8 bytes): PID-page rumble - an enable nibble, four 8-bit motor
//           magnitudes (left/right + left/right trigger), duration, delay, loop.
// Visible to DirectInput / Windows.Gaming.Input / Steam Input (not XInput/xusb).
// ---------------------------------------------------------------------------
static const UCHAR k_XboxOneReportDescriptor[] = {
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x05,                    // Usage (Game Pad)
    0xA1, 0x01,                    // Collection (Application)
    0xA1, 0x00,                    //   Collection (Physical)
    0x09, 0x30,                    //     Usage (X)
    0x09, 0x31,                    //     Usage (Y)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65535)
    0x95, 0x02,                    //     Report Count (2)
    0x75, 0x10,                    //     Report Size (16)
    0x81, 0x02,                    //     Input (Data,Var,Abs)        ; left stick
    0xC0,                          //   End Collection
    0xA1, 0x00,                    //   Collection (Physical)
    0x09, 0x33,                    //     Usage (Rx)
    0x09, 0x34,                    //     Usage (Ry)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65535)
    0x95, 0x02,                    //     Report Count (2)
    0x75, 0x10,                    //     Report Size (16)
    0x81, 0x02,                    //     Input (Data,Var,Abs)        ; right stick
    0xC0,                          //   End Collection
    0x05, 0x01,                    //   Usage Page (Generic Desktop)
    0x09, 0x32,                    //   Usage (Z)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x26, 0xFF, 0x03,              //   Logical Maximum (1023)
    0x95, 0x01,                    //   Report Count (1)
    0x75, 0x0A,                    //   Report Size (10)
    0x81, 0x02,                    //   Input (Data,Var,Abs)          ; left trigger
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x00,                    //   Logical Maximum (0)
    0x75, 0x06,                    //   Report Size (6)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Const,Var,Abs)         ; 6-bit pad
    0x05, 0x01,                    //   Usage Page (Generic Desktop)
    0x09, 0x35,                    //   Usage (Rz)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x26, 0xFF, 0x03,              //   Logical Maximum (1023)
    0x95, 0x01,                    //   Report Count (1)
    0x75, 0x0A,                    //   Report Size (10)
    0x81, 0x02,                    //   Input (Data,Var,Abs)          ; right trigger
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x00,                    //   Logical Maximum (0)
    0x75, 0x06,                    //   Report Size (6)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Const,Var,Abs)         ; 6-bit pad
    0x05, 0x09,                    //   Usage Page (Button)
    0x19, 0x01,                    //   Usage Minimum (Button 1)
    0x29, 0x0A,                    //   Usage Maximum (Button 10)
    0x95, 0x0A,                    //   Report Count (10)
    0x75, 0x01,                    //   Report Size (1)
    0x81, 0x02,                    //   Input (Data,Var,Abs)          ; 10 buttons
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x00,                    //   Logical Maximum (0)
    0x75, 0x06,                    //   Report Size (6)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Const,Var,Abs)         ; 6-bit pad
    0x05, 0x01,                    //   Usage Page (Generic Desktop)
    0x09, 0x39,                    //   Usage (Hat switch)
    0x15, 0x01,                    //   Logical Minimum (1)
    0x25, 0x08,                    //   Logical Maximum (8)
    0x35, 0x00,                    //   Physical Minimum (0)
    0x46, 0x3B, 0x01,              //   Physical Maximum (315)
    0x66, 0x14, 0x00,              //   Unit (English Rotation: deg)
    0x75, 0x04,                    //   Report Size (4)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x42,                    //   Input (Data,Var,Abs,Null State); D-pad hat
    0x75, 0x04,                    //   Report Size (4)
    0x95, 0x01,                    //   Report Count (1)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x00,                    //   Logical Maximum (0)
    0x35, 0x00,                    //   Physical Minimum (0)
    0x45, 0x00,                    //   Physical Maximum (0)
    0x65, 0x00,                    //   Unit (None)
    0x81, 0x03,                    //   Input (Const,Var,Abs)         ; 4-bit pad
    0xA1, 0x02,                    //   Collection (Logical)          ; rumble output
    0x05, 0x0F,                    //     Usage Page (PID)
    0x09, 0x97,                    //     Usage (DC Enable Actuators)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x01,                    //     Logical Maximum (1)
    0x75, 0x04,                    //     Report Size (4)
    0x95, 0x01,                    //     Report Count (1)
    0x91, 0x02,                    //     Output (Data,Var,Abs)       ; enable nibble
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x00,                    //     Logical Maximum (0)
    0x91, 0x03,                    //     Output (Const,Var,Abs)      ; 4-bit pad
    0x09, 0x70,                    //     Usage (Magnitude)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x64,                    //     Logical Maximum (100)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x04,                    //     Report Count (4)
    0x91, 0x02,                    //     Output (Data,Var,Abs)       ; 4 motor magnitudes
    0x09, 0x50,                    //     Usage (Duration)
    0x66, 0x01, 0x10,              //     Unit (SI Linear: s)
    0x55, 0x0E,                    //     Unit Exponent (-2)
    0x26, 0xFF, 0x00,              //     Logical Maximum (255)
    0x95, 0x01,                    //     Report Count (1)
    0x91, 0x02,                    //     Output (Data,Var,Abs)       ; duration
    0x09, 0xA7,                    //     Usage (Start Delay)
    0x91, 0x02,                    //     Output (Data,Var,Abs)       ; delay
    0x65, 0x00,                    //     Unit (None)
    0x55, 0x00,                    //     Unit Exponent (0)
    0x09, 0x7C,                    //     Usage (Loop Count)
    0x91, 0x02,                    //     Output (Data,Var,Abs)       ; loop count
    0xC0,                          //   End Collection
    0x05, 0x01,                    //   Usage Page (Generic Desktop)
    0x09, 0x80,                    //   Usage (System Control)
    0xA1, 0x00,                    //   Collection (Physical)
    0x09, 0x85,                    //     Usage (System Main Menu)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x01,                    //     Logical Maximum (1)
    0x95, 0x01,                    //     Report Count (1)
    0x75, 0x01,                    //     Report Size (1)
    0x81, 0x02,                    //     Input (Data,Var,Abs)        ; guide button
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x00,                    //     Logical Maximum (0)
    0x75, 0x07,                    //     Report Size (7)
    0x95, 0x01,                    //     Report Count (1)
    0x81, 0x03,                    //     Input (Const,Var,Abs)       ; 7-bit pad
    0xC0,                          //   End Collection
    0x05, 0x06,                    //   Usage Page (Generic Device Controls)
    0x09, 0x20,                    //   Usage (Battery Strength)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x26, 0xFF, 0x00,              //   Logical Maximum (255)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x02,                    //   Input (Data,Var,Abs)          ; battery
    0xC0,                          // End Collection
};

// ---------------------------------------------------------------------------
// Registry. Types absent here return NULL from VoidInputGetDeviceDesc and so
// fail CREATE with STATUS_NOT_SUPPORTED until their milestone lands.
// ---------------------------------------------------------------------------
static const VOIDINPUT_DEVICE_DESC k_Devices[] = {
    {
        VoidInputDeviceMouse,
        0x1BCF, 0x0005, 0x0100,        // generic optical-mouse identity (override via CREATE)
        k_MouseReportDescriptor,
        (USHORT)sizeof(k_MouseReportDescriptor),
        TRUE,                          // numbered reports (ids 1 and 2)
        TRUE,                          // singleton
        FALSE,                         // not a gamepad
        VOIDINPUT_EVT_NONE,            // input-only
    },
    {
        VoidInputDeviceKeyboard,
        0x1A2C, 0x0042, 0x0100,        // generic USB-keyboard identity (override via CREATE)
        k_KeyboardReportDescriptor,
        (USHORT)sizeof(k_KeyboardReportDescriptor),
        TRUE,                          // numbered reports (ids 1 and 2)
        TRUE,                          // singleton
        FALSE,                         // not a gamepad
        VOIDINPUT_EVT_NONE,            // input-only (lock-LED output is a later milestone)
    },
    {
        VoidInputDeviceXboxOne,
        0x045E, 0x0B13, 0x0100,        // Xbox wireless controller (HID/BLE form)
        k_XboxOneReportDescriptor,
        (USHORT)sizeof(k_XboxOneReportDescriptor),
        FALSE,                         // single un-numbered input report
        FALSE,                         // not a singleton (up to 4 pads)
        TRUE,                          // gamepad (counts against the 4-pad cap)
        VOIDINPUT_EVT_WRITE_REPORT,    // force-feedback output report
    },
};

const VOIDINPUT_DEVICE_DESC* VoidInputGetDeviceDesc(VOIDINPUT_DEVICE_TYPE type)
{
    for (size_t i = 0; i < ARRAYSIZE(k_Devices); ++i) {
        if (k_Devices[i].Type == type) {
            return &k_Devices[i];
        }
    }
    return NULL;   // type not implemented yet
}

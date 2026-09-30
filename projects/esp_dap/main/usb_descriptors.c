#include "tusb.h"
#include "usb_descriptors.h"

// HID report descriptor for CMSIS-DAP (64 byte vendor)
const uint8_t hid_report_desc[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (0x01)
    0xA1, 0x01,        // Collection (Application)
    0x15, 0x00,        // Logical Minimum (0)
    0x26, 0xFF, 0x00,  // Logical Maximum (255)
    0x75, 0x08,        // Report Size (8)
    0x95, 0x40,        // Report Count (64)
    0x09, 0x01,        // Usage (0x01)
    0x81, 0x02,        // Input (Data,Var,Abs)
    0x95, 0x40,        // Report Count (64)
    0x09, 0x01,        // Usage (0x01)
    0x91, 0x02,        // Output (Data,Var,Abs)
    0x95, 0x01,        // Report Count (1)
    0x09, 0x01,        // Usage (0x01)
    0xB1, 0x02,        // Feature (Data,Var,Abs)
    0xC0               // End Collection
};

// Device descriptor
const uint8_t desc_device[] = {
    0x12,                       // bLength
    TUSB_DESC_DEVICE,           // bDescriptorType
    0x00, 0x02,                 // bcdUSB 2.00
    0x00,                       // bDeviceClass (defined in interface)
    0x00,                       // bDeviceSubClass
    0x00,                       // bDeviceProtocol
    0x40,                       // bMaxPacketSize0 64
    0x3A, 0x30,                 // idVendor 0x303A Espressif
    0x05, 0x40,                 // idProduct 0x4005 (CDC+HID)
    0x00, 0x01,                 // bcdDevice 1.00
    0x01,                       // iManufacturer
    0x02,                       // iProduct
    0x03,                       // iSerialNumber
    0x01                        // bNumConfigurations
};

// String descriptors index
enum {
    STRID_LANGID = 0,
    STRID_MANU,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_HID
};

const char* desc_strs[] = {
    (char[]){0x09, 0x04},          // 0: English
    "Espressif",                   // 1
    "ESP-DAP",                     // 2
    "ESPDAP001",                   // 3
    "ESP-DAP CDC",                 // 4
    "ESP-DAP HID",                 // 5
};

// Configuration descriptor: CDC (IAD) + HID
// Total length = Config(9) + IAD(8) + CDC Control(9+5+4+5) + CDC Data 9 + 7+7 + HID 9+9+7+7
// Use helpers from tinyusb: TUD_CONFIG_DESCRIPTOR, TUD_CDC_DESCRIPTOR, TUD_HID_DESCRIPTOR

// Endpoint numbers
enum {
    EP_CDC_NOTIF = 1,
    EP_CDC_DATA  = 2,
    EP_HID_OUT   = 3,
    EP_HID_IN    = 3, // same number, different dir
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

const uint8_t desc_cfg[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // CDC: Interface 0 + 1 (IAD)
    TUD_CDC_DESCRIPTOR(0, STRID_CDC, 0x80 | EP_CDC_NOTIF, 8, EP_CDC_DATA, 0x80 | EP_CDC_DATA, 64),

    // HID: Interface 2 (IN+OUT for CMSIS-DAP)
    TUD_HID_INOUT_DESCRIPTOR(2, STRID_HID, HID_ITF_PROTOCOL_NONE, sizeof(hid_report_desc), EP_HID_OUT, 0x80 | EP_HID_IN, 64, 1),
};

// Callback to provide HID report descriptor
uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return hid_report_desc;
}

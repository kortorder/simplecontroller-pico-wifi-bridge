// Original descriptor adapter. Device identity is supplied explicitly by the private build.
#include "tusb.h"
#include "scx2.hpp"
#include <algorithm>
#include <cstring>
static_assert(SC_USB_POWER_MA >= 2 && SC_USB_POWER_MA <= 500, "USB power declaration must be reviewed");
namespace {
const tusb_desc_device_t device = {
    sizeof(tusb_desc_device_t), TUSB_DESC_DEVICE, 0x0200, 0, 0, 0,
    CFG_TUD_ENDPOINT0_SIZE, SC_USB_VID, SC_USB_PID, 0x0100, 1, 2, 0, 1
};
constexpr std::uint16_t config_length = TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN;
std::uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1,1,0,config_length,0,SC_USB_POWER_MA),
    TUD_HID_DESCRIPTOR(0,0,HID_ITF_PROTOCOL_NONE,0,0x81,16,4)
};
}
extern "C" const std::uint8_t* tud_descriptor_device_cb() { return reinterpret_cast<const std::uint8_t*>(&device); }
extern "C" const std::uint8_t* tud_descriptor_configuration_cb(std::uint8_t index) {
    if(index!=0) return nullptr;
    // HID report descriptor length is the 16-bit field at configuration offset 25.
    constexpr auto offset = TUD_CONFIG_DESC_LEN + 9 + 7;
    configuration[offset]=std::uint8_t(scx2::hid_descriptor_size);
    configuration[offset+1]=std::uint8_t(scx2::hid_descriptor_size>>8);
    return configuration;
}
extern "C" const std::uint8_t* tud_hid_descriptor_report_cb(std::uint8_t instance) {
    return instance==0 ? scx2::hid_descriptor : nullptr;
}
extern "C" const std::uint16_t* tud_descriptor_string_cb(std::uint8_t index, std::uint16_t) {
    static std::uint16_t text[64];
    if(index==0) {text[0]=(TUSB_DESC_STRING<<8)|4; text[1]=0x0409; return text;}
    const char* s=index==1 ? "SimpleController prototype" : index==2 ? "SCX2 unverified XAC bridge" : nullptr;
    if(!s) return nullptr;
    const auto n=std::min<std::size_t>(std::strlen(s),63);
    for(std::size_t i=0;i<n;++i) text[i+1]=std::uint8_t(s[i]);
    text[0]=std::uint16_t((TUSB_DESC_STRING<<8)|(2*n+2)); return text;
}

// Original TinyUSB application driver for a single XInput gamepad interface.
// Wire/report reference: GP2040-CE (MIT), documented in docs/CRONUS_ZEN.md.
// No console authentication, headset, vibration or Microsoft identity is claimed.
#include "xinput_device.hpp"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include <array>
#include <cstring>

namespace {
std::uint8_t port=0, endpoint_in=0, endpoint_out=0;
bool pending=false;
alignas(4) std::array<std::uint8_t,20> input{};
alignas(4) std::array<std::uint8_t,32> output{};
void init() { endpoint_in=endpoint_out=0; pending=false; }
bool deinit() { init(); return true; }
void reset(std::uint8_t rhport) { port=rhport; init(); sc_xinput_reset_cb(); }
bool receive_output() {
    if(!endpoint_out || usbd_edpt_busy(port,endpoint_out)) return true;
    if(!usbd_edpt_claim(port,endpoint_out)) return false;
    if(usbd_edpt_xfer(port,endpoint_out,output.data(),output.size())) return true;
    usbd_edpt_release(port,endpoint_out); return false;
}
std::uint16_t open(std::uint8_t rhport, const tusb_desc_interface_t* interface, std::uint16_t maximum) {
    // 9-byte interface, 17-byte XInput gamepad descriptor, two 7-byte endpoints.
    constexpr std::uint16_t length=40;
    if(maximum<length || interface->bLength!=9 || interface->bInterfaceNumber!=0 ||
       interface->bAlternateSetting!=0 || interface->bNumEndpoints!=2 ||
       interface->bInterfaceClass!=0xff || interface->bInterfaceSubClass!=0x5d ||
       interface->bInterfaceProtocol!=1) return 0;
    const auto* bytes=reinterpret_cast<const std::uint8_t*>(interface);
    if(bytes[9]!=17 || bytes[10]!=0x21 || bytes[13]!=1 || bytes[15]!=0x81 || bytes[16]!=20 || bytes[22]!=0x02) return 0;
    const auto* in=reinterpret_cast<const tusb_desc_endpoint_t*>(bytes+26);
    const auto* out=reinterpret_cast<const tusb_desc_endpoint_t*>(bytes+33);
    if(in->bLength!=7 || out->bLength!=7 || in->bDescriptorType!=TUSB_DESC_ENDPOINT || out->bDescriptorType!=TUSB_DESC_ENDPOINT ||
       in->bEndpointAddress!=0x81 || out->bEndpointAddress!=0x02 ||
       in->bmAttributes.xfer!=TUSB_XFER_INTERRUPT || out->bmAttributes.xfer!=TUSB_XFER_INTERRUPT ||
       tu_edpt_packet_size(in)!=32 || tu_edpt_packet_size(out)!=32) return 0;
    if(!usbd_edpt_open(rhport,in) || !usbd_edpt_open(rhport,out)) return 0;
    port=rhport; endpoint_in=0x81; endpoint_out=0x02; pending=false;
    if(!receive_output()) { endpoint_in=endpoint_out=0; return 0; }
    return length;
}
bool control(std::uint8_t, std::uint8_t, const tusb_control_request_t*) {
    // Standard interface requests are handled by TinyUSB. Unknown class/vendor
    // requests must STALL rather than falsely acknowledge authentication.
    return false;
}
bool complete(std::uint8_t rhport, std::uint8_t endpoint, xfer_result_t result, std::uint32_t count) {
    if(rhport!=port) return false;
    if(endpoint==endpoint_in && endpoint_in) {
        if(!pending) return true; // Ignore completions invalidated by a reset.
        pending=false;
        if(result!=XFER_RESULT_SUCCESS || count!=input.size()) sc_xinput_report_failed_cb();
        else sc_xinput_report_complete_cb(input.data(),input.size());
        return true;
    }
    if(endpoint==endpoint_out && endpoint_out) {
        // Drain LED/rumble output so host writes cannot block. No output can
        // alter phone input, trigger pressure, or the output ownership policy.
        if(result!=XFER_RESULT_SUCCESS || count>output.size() || !receive_output()) sc_xinput_report_failed_cb();
        return true;
    }
    return false;
}
const usbd_class_driver_t driver={"SCX2 XInput",init,deinit,reset,open,control,complete,nullptr};
}
bool sc_xinput_ready() {
    return endpoint_in && tud_mounted() && !tud_suspended() && !pending && !usbd_edpt_busy(port,endpoint_in);
}
bool sc_xinput_send(const std::uint8_t* report, std::size_t length) {
    if(!report || length!=input.size() || !sc_xinput_ready() || !usbd_edpt_claim(port,endpoint_in)) return false;
    std::memcpy(input.data(),report,length); pending=true;
    if(usbd_edpt_xfer(port,endpoint_in,input.data(),input.size())) return true;
    pending=false; usbd_edpt_release(port,endpoint_in); return false;
}
extern "C" const usbd_class_driver_t* usbd_app_driver_get_cb(std::uint8_t* count) { *count=1; return &driver; }
extern "C" bool tud_vendor_control_xfer_cb(std::uint8_t rhport, std::uint8_t stage, const tusb_control_request_t* request) {
    // Microsoft OS 1.0 compatible-ID descriptor enables standard XUSB drivers
    // without impersonating another manufacturer's USB vendor/product identity.
    static const std::uint8_t compatible_id[]={
        40,0,0,0, 0,1, 4,0, 1,0,0,0,0,0,0,0,
        0,1, 'X','U','S','B','1','0',0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0
    };
    if(request->bmRequestType!=0xc0 || request->bRequest!=0x20 || request->wIndex!=4 || request->wValue!=0) return false;
    if(stage!=CONTROL_STAGE_SETUP) return true;
    return tud_control_xfer(rhport,request,const_cast<std::uint8_t*>(compatible_id),sizeof(compatible_id));
}

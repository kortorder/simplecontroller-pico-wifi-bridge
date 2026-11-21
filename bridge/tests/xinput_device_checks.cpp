// Runs the real XInput driver and USB descriptors with a small fake TinyUSB host.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#define SC_USB_XINPUT 1
#define SC_USB_VID 0x1209
#define SC_USB_PID 0x0001
#define SC_USB_POWER_MA 500
#define CFG_TUD_ENDPOINT0_SIZE 64
#define TUD_CONFIG_DESCRIPTOR(n,interfaces,string,total,attributes,power) 9,2,(total)&255,(total)>>8,interfaces,n,string,0x80|(attributes),(power)/2
constexpr int TUSB_DESC_DEVICE=1,TUSB_DESC_STRING=3,TUSB_DESC_ENDPOINT=5,TUSB_XFER_INTERRUPT=3,CONTROL_STAGE_SETUP=0;
enum xfer_result_t { XFER_RESULT_SUCCESS=0,XFER_RESULT_FAILED=1 };
struct __attribute__((packed)) tusb_desc_interface_t {std::uint8_t bLength,bDescriptorType,bInterfaceNumber,bAlternateSetting,bNumEndpoints,bInterfaceClass,bInterfaceSubClass,bInterfaceProtocol,iInterface;};
struct __attribute__((packed)) tusb_desc_endpoint_t {std::uint8_t bLength,bDescriptorType,bEndpointAddress;union{std::uint8_t raw;struct{std::uint8_t xfer:2,sync:2,usage:2,reserved:2;};}bmAttributes;std::uint16_t wMaxPacketSize;std::uint8_t bInterval;};
struct __attribute__((packed)) tusb_desc_device_t {std::uint8_t bLength,bDescriptorType;std::uint16_t bcdUSB;std::uint8_t bDeviceClass,bDeviceSubClass,bDeviceProtocol,bMaxPacketSize0;std::uint16_t idVendor,idProduct,bcdDevice;std::uint8_t iManufacturer,iProduct,iSerialNumber,bNumConfigurations;};
struct tusb_control_request_t{std::uint8_t bmRequestType,bRequest;std::uint16_t wValue,wIndex,wLength;};
struct usbd_class_driver_t {const char* name;void(*init)();bool(*deinit)();void(*reset)(std::uint8_t);std::uint16_t(*open)(std::uint8_t,const tusb_desc_interface_t*,std::uint16_t);bool(*control_xfer_cb)(std::uint8_t,std::uint8_t,const tusb_control_request_t*);bool(*xfer_cb)(std::uint8_t,std::uint8_t,xfer_result_t,std::uint32_t);void(*sof)(std::uint8_t,std::uint32_t);};
namespace fake {
unsigned checks=0,success=0,failure=0,resets=0;bool mounted=true,suspended=false,refuse=false;
struct Endpoint{bool opened=false,busy=false,claimed=false;std::uint8_t* buffer=nullptr;std::uint16_t size=0;};
std::array<Endpoint,256> endpoints{};std::vector<std::uint8_t> received,control;
void check(bool ok,const char* reason){if(!ok){std::cerr<<reason<<'\n';std::exit(1);}++checks;}
}
std::uint16_t tu_edpt_packet_size(const tusb_desc_endpoint_t* p){return p->wMaxPacketSize;}
bool tud_mounted(){return fake::mounted;}bool tud_suspended(){return fake::suspended;}
bool usbd_edpt_open(std::uint8_t,const tusb_desc_endpoint_t* p){fake::endpoints[p->bEndpointAddress].opened=true;return true;}
bool usbd_edpt_busy(std::uint8_t,std::uint8_t p){return fake::endpoints[p].busy;}
bool usbd_edpt_claim(std::uint8_t,std::uint8_t p){auto& e=fake::endpoints[p];if(e.busy||e.claimed)return false;e.claimed=true;return true;}
bool usbd_edpt_release(std::uint8_t,std::uint8_t p){fake::endpoints[p].claimed=false;return true;}
bool usbd_edpt_xfer(std::uint8_t,std::uint8_t p,std::uint8_t* buffer,std::uint16_t size){auto& e=fake::endpoints[p];if(fake::refuse||e.busy||!e.opened)return false;e.busy=true;e.claimed=false;e.buffer=buffer;e.size=size;return true;}
bool tud_control_xfer(std::uint8_t,const tusb_control_request_t* request,void* buffer,std::uint16_t size){auto* b=static_cast<std::uint8_t*>(buffer);fake::control.assign(b,b+std::min(request->wLength,size));return true;}
extern "C" void sc_xinput_report_complete_cb(const std::uint8_t* bytes,std::uint16_t n){++fake::success;fake::received.assign(bytes,bytes+n);}
extern "C" void sc_xinput_report_failed_cb(){++fake::failure;}
extern "C" void sc_xinput_reset_cb(){++fake::resets;}
#include "../firmware/xinput_device.cpp"
#include "../firmware/usb_descriptors.cpp"
int main(){
 using namespace fake;
 std::uint8_t count=0;auto* api=usbd_app_driver_get_cb(&count);check(count==1&&api->reset&&api->xfer_cb,"Custom driver registered");api->init();
 const auto* config=tud_descriptor_configuration_cb(0);check(config&&config[2]==49&&config[3]==0&&config[4]==1&&config[8]==250,"Single gamepad configuration length and power correct");check(!tud_descriptor_configuration_cb(1),"Unknown configuration rejected");
 auto* identity=reinterpret_cast<const tusb_desc_device_t*>(tud_descriptor_device_cb());check(identity->idVendor==0x1209&&identity->idProduct==1&&identity->bDeviceClass==0xff,"Explicit USB identity preserved");
 auto* os=tud_descriptor_string_cb(0xee,0);check(os&&os[0]==0x0312&&os[8]==0x20,"OS compatible-ID query advertised");check(!tud_descriptor_string_cb(200,0),"Unknown string rejected");
 auto* iface=reinterpret_cast<const tusb_desc_interface_t*>(config+9);
 check(!api->open(0,iface,39),"Truncated interface rejected without reading past bound");
 std::array<std::uint8_t,40> broken{};std::memcpy(broken.data(),config+9,40);broken[6]=0;check(!api->open(0,reinterpret_cast<const tusb_desc_interface_t*>(broken.data()),40),"Wrong interface subclass rejected");
 std::memcpy(broken.data(),config+9,40);broken[28]=0x83;check(!api->open(0,reinterpret_cast<const tusb_desc_interface_t*>(broken.data()),40),"Wrong endpoint rejected");
 check(api->open(0,iface,40)==40&&endpoints[0x81].opened&&endpoints[2].busy,"Actual descriptor opens gamepad and drains host output");
 check(sc_xinput_ready(),"Mounted, idle endpoint can accept report");
 std::array<std::uint8_t,20> report{};report[1]=20;report[4]=128;report[5]=178;
 check(!sc_xinput_send(nullptr,20)&&!sc_xinput_send(report.data(),19),"Invalid report cannot send");
 check(sc_xinput_send(report.data(),20)&&!sc_xinput_ready()&&success==0,"Submission is pending, not a completion");
 report[4]=0;check(endpoints[0x81].buffer[4]==128,"Driver owns immutable copy until actual completion");
 check(!sc_xinput_send(report.data(),20),"Cannot replace in-flight press with another packet");
 endpoints[0x81].busy=false;check(api->xfer_cb(0,0x81,XFER_RESULT_SUCCESS,20)&&success==1&&received[4]==128&&received[5]==178,"Only actual endpoint completion delivers original pressure");
 check(sc_xinput_send(report.data(),20),"Endpoint reusable after completion");endpoints[0x81].busy=false;api->xfer_cb(0,0x81,XFER_RESULT_SUCCESS,19);check(failure==1&&success==1,"Short completion fails, cannot ACK");
 check(sc_xinput_send(report.data(),20),"Next synthetic send accepted");endpoints[0x81].busy=false;api->xfer_cb(0,0x81,XFER_RESULT_FAILED,20);check(failure==2&&success==1,"Failed completion cannot ACK");
 refuse=true;check(!sc_xinput_send(report.data(),20)&&!endpoints[0x81].claimed,"Submission failure releases claim");refuse=false;
 check(sc_xinput_send(report.data(),20),"Report in flight before reset");endpoints={};api->reset(0);check(resets==1&&!sc_xinput_ready(),"Bus reset invalidates endpoints and notifies adapter");api->xfer_cb(0,0x81,XFER_RESULT_SUCCESS,20);check(success==1,"Old completion after reset cannot revive report");
 check(api->open(0,iface,40)==40,"Can re-enumerate after reset");
 endpoints[2].busy=false;api->xfer_cb(0,2,XFER_RESULT_SUCCESS,8);check(endpoints[2].busy&&success==1,"LED/rumble transfer rearmed without input callback");
 endpoints[2].busy=false;api->xfer_cb(0,2,XFER_RESULT_FAILED,0);check(failure==3,"Failed host output endpoint triggers device fault");
 suspended=true;check(!sc_xinput_ready()&&!sc_xinput_send(report.data(),20),"Suspend cannot send input");suspended=false;mounted=false;check(!sc_xinput_ready(),"Unmount cannot send input");mounted=true;
 tusb_control_request_t query{0xc0,0x20,0,4,255};check(tud_vendor_control_xfer_cb(0,0,&query)&&fake::control.size()==40&&fake::control[18]=='X'&&fake::control[23]=='0',"Compatible-ID descriptor identifies XUSB10");
 query.wLength=8;check(tud_vendor_control_xfer_cb(0,0,&query)&&fake::control.size()==8,"Control short read bounded by host request");query.bRequest=0x81;check(!tud_vendor_control_xfer_cb(0,0,&query),"Authentication requests are not fabricated");
 check(!api->control_xfer_cb(0,0,&query),"Unsupported class requests stall");check(api->deinit()&&!sc_xinput_ready(),"Driver deinit invalidates endpoints");
 std::cout<<"PASS: "<<checks<<" actual XInput driver/descriptor checks with fake TinyUSB host.\n";
}

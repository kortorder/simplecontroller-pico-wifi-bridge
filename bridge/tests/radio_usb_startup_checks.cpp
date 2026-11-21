// Runs the real firmware main loop + portable receiver with explicit fake SDK time,
// USB interrupts and Wi-Fi calls. No USB, radio or credentials used by this test.
#include "scx2.hpp"
#include "xinput_report.hpp"
#include "../firmware/lwipopts.h"
#if SC_TEST_OLD_UDP_POOL
#undef MEMP_NUM_UDP_PCB
#define MEMP_NUM_UDP_PCB 2
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#if SC_CHAIR_RELAY
#include <vector>
#endif

using u16_t = std::uint16_t;
using hid_report_type_t = int;
[[maybe_unused]] constexpr int HID_REPORT_TYPE_INPUT=1, IPADDR_TYPE_V4=0, PBUF_TRANSPORT=0, PBUF_RAM=0, ERR_OK=0;
[[maybe_unused]] constexpr int SC_WIFI_COUNTRY=0, SC_UDP_PORT=42736, SC_XAC_RIGHT_PORT=0, CYW43_LINK_UP=3, CYW43_ITF_STA=0, CYW43_AUTH_WPA2_AES_PSK=0;
constexpr char SC_WIFI_SSID[]="synthetic-host-test", SC_WIFI_PASSWORD[]="synthetic-host-test";
#if SC_CHAIR_RELAY
constexpr char SC_CHAIR_RELAY_ADDRESS[]="192.0.2.20";
constexpr unsigned CYW43_NONE_PM=0;
#endif
struct rng_128_t {std::uint64_t r[2];};
struct ip_addr_t {unsigned address=1;};
const ip_addr_t* IP_ADDR_ANY=nullptr;
struct udp_pcb { bool allocated=false; }; struct pbuf {u16_t tot_len=0; std::array<std::uint8_t,80> bytes{}; ip_addr_t alias_address{};};
int cyw43_state=0;
namespace fake {
    std::uint64_t time=0, last_feed=0; unsigned watchdog_ms=0, joins=0, checks=0, loop=0;
    bool mounted=true, busy=false, queued=false, disconnected=false, reboot=false, retired=false;
    bool expensive_boot_finished=false, offer_phase=false;
    int phase=0; unsigned phase_loop=0;
    udp_pcb* dhcp_socket=nullptr;
#if SC_USB_XINPUT
    scx2::XInputReport hid{};
#else
    scx2::HidReport hid{};
#endif
    std::optional<scx2::Packet> response,offer;
#if SC_CHAIR_RELAY
    std::vector<scx2::Packet> chair_packets;
    unsigned sockets=0;
    bool poison_address_on_free=false;
    unsigned last_app_reply_address=0;
    std::vector<pbuf*> poisoned_pbufs;
    bool station_enabled=false;
    int power_result=0;
    unsigned power_calls=0;
#endif
    struct Done{};
    void check(bool ok,const char* reason) {if(!ok){std::cerr<<reason<<'\n';std::exit(1);} ++checks;}
    void advance(std::uint64_t duration) {time+=duration; if(watchdog_ms && time-last_feed>watchdog_ms*1000u){std::cerr<<"simulated hardware watchdog expired\n";std::exit(1);}}
}
std::uint64_t time_us_64(){return fake::time;}
void get_rand_128(rng_128_t* p){static std::uint64_t n=1;p->r[0]=++n;p->r[1]=n+1;}
void watchdog_enable(unsigned ms,bool){if(ms>250 && fake::offer_phase)fake::check(false,"extended watchdog after offer");fake::watchdog_ms=ms;fake::last_feed=fake::time;}
void watchdog_update(){fake::last_feed=fake::time;}
void watchdog_reboot(int,int,int){fake::reboot=true;}
bool tusb_init(){return true;} bool tud_mounted(){return fake::mounted;} bool tud_suspended(){return false;}
bool tud_hid_ready(){return fake::mounted && !fake::busy;}
void tud_disconnect(){fake::disconnected=true;fake::mounted=false;}
bool tud_hid_report(int,const void* report,unsigned size){fake::check(fake::expensive_boot_finished,"HID submitted before slow startup completed");if(fake::busy)return false;fake::busy=true;std::memcpy(fake::hid.data(),report,size);return true;}
#if SC_USB_XINPUT
extern "C" void sc_xinput_report_complete_cb(const std::uint8_t*,std::uint16_t);
bool sc_xinput_ready(){return tud_hid_ready();}
bool sc_xinput_send(const std::uint8_t* data,std::size_t size){return tud_hid_report(0,data,size);}
#else
extern "C" void tud_hid_report_complete_cb(std::uint8_t,const std::uint8_t*,std::uint16_t);
#endif
void tud_task_ext(std::uint32_t,bool){if(fake::queued){fake::queued=false;fake::busy=false;
#if SC_USB_XINPUT
sc_xinput_report_complete_cb(fake::hid.data(),fake::hid.size());
#else
tud_hid_report_complete_cb(0,fake::hid.data(),fake::hid.size());
#endif
}}
void ip_addr_copy(ip_addr_t& dst,const ip_addr_t& src){dst=src;}
bool ip_addr_cmp(const ip_addr_t* a,const ip_addr_t* b){return a->address==b->address;}
#if SC_CHAIR_RELAY
bool ipaddr_aton(const char*,ip_addr_t* address){address->address=20;return true;}
#endif
pbuf* pbuf_alloc(int,u16_t size,int){auto* p=new pbuf;p->tot_len=size;return p;}
int pbuf_take(pbuf* p,const void* b,u16_t n){std::memcpy(p->bytes.data(),b,n);return ERR_OK;}
void pbuf_free(pbuf* p){
#if SC_CHAIR_RELAY
if(fake::poison_address_on_free){p->alias_address.address=0xbad;fake::poisoned_pbufs.push_back(p);return;}
#endif
delete p;}
void pbuf_copy_partial(pbuf* p,void* b,u16_t n,u16_t){std::memcpy(b,p->bytes.data(),n);}
int udp_sendto(udp_pcb*,pbuf* p,const ip_addr_t* address,u16_t port){
#if SC_CHAIR_RELAY
if(port==42737){fake::chair_packets.push_back(scx2::decode(p->bytes.data(),p->tot_len).value());return ERR_OK;}
fake::last_app_reply_address=address->address;
#else
(void)port;(void)address;
#endif
fake::response=scx2::decode(p->bytes.data(),p->tot_len);return ERR_OK;}
udp_pcb* udp_new_ip_type(int){
static udp_pcb pcbs[MEMP_NUM_UDP_PCB];
for(auto& pcb:pcbs)if(!pcb.allocated){pcb.allocated=true;return &pcb;}
return nullptr;
} int udp_bind(udp_pcb*,const ip_addr_t*,u16_t){return ERR_OK;}
void udp_recv(udp_pcb*,void(*)(void*,udp_pcb*,pbuf*,const ip_addr_t*,u16_t),void*){} void udp_remove(udp_pcb* pcb){if(pcb)pcb->allocated=false;}
int cyw43_arch_init_with_country(int){fake::advance(70'000);return 0;}
void cyw43_arch_enable_sta_mode(){
#if SC_CHAIR_RELAY
fake::station_enabled=true;
#endif
fake::advance(320'000);} void cyw43_arch_deinit(){}
#if SC_CHAIR_RELAY
int cyw43_wifi_pm(int*,unsigned mode){fake::check(fake::station_enabled,"power management set before station mode");fake::check(mode==CYW43_NONE_PM,"relay did not request no power save");++fake::power_calls;return fake::power_result;}
#endif
int cyw43_tcpip_link_status(int*,int){return 0;}
int cyw43_arch_wifi_connect_async(const char*,const char*,int); void cyw43_arch_poll(); void tight_loop_contents();
#define main firmware_main
#include "../firmware/main.cpp"
#undef main
void inject(const scx2::Packet& packet){auto bytes=scx2::encode(packet).value();auto* p=pbuf_alloc(0,bytes.size,0);pbuf_take(p,bytes.bytes.data(),bytes.size);ip_addr_t source;fake::response.reset();receive_udp(nullptr,listener,p,&source,1234);}
scx2::Packet hello(){scx2::Packet p;p.kind=scx2::Kind::hello;p.session[0]=1;
#if SC_USB_XINPUT
 p.format=scx2::Format::usb_pressure;p.capabilities=4;
#endif
return p;}
scx2::Packet sequence(std::uint64_t seq,unsigned buttons){auto p=*fake::offer;p.kind=scx2::Kind::state;p.capabilities=0;p.hid_ready=false;p.sequence=seq;p.state.buttons=buttons;return p;}
int cyw43_arch_wifi_connect_async(const char*,const char*,int){
    if(!fake::dhcp_socket)fake::dhcp_socket=udp_new_ip_type(IPADDR_TYPE_V4);
    fake::check(fake::dhcp_socket!=nullptr,"DHCP PCB allocation failed");
    ++fake::joins;fake::check(fake::watchdog_ms==2000,"pre-offer synchronous join lacks bounded startup watchdog");
    fake::advance(375'000); // async join itself still performs synchronous IOCTL work.
    if(listener){inject(hello());fake::check(!fake::response,"UDP admitted during prolonged radio call");}
    fake::expensive_boot_finished=true;return 0;
}
void cyw43_arch_poll(){
    // A USB IRQ can enqueue success during radio polling. Only the next TinyUSB
    // task dispatches it. The cumulative report age is 300 ms across three polls.
    if(fake::phase==4 && fake::loop==fake::phase_loop+2)fake::queued=true;
}
void tight_loop_contents(){
    using namespace fake;
    ++loop;
    if(reboot){check(disconnected && !receiver->armed(),"fault did not disconnect/retire");check(phase==6,"unexpected pre-arm or queued-completion reboot");throw Done{};}
    switch(phase){
    case 0:
        if(loop==5){inject(hello());check(response && !response->hid_ready && !receiver->active(),"pending mount neutral advertised READY");}
        if(joins>=2){check(busy && !receiver->ready() && !ever_offered_session,"unpolled startup changed readiness");queued=true;phase=1;}
        break;
    case 1:
        check(receiver->ready(),"real neutral completion did not establish readiness");inject(hello());check(response && response->hid_ready,"no offer after opening HID");offer=response;offer_phase=true;next_join=0;inject(sequence(1,0));phase=2;break;
    case 2:
        check(busy && !receiver->armed(),"session armed without neutral ACK");queued=true;phase=3;break;
    case 3:
        check(response && response->kind==scx2::Kind::ack && response->sequence==1 && receiver->armed(),"missing actual neutral ACK");inject(sequence(2,1));phase=4;phase_loop=loop+1;break;
    case 4:
        if(response && response->kind==scx2::Kind::ack && response->sequence==2){check(receiver->completed_output().buttons==1,"late live completion lost");check(joins==2,"rejoined during an active session");if(retired){auto p=*offer;p.kind=scx2::Kind::stop;p.capabilities=0;p.sequence=3;p.hid_ready=false;inject(p);}else inject(sequence(3,2));phase=5;}break;
    case 5:
        check(busy,"missing live or retirement transfer");phase=6;break;
    case 6: break;
    }
    if(loop>300)check(false,"test stalled");advance(100'000);
}
#if !SC_USB_XINPUT
void usb_trigger_callbacks(){
    using namespace fake;
    watchdog_ms=0;mounted=true;busy=false;queued=false;usb_fault=false;submission.reset();idle_units=0;expensive_boot_finished=true;
    receiver.emplace(random_token(nullptr),random_token);receiver->set_usb_available(true,now());
    auto first=receiver->take_report(now()).value();(void)receiver->complete_report(first.ticket,true,now());
    auto request=hello();request.format=scx2::Format::usb_buttons;request.capabilities=2;
    auto ready=receiver->receive(request,now()).value();auto neutral=ready;neutral.kind=scx2::Kind::state;neutral.capabilities=0;neutral.hid_ready=false;neutral.sequence=1;
    (void)receiver->receive(neutral,now());service_reports();queued=true;tud_task_ext(0,false);
    auto press=neutral;press.sequence=2;press.state.buttons=1;press.state.triggers={65535,65535};
    (void)receiver->receive(press,now());service_reports();check(hid[0]==0x13,"format4 live report lost LT/RT bits");queued=true;tud_task_ext(0,false);
    std::array<std::uint8_t,10> snapshot{};check(tud_hid_get_report_cb(0,0,HID_REPORT_TYPE_INPUT,snapshot.data(),10)==10&&snapshot[0]==0x13,"GET_REPORT lost format4 trigger bits");
    (void)tud_hid_set_idle_cb(0,1);fake::time+=5000;service_reports();check(submission&&!submission->ticket&&hid[0]==0x13,"HID idle repeat lost format4 trigger bits");queued=true;tud_task_ext(0,false);
    fake::time+=scx2::Receiver::lease_us;check(tud_hid_get_report_cb(0,0,HID_REPORT_TYPE_INPUT,snapshot.data(),10)==10&&snapshot[0]==0&&snapshot[1]==0x80,"GET_REPORT resurrected triggers after expiry");
    service_reports();check(hid[0]==0&&hid[1]==0x80,"expiry USB barrier retained triggers");
}
#else
void usb_trigger_callbacks(){
    using namespace fake;
    watchdog_ms=0;mounted=true;busy=false;queued=false;disconnected=false;reboot=false;usb_fault=false;submission.reset();idle_units=0;expensive_boot_finished=true;
    receiver.emplace(random_token(nullptr),random_token,nullptr,nullptr,scx2::UsbOutput::xinput);receiver->set_usb_available(true,now());
    auto first=receiver->take_report(now()).value();(void)receiver->complete_report(first.ticket,true,now());
    auto ready=receiver->receive(hello(),now()).value();auto neutral=ready;neutral.kind=scx2::Kind::state;neutral.capabilities=0;neutral.hid_ready=false;neutral.sequence=1;
    (void)receiver->receive(neutral,now());service_reports();queued=true;tud_task_ext(0,false);
    auto press=neutral;press.sequence=2;press.state.buttons=1;press.state.triggers={32768,45875};press.state.axes={1000,2000,3000,4000};
    (void)receiver->receive(press,now());service_reports();check(hid[3]==0x10&&hid[4]==128&&hid[5]==179,"format5 main adapter lost partial trigger/A fields");
    check(hid[6]==0xe8&&hid[7]==3&&hid[8]==0xd0&&hid[9]==7,"XInput left stick changed direction/scale");
    check(hid[10]==0&&hid[11]==0&&hid[12]==0&&hid[13]==0,"External chair ownership leaked phone right stick");
    queued=true;tud_task_ext(0,false);check(receiver->completed_output()==press.state,"Physical report completion lost original state");
    auto stop=neutral;stop.kind=scx2::Kind::stop;stop.sequence=3;response.reset();(void)receiver->receive(stop,now());service_reports();
    check(hid[4]==0&&hid[5]==0&&hid[3]==0&&!receiver->armed(),"STOP did not submit released XInput state");check(!response,"STOPPED issued before neutral callback");queued=true;tud_task_ext(0,false);check(response&&response->kind==scx2::Kind::stopped,"STOPPED missing after neutral callback");
    auto request=hello();request.session[0]=9;ready=receiver->receive(request,now()).value();neutral=ready;neutral.kind=scx2::Kind::state;neutral.capabilities=0;neutral.hid_ready=false;neutral.sequence=1;
    (void)receiver->receive(neutral,now());service_reports();queued=true;tud_task_ext(0,false);press=neutral;press.sequence=2;press.state.triggers={65535,65535};
    (void)receiver->receive(press,now());service_reports();check(submission&&submission->sent,"Missing pending IN transfer for suspend test");response.reset();
    tud_suspend_cb(false);check(disconnected&&reboot&&usb_fault&&!receiver->active()&&!submission,"Suspend did not invalidate queued press and disconnect");
    queued=true;tud_task_ext(0,false);check(!response&&!receiver->armed(),"Old completion after suspend revived session");
}
#endif
#ifndef SC_CHAIR_RELAY_STANDALONE
int main(int argc,char** argv){fake::retired=argc==2 && std::string(argv[1])=="retired";
#if SC_CHAIR_RELAY
fake::power_result=argc==2&&std::string(argv[1])=="pm-failed" ? -1 : 0;
#endif
try{firmware_main();}catch(const fake::Done&){}
#if SC_CHAIR_RELAY
fake::check(fake::power_calls==1,"real main did not configure PM exactly once");
fake::check(chair_radio_low_latency==(fake::power_result==0),"real main ignored PM setup result");
if(fake::power_result)fake::check(fake::chair_packets.empty()&&!chair_socket,"PM failure leaked relay traffic");
#endif
usb_trigger_callbacks();std::cout<<fake::checks<<" adapter assertions passed ("<<(fake::retired?"retirement":"live")<<" stall + USB triggers)\n";}
#endif

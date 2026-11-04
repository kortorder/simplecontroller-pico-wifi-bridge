// Runs the real firmware main loop + portable receiver with explicit fake SDK time,
// USB interrupts and Wi-Fi calls. No USB, radio or credentials used by this test.
#include "scx2.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>

using u16_t = std::uint16_t;
using hid_report_type_t = int;
constexpr int HID_REPORT_TYPE_INPUT=1, IPADDR_TYPE_V4=0, PBUF_TRANSPORT=0, PBUF_RAM=0, ERR_OK=0;
constexpr int SC_WIFI_COUNTRY=0, SC_UDP_PORT=42736, SC_XAC_RIGHT_PORT=0, CYW43_LINK_UP=3, CYW43_ITF_STA=0, CYW43_AUTH_WPA2_AES_PSK=0;
constexpr char SC_WIFI_SSID[]="synthetic-host-test", SC_WIFI_PASSWORD[]="synthetic-host-test";
struct rng_128_t {std::uint64_t r[2];};
struct ip_addr_t {unsigned address=1;};
const ip_addr_t* IP_ADDR_ANY=nullptr;
struct udp_pcb {}; struct pbuf {u16_t tot_len=0; std::array<std::uint8_t,80> bytes{};};
int cyw43_state=0;
namespace fake {
    std::uint64_t time=0, last_feed=0; unsigned watchdog_ms=0, joins=0, checks=0, loop=0;
    bool mounted=true, busy=false, queued=false, disconnected=false, reboot=false, retired=false;
    bool expensive_boot_finished=false, offer_phase=false;
    int phase=0; unsigned phase_loop=0; scx2::HidReport hid{}; std::optional<scx2::Packet> response,offer;
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
extern "C" void tud_hid_report_complete_cb(std::uint8_t,const std::uint8_t*,std::uint16_t);
void tud_task_ext(std::uint32_t,bool){if(fake::queued){fake::queued=false;fake::busy=false;tud_hid_report_complete_cb(0,fake::hid.data(),fake::hid.size());}}
void ip_addr_copy(ip_addr_t& dst,const ip_addr_t& src){dst=src;}
bool ip_addr_cmp(const ip_addr_t* a,const ip_addr_t* b){return a->address==b->address;}
pbuf* pbuf_alloc(int,u16_t size,int){auto* p=new pbuf;p->tot_len=size;return p;}
int pbuf_take(pbuf* p,const void* b,u16_t n){std::memcpy(p->bytes.data(),b,n);return ERR_OK;}
void pbuf_free(pbuf* p){delete p;}
void pbuf_copy_partial(pbuf* p,void* b,u16_t n,u16_t){std::memcpy(b,p->bytes.data(),n);}
int udp_sendto(udp_pcb*,pbuf* p,const ip_addr_t*,u16_t){fake::response=scx2::decode(p->bytes.data(),p->tot_len);return ERR_OK;}
udp_pcb* udp_new_ip_type(int){static udp_pcb pcb;return &pcb;} int udp_bind(udp_pcb*,const ip_addr_t*,u16_t){return ERR_OK;}
void udp_recv(udp_pcb*,void(*)(void*,udp_pcb*,pbuf*,const ip_addr_t*,u16_t),void*){} void udp_remove(udp_pcb*){}
int cyw43_arch_init_with_country(int){fake::advance(70'000);return 0;}
void cyw43_arch_enable_sta_mode(){fake::advance(320'000);} void cyw43_arch_deinit(){}
int cyw43_tcpip_link_status(int*,int){return 0;}
int cyw43_arch_wifi_connect_async(const char*,const char*,int); void cyw43_arch_poll(); void tight_loop_contents();
#define main firmware_main
#include "../firmware/main.cpp"
#undef main
void inject(const scx2::Packet& packet){auto bytes=scx2::encode(packet).value();auto* p=pbuf_alloc(0,bytes.size,0);pbuf_take(p,bytes.bytes.data(),bytes.size);ip_addr_t source;fake::response.reset();receive_udp(nullptr,listener,p,&source,1234);}
scx2::Packet hello(){scx2::Packet p;p.kind=scx2::Kind::hello;p.session[0]=1;return p;}
scx2::Packet sequence(std::uint64_t seq,unsigned buttons){auto p=*fake::offer;p.kind=scx2::Kind::state;p.hid_ready=false;p.sequence=seq;p.state.buttons=buttons;return p;}
int cyw43_arch_wifi_connect_async(const char*,const char*,int){
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
        if(response && response->kind==scx2::Kind::ack && response->sequence==2){check(receiver->completed_output().buttons==1,"late live completion lost");check(joins==2,"rejoined during an active session");if(retired){auto p=*offer;p.kind=scx2::Kind::stop;p.sequence=3;p.hid_ready=false;inject(p);}else inject(sequence(3,2));phase=5;}break;
    case 5:
        check(busy,"missing live or retirement transfer");phase=6;break;
    case 6: break;
    }
    if(loop>300)check(false,"test stalled");advance(100'000);
}
int main(int argc,char** argv){fake::retired=argc==2 && std::string(argv[1])=="retired";try{firmware_main();}catch(const fake::Done&){}std::cout<<fake::checks<<" adapter assertions passed ("<<(fake::retired?"retirement":"live")<<" stall)\n";}

// Personal prototype adapter. Physical XAC operation remains unverified.
#include "scx2.hpp"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "bridge_config.h"
#include "listener_retry.hpp"
#include <algorithm>
#include <cstring>
#include <optional>
#if SC_TRIGGER_MCP4251
#include "pico_trigger_output.hpp"
#endif

namespace {
std::optional<scx2::Receiver> receiver;
udp_pcb* listener=nullptr;
scx2::ListenerStartRetry listener_start;
ip_addr_t peer{};
std::uint16_t peer_port=0;
bool peer_known=false, radio_on=false, usb_fault=false;
// Boot lifetime, deliberately not cleared by suspend, unmount or session retirement.
bool ever_offered_session=false, radio_setup_in_progress=false;
std::uint64_t usb_generation=0, next_join=0, last_report_us=0;
std::uint8_t idle_units=0;
struct Submission {
    std::optional<std::uint64_t> ticket;
    scx2::HidReport bytes;
    std::uint64_t generation;
    bool neutral;
    bool sent=false;
    std::uint64_t sent_at=0;
};
std::optional<Submission> submission;
constexpr scx2::Port xac_port=SC_XAC_RIGHT_PORT ? scx2::Port::right : scx2::Port::left;
std::uint64_t now() { return time_us_64(); }
scx2::Token random_token(void*) {
    rng_128_t r{}; get_rand_128(&r); scx2::Token token{};
    for(unsigned i=0;i<16;++i) token[i]=std::uint8_t(r.r[i/8] >> (8*(i%8)));
    return token;
}
void send(const scx2::Packet& packet, const ip_addr_t* address, std::uint16_t port) {
    if(!listener || !address) return;
    auto bytes=scx2::encode(packet); if(!bytes) return;
    auto* p=pbuf_alloc(PBUF_TRANSPORT,std::uint16_t(bytes->size),PBUF_RAM); if(!p) return;
    if(pbuf_take(p,bytes->bytes.data(),std::uint16_t(bytes->size))==ERR_OK) (void)udp_sendto(listener,p,address,port);
    pbuf_free(p);
}
void reply_to_peer(const std::optional<scx2::Packet>& reply) { if(reply && peer_known) send(*reply,&peer,peer_port); }
void receive_udp(void*,udp_pcb*,pbuf* p,const ip_addr_t* address,u16_t port) {
    if(!p) return;
    // SDK radio setup/join can execute synchronous IOCTLs. Never create an
    // output session while the startup watchdog window is in effect.
    if(radio_setup_in_progress) {pbuf_free(p);return;}
    const auto time=now(); receiver->poll(time);
    std::array<std::uint8_t,80> bytes{};
    const auto size=p->tot_len;
    if(size!=64 && size!=76 && size!=80) {pbuf_free(p);return;}
    pbuf_copy_partial(p,bytes.data(),size,0); pbuf_free(p);
    auto packet=scx2::decode(bytes.data(),size); if(!packet) return;
    if(receiver->active() && peer_known && (!ip_addr_cmp(address,&peer) || port!=peer_port)) return;
    const bool was_active=receiver->active();
    auto reply=receiver->receive(*packet,time);
    if(reply && reply->kind==scx2::Kind::ready && reply->hid_ready && !was_active) {
        ever_offered_session=true;
        ip_addr_copy(peer,*address); peer_port=port; peer_known=true;
    }
    if(reply) send(*reply,address,port);
}
void change_usb_epoch() {
    ++usb_generation; submission.reset(); listener_start.reset();
    if(receiver) receiver->set_usb_available(false,now());
}
void fail_usb() {
    // Do not keep a failed endpoint mounted forever with its last non-neutral report.
    // XAC behavior on disconnect/reset remains a required physical fault test.
    usb_fault=true; submission.reset(); receiver->set_usb_available(false,now());
    tud_disconnect(); watchdog_reboot(0,0,10);
}
void service_reports() {
    if(!tud_mounted() || tud_suspended() || usb_fault) return;
    // A host may leave its interrupt endpoint unpolled until a HID client opens it.
    // Before the first offer this boot, only neutral can exist. Leave it pending:
    // no synthetic completion, no READY and no output. After any successful offer,
    // even a neutral or idle transfer keeps the normal fault/reset deadline.
    if(submission && submission->sent && (ever_offered_session || !submission->neutral) &&
       now()-submission->sent_at>=250'000) {
        // A live CPU cannot keep feeding the watchdog while USB completion stalls.
        if(submission->ticket) (void)receiver->complete_report(*submission->ticket,false,now());
        fail_usb(); return;
    }
    if(!submission && tud_hid_ready()) {
        auto work=receiver->take_report(now());
        if(work) submission=Submission{work->ticket,scx2::hid_report(work->state,xac_port).value(),usb_generation,work->state.neutral()};
        else if(idle_units && now()-last_report_us>=std::uint64_t(idle_units)*4000) {
            const auto state=receiver->armed() ? receiver->completed_output() : scx2::State{};
            submission=Submission{std::nullopt,scx2::hid_report(state,xac_port).value(),usb_generation,state.neutral()};
        }
    }
    if(submission && !submission->sent && tud_hid_ready()) {
        if(tud_hid_report(0,submission->bytes.data(),submission->bytes.size())) {
            submission->sent=true; submission->sent_at=now();
        }
        else {
            // No unsent report may wait across a lease expiry and later revive old input.
            if(submission->ticket) (void)receiver->complete_report(*submission->ticket,false,now());
            fail_usb();
        }
    }
}
bool can_start_or_join_radio() {
    if(receiver->active()) return false;
    // Once an offer has existed, finish any release barrier before blocking on
    // radio work. Never extend the loop watchdog after that first offer.
    return !ever_offered_session ||
        (!submission && receiver->ready() && receiver->completed_output().neutral());
}
template<typename Work> void run_radio_setup(Work work) {
    radio_setup_in_progress=true;
    if(!ever_offered_session) watchdog_enable(2000,false);
    work();
    if(!ever_offered_session) watchdog_enable(250,false);
    radio_setup_in_progress=false;
}
void request_join() {
    (void)cyw43_arch_wifi_connect_async(SC_WIFI_SSID,SC_WIFI_PASSWORD,CYW43_AUTH_WPA2_AES_PSK);
    next_join=now()+20'000'000;
}
void stop_radio() {
    if(listener) {udp_remove(listener);listener=nullptr;}
    if(radio_on) {cyw43_arch_deinit();radio_on=false;}
    peer_known=false;
}
}
extern "C" void tud_mount_cb() { change_usb_epoch(); }
extern "C" void tud_umount_cb() { change_usb_epoch(); }
extern "C" void tud_suspend_cb(bool) { change_usb_epoch(); }
extern "C" void tud_resume_cb() { change_usb_epoch(); }
extern "C" void tud_hid_report_complete_cb(std::uint8_t instance,const std::uint8_t* report,std::uint16_t length) {
    if(instance!=0 || !submission || !submission->sent || submission->generation!=usb_generation ||
        length!=submission->bytes.size() || std::memcmp(report,submission->bytes.data(),length)!=0) return;
    auto ticket=submission->ticket; submission.reset(); last_report_us=now();
    if(ticket) {
        auto reply = receiver->complete_report(*ticket,true,now());
        // Analog readback takes bounded time. Recheck the live lease before ACK,
        // even when this completion began just before its expiry boundary.
        receiver->poll(now());
        if(reply && reply->kind == scx2::Kind::ack && !receiver->armed()) return;
        reply_to_peer(reply);
    }
}
extern "C" void tud_hid_report_failed_cb(std::uint8_t instance,hid_report_type_t,const std::uint8_t*,std::uint16_t) {
    if(instance!=0) return;
    if(submission && submission->ticket) (void)receiver->complete_report(*submission->ticket,false,now());
    fail_usb();
}
extern "C" std::uint16_t tud_hid_get_report_cb(std::uint8_t instance,std::uint8_t id,hid_report_type_t type,std::uint8_t* buffer,std::uint16_t requested) {
    if(!receiver || instance!=0 || id!=0 || type!=HID_REPORT_TYPE_INPUT) return 0;
    receiver->poll(now());
    auto report=scx2::hid_report(receiver->armed() ? receiver->completed_output() : scx2::State{},xac_port).value();
    const auto n=std::min<std::uint16_t>(requested,report.size()); std::memcpy(buffer,report.data(),n); return n;
}
extern "C" void tud_hid_set_report_cb(std::uint8_t,std::uint8_t,hid_report_type_t,const std::uint8_t*,std::uint16_t) {}
extern "C" bool tud_hid_set_idle_cb(std::uint8_t instance,std::uint8_t rate) { if(instance!=0)return false;idle_units=rate;return true; }
int main() {
#if SC_TRIGGER_MCP4251
    PicoTriggerIO trigger_io;
    trigger_io.initialize();
    scx2::Mcp4251Triggers triggers(trigger_io);
    (void)triggers.initialize(); // Missing/failed circuit stays unavailable; never falls back to binary.
    receiver.emplace(random_token(nullptr),random_token,nullptr,&triggers);
#else
    receiver.emplace(random_token(nullptr),random_token);
#endif
    if(!tusb_init()) while(true) tight_loop_contents();
    watchdog_enable(250,false);
    while(true) {
        tud_task_ext(0,false);
        if(usb_fault) { tight_loop_contents(); continue; } // Let the armed reset run; never feed it.
        const bool usable=tud_mounted() && !tud_suspended() && !usb_fault;
        receiver->set_usb_available(usable,now()); receiver->poll(now());
        const bool want_radio=usable && SC_WIFI_SSID[0]!='\0';
        if(!want_radio) { if(radio_on) stop_radio(); listener_start.reset(); }
        if(want_radio && !radio_on && listener_start.due(now()) && can_start_or_join_radio()) {
            run_radio_setup([] {
                if(cyw43_arch_init_with_country(SC_WIFI_COUNTRY)==0) {
                    radio_on=true; cyw43_arch_enable_sta_mode(); request_join();
                } else { listener_start.failed(now()); }
            });
        }
        if(want_radio && radio_on && !listener && listener_start.due(now())) {
            listener=udp_new_ip_type(IPADDR_TYPE_V4);
            if(listener && udp_bind(listener,IP_ADDR_ANY,SC_UDP_PORT)==ERR_OK) {
                udp_recv(listener,receive_udp,nullptr); listener_start.succeeded();
            } else {
                if(listener) udp_remove(listener);
                listener=nullptr; listener_start.failed(now());
            }
        }
        // A failed allocation/bind cannot leave a permanently unreachable live radio.
        // Five failed starts stop it until USB is detached/resumed or the board resets.
        if(listener_start.faulted() && radio_on) stop_radio();
        if(radio_on) {
            cyw43_arch_poll();
            const auto time=now();
            if(listener && time>=next_join && can_start_or_join_radio() &&
               cyw43_tcpip_link_status(&cyw43_state,CYW43_ITF_STA)!=CYW43_LINK_UP) {
                run_radio_setup(request_join);
            }
        }
        // RP2040 USB IRQs queue completions; callbacks run only when TinyUSB is
        // pumped. Drain events produced during radio work before evaluating any
        // report deadline, and submit the boot neutral only after radio startup.
        tud_task_ext(0,false);
        if(usb_fault) continue; // A failed callback has already armed a reboot.
        receiver->set_usb_available(tud_mounted() && !tud_suspended(),now());
        receiver->poll(now()); service_reports();
        if(!usb_fault) watchdog_update();
        tight_loop_contents();
    }
}

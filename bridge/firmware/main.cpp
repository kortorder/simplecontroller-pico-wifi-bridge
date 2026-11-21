// SCX2 board adapter. XAC mode is retained; the optional Zen path needs its own hardware test.
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
#ifndef SC_USB_XINPUT
#define SC_USB_XINPUT 0
#endif
#ifndef SC_XINPUT_EXTERNAL_RS
#define SC_XINPUT_EXTERNAL_RS 0
#endif
#ifndef SC_CHAIR_RELAY
#define SC_CHAIR_RELAY 0
#endif
#if SC_CHAIR_RELAY
#include "chair_relay_client.hpp"
#endif
#if SC_USB_XINPUT
#include "xinput_report.hpp"
#include "xinput_device.hpp"
#endif
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
#if SC_CHAIR_RELAY
std::optional<scx2::ChairRelayClient> chair_relay;
udp_pcb* chair_socket=nullptr;
ip_addr_t chair_address{};
std::uint64_t upstream_generation=0;
std::uint64_t next_chair_socket_attempt=0;
bool chair_radio_low_latency=false;
constexpr std::uint16_t chair_port=42737;
#endif
std::uint8_t idle_units=0;
#if SC_USB_XINPUT
using UsbReport=scx2::XInputReport;
bool usb_report_ready() { return sc_xinput_ready(); }
bool submit_usb_report(const UsbReport& report) { return sc_xinput_send(report.data(),report.size()); }
UsbReport make_usb_report(const scx2::State& state,scx2::Format) { return scx2::xinput_report(state,SC_XINPUT_EXTERNAL_RS).value(); }
#else
using UsbReport=scx2::HidReport;
bool usb_report_ready() { return tud_hid_ready(); }
bool submit_usb_report(const UsbReport& report) { return tud_hid_report(0,report.data(),report.size()); }
constexpr scx2::Port xac_port=SC_XAC_RIGHT_PORT ? scx2::Port::right : scx2::Port::left;
UsbReport make_usb_report(const scx2::State& state,scx2::Format format) { return scx2::hid_report(state,xac_port,format).value(); }
#endif
struct Submission {
    std::optional<std::uint64_t> ticket;
    UsbReport bytes;
    std::uint64_t generation;
    bool neutral;
    bool sent=false;
    std::uint64_t sent_at=0;
};
std::optional<Submission> submission;
std::uint64_t now() { return time_us_64(); }
scx2::Token random_token(void*) {
    rng_128_t r{}; get_rand_128(&r); scx2::Token token{};
    for(unsigned i=0;i<16;++i) token[i]=std::uint8_t(r.r[i/8] >> (8*(i%8)));
    return token;
}
void send_on(udp_pcb* socket,const scx2::Packet& packet, const ip_addr_t* address, std::uint16_t port) {
    if(!socket || !address) return;
    auto bytes=scx2::encode(packet); if(!bytes) return;
    auto* p=pbuf_alloc(PBUF_TRANSPORT,std::uint16_t(bytes->size),PBUF_RAM); if(!p) return;
    if(pbuf_take(p,bytes->bytes.data(),std::uint16_t(bytes->size))==ERR_OK) (void)udp_sendto(socket,p,address,port);
    pbuf_free(p);
}
void send(const scx2::Packet& packet, const ip_addr_t* address, std::uint16_t port) { send_on(listener,packet,address,port); }
#if SC_CHAIR_RELAY
void sync_chair_source() {
    if(chair_relay && receiver) chair_relay->source(receiver->armed() ? receiver->completed_output() : scx2::State{},
        receiver->armed(),upstream_generation,now());
}
void flush_chair_packet() {
    if(chair_socket && chair_relay) {
        if(auto packet=chair_relay->take_packet(now())) send_on(chair_socket,*packet,&chair_address,chair_port);
    }
}
void invalidate_chair_source() {
    if(chair_relay) { chair_relay->invalidate(now()); flush_chair_packet(); }
}
void configure_chair_radio() {
    // Apply only after station mode is enabled. Power-save buffering can exceed
    // the relay's short safety lease; a failed setup must not enable this route.
    chair_radio_low_latency=cyw43_wifi_pm(&cyw43_state,CYW43_NONE_PM)==0;
    if(!chair_radio_low_latency) {
        invalidate_chair_source();
        if(chair_socket) { udp_remove(chair_socket); chair_socket=nullptr; }
    }
}
void receive_chair_udp(void*,udp_pcb*,pbuf* p,const ip_addr_t* address,u16_t port) {
    if(!p) return;
    // lwIP may point address into p's storage; never retain that pointer after free.
    const ip_addr_t source=*address;
    if(!chair_radio_low_latency || !chair_relay || p->tot_len!=64 || port!=chair_port || !ip_addr_cmp(&source,&chair_address)) { pbuf_free(p); return; }
    std::array<std::uint8_t,64> bytes{}; pbuf_copy_partial(p,bytes.data(),bytes.size(),0); pbuf_free(p);
    if(auto packet=scx2::decode(bytes.data(),bytes.size())) chair_relay->receive(*packet,now());
}
void service_chair_relay() {
    if(!radio_on || !chair_radio_low_latency) { invalidate_chair_source(); return; }
    sync_chair_source();
    if(!chair_socket && now()>=next_chair_socket_attempt && ipaddr_aton(SC_CHAIR_RELAY_ADDRESS,&chair_address)) {
        next_chair_socket_attempt=now()+250'000;
        chair_socket=udp_new_ip_type(IPADDR_TYPE_V4);
        if(chair_socket && udp_bind(chair_socket,IP_ADDR_ANY,0)==ERR_OK) udp_recv(chair_socket,receive_chair_udp,nullptr);
        else { if(chair_socket) udp_remove(chair_socket); chair_socket=nullptr; }
    }
    flush_chair_packet();
}
#endif
void reply_to_peer(const std::optional<scx2::Packet>& reply) { if(reply && peer_known) send(*reply,&peer,peer_port); }
void receive_udp(void*,udp_pcb*,pbuf* p,const ip_addr_t* address,u16_t port) {
    if(!p) return;
    // lwIP may point address into p's storage, including through pbuf_free below.
    const ip_addr_t source=*address;
    // SDK radio setup/join can execute synchronous IOCTLs. Never create an
    // output session while the startup watchdog window is in effect.
    if(radio_setup_in_progress) {pbuf_free(p);return;}
    const auto time=now(); receiver->poll(time);
    std::array<std::uint8_t,80> bytes{};
    const auto size=p->tot_len;
    if(size!=64 && size!=76 && size!=80) {pbuf_free(p);return;}
    pbuf_copy_partial(p,bytes.data(),size,0); pbuf_free(p);
    auto packet=scx2::decode(bytes.data(),size); if(!packet) return;
    if(receiver->active() && peer_known && (!ip_addr_cmp(&source,&peer) || port!=peer_port)) return;
    const bool was_active=receiver->active();
    auto reply=receiver->receive(*packet,time);
    if(reply && reply->kind==scx2::Kind::ready && reply->hid_ready && !was_active) {
        ever_offered_session=true;
        ip_addr_copy(peer,source); peer_port=port; peer_known=true;
#if SC_CHAIR_RELAY
        ++upstream_generation;
#endif
    }
#if SC_CHAIR_RELAY
    sync_chair_source();
#endif
    if(reply) send(*reply,&source,port);
}
void change_usb_epoch() {
    ++usb_generation; submission.reset(); listener_start.reset();
    if(receiver) receiver->set_usb_available(false,now());
#if SC_CHAIR_RELAY
    ++upstream_generation; invalidate_chair_source();
#endif
}
void fail_usb() {
    // Do not keep a failed endpoint mounted forever with its last non-neutral report.
    // XAC behavior on disconnect/reset remains a required physical fault test.
    usb_fault=true; submission.reset(); receiver->set_usb_available(false,now());
#if SC_CHAIR_RELAY
    ++upstream_generation; invalidate_chair_source();
#endif
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
    if(!submission && usb_report_ready()) {
        auto work=receiver->take_report(now());
        if(work) submission=Submission{work->ticket,make_usb_report(work->state,work->format),usb_generation,work->state.neutral()};
        else if(idle_units && now()-last_report_us>=std::uint64_t(idle_units)*4000) {
            const auto state=receiver->armed() ? receiver->completed_output() : scx2::State{};
            submission=Submission{std::nullopt,make_usb_report(state,receiver->completed_format()),usb_generation,state.neutral()};
        }
    }
    if(submission && !submission->sent && usb_report_ready()) {
        if(submit_usb_report(submission->bytes)) {
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
        (!submission && receiver->usb_ready() && receiver->completed_output().neutral());
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
#if SC_CHAIR_RELAY
    invalidate_chair_source();
    if(chair_socket) { udp_remove(chair_socket); chair_socket=nullptr; }
    next_chair_socket_attempt=0;
    chair_radio_low_latency=false;
#endif
    if(listener) {udp_remove(listener);listener=nullptr;}
    if(radio_on) {cyw43_arch_deinit();radio_on=false;}
    peer_known=false;
}
}
extern "C" void tud_mount_cb() { change_usb_epoch(); }
extern "C" void tud_umount_cb() { change_usb_epoch(); }
extern "C" void tud_suspend_cb(bool) {
    change_usb_epoch();
#if SC_USB_XINPUT
    // A host can suspend with a press queued at the IN endpoint. Disconnect and
    // restart instead of letting that old packet complete after a later resume,
    // or waiting forever for a completion that a sleeping host will never send.
    if(receiver) fail_usb();
#endif
}
extern "C" void tud_resume_cb() { change_usb_epoch(); }
void complete_usb_report(const std::uint8_t* report,std::uint16_t length) {
    if( !submission || !submission->sent || submission->generation!=usb_generation ||
        length!=submission->bytes.size() || std::memcmp(report,submission->bytes.data(),length)!=0) return;
    auto ticket=submission->ticket; submission.reset(); last_report_us=now();
    if(ticket) {
        auto reply = receiver->complete_report(*ticket,true,now());
        // Analog readback takes bounded time. Recheck the live lease before ACK,
        // even when this completion began just before its expiry boundary.
        receiver->poll(now());
#if SC_CHAIR_RELAY
        sync_chair_source();
#endif
        if(reply && reply->kind == scx2::Kind::ack && !receiver->armed()) return;
        reply_to_peer(reply);
    }
}
void failed_usb_report() {
    if(submission && submission->ticket) (void)receiver->complete_report(*submission->ticket,false,now());
    fail_usb();
}
#if SC_USB_XINPUT
extern "C" void sc_xinput_reset_cb() { change_usb_epoch(); }
extern "C" void sc_xinput_report_complete_cb(const std::uint8_t* report,std::uint16_t length) { complete_usb_report(report,length); }
extern "C" void sc_xinput_report_failed_cb() { failed_usb_report(); }
#else
extern "C" void tud_hid_report_complete_cb(std::uint8_t instance,const std::uint8_t* report,std::uint16_t length) {
    if(instance==0) complete_usb_report(report,length);
}
extern "C" void tud_hid_report_failed_cb(std::uint8_t instance,hid_report_type_t,const std::uint8_t*,std::uint16_t) {
    if(instance==0) failed_usb_report();
}
extern "C" std::uint16_t tud_hid_get_report_cb(std::uint8_t instance,std::uint8_t id,hid_report_type_t type,std::uint8_t* buffer,std::uint16_t requested) {
    if(!receiver || instance!=0 || id!=0 || type!=HID_REPORT_TYPE_INPUT) return 0;
    receiver->poll(now());
    auto report=scx2::hid_report(receiver->armed() ? receiver->completed_output() : scx2::State{},xac_port,receiver->completed_format()).value();
    const auto n=std::min<std::uint16_t>(requested,report.size()); std::memcpy(buffer,report.data(),n); return n;
}
extern "C" void tud_hid_set_report_cb(std::uint8_t,std::uint8_t,hid_report_type_t,const std::uint8_t*,std::uint16_t) {}
extern "C" bool tud_hid_set_idle_cb(std::uint8_t instance,std::uint8_t rate) { if(instance!=0)return false;idle_units=rate;return true; }
#endif
int main() {
#if SC_CHAIR_RELAY
    chair_relay.emplace(random_token);
#endif
#if SC_TRIGGER_MCP4251
    PicoTriggerIO trigger_io;
    trigger_io.initialize();
    scx2::Mcp4251Triggers triggers(trigger_io);
    (void)triggers.initialize(); // Missing/failed circuit stays unavailable; never falls back to binary.
    receiver.emplace(random_token(nullptr),random_token,nullptr,&triggers);
#else
    receiver.emplace(random_token(nullptr),random_token,nullptr,nullptr,SC_USB_XINPUT ? scx2::UsbOutput::xinput : scx2::UsbOutput::xac);
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
                    radio_on=true; cyw43_arch_enable_sta_mode();
#if SC_CHAIR_RELAY
                    configure_chair_radio();
#endif
                    request_join();
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
#if SC_CHAIR_RELAY
        service_chair_relay();
#endif
        if(!usb_fault) watchdog_update();
        tight_loop_contents();
    }
}

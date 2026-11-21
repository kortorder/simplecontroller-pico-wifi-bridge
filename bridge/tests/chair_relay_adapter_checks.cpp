#define SC_CHAIR_RELAY 1
#define SC_CHAIR_RELAY_STANDALONE 1
#include "radio_usb_startup_checks.cpp"

namespace {
void inject_chair(const scx2::Packet& packet,unsigned address=20,unsigned port=42737) {
    auto wire=scx2::encode(packet).value();auto* p=pbuf_alloc(0,wire.size,0);pbuf_take(p,wire.bytes.data(),wire.size);
    p->alias_address.address=address;fake::poison_address_on_free=true;
    receive_chair_udp(nullptr,chair_socket,p,&p->alias_address,port);fake::poison_address_on_free=false;
    for(auto* held:fake::poisoned_pbufs) delete held;fake::poisoned_pbufs.clear();
}
void inject_aliased_app(const scx2::Packet& packet,unsigned address=1) {
    auto wire=scx2::encode(packet).value();auto* p=pbuf_alloc(0,wire.size,0);pbuf_take(p,wire.bytes.data(),wire.size);
    p->alias_address.address=address;fake::response.reset();fake::poison_address_on_free=true;
    receive_udp(nullptr,listener,p,&p->alias_address,1234);fake::poison_address_on_free=false;
    for(auto* held:fake::poisoned_pbufs) delete held;fake::poisoned_pbufs.clear();
}
void complete_phone() { fake::queued=true;tud_task_ext(0,false); }
scx2::Packet last_chair_packet() {fake::check(!fake::chair_packets.empty(),"chair packet missing");return fake::chair_packets.back();}
}
int main(int argc,char** argv) {
    using namespace fake;
    watchdog_ms=0;mounted=true;busy=false;queued=false;usb_fault=false;expensive_boot_finished=true;radio_on=true;
    dhcp_socket=udp_new_ip_type(IPADDR_TYPE_V4);check(dhcp_socket!=nullptr,"DHCP socket missing");
    listener=udp_new_ip_type(IPADDR_TYPE_V4);check(listener!=nullptr&&listener!=dhcp_socket,"app listener did not get separate socket");chair_relay.emplace(random_token);
    const bool pm_failure=argc>1&&std::string(argv[1])=="pm-failed";
    power_result=pm_failure ? -1 : 0;cyw43_arch_enable_sta_mode();configure_chair_radio();
    check(power_calls==1&&chair_radio_low_latency==!pm_failure,"relay power-management result was ignored");
    receiver.emplace(random_token(nullptr),random_token);receiver->set_usb_available(true,now());
    service_reports();complete_phone();
    auto h=hello();h.format=scx2::Format::usb_buttons;h.capabilities=2;inject_aliased_app(h);
    check(response&&response->hid_ready&&upstream_generation==1,"new app offer missing generation");offer=response;
    check(peer.address==1&&last_app_reply_address==1,"app peer or READY reply used pbuf-owned address after free");
    auto neutral=sequence(1,0);inject_aliased_app(neutral);service_reports();
    check(submission&&submission->ticket,"valid aliased source address was rejected after free");
    service_chair_relay();check(chair_packets.empty(),"relay started before actual upstream neutral USB completion");
    complete_phone();service_chair_relay();
    if(pm_failure) {
        check(!chair_socket&&chair_packets.empty()&&!chair_relay->ready(),"power-management failure enabled relay");
        auto press=sequence(2,1);press.state.axes={1000,2000,14000,-25000};press.state.triggers={65535,65535};
        inject(press);service_reports();check(hid[0]==0x13,"power-management failure disabled ordinary USB inputs");
        complete_phone();service_chair_relay();
        check(receiver->armed()&&receiver->completed_output()==press.state&&response&&response->kind==scx2::Kind::ack,"power-management failure broke existing app/USB session");
        check(!chair_socket&&chair_packets.empty()&&!chair_relay->ready(),"ordinary phone input bypassed failed relay setup");
        stop_radio();check(!chair_radio_low_latency&&!radio_on&&!listener,"failed setup retained radio state");
        std::cout<<checks<<" real-main relay PM failure assertions passed\n";return 0;
    }
    check(chair_socket!=nullptr,"relay socket allocation failed with DHCP and app listener");
    check(chair_socket!=listener&&chair_socket!=dhcp_socket,"relay shares the app or DHCP UDP socket");
    auto relay_hello=last_chair_packet();check(relay_hello.kind==scx2::Kind::hello&&relay_hello.format==scx2::Format::legacy,"relay HELLO not isolated format2");
    scx2::Receiver remote(random_token(nullptr),random_token,nullptr,nullptr,scx2::UsbOutput::xac,250'000);
    remote.set_usb_available(true,now());auto barrier=remote.take_report(now()).value();remote.complete_report(barrier.ticket,true,now());
    auto ready=remote.receive(relay_hello,now()).value();
    auto count=chair_packets.size();inject_chair(ready,21);service_chair_relay();check(chair_packets.size()==count,"wrong IP response accepted");
    inject_chair(ready,20,42736);service_chair_relay();check(chair_packets.size()==count,"wrong port response accepted");
    inject_chair(ready);service_chair_relay();auto relay_arm=last_chair_packet();
    check(relay_arm.kind==scx2::Kind::state&&relay_arm.sequence==1&&relay_arm.state.neutral(),"relay arm not neutral");
    remote.receive(relay_arm,now());auto job=remote.take_report(now()).value();auto ack=remote.complete_report(job.ticket,true,now()).value();inject_chair(ack);
    check(chair_relay->ready(),"remote actual USB completion did not arm client");
    auto press=sequence(2,1);press.state.axes={1000,2000,14000,-25000};press.state.triggers={65535,65535};
    count=chair_packets.size();inject(press);service_reports();service_chair_relay();
    check(chair_packets.size()==count,"camera forwarded before actual phone USB completion");
    check(hid[0]==0x13,"optional relay changed USB-button LT/RT mapping");
    complete_phone();service_chair_relay();auto forwarded=last_chair_packet();
    check(forwarded.state.axes==std::array<std::int16_t,4>{0,0,14000,-25000}&&forwarded.state.buttons==0&&forwarded.state.triggers==scx2::TriggerPressure{},"adapter forwarded nonsanitized state");
    remote.receive(forwarded,now());job=remote.take_report(now()).value();ack=remote.complete_report(job.ticket,true,now()).value();inject_chair(ack);
    // Multiple actual USB completions in one main-loop drain must preserve a brief release.
    auto release=sequence(3,0);inject(release);service_reports();complete_phone();
    press.sequence=4;press.state.axes[2]=16000;inject(press);service_reports();complete_phone();service_chair_relay();
    auto released=last_chair_packet();check(released.state.neutral(),"actual main coalesced transient camera release");
    // A new upstream session cannot inherit a completed output, and suspend sends STOP.
    const auto epoch=upstream_generation;count=chair_packets.size();tud_suspend_cb(false);
    check(upstream_generation==epoch+1&&!receiver->armed(),"suspend did not retire upstream generation");
    check(chair_packets.size()==count+1&&last_chair_packet().kind==scx2::Kind::stop,"suspend did not send relay STOP");
    auto stopped=last_chair_packet();remote.receive(stopped,now());job=remote.take_report(now()).value();
    ack=remote.complete_report(job.ticket,true,now()).value();inject_chair(ack);
    service_chair_relay();check(!chair_relay->ready(),"unarmed upstream revived relay");
    // Resume/re-arm the app and prove its new session starts a fresh relay too.
    receiver->set_usb_available(true,now());service_reports();complete_phone();
    h.session[0]=9;inject(h);check(response&&response->hid_ready&&upstream_generation==epoch+2,"new app session did not advance generation");offer=response;
    neutral=sequence(1,0);inject(neutral);service_reports();complete_phone();service_chair_relay();
    auto next_hello=last_chair_packet();check(next_hello.kind==scx2::Kind::hello&&next_hello.session!=relay_hello.session,"new upstream session reused relay identity");
    ready=remote.receive(next_hello,now()).value();inject_chair(ready);service_chair_relay();
    relay_arm=last_chair_packet();check(relay_arm.sequence==1&&relay_arm.state.neutral(),"new session inherited previous phone camera");
    remote.receive(relay_arm,now());job=remote.take_report(now()).value();ack=remote.complete_report(job.ticket,true,now()).value();inject_chair(ack);
    count=chair_packets.size();fail_usb();
    check(reboot&&disconnected&&usb_fault&&!receiver->armed(),"USB failure did not retire source");
    check(chair_packets.size()==count+1&&last_chair_packet().kind==scx2::Kind::stop,"USB failure omitted relay STOP");
    stop_radio();check(!chair_socket&&!listener&&!radio_on&&!chair_radio_low_latency,"radio stop retained optional socket or PM readiness");
    auto* spare1=udp_new_ip_type(IPADDR_TYPE_V4);auto* spare2=udp_new_ip_type(IPADDR_TYPE_V4);
    check(spare1&&spare2&&spare1!=spare2&&!udp_new_ip_type(IPADDR_TYPE_V4),"socket cleanup did not return exactly two slots while DHCP remains");
    udp_remove(spare1);udp_remove(spare2);udp_remove(dhcp_socket);dhcp_socket=nullptr;
    std::cout<<checks<<" real-main chair relay assertions passed\n";
}

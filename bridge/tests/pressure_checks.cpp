// Portable proportional-trigger tests. The backend is explicitly fake; no pins/USB/network.
#include "scx2.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
using namespace scx2;
namespace {
int checks=0, failures=0;
void require(bool value,const char* name) { ++checks; if(!value) { ++failures;std::cerr<<"FAIL: "<<name<<'\n'; } }
Token token(unsigned value) { Token t{};t.fill(std::uint8_t(value));return t; }
Token entropy(void*) { return token(91); }
struct Backend final: TriggerOutputBackend {
    bool available=true, accept=true;
    TriggerPressure output{};
    std::vector<TriggerPressure> accepted;
    unsigned releases=0;
    bool ready() const noexcept override { return available; }
    bool apply(TriggerPressure p) noexcept override {
        if(!accept) return false;
        output=p;accepted.push_back(p);return true;
    }
    TriggerApplyResult apply_before(TriggerPressure p,std::uint64_t) noexcept override {
        // This fake has no advancing physical I/O clock; Receiver checks entry time.
        return apply(p) ? TriggerApplyResult::accepted : TriggerApplyResult::fault;
    }
    void release() noexcept override { output={};++releases; }
};
Packet hello(Format format=Format::pressure,unsigned session=2) {
    Packet p;p.format=format;p.session=token(session);p.capabilities=format==Format::pressure?proportional_triggers:0;return p;
}
Packet state(const Packet& offer,std::uint64_t sequence,TriggerPressure pressure={}) {
    Packet p=offer;p.kind=Kind::state;p.hid_ready=false;p.capabilities=0;p.sequence=sequence;p.state.triggers=pressure;return p;
}
Packet stop(const Packet& offer,std::uint64_t sequence) { auto p=state(offer,sequence);p.kind=Kind::stop;return p; }
void mount(Receiver& r,std::uint64_t now=0) {
    r.set_usb_available(true,now);auto work=r.take_report(now);if(!work)std::abort();(void)r.complete_report(work->ticket,true,now);
}
Packet arm(Receiver& r,std::uint64_t now=0) {
    auto offer=r.receive(hello(),now);if(!offer || !offer->hid_ready)std::abort();
    (void)r.receive(state(*offer,1),now);auto work=r.take_report(now);if(!work)std::abort();
    auto ack=r.complete_report(work->ticket,true,now);if(!ack || !r.armed())std::abort();return *offer;
}
void press(Receiver& r,const Packet& offer,TriggerPressure pressure={13107,32768},std::uint64_t at=1) {
    (void)r.receive(state(offer,2,pressure),at);auto work=r.take_report(at);if(!work)std::abort();
    if(!r.complete_report(work->ticket,true,at))std::abort();
}
std::vector<std::uint8_t> read(const std::string& path) {
    std::ifstream input(path);std::string raw;input>>raw;if(!input || raw.size()%2)std::abort();
    std::vector<std::uint8_t> bytes;
    for(std::size_t i=0;i<raw.size();i+=2)bytes.push_back(std::uint8_t(std::stoul(raw.substr(i,2),nullptr,16)));
    return bytes;
}
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const std::string path=argv[1];
    for(const auto* name: {"pressure-hello","pressure-unavailable","pressure-ready","pressure-neutral-first","pressure-tenth-half","pressure-mixed-full","pressure-ack","pressure-stop-max","pressure-stopped-max"}) {
        const auto bytes=read(path+"/"+name+".hex");auto p=decode(bytes.data(),bytes.size());auto wire=p?encode(*p):std::nullopt;
        require(wire && wire->size==bytes.size() && std::equal(bytes.begin(),bytes.end(),wire->bytes.begin()),name);
    }
    auto bytes=read(path+"/pressure-tenth-half.hex");auto sample=decode(bytes.data(),bytes.size()).value();
    require(sample.state.triggers==TriggerPressure{6554,32768} && sample.sequence==2,"Partial pressures survive network-byte-order decoding");
    auto mixed_bytes=read(path+"/pressure-mixed-full.hex");auto mixed=decode(mixed_bytes.data(),mixed_bytes.size()).value();
    require(mixed.sequence==9007199254740993ULL && mixed.state.triggers==TriggerPressure{65535,13107},"Full pressure and exact UInt64 preserve existing axes/buttons");
    bool lengths=true;
    for(std::size_t n=0;n<80;++n)lengths &= !decode(bytes.data(),n);
    auto extra=bytes;extra.push_back(0);lengths &= !decode(extra.data(),extra.size());
    require(lengths,"Pressure STATE must be exactly80 bytes, including when truncated to legacy76");
    auto bad=bytes;bad[4]=2;require(!decode(bad.data(),bad.size()),"Legacy version cannot accept extended STATE length");
    bool headers=true;
    for(const auto mutation: {std::pair<unsigned,unsigned>{4,4},{6,2},{7,1},{64,4},{67,1}}) {
        bad=bytes;bad[mutation.first]=std::uint8_t(mutation.second);headers &= !decode(bad.data(),bad.size());
    }
    require(headers,"Unknown format/flags/capabilities/digital-trigger bits/reserved state are rejected");
    auto request=hello();request.capabilities=0;require(!encode(request),"Format3 HELLO must explicitly request proportional capability");
    request=hello();request.capabilities=3;require(!encode(request),"Unknown requested capability bits are rejected");
    request=hello(Format::legacy);request.capabilities=1;require(!encode(request),"Legacy HELLO remains reserved-zero");
    auto old=sample;old.format=Format::legacy;require(!encode(old),"Legacy encoding cannot silently discard pressure");
    auto ready_packet=decode(read(path+"/pressure-ready.hex").data(),64).value();
    ready_packet.capabilities=0;require(!encode(ready_packet),"Output-capable format3 READY cannot omit pressure capability");
    auto base=mixed.state;base.triggers={};
    require(hid_report(base)==hid_report(mixed.state) && hid_report(base,Port::right)==hid_report(mixed.state,Port::right),"Pressure never becomes HID buttons or spare axes on either XAC port");

    Receiver absent(token(32),entropy);mount(absent);auto unavailable=absent.receive(hello(),1);
    require(unavailable && unavailable->format==Format::pressure && !unavailable->hid_ready && !unavailable->capabilities && unavailable->challenge==Token{} && !absent.active(),"Absent backend explicitly declines pressure without reserving or arming");
    auto legacy=absent.receive(hello(Format::legacy),2);
    require(legacy && legacy->hid_ready && legacy->format==Format::legacy && !legacy->capabilities,"USB-only receiver still offers unchanged legacy sessions");
    Backend backend;Receiver r(token(32),entropy,nullptr,&backend);mount(r);auto offer=r.receive(hello(),1).value();
    require(offer.hid_ready && offer.capabilities==1 && backend.output.neutral(),"Offer requires USB neutral and accepted backend zero");
    (void)r.receive(state(offer,1,{1,0}),2);
    require(!r.armed() && !r.queued() && backend.output.neutral(),"Even minimal nonzero first pressure cannot bypass neutral arming");
    (void)r.receive(state(offer,1),3);auto neutral=r.take_report(3).value();
    require(!r.armed(),"Receiving neutral STATE alone does not arm pressure output");
    auto ack=r.complete_report(neutral.ticket,true,4);
    require(ack && ack->format==Format::pressure && !ack->capabilities && r.armed() && backend.output.neutral(),"ACK1 follows both neutral USB completion and accepted backend zero");
    auto mismatched=state(offer,2);mismatched.format=Format::legacy;(void)r.receive(mismatched,5);
    require(r.queued()==0 && !r.receive(hello(Format::legacy),6),"A live session cannot change wire format with state or duplicate hello");
    (void)r.receive(state(offer,2,{13107,0}),7);(void)r.receive(state(offer,3,{52428,32768}),8);(void)r.receive(state(offer,4),9);
    auto low=r.take_report(10).value();require(backend.output.neutral(),"Queued positive pressure waits for current USB completion");
    auto low_ack=r.complete_report(low.ticket,true,11);
    require(low_ack && backend.output==TriggerPressure{13107,0},"Partial pressure is accepted before its ACK is returned");
    auto high=r.take_report(12).value();(void)r.complete_report(high.ticket,true,13);
    require(high.state.triggers==TriggerPressure{52428,32768} && backend.output==high.state.triggers,"Pressure-only0.2-to0.8 change is ordered and retained");
    auto up=r.take_report(14).value();(void)r.complete_report(up.ticket,true,15);
    require(up.state.neutral() && backend.output.neutral(),"Ordinary pressure release stays behind prior ordered snapshots");
    auto duplicate=r.receive(state(offer,4,{65535,65535}),16);
    require(duplicate && backend.output.neutral() && r.queued()==0,"Duplicate sequence cannot substitute a different pressure");

    { Backend b;Receiver x(token(32),entropy,nullptr,&b);mount(x);auto o=arm(x);press(x,o);
      (void)x.receive(state(o,3,{65535,65535}),2);auto pending=x.take_report(2).value();(void)x.receive(stop(o,4),3);
      require(!x.active() && b.output.neutral() && x.accepted_trigger_output().neutral(),"Stop releases analog immediately before pending USB neutral");
      require(!x.complete_report(pending.ticket,true,4) && b.output.neutral(),"Stale successful USB completion cannot reassert analog pressure");
      auto zero=x.take_report(5).value();auto stopped=x.complete_report(zero.ticket,true,6);
      require(stopped && stopped->format==Format::pressure && stopped->kind==Kind::stopped && b.output.neutral(),"Stopped preserves format3 and waits for USB neutral");
      auto newer=x.receive(hello(Format::legacy),7).value();auto old_stop=x.receive(stop(o,4),8);
      require(old_stop && x.active() && newer.format==Format::legacy && newer.challenge!=o.challenge && b.output.neutral(),"Old format3 STOP cannot cancel a new format2 offer using the same UUID"); }
    { Backend b;Receiver x(token(32),entropy,nullptr,&b);mount(x);auto o=arm(x);press(x,o);
      (void)x.receive(state(o,3,{65535,65535}),2);auto pending=x.take_report(2).value();
      require(!x.complete_report(pending.ticket,true,2+Receiver::lease_us) && b.output.neutral() && !x.active(),"Exact lease deadline beats positive pressure completion"); }
    { Backend b;Receiver x(token(32),entropy,nullptr,&b);mount(x);auto o=arm(x);press(x,o);
      auto invalid=state(o,0,{65535,65535});(void)x.receive(invalid,1+Receiver::lease_us);
      require(b.output.neutral() && !x.active(),"Malformed traffic cannot postpone analog release at expiry"); }
    { Backend b;Receiver x(token(32),entropy,nullptr,&b);mount(x);auto o=arm(x);press(x,o);
      (void)x.receive(state(o,3,{65535,65535}),2);auto pending=x.take_report(2).value();x.set_usb_available(false,3);
      require(b.output.neutral() && !x.active(),"USB loss immediately releases both analog channels");
      x.set_usb_available(true,4);require(!x.complete_report(pending.ticket,true,5) && b.output.neutral(),"Prior USB epoch cannot revive pressure after remount"); }
    { Backend b;Receiver x(token(32),entropy,nullptr,&b);mount(x);auto o=arm(x);press(x,o);
      (void)x.receive(state(o,3,{65535,65535}),2);auto pending=x.take_report(2).value();
      require(!x.complete_report(pending.ticket,false,3) && b.output.neutral() && !x.ready(),"USB transfer failure clears pressure without ACK"); }
    { Backend b;Receiver x(token(32),entropy,nullptr,&b);mount(x,10);auto o=arm(x,10);press(x,o,{65535,65535},11);
      (void)x.receive(state(o,3,{32768,32768}),12);auto pending=x.take_report(12).value();
      require(!x.complete_report(pending.ticket,true,9) && b.output.neutral(),"Backward clock consumes known ticket while releasing pressure");
      require(x.take_report(13).has_value(),"Clock recovery can schedule neutral instead of stranding consumed ticket"); }
    { Backend b;Receiver x(token(32),entropy,nullptr,&b);mount(x);auto o=arm(x);press(x,o);
      for(unsigned i=0;i<17;++i)(void)x.receive(state(o,3+i,{std::uint16_t(i+1),65535}),2+i);
      require(x.fault()==Fault::queue_overflow && b.output.neutral() && !x.active() && x.queued()==0,"Bounded queue overflow discards backlog and immediately zeros pressure"); }
    { Backend b;Receiver x(token(32),entropy,nullptr,&b);mount(x);auto o=arm(x);press(x,o);b.available=false;x.poll(2);
      require(x.fault()==Fault::trigger_output && b.output.neutral() && !x.active() && !x.ready(),"Backend readiness loss fails closed during a live session");
      b.available=true;auto reply=x.receive(hello(),3);
      require(!reply || !reply->hid_ready,"Backend fault cannot silently rearm after readiness returns"); }
    { Backend b;Receiver x(token(32),entropy,nullptr,&b);mount(x);auto o=arm(x);press(x,o);b.accept=false;
      (void)x.receive(state(o,3,{65535,65535}),2);auto pending=x.take_report(2).value();
      require(!x.complete_report(pending.ticket,true,3) && b.output.neutral() && x.fault()==Fault::trigger_output,"Backend rejects positive command: no ACK and immediate release"); }
    { Backend b;b.available=false;Receiver x(token(32),entropy,nullptr,&b);mount(x);auto denied=x.receive(hello(),1);
      require(denied && !denied->hid_ready && !denied->capabilities && !x.active(),"Uninitialized backend does not advertise proportional capability");
      b.available=true;b.accept=false;denied=x.receive(hello(),2);
      require(!x.ready() && !x.active() && b.output.neutral() && x.fault()==Fault::trigger_output,"Failed initial backend zero prevents pressure readiness"); }
    { Backend b;Receiver x(token(32),entropy,nullptr,&b);mount(x);
      b.available=false;x.poll(1);b.available=true;b.accept=false;
      auto denied=x.receive(hello(),2);
      require((!denied || !denied->hid_ready) && !x.active() && x.fault()==Fault::trigger_output && b.output.neutral(),
              "Idle backend reinitialization requires fresh accepted zero before any pressure offer"); }
    if(failures) { std::cerr<<failures<<" of "<<checks<<" pressure checks failed\n";return 1; }
    std::cout<<"PASS: "<<checks<<" format3 proportional codec/backend/ownership checks. Fake backend only; no physical pressure output.\n";
}

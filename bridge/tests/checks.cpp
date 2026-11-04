// Original host checks. No networking, device access or board SDK is used.
#include "scx2.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
using namespace scx2;
namespace {
int checks=0, failures=0;
void require(bool value, const char* name) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; } }
Token token(unsigned value) { Token t{}; for (unsigned i=0;i<16;++i) t[i]=std::uint8_t(value+i); return t; }
Token entropy(void*) { return token(80); } // Repeated entropy must still yield distinct offer counters.
Token zero_entropy(void*) { return {}; }
Packet hello(unsigned session=1) { Packet p; p.session=token(session); return p; }
Packet state_packet(const Packet& offer, std::uint64_t seq, State state={}) {
    Packet p=offer; p.kind=Kind::state; p.hid_ready=false; p.sequence=seq; p.state=state; return p;
}
Packet stop_packet(const Packet& offer, std::uint64_t seq) { auto p=state_packet(offer,seq); p.kind=Kind::stop; return p; }
void mount(Receiver& r, std::uint64_t now=0) {
    r.set_usb_available(true,now); auto work=r.take_report(now);
    if (!work || !work->neutral_barrier) std::abort();
    (void)r.complete_report(work->ticket,true,now);
}
Packet arm(Receiver& r, std::uint64_t now=0) {
    auto offer=r.receive(hello(),now); if (!offer || !offer->hid_ready) std::abort();
    (void)r.receive(state_packet(*offer,1),now); auto work=r.take_report(now); if (!work) std::abort();
    auto ack=r.complete_report(work->ticket,true,now); if (!ack || ack->sequence!=1 || !r.armed()) std::abort();
    return *offer;
}
std::vector<std::uint8_t> read_hex(const std::string& path) {
    std::ifstream in(path); std::string text; in>>text;
    if (!in || text.size()%2) { std::cerr<<"Fixture unreadable: "<<path<<'\n'; std::exit(2); }
    std::vector<std::uint8_t> bytes;
    for (std::size_t i=0;i<text.size();i+=2) bytes.push_back(std::uint8_t(std::stoul(text.substr(i,2),nullptr,16)));
    return bytes;
}
}
int main(int argc, char** argv) {
    if (argc!=2) { std::cerr<<"Usage: bridge-checks fixtures-directory\n"; return 2; }
    const std::string directory=argv[1];
    const char* names[] = {"hello","ready","unavailable","neutral-first","mixed-precise-sequence","ack","stop-max","stopped-max"};
    for (const auto* name:names) {
        auto bytes=read_hex(directory+"/"+name+".hex"); auto p=decode(bytes.data(),bytes.size());
        auto encoded=p ? encode(*p) : std::nullopt;
        require(encoded && encoded->size==bytes.size() && std::equal(bytes.begin(),bytes.end(),encoded->bytes.begin()),name);
    }
    auto mixed_bytes=read_hex(directory+"/mixed-precise-sequence.hex");
    auto mixed=decode(mixed_bytes.data(),mixed_bytes.size()).value();
    require(mixed.sequence==9007199254740993ULL && mixed.state.buttons==0x255 && mixed.state.hat==3 &&
            mixed.state.axes==std::array<std::int16_t,4>{32767,-32767,1234,-4321},"Golden wire preserves exact UInt64 and signed network-order axes");
    bool sizes_rejected=true;
    for (std::size_t n=0;n<76;++n) sizes_rejected &= !decode(mixed_bytes.data(),n);
    mixed_bytes.push_back(0); sizes_rejected &= !decode(mixed_bytes.data(),mixed_bytes.size()); mixed_bytes.pop_back();
    require(sizes_rejected,"Every truncated state and extra trailing byte is rejected");
    for (const auto mutation: std::array<std::pair<unsigned,unsigned>,9>{{{0,0},{4,1},{5,9},{6,2},{7,1},{64,0x80},{66,9},{67,1},{68,0x80}}}) {
        auto bad=mixed_bytes; bad[mutation.first]=std::uint8_t(mutation.second);
        if (mutation.first==68) bad[69]=0;
        require(!decode(bad.data(),bad.size()),"Invalid magic/version/kind/flags/reserved/state data is rejected");
    }
    auto bad=hello(); bad.session={}; require(!encode(bad),"Zero session token is rejected");
    bad=mixed; bad.sequence=std::numeric_limits<std::uint64_t>::max(); require(!encode(bad),"Maximum sequence is reserved for stop, never state/wrap");
    bad=mixed; bad.sequence=0; require(!encode(bad),"Zero state sequence cannot restart ordering");

    const auto neutral=hid_report({}).value();
    require(neutral==HidReport{0,0x80,0,0,0,0,0,0,0,0},"HID neutral uses a null hat, centered axes and no buttons");
    constexpr unsigned left_slots[]={5,6,11,12,4,10,7,8,3,9};
    constexpr unsigned right_slots[]={11,12,5,6,10,4,1,2,9,3};
    bool all_buttons=true;
    for (unsigned i=0;i<10;++i) {
        State s; s.buttons=std::uint16_t(1u<<i);
        auto l=hid_report(s,Port::left).value(), r=hid_report(s,Port::right).value();
        all_buttons &= ((unsigned(l[0]) | unsigned(l[1])<<8)&0xfff)==(1u<<(left_slots[i]-1));
        all_buttons &= ((unsigned(r[0]) | unsigned(r[1])<<8)&0xfff)==(1u<<(right_slots[i]-1));
    }
    require(all_buttons,"All ten controller actions map to the specified left/right XAC button slots");
    State extremes; extremes.axes={32767,-32767,-32767,32767};
    require(hid_report(extremes)==HidReport{0,0x80,0xff,0x7f,0xff,0x7f,1,0x80,1,0x80},"HID packing is little-endian and reverses positive-up Y");
    require(hid_report(extremes,Port::right)==HidReport{0,0x80,1,0x80,1,0x80,0xff,0x7f,0xff,0x7f},"Right-port report swaps the primary and secondary sticks");
    bad.state.axes[0]=-32768; require(!hid_report(bad.state),"HID packer rejects invalid axes instead of overflowing sign reversal");
    struct Globals { unsigned size=0,count=0; } current;
    std::vector<Globals> stack; unsigned bits=0; bool hat_null=false; bool balanced=true;
    for (std::size_t i=0;i<hid_descriptor_size;) {
        auto prefix=hid_descriptor[i++]; unsigned n=prefix&3; if(n==3)n=4;
        if(i+n>hid_descriptor_size) { balanced=false; break; }
        unsigned value=0; for(unsigned j=0;j<n;++j)value|=unsigned(hid_descriptor[i+j])<<(8*j); i+=n;
        switch(prefix&0xfc) {
        case 0x74: current.size=value; break;
        case 0x94: current.count=value; break;
        case 0xa4: stack.push_back(current); break;
        case 0xb4: if(stack.empty())balanced=false; else {current=stack.back();stack.pop_back();} break;
        case 0x80: bits+=current.size*current.count; if(current.size==4)hat_null=(value&0x40)!=0; break;
        default: break;
        }
    }
    require(balanced && stack.empty() && bits==80 && hat_null,"Independent descriptor item walk yields exactly ten input bytes and a null-capable hat");

    Receiver unavailable(token(32),entropy);
    auto unready=unavailable.receive(hello(),0);
    require(unready && !unready->hid_ready && unready->challenge==Token{} && !unavailable.active(),"Unavailable USB cannot offer an output-capable session");
    unavailable.set_usb_available(true,1); auto barrier=unavailable.take_report(1).value();
    require(!unavailable.ready() && barrier.neutral_barrier && barrier.state.neutral(),"USB mount stays unready until its neutral report completes");
    (void)unavailable.complete_report(barrier.ticket,true,2);
    require(unavailable.ready(),"Completed neutral report makes the mounted interface ready");
    auto offer=unavailable.receive(hello(),3).value(); State pressed; pressed.buttons=1;
    (void)unavailable.receive(state_packet(offer,1,pressed),4);
    (void)unavailable.receive(state_packet(offer,2),5);
    require(!unavailable.armed() && unavailable.queued()==0,"First output cannot be nonneutral or skip sequence one");
    (void)unavailable.receive(state_packet(offer,1),6); auto first=unavailable.take_report(6).value();
    (void)unavailable.receive(state_packet(offer,2,pressed),7);
    require(!unavailable.armed() && unavailable.queued()==0,"New input cannot race ahead of first neutral USB completion");
    auto first_ack=unavailable.complete_report(first.ticket,true,8);
    require(first_ack && first_ack->sequence==1 && unavailable.armed(),"ACK1 is sent only after the neutral USB transfer arms the session");
    (void)unavailable.receive(state_packet(offer,2,pressed),9);
    (void)unavailable.receive(state_packet(offer,3),10);
    auto down=unavailable.take_report(10).value(); auto ack2=unavailable.complete_report(down.ticket,true,11);
    auto up=unavailable.take_report(11).value(); auto ack3=unavailable.complete_report(up.ticket,true,12);
    require(down.state==pressed && up.state.neutral() && ack2->sequence==2 && ack3->sequence==3 && unavailable.completed_output().neutral(),
            "Quick press/release remains ordered in USB reports and completion acknowledgments");
    auto duplicate=unavailable.receive(state_packet(offer,3,pressed),100);
    require(duplicate && duplicate->sequence==3 && unavailable.queued()==0 && unavailable.completed_output().neutral(),"Duplicate sequence cannot change its original completed snapshot");
    auto same_offer=unavailable.receive(hello(),200);
    require(same_offer && same_offer->challenge==offer.challenge,"Repeated hello returns the existing challenge");
    unavailable.poll(1'000'010);
    require(!unavailable.active() && !unavailable.ready(),"Duplicate state and hello do not extend the last fresh-state lease");

    Receiver invalid_at_expiry(token(32),entropy); mount(invalid_at_expiry); auto expired_offer=arm(invalid_at_expiry);
    auto invalid=state_packet(expired_offer,0,pressed); (void)invalid_at_expiry.receive(invalid,Receiver::lease_us);
    require(!invalid_at_expiry.active(),"Invalid packets still expire the receiver before rejection");
    Receiver busy(token(32),entropy); mount(busy); auto busy_offer=arm(busy);
    (void)busy.receive(state_packet(busy_offer,2,pressed),1);
    auto inflight=busy.take_report(1).value(); (void)busy.receive(stop_packet(busy_offer,3),2);
    require(!busy.active() && !busy.ready() && !busy.receive(hello(99),3)->hid_ready,"Stop fences the active token while an old USB report is still in flight");
    auto late_ack=busy.complete_report(inflight.ticket,true,4);
    require(!late_ack && busy.completed_output()==pressed,"An invalidated physical transfer may finish but gets no stale ACK");
    auto stop_neutral=busy.take_report(4).value(); auto stopped=busy.complete_report(stop_neutral.ticket,true,5);
    require(stop_neutral.neutral_barrier && stopped && stopped->kind==Kind::stopped && busy.completed_output().neutral(),"Stopped is delayed until the prioritized neutral report completes");
    auto new_offer=busy.receive(hello(),6).value();
    require(new_offer.challenge!=busy_offer.challenge,"Reusing a client UUID gets a new challenge even with repeated entropy");
    require(!busy.receive(state_packet(busy_offer,4,pressed),7) && busy.queued()==0,"Old challenge state cannot enter the newly offered session");
    auto old_stop=busy.receive(stop_packet(busy_offer,3),8);
    require(old_stop && old_stop->kind==Kind::stopped && busy.active(),"An idempotent old stop acknowledgment never retires a newer session");
    Receiver rebooted(token(64),entropy); mount(rebooted); auto reboot_offer=rebooted.receive(hello(),0).value();
    (void)rebooted.receive(state_packet(busy_offer,1),1);
    require(reboot_offer.boot!=busy_offer.boot && rebooted.queued()==0 && !rebooted.armed(),"A different boot epoch rejects old neutral/state replay");

    Receiver overflow(token(32),entropy); mount(overflow); auto overflow_offer=arm(overflow);
    for(unsigned i=0;i<Receiver::queue_capacity;++i) (void)overflow.receive(state_packet(overflow_offer,2+i,pressed),i+1);
    require(overflow.queued()==16,"Report queue is bounded and holds sixteen ordered pending snapshots");
    (void)overflow.receive(state_packet(overflow_offer,18,pressed),17);
    require(!overflow.active() && overflow.queued()==0 && overflow.fault()==Fault::queue_overflow && overflow.take_report(18)->neutral_barrier,
            "Overflow faults neutral instead of dropping a release or replaying backlog");
    Receiver loss(token(32),entropy); mount(loss); auto loss_offer=arm(loss);
    (void)loss.receive(state_packet(loss_offer,2,pressed),1); auto lost_work=loss.take_report(1).value();
    loss.set_usb_available(false,2); loss.set_usb_available(true,3); auto remount=loss.take_report(3).value();
    require(!loss.complete_report(lost_work.ticket,true,4) && !loss.ready(),"Old completion ticket cannot make a remounted device ready");
    (void)loss.complete_report(remount.ticket,true,5);
    require(loss.ready() && !loss.active() && loss.completed_output().neutral(),"USB resume requires completed neutral and leaves old session retired");
    Receiver failed(token(32),entropy); mount(failed); auto failed_offer=arm(failed);
    (void)failed.receive(state_packet(failed_offer,2,pressed),1); auto failed_work=failed.take_report(1).value();
    require(!failed.complete_report(failed_work.ticket,false,2) && !failed.ready() && !failed.active() && failed.fault()==Fault::usb_transfer,
            "Failed USB transfer returns no ACK and invalidates readiness/session");
    Receiver boundary(token(32),entropy); mount(boundary); auto boundary_offer=arm(boundary);
    (void)boundary.receive(state_packet(boundary_offer,2,pressed),1); auto boundary_work=boundary.take_report(1).value();
    require(!boundary.complete_report(boundary_work.ticket,true,1+Receiver::lease_us) && !boundary.active() && boundary.take_report(2+Receiver::lease_us)->neutral_barrier,
            "Lease expiry wins over a USB completion at the exact deadline");
    Receiver clock_fault(token(32),entropy); mount(clock_fault,100); auto clock_offer=arm(clock_fault,100);
    (void)clock_fault.receive(state_packet(clock_offer,2,pressed),101); auto clock_work=clock_fault.take_report(102).value();
    require(!clock_fault.complete_report(clock_work.ticket,true,99) && clock_fault.fault()==Fault::clock && !clock_fault.active(),"Backward-time completion faults neutral without ACK");
    auto recovery=clock_fault.take_report(103);
    require(recovery && recovery->neutral_barrier,"A known completed ticket cannot strand neutral after a clock fault");
    clock_fault.poll(std::numeric_limits<std::uint64_t>::max());
    require(clock_fault.fault()==Fault::clock && !clock_fault.ready(),"Time overflow cannot wrap a lease into the past");
    Receiver sequence(token(32),entropy); mount(sequence); auto sequence_offer=arm(sequence);
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    (void)sequence.receive(state_packet(sequence_offer,maximum-1,pressed),1); auto last_state=sequence.take_report(1).value();
    require(sequence.complete_report(last_state.ticket,true,2)->sequence==maximum-1,"Full unsigned state sequence precision reaches the last permitted value");
    (void)sequence.receive(stop_packet(sequence_offer,maximum),3); auto last_neutral=sequence.take_report(3).value();
    require(sequence.complete_report(last_neutral.ticket,true,4)->sequence==maximum && !sequence.active(),"Reserved maximum sequence closes the session without wrapping");
    Receiver no_entropy(token(32),zero_entropy); mount(no_entropy);
    require(!no_entropy.receive(hello(),0) && no_entropy.fault()==Fault::entropy && !no_entropy.ready(),"Missing entropy fails closed");
    Receiver bad_boot({},entropy);
    require(!bad_boot.ready() && !bad_boot.receive(hello(),0),"A zero boot token cannot advertise a usable session");
    if(failures) { std::cerr<<failures<<" of "<<checks<<" bridge host checks failed.\n"; return 1; }
    std::cout<<"PASS: "<<checks<<" SCX2 codec, receiver, lifecycle and HID host checks.\n";
    std::cout<<"No USB controller, Wi-Fi, firmware image, power draw or Xbox behavior was tested.\n";
}

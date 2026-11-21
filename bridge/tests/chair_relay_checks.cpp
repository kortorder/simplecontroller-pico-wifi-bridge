#include "chair_relay_client.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace scx2;
namespace {
unsigned checks = 0;
void check(bool ok, const char* why) { ++checks; if (!ok) { std::cerr << "check " << checks << ": " << why << '\n'; std::exit(1); } }
Token token(unsigned n) { Token t{}; t[0] = n; return t; }
Token random(void* p) { auto& n = *static_cast<unsigned*>(p); return token(++n); }
struct Fixture {
    unsigned seed = 10;
    std::uint64_t time = 0;
    ChairRelayClient client{random, &seed};
    Receiver chair{token(2), random, &seed, nullptr, UsbOutput::xac, 250'000};
    State source{};
    Fixture() {
        chair.set_usb_available(true, time);
        complete(); client.source(source, true, 1, time);
    }
    std::optional<Packet> complete() {
        auto job = chair.take_report(time); check(bool(job), "expected actual USB job");
        return chair.complete_report(job->ticket, true, time);
    }
    Packet outgoing() {
        auto p = client.take_packet(time); check(bool(p), "expected outgoing relay packet");
        auto wire = encode(*p); check(bool(wire), "client emitted invalid packet");
        return decode(wire->bytes.data(), wire->size).value();
    }
    void receive_at_chair(const Packet& p) {
        auto reply = chair.receive(p, time); if (reply) client.receive(*reply, time);
    }
    void complete_at_chair() { if (auto reply = complete()) client.receive(*reply, time); }
    void arm() {
        auto hello = outgoing(); check(hello.kind == Kind::hello, "fresh connection must HELLO");
        receive_at_chair(hello);
        auto arm = outgoing(); check(arm.sequence == 1 && arm.state.neutral(), "first state must be neutral");
        receive_at_chair(arm); check(!client.ready(), "client ready before actual USB completion");
        complete_at_chair(); check(client.ready(), "client did not recognize neutral completion");
    }
    void update(std::int16_t x, std::int16_t y) {
        source.axes[2] = x; source.axes[3] = y; client.source(source, true, 1, time);
    }
};
}
int main() {
    {
        Fixture f; f.arm(); f.source.buttons = 0xffff; f.source.hat = 0;
        f.source.axes = {1200,-3400,16000,-22000}; f.source.triggers = {65535,32000};
        f.client.source(f.source, true, 1, f.time); auto p = f.outgoing();
        check(p.format == Format::legacy && p.state.buttons == 0 && p.state.hat == 8 && p.state.triggers == TriggerPressure{}, "noncamera controls leaked to chair");
        check(p.state.axes == std::array<std::int16_t,4>{0,0,16000,-22000}, "right-stick mapping changed");
        f.receive_at_chair(p); check(f.chair.completed_output().neutral(), "state applied without USB completion");
        f.complete_at_chair(); check(f.chair.completed_output().axes[2] == 16000, "right stick missing after completion");
        f.time += 49'999; check(!f.client.take_packet(f.time), "heartbeat too early");
        ++f.time; auto heartbeat = f.outgoing(); check(heartbeat.sequence == p.sequence+1 && heartbeat.state == p.state, "heartbeat must renew live source with new sequence");
        f.receive_at_chair(heartbeat); f.complete_at_chair();
        f.time += 1; f.update(0,0); auto release = f.outgoing(); check(release.state.neutral(), "neutral did not release immediately");
        f.receive_at_chair(release); f.complete_at_chair(); check(f.chair.completed_output().neutral(), "release did not reach chair USB");
    }
    {
        Fixture f; f.arm(); f.update(1000,2000); auto press = f.outgoing();
        // Release and another press both arrive before any outgoing network poll.
        f.update(0,0); f.update(3000,4000); auto release = f.outgoing();
        check(release.sequence > press.sequence && release.state.neutral(), "transient release coalesced away");
        f.receive_at_chair(press); f.receive_at_chair(release);
        auto old_ack = f.complete(); check(bool(old_ack), "press USB ACK missing"); f.client.receive(*old_ack,f.time);
        check(!f.client.take_packet(f.time), "old ACK cleared pending release");
        f.complete_at_chair(); auto fresh = f.outgoing(); check(fresh.state.axes[2] == 3000, "new intent lost after release ACK");
    }
    {
        Fixture f; f.arm(); f.update(1000,2000); auto press = f.outgoing(); f.receive_at_chair(press); f.complete_at_chair();
        f.time += 50'000; auto lost = f.outgoing(); (void)lost;
        f.time += 250'000; auto stop = f.outgoing(); check(stop.kind == Kind::stop && f.client.awaiting_neutral(), "ACK timeout did not stop and require neutral");
        f.receive_at_chair(stop); f.complete_at_chair();
        // Old held state is sampled continuously by the main loop; it must not resume.
        f.time += 250'000; f.update(1000,2000); f.arm(); auto no_replay = f.client.take_packet(f.time);
        check(!no_replay && f.client.awaiting_neutral(), "old press replayed after reconnect");
        f.update(0,0); f.update(5000,6000); auto fresh = f.outgoing(); check(fresh.state.axes[2] == 5000, "fresh neutral and touch did not restore camera");
    }
    {
        Fixture f; f.arm(); f.update(5000,0); auto press = f.outgoing(); f.receive_at_chair(press); f.complete_at_chair();
        f.client.source(f.source, false, 1, f.time); auto stop = f.outgoing(); check(stop.kind == Kind::stop, "upstream retirement did not STOP");
        f.receive_at_chair(stop); f.complete_at_chair();
        f.client.source(f.source, true, 2, f.time); f.arm(); check(f.client.awaiting_neutral(), "new upstream generation reused old state");
        f.client.invalidate(f.time); check(f.outgoing().kind == Kind::stop, "USB/radio invalidation did not STOP");
    }
    {
        Fixture f; auto hello = f.outgoing(); auto offer = f.chair.receive(hello,f.time).value();
        auto wrong = offer; wrong.session[0] ^= 1; f.client.receive(wrong,f.time); check(!f.client.take_packet(f.time), "wrong session READY accepted");
        wrong = offer; wrong.format = Format::usb_buttons; wrong.capabilities = 2; f.client.receive(wrong,f.time); check(!f.client.take_packet(f.time), "wrong format READY accepted");
        f.client.receive(offer,f.time); auto arm = f.outgoing(); f.receive_at_chair(arm); auto ack = f.complete().value();
        wrong = ack; wrong.boot[0] ^= 1; f.client.receive(wrong,f.time); check(!f.client.ready(), "wrong boot ACK accepted");
        wrong = ack; wrong.challenge[0] ^= 1; f.client.receive(wrong,f.time); check(!f.client.ready(), "wrong challenge ACK accepted");
        wrong = ack; ++wrong.sequence; f.client.receive(wrong,f.time); check(!f.client.ready(), "wrong sequence ACK accepted");
        f.client.receive(ack,f.time); check(f.client.ready(), "valid ACK refused");
    }
    {
        Fixture f; f.arm(); f.update(1000,0); auto press=f.outgoing(); f.receive_at_chair(press); f.complete_at_chair();
        f.time=249'999; f.chair.poll(f.time); check(f.chair.armed(), "short lease expired early");
        f.time=250'000; f.chair.poll(f.time); check(!f.chair.armed(), "chair lease not bounded to250ms");
        f.complete_at_chair(); check(f.chair.completed_output().neutral(), "chair lease did not submit neutral source");
        Receiver old(token(2),random,&f.seed); old.set_usb_available(true,0);
        auto work=old.take_report(0).value(); old.complete_report(work.ticket,true,0);
        Packet hello; hello.session=token(1); auto offer=old.receive(hello,0).value();
        old.poll(999'999); check(old.active(), "default receiver lease changed"); old.poll(1'000'000); check(!old.active(), "default receiver lease no longer1s");
        Receiver invalid(token(2),random,&f.seed,nullptr,UsbOutput::xac,0); invalid.set_usb_available(true,0); check(!invalid.usb_ready(), "zero lease was allowed");
        (void)offer;
    }
    {
        Fixture f; f.arm(); f.update(1234,0); auto p=f.outgoing(); f.receive_at_chair(p); f.complete_at_chair();
        // Last upstream packet at t=0, remaining armed until1s. Its last relay
        // heartbeat may reach chair immediately before that bound; STOP is lost.
        for (f.time=50'000;f.time<1'000'000;f.time+=50'000) { f.client.source(f.source,true,1,f.time); auto h=f.outgoing();f.receive_at_chair(h);f.complete_at_chair(); }
        f.client.source(f.source,false,1,f.time); check(f.outgoing().kind==Kind::stop,"upstream expiry did not stop");
        f.chair.poll(1'250'000); check(!f.chair.armed(),"combined loss retained source beyond1.25s");
    }
    {
        Fixture f; f.time=10; f.arm(); f.client.source({},true,1,9); check(!f.client.ready(),"clock reversal retained live session");
        ChairRelayClient bad(nullptr); bad.source({},true,1,0);check(!bad.take_packet(0),"missing entropy generated session");
    }
    {
        Fixture f; auto hello=f.outgoing();f.update(12000,0);
        f.time=2'000'000;auto ready=f.chair.receive(hello,f.time).value();f.client.receive(ready,f.time);
        auto neutral=f.outgoing();check(neutral.state.neutral(),"late first connection omitted neutral arm");
        f.receive_at_chair(neutral);f.complete_at_chair();f.update(12000,0);
        check(f.client.awaiting_neutral()&&!f.client.take_packet(f.time),"old hold resumed when chair first appeared late");
        f.update(0,0);f.update(14000,0);check(f.outgoing().state.axes[2]==14000,"fresh neutral did not unlock late first connection");
    }
    std::cout << checks << " chair relay assertions passed\n";
}

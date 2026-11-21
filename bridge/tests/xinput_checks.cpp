// Format5, physical-output ownership and USB report checks. No attached hardware.
#include "scx2.hpp"
#include "xinput_report.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
using namespace scx2;
namespace {
unsigned checks=0;
void check(bool ok,const std::string& reason){if(!ok){std::cerr<<reason<<'\n';std::exit(1);}++checks;}
Token token(unsigned n){Token t{};t.fill(std::uint8_t(n));return t;}
Token entropy(void*){return token(7);}
Packet hello(unsigned n=1){Packet p;p.format=Format::usb_pressure;p.capabilities=4;p.session=token(n);return p;}
Packet state(Packet p,std::uint64_t sequence,TriggerPressure pressure={}){p.kind=Kind::state;p.capabilities=0;p.hid_ready=false;p.sequence=sequence;p.state.triggers=pressure;return p;}
void mount(Receiver& r,std::uint64_t at=0){r.set_usb_available(true,at);auto n=r.take_report(at).value();check(n.state.neutral(),"Mount barrier is neutral");(void)r.complete_report(n.ticket,true,at);}
Packet arm(Receiver& r,std::uint64_t at=1,unsigned id=1){auto p=r.receive(hello(id),at).value();check(p.hid_ready&&p.capabilities==4,"Pressure offer only after USB neutral");(void)r.receive(state(p,1),at);auto n=r.take_report(at).value();check(n.state.neutral()&&n.format==Format::usb_pressure&&!r.armed(),"Session starts with actual neutral transfer");auto ack=r.complete_report(n.ticket,true,at);check(ack&&r.armed(),"Neutral USB completion arms");return p;}
std::vector<std::uint8_t> read(const std::string& path){std::ifstream f(path);std::string s;f>>s;check(bool(f),"Fixture is readable");std::vector<std::uint8_t> b;for(std::size_t i=0;i<s.size();i+=2)b.push_back(std::uint8_t(std::stoul(s.substr(i,2),nullptr,16)));return b;}
struct Backend:TriggerOutputBackend{unsigned applies=0;bool ready()const noexcept override{return true;}bool apply(TriggerPressure)noexcept override{++applies;return true;}TriggerApplyResult apply_before(TriggerPressure p,std::uint64_t)noexcept override{apply(p);return TriggerApplyResult::accepted;}void release()noexcept override{}};
}
int main(int argc,char** argv){
 if(argc!=2)return 2;
 for(auto name:{"zen-hello","zen-unavailable","zen-ready","zen-neutral-first","zen-left-tenth","zen-right-full","zen-both-mixed","zen-ack","zen-stop-max","zen-stopped-max"}){
  auto b=read(std::string(argv[1])+"/"+name+".hex");auto p=decode(b.data(),b.size());auto w=p?encode(*p):std::nullopt;
  check(w&&w->size==b.size()&&std::equal(b.begin(),b.end(),w->bytes.begin()),std::string("Golden ")+name);
 }
 auto b=read(std::string(argv[1])+"/zen-both-mixed.hex");auto p=decode(b.data(),b.size()).value();
 check(p.state.triggers==TriggerPressure{32768,45875},"Shared fixture retains independent partial LT/RT");
 bool lengths=true;for(unsigned n=0;n<b.size();++n)lengths&=!decode(b.data(),n);auto extended=b;extended.push_back(0);lengths&=!decode(extended.data(),extended.size());check(lengths,"STATE exact length only");
 for(auto capability:{0,1,2,3,5,255}){auto q=hello();q.capabilities=std::uint8_t(capability);check(!encode(q),"HELLO cannot mix output capabilities");}
 auto ready=read(std::string(argv[1])+"/zen-ready.hex");for(auto capability:{0,1,2,3,5,255}){auto q=ready;q[7]=std::uint8_t(capability);check(!decode(q.data(),q.size()),"Usable READY requires exactly bit4");}
 auto bad=p;bad.capabilities=4;check(!encode(bad),"STATE capability reserved");bad=p;bad.state.axes[0]=-32768;check(!encode(bad),"Reserved axis value rejected");
 bad=p;bad.state.buttons=1024;check(!encode(bad),"Button mask remains ten bits");
 check(!hid_report(p.state,Port::left,Format::usb_pressure),"XAC encoder refuses format5 rather than silently dropping pressure");
 State s;s.buttons=1023;s.hat=3;s.axes={32767,-32767,1234,-4321};s.triggers={32768,45875};
 const auto out=xinput_report(s).value();
 check(out==XInputReport{0,20,0xfa,0xf3,128,179,255,127,1,128,0xd2,4,0x1f,0xef,0,0,0,0,0,0},"Independent expected XInput bytes preserve all controls");
 const auto chair=xinput_report(s,true).value();check(std::equal(out.begin(),out.begin()+10,chair.begin())&&std::all_of(chair.begin()+10,chair.begin()+14,[](auto n){return n==0;})&&std::equal(out.begin()+14,out.end(),chair.begin()+14),"Chair ownership suppresses only RX/RY, retaining RS click and all other controls");
 auto neutral=xinput_report({}).value();check(neutral[1]==20&&std::count(neutral.begin(),neutral.end(),0)==19,"Neutral releases every physical field");
 constexpr std::uint16_t buttons[]={0x1000,0x2000,0x4000,0x8000,0x0100,0x0200,0x0020,0x0010,0x0040,0x0080};
 for(unsigned i=0;i<10;++i){State q;q.buttons=1u<<i;auto r=xinput_report(q).value();check((r[2]|(r[3]<<8))==buttons[i],"Every logical button retains its Xbox identity");}
 constexpr unsigned directions[]={1,9,8,10,2,6,4,5,0};for(unsigned h=0;h<9;++h){State q;q.hat=h;check(xinput_report(q)->at(2)==directions[h],"Hat including diagonals maps correctly");}
 bool quantization=true;unsigned previous=0;for(unsigned value=0;value<=65535;++value){State q;q.triggers={std::uint16_t(value),std::uint16_t(65535-value)};auto r=xinput_report(q).value();quantization&=r[4]>=previous&&std::abs(int(r[4])*257-int(value))<=128&&r[5]==255-r[4];previous=r[4];}check(quantization,"All 65536 trigger levels are monotonic, independent and rounded within half an 8-bit step");
 {Receiver xac(token(9),entropy);mount(xac);auto q=xac.receive(hello(),1);check(q&&!q->hid_ready&&!q->capabilities&&!xac.active(),"Existing XAC USB build rejects Zen capability");}
 {Backend analog;Receiver xac(token(9),entropy,nullptr,&analog);mount(xac);auto q=xac.receive(hello(),1);check(q&&!q->hid_ready&&!xac.active(),"MCP analog circuit cannot masquerade as XInput USB");}
 {Receiver zen(token(9),entropy,nullptr,nullptr,UsbOutput::xinput);mount(zen);
  for(auto format:{Format::legacy,Format::pressure,Format::usb_buttons}){auto q=hello();q.format=format;q.capabilities=format==Format::legacy?0:format==Format::pressure?1:2;auto unavailable=zen.receive(q,1);check(unavailable&&!unavailable->hid_ready&&!zen.active(),"XInput does not silently reinterpret an XAC mode");}
  auto offer=arm(zen);auto press=state(offer,2,{32768,45875});press.state.buttons=1;press.state.axes={1000,2000,3000,4000};(void)zen.receive(press,2);
  auto transfer=zen.take_report(2).value();check(transfer.state==press.state&&!zen.receive(press,3),"Retry cannot ACK pending transfer");check(zen.completed_output().neutral(),"No output completion assumed before USB callback");
  auto ack=zen.complete_report(transfer.ticket,true,4);check(ack&&ack->format==Format::usb_pressure&&ack->sequence==2&&zen.completed_output()==press.state,"USB callback commits actual pressure state");
  check(zen.accepted_trigger_output().neutral(),"No GPIO trigger backend used");
  auto stop=state(offer,3);stop.kind=Kind::stop;check(!zen.receive(stop,5)&&!zen.active(),"STOP retires immediately, waits for physical neutral");
  auto release=zen.take_report(5).value();check(release.neutral_barrier&&release.state.neutral(),"STOP generates all-zero XInput control fields");
  check(!zen.receive(stop,6),"STOP retry before neutral transfer cannot report success");
  auto stopped=zen.complete_report(release.ticket,true,7);check(stopped&&stopped->kind==Kind::stopped&&stopped->format==Format::usb_pressure,"STOPPED only after physical neutral");
  auto next=arm(zen,8,2);(void)zen.receive(press,9);check(zen.queued()==0,"Old-session input remains fenced");
  (void)zen.receive(state(next,2,{65535,65535}),10);auto live=zen.take_report(10).value();zen.poll(1'000'010);
  check(!zen.complete_report(live.ticket,true,1'000'011)&&!zen.armed(),"Late physical press completion cannot ACK expired session");
  check(zen.take_report(1'000'012)->state.neutral(),"Lease expiry schedules neutral even after stale completion");
 }
 {Receiver zen(token(9),entropy,nullptr,nullptr,UsbOutput::xinput);mount(zen);auto offer=arm(zen);(void)zen.receive(state(offer,2,{65535,12345}),2);auto live=zen.take_report(2).value();zen.set_usb_available(false,3);check(!zen.complete_report(live.ticket,true,4)&&!zen.active(),"Reset fences in-flight pressure");mount(zen,5);auto next=arm(zen,6,3);check(next.challenge!=offer.challenge&&zen.completed_output().neutral(),"Re-enumeration needs fresh neutral/session");(void)zen.receive(state(next,2,{12345,65535}),7);auto fail=zen.take_report(7).value();check(!zen.complete_report(fail.ticket,false,8)&&zen.fault()==Fault::usb_transfer&&!zen.active()&&!zen.usb_ready(),"Failed USB completion cannot ACK or stay armed");}
 {Receiver zen(token(9),entropy,nullptr,nullptr,UsbOutput::xinput);mount(zen);auto offer=arm(zen);for(unsigned i=0;i<17;++i)(void)zen.receive(state(offer,i+2,{65535,1}),i+2);check(!zen.active()&&zen.fault()==Fault::queue_overflow&&zen.take_report(20)->state.neutral(),"Overflow discards trigger queue and schedules neutral");}
 std::cout<<"PASS: "<<checks<<" XInput/format5 checks plus exhaustive trigger quantization. No physical Cronus/Xbox tested.\n";
}

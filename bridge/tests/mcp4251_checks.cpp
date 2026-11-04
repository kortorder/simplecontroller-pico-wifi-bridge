// Original host checks. SPI and output gates are fake; no jack voltage is measured.
#include "mcp4251_driver.hpp"
#include "scx2.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
using namespace scx2;
namespace {
int checks=0, failures=0;
void require(bool result,const std::string& name) {
    ++checks;
    if(!result) { ++failures; std::cerr<<"FAIL: "<<name<<'\n'; }
}
using Bytes=std::array<std::uint8_t,2>;
struct FakeIO final: Mcp4251IO {
    enum class Reply { chip, zero, ones } reply=Reply::chip;
    bool good=true, output=false;
    std::uint64_t time=0, transaction_us=0;
    std::array<std::uint16_t,5> memory{128,128,0,0,511};
    std::vector<Bytes> transactions;
    std::vector<bool> gates;
    std::vector<char> order;
    unsigned fail_at=0, command_error_at=0, corrupt_at=0, power_loss_at=0;
    bool power_good() const noexcept override { return good; }
    std::uint64_t monotonic_us() const noexcept override { return time; }
    void gate(bool enabled) noexcept override {
        gates.push_back(enabled);order.push_back(enabled?'E':'D');output=enabled&&good;
    }
    bool transfer(Bytes tx,Bytes& rx) noexcept override {
        transactions.push_back(tx);order.push_back('S');
        const auto maximum=std::numeric_limits<std::uint64_t>::max();
        time=transaction_us>maximum-time ? maximum : time+transaction_us;
        const auto n=transactions.size();
        if(n==power_loss_at) {good=false;output=false;}
        if(n==fail_at || !good) return false;
        if(reply==Reply::zero) {rx={0,0};return true;}
        if(reply==Reply::ones) {rx={255,255};return true;}
        // Independent chip model: only documented volatile W0/W1/TCON accesses.
        const unsigned address=tx[0]>>4, command=(tx[0]>>2)&3;
        if(n==command_error_at || (address!=0 && address!=1 && address!=4) ||
           (command!=0 && command!=3)) {rx={0xfc,0};return true;}
        if(command==0) {
            auto value=std::uint16_t((std::uint16_t(tx[0]&1)<<8)|tx[1]);
            if(address==4) value|=0x100; // TCON D8 reads one, irrespective of write.
            memory[address]=value;rx={255,255};
        } else {
            auto value=memory[address];
            if(n==corrupt_at) value^=1;
            rx={std::uint8_t(0xfe|((value>>8)&1)),std::uint8_t(value)};
        }
        return true;
    }
    void clear_trace() {transactions.clear();gates.clear();order.clear();}
};
void initialized(Mcp4251Triggers& driver) { if(!driver.initialize())std::abort(); }
Token token(unsigned n) {Token t{};t.fill(std::uint8_t(n));return t;}
Token entropy(void*) {return token(99);}
Packet hello() {Packet p;p.format=Format::pressure;p.capabilities=1;p.session=token(2);return p;}
Packet state(Packet p,std::uint64_t sequence,TriggerPressure pressure={}) {
    p.kind=Kind::state;p.sequence=sequence;p.hid_ready=false;p.capabilities=0;p.state.triggers=pressure;return p;
}
Packet arm(Receiver& receiver) {
    receiver.set_usb_available(true,0);
    auto report=receiver.take_report(0).value();(void)receiver.complete_report(report.ticket,true,0);
    auto offer=receiver.receive(hello(),0).value();
    (void)receiver.receive(state(offer,1),0);report=receiver.take_report(0).value();
    if(!receiver.complete_report(report.ticket,true,0))std::abort();return offer;
}
}
int main() {
    {
        FakeIO io;Mcp4251Triggers driver(io);
        require(!driver.ready() && !io.output && io.transactions.empty(),"Construction cannot advertise an uninitialized circuit");
        require(driver.initialize() && driver.ready(),"Initialization accepts a responding supervised chip");
        const std::vector<Bytes> expected{{0x41,0xff},{0x4c,0xff},{0,0},{0x10,0},{0x0c,0xff},{0x1c,0xff}};
        require(io.transactions==expected,"Initialization writes and verifies TCON1FF and both zero wipers using exact framed commands");
        require(io.order.front()=='D' && std::none_of(io.gates.begin(),io.gates.end(),[](bool x){return x;}),"Initialization disconnects outputs before SPI and never enables them");
        require(io.memory[0]==0 && io.memory[1]==0 && io.memory[4]==511,"Power-on midscale is replaced with verified zero");
    }
    {
        FakeIO io;io.good=false;Mcp4251Triggers driver(io);
        require(!driver.initialize() && !driver.ready() && !io.output && io.transactions.empty(),"Bad supervisor prevents all initialization SPI and leaves gates open");
        io.good=true;require(!driver.apply({65535,65535}) && io.transactions.empty(),"Power recovery alone cannot bypass failed initialization");
    }
    for(auto reply:{FakeIO::Reply::zero,FakeIO::Reply::ones}) {
        FakeIO io;io.reply=reply;Mcp4251Triggers driver(io);
        require(!driver.initialize() && !driver.ready() && !io.output,"Missing-chip constant response fails initialization: "+std::to_string(int(reply)));
    }
    for(unsigned step=1;step<=6;++step) {
        FakeIO io;io.fail_at=step;Mcp4251Triggers driver(io);
        require(!driver.initialize() && !driver.ready() && !io.output && io.transactions.size()==step,
                "Initialization transaction failure is bounded and never arms: "+std::to_string(step));
    }
    for(unsigned step:{1u,2u,3u,4u,5u,6u}) {
        FakeIO io;io.command_error_at=step;Mcp4251Triggers driver(io);
        require(!driver.initialize() && !driver.ready() && !io.output,"CMDERR on initialization read/write rejects even zero data: "+std::to_string(step));
    }
    {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);io.clear_trace();
        require(driver.apply({65535,32768}) && io.output && io.memory[0]==256 && io.memory[1]==128,"Independent full/half pressures use the ninth wiper bit and both channels");
        const std::vector<Bytes> expected{{0x4c,0xff},{0x01,0x00},{0x10,0x80},{0x0c,0xff},{0x1c,0xff}};
        require(io.transactions==expected && io.order.back()=='E',"Outputs enable only after both wiper writes and matching readbacks");
        io.clear_trace();require(driver.apply({65535,32768}),"Unchanged held pressure remains accepted");
        require(io.transactions==std::vector<Bytes>{{0x4c,0xff},{0x0c,0xff},{0x1c,0xff}} &&
                io.gates==std::vector<bool>{true} && io.output,"Held heartbeat verifies only: no zero write or relay off glitch");
        io.clear_trace();require(driver.apply({65534,32767}),"Distinct wire pressures within a wiper step remain accepted");
        require(io.transactions.size()==3 && io.gates==std::vector<bool>{true},"Same quantized wiper target avoids redundant writes or gate cycling");
        io.clear_trace();require(driver.apply({16384,49151}) && io.memory[0]==64 && io.memory[1]==192 && io.output,"Live fractional change updates both channels without a zero pulse");
        require(io.gates==std::vector<bool>{true},"Nonzero transition does not briefly release both trigger gates");
        io.clear_trace();require(driver.apply({0,65535}) && io.memory[0]==0 && io.memory[1]==256 && io.output,"A zero left trigger does not release an independently held right trigger");
        io.clear_trace();require(driver.apply({}) && !io.output && io.memory[0]==0 && io.memory[1]==0,"Complete release clears both registers and stays disconnected");
        require(io.order.front()=='D' && std::none_of(io.gates.begin(),io.gates.end(),[](bool x){return x;}),"Complete release opens gates before the first SPI operation");
    }
    {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);(void)driver.apply({65535,65535});io.clear_trace();
        io.good=false;io.fail_at=1;driver.release();
        require(!io.output && io.transactions.empty() && io.gates==std::vector<bool>{false},"Release needs no SPI, even with lost power and a failed bus");
        driver.release();require(io.transactions.empty() && !io.output,"Repeated release is harmless and still independent of SPI");
    }
    for(unsigned step=1;step<=5;++step) {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);(void)driver.apply({32768,32768});io.clear_trace();io.fail_at=step;
        require(!driver.apply({65535,16384}) && !io.output && !driver.ready() && io.transactions.size()==step,
                "Any live update failure disconnects both outputs, including a partial write: "+std::to_string(step));
        const auto transfers=io.transactions.size();io.fail_at=0;
        require(!driver.apply({65535,65535}) && !io.output && io.transactions.size()==transfers,
                "Transfer fault remains sticky without explicit reinitialization: "+std::to_string(step));
    }
    for(unsigned step:{1u,2u,3u,4u,5u}) {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);(void)driver.apply({32768,32768});io.clear_trace();io.command_error_at=step;
        require(!driver.apply({65535,16384}) && !io.output && !driver.ready(),"Live CMDERR cannot accept a positive command: "+std::to_string(step));
    }
    for(unsigned step:{1u,2u,3u}) {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);(void)driver.apply({65535,32768});io.clear_trace();io.corrupt_at=step;
        require(!driver.apply({65535,32768}) && !io.output && !driver.ready(),"Heartbeat detects TCON or either wiper mismatch: "+std::to_string(step));
    }
    {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);(void)driver.apply({65535,65535});io.memory[0]=128;io.memory[1]=128;
        require(!driver.apply({65535,65535}) && !io.output && !driver.ready(),"Unexpected reset to midscale is rejected instead of acknowledged");
    }
    for(unsigned step=1;step<=5;++step) {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);(void)driver.apply({32768,32768});io.clear_trace();io.power_loss_at=step;
        require(!driver.apply({65535,65535}) && !io.output && !driver.ready(),"Supervisor loss during a transaction removes output: "+std::to_string(step));
        io.good=true;io.power_loss_at=0;const auto count=io.transactions.size();
        require(!driver.apply({65535,65535}) && !io.output && io.transactions.size()==count,"Supervisor fault cannot self-rearm when power returns: "+std::to_string(step));
    }
    {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);(void)driver.apply({65535,65535});io.clear_trace();io.fail_at=1;
        require(!driver.apply({}) && !io.output && io.order.front()=='D',"Neutral output is disconnected even when its first verification fails");
        io.fail_at=0;io.clear_trace();require(driver.initialize() && driver.ready() && !io.output && io.memory[0]==0 && io.memory[1]==0,"Explicit reinitialization repeats the complete disconnected zero procedure");
    }
    {
        bool monotonic=true,nearest=true;unsigned previous=0;std::array<bool,257> reached{};
        for(unsigned pressure=0;pressure<=65535;++pressure) {
            const auto value=Mcp4251Triggers::wiper(std::uint16_t(pressure));
            monotonic &= value>=previous && value<=256;
            const auto distance=std::int64_t(value)*65535-std::int64_t(pressure)*256;
            nearest &= std::abs(distance)<=32767;
            reached[value]=true;previous=value;
        }
        require(monotonic && Mcp4251Triggers::wiper(0)==0 && Mcp4251Triggers::wiper(65535)==256,"All65536 pressure values map monotonically between exact endpoints");
        require(nearest && std::all_of(reached.begin(),reached.end(),[](bool x){return x;}),"Quantization chooses the nearest of all257 reachable wiper positions");
    }
    {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);Receiver receiver(token(3),entropy,nullptr,&driver);auto offer=arm(receiver);
        (void)receiver.receive(state(offer,2,{32768,65535}),1);auto work=receiver.take_report(1).value();
        require(!io.output && receiver.accepted_trigger_output().neutral(),"Actual receiver cannot enable MCP output before matching USB completion");
        const auto ack=receiver.complete_report(work.ticket,true,2);
        require(ack && ack->kind==Kind::ack && io.output && io.memory[0]==128 && io.memory[1]==256,"Receiver ACK follows the driver's verified complete dual-channel command");
        (void)receiver.receive(state(offer,3,{65535,65535}),3);work=receiver.take_report(3).value();
        auto stop=state(offer,4);stop.kind=Kind::stop;io.clear_trace();(void)receiver.receive(stop,4);
        require(!io.output && io.transactions.empty(),"Receiver Stop disables the real driver contract immediately without queued SPI");
        require(!receiver.complete_report(work.ticket,true,5) && !io.output,"Stale USB completion cannot reassert MCP output after Stop");
    }
    {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);Receiver receiver(token(3),entropy,nullptr,&driver);auto offer=arm(receiver);
        (void)receiver.receive(state(offer,2,{32768,65535}),1);auto work=receiver.take_report(1).value();io.clear_trace();io.fail_at=3;
        require(!receiver.complete_report(work.ticket,true,2) && !io.output && !receiver.active() && receiver.fault()==Fault::trigger_output,
                "Partial SPI update after USB completion returns no ACK and retires the receiver");
    }
    for(unsigned elapsed=1;elapsed<=10;++elapsed) {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);io.clear_trace();io.time=100;io.transaction_us=2;
        const auto result=driver.apply_before({32768,65535},100+elapsed);
        require(result==TriggerApplyResult::expired && !io.output && driver.ready() &&
                std::none_of(io.gates.begin(),io.gates.end(),[](bool x){return x;}),
                "SPI crossing deadline never enables output or poisons healthy state: "+std::to_string(elapsed));
    }
    for(unsigned failed_step=1;failed_step<=5;++failed_step) {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);io.clear_trace();io.time=100;io.transaction_us=2;io.fail_at=failed_step;
        require(driver.apply_before({32768,65535},100+2*failed_step)==TriggerApplyResult::fault && !io.output && !driver.ready(),
                "Actual SPI failure remains sticky when that transaction also crosses expiry: "+std::to_string(failed_step));
    }
    for(unsigned failed_step:{1u,4u,5u}) {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);io.clear_trace();io.time=100;io.transaction_us=2;io.corrupt_at=failed_step;
        require(driver.apply_before({32768,65535},100+2*failed_step)==TriggerApplyResult::fault && !io.output && !driver.ready(),
                "Readback mismatch cannot masquerade as recoverable expiry: "+std::to_string(failed_step));
    }
    {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);io.clear_trace();io.time=100;io.transaction_us=2;io.command_error_at=2;
        require(driver.apply_before({32768,65535},104)==TriggerApplyResult::fault && !io.output && !driver.ready(),
                "CMDERR stays a hardware fault when the command crosses expiry");
    }
    {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);io.clear_trace();io.time=100;
        require(driver.apply_before({65535,65535},100)==TriggerApplyResult::expired && io.transactions.empty() && !io.output && driver.ready(),
                "Exactly expired command is released without any new SPI transaction");
        io.time=std::numeric_limits<std::uint64_t>::max();io.transaction_us=1;
        require(driver.apply_before({},std::numeric_limits<std::uint64_t>::max())==TriggerApplyResult::accepted && !io.output,
                "Unbounded inactive zero accepts UINT64MAX without deadline arithmetic overflow");
    }
    {
        FakeIO io;Mcp4251Triggers driver(io);initialized(driver);Receiver receiver(token(3),entropy,nullptr,&driver);auto offer=arm(receiver);
        (void)receiver.receive(state(offer,2,{32768,65535}),1);auto work=receiver.take_report(1).value();
        const auto deadline=1+Receiver::lease_us;io.time=deadline-10;io.transaction_us=2;io.clear_trace();
        const auto ack=receiver.complete_report(work.ticket,true,io.time);
        require(!ack && !receiver.active() && receiver.fault()==Fault::none && driver.ready() && !io.output &&
                receiver.accepted_trigger_output().neutral(),"Receiver treats expiry during verified SPI as a neutral recoverable lease end, not hardware fault");
        require(std::none_of(io.gates.begin(),io.gates.end(),[](bool x){return x;}),"No transient gate assertion occurs when the final readback completes exactly at expiry");
        io.transaction_us=0;
        work=receiver.take_report(io.time).value();(void)receiver.complete_report(work.ticket,true,io.time);
        auto new_offer=receiver.receive(hello(),io.time).value();
        (void)receiver.receive(state(new_offer,1),io.time);work=receiver.take_report(io.time).value();
        const auto fresh_ack=receiver.complete_report(work.ticket,true,io.time);
        require(new_offer.hid_ready && new_offer.challenge!=offer.challenge && fresh_ack && receiver.armed() && !io.output,
                "Fresh neutral handshake succeeds after SPI-time expiry without resetting the backend");
    }
    if(failures) {std::cerr<<failures<<" of "<<checks<<" MCP4251 checks failed\n";return 1;}
    std::cout<<"PASS: "<<checks<<" MCP4251 driver/SPI/gate/receiver host checks. Fake I/O only; no electrical or XAC measurement.\n";
}

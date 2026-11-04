// Host-only test peer. It binds IPv4 loopback only and completes explicitly fake USB jobs.
#include "scx2.hpp"
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
std::uint64_t now() { return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count()); }
scx2::Token token(void*) { scx2::Token t{}; t.fill(0x62); return t; }
std::string hex(const scx2::HidReport& bytes) { std::ostringstream s; for(auto b:bytes)s<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b); return s.str(); }
std::vector<std::uint8_t> unhex(const std::string& raw) {
    if(raw.size()>160 || raw.size()%2) return {};
    std::vector<std::uint8_t> bytes;
    for(std::size_t i=0;i<raw.size();i+=2) {
        const auto pair=raw.substr(i,2); if(pair.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos) return {};
        bytes.push_back(std::uint8_t(std::stoul(pair,nullptr,16)));
    }
    return bytes;
}
// Explicit host-test backend. No GPIO, physical circuit or XAC output exists here.
struct FakePressureBackend final : scx2::TriggerOutputBackend {
    bool available=true, accept=true;
    scx2::TriggerPressure output{};
    std::vector<scx2::TriggerPressure> applied;
    bool ready() const noexcept override { return available; }
    bool apply(scx2::TriggerPressure pressure) noexcept override {
        if(!ready() || !accept) return false;
        output=pressure; if(applied.size()<1024) applied.push_back(pressure); return true;
    }
    scx2::TriggerApplyResult apply_before(scx2::TriggerPressure pressure,std::uint64_t deadline) noexcept override {
        if(now()>=deadline) { release();return scx2::TriggerApplyResult::expired; }
        return apply(pressure) ? scx2::TriggerApplyResult::accepted : scx2::TriggerApplyResult::fault;
    }
    void release() noexcept override { output={}; }
};
bool same(const sockaddr_in& a,const sockaddr_in& b) { return a.sin_addr.s_addr==b.sin_addr.s_addr && a.sin_port==b.sin_port; }
int loopback_socket() {
    int fd=socket(AF_INET,SOCK_DGRAM,0); if(fd<0) return -1;
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0) { close(fd); return -1; }
    return fd;
}
}
int main(int argc,char** argv) {
    bool pressure_backend=false,auto_start=false,legacy_only=false;
    unsigned duration_seconds=60;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--pressure-backend") pressure_backend=true;
        else if(arg=="--auto") auto_start=true;
        else if(arg=="--legacy-only") legacy_only=true;
        else if(arg=="--duration" && i+1<argc) {
            const std::string value=argv[++i];
            if(value.empty() || value.size()>4 || value.find_first_not_of("0123456789")!=std::string::npos) return 7;
            duration_seconds=unsigned(std::stoul(value));
            if(!duration_seconds || duration_seconds>3600) return 7;
        } else { std::cerr<<"Usage: udp-peer [--pressure-backend] [--auto] [--legacy-only] [--duration 1..3600]\n";return 7; }
    }
    if(pressure_backend && legacy_only) return 7;
    const int fd=loopback_socket(), foreign=loopback_socket();
    if(fd<0 || foreign<0) { std::cerr<<"Loopback socket unavailable: "<<std::strerror(errno)<<'\n'; return 2; }
    sockaddr_in local{}; socklen_t length=sizeof(local);
    if(getsockname(fd,reinterpret_cast<sockaddr*>(&local),&length)<0) return 3;
    std::cout<<"PORT "<<ntohs(local.sin_port)<<std::endl;
    FakePressureBackend pressure;
    scx2::Receiver receiver(token(nullptr),token,nullptr,pressure_backend ? &pressure : nullptr);
    receiver.set_usb_available(true,now()); auto initial=receiver.take_report(now()).value();
    (void)receiver.complete_report(initial.ticket,true,now());
    sockaddr_in peer{},old_peer{}; bool known=false,old_known=false,automatic=auto_start,drop_ack=false,discard=false;
    std::uint64_t received=0,rejected=0,completed=0,last_sequence=0;
    std::vector<unsigned> completed_buttons;
    auto raw_send=[&](int socket,const std::vector<std::uint8_t>& bytes,const sockaddr_in& destination) {
        if(!bytes.empty()) (void)sendto(socket,bytes.data(),bytes.size(),0,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination));
    };
    auto send=[&](const std::optional<scx2::Packet>& reply,const sockaddr_in& destination) {
        if(!reply) return;
        if(drop_ack && reply->kind==scx2::Kind::ack) { drop_ack=false; return; }
        const auto bytes=scx2::encode(*reply).value();
        (void)sendto(fd,bytes.bytes.data(),bytes.size,0,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination));
    };
    auto complete=[&]() {
        auto work=receiver.take_report(now());
        if(!work) return false;
        ++completed;
        if(completed_buttons.size()<1024) completed_buttons.push_back(work->state.buttons);
        auto reply=receiver.complete_report(work->ticket,true,now());
        if(reply) last_sequence=reply->sequence;
        if(known) send(reply,peer);
        return true;
    };
    const auto deadline=now()+std::uint64_t(duration_seconds)*1'000'000;
    while(now()<deadline) {
        receiver.poll(now()); if(automatic) (void)complete();
        fd_set inputs; FD_ZERO(&inputs); FD_SET(fd,&inputs); FD_SET(STDIN_FILENO,&inputs);
        timeval timeout{0,5000};
        const int ready=select(fd+1,&inputs,nullptr,nullptr,&timeout);
        if(ready<0) { if(errno==EINTR) continue; return 4; }
        if(FD_ISSET(STDIN_FILENO,&inputs)) {
            std::string line; if(!std::getline(std::cin,line)) break;
            if(line.size()>1024) return 5;
            std::istringstream command(line); std::string op,arg; command>>op>>arg;
            if(op=="quit") { std::cout<<"OK"<<std::endl; close(fd);close(foreign);return 0; }
            if(op=="auto") { automatic=arg=="on"; std::cout<<"OK"<<std::endl; }
            else if(op=="discard") { discard=arg=="on"; std::cout<<"OK"<<std::endl; }
            else if(op=="backend") { pressure.available=arg=="on";receiver.poll(now());std::cout<<"OK"<<std::endl; }
            else if(op=="backend_fail") { pressure.accept=arg!="on";std::cout<<"OK"<<std::endl; }
            else if(op=="pressure") std::cout<<pressure.output.left<<' '<<pressure.output.right<<std::endl;
            else if(op=="pressure_reports") { for(auto value:pressure.applied)std::cout<<value.left<<':'<<value.right<<',';std::cout<<std::endl; }
            else if(op=="drop_ack") { drop_ack=true; std::cout<<"OK"<<std::endl; }
            else if(op=="complete") { std::cout<<(complete()?"DONE":"EMPTY")<<std::endl; }
            else if(op=="output") std::cout<<hex(scx2::hid_report(receiver.completed_output()).value())<<std::endl;
            else if(op=="status") std::cout<<receiver.active()<<' '<<receiver.armed()<<' '<<received<<' '<<rejected<<' '<<completed<<' '<<last_sequence<<' '<<receiver.queued()<<std::endl;
            else if(op=="reports") { for(auto buttons:completed_buttons)std::cout<<buttons<<',';std::cout<<std::endl; }
            else if(op=="foreign" && known) { raw_send(foreign,unhex(arg),peer);std::cout<<"OK"<<std::endl; }
            else if(op=="send_old" && old_known) { raw_send(fd,unhex(arg),old_peer);std::cout<<"OK"<<std::endl; }
            else if(op=="send" && known) { raw_send(fd,unhex(arg),peer);std::cout<<"OK"<<std::endl; }
            else { std::cout<<"INVALID"<<std::endl; }
        }
        if(FD_ISSET(fd,&inputs)) {
            std::array<std::uint8_t,1024> bytes{}; sockaddr_in address{}; socklen_t n=sizeof(address);
            const auto count=recvfrom(fd,bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&address),&n);
            if(count<0) continue;
            ++received; receiver.poll(now()); if(discard) continue;
            auto packet=scx2::decode(bytes.data(),std::size_t(count)); if(!packet || (legacy_only && packet->format!=scx2::Format::legacy)) continue;
            if(receiver.active() && known && !same(address,peer)) { ++rejected;continue; }
            const bool active=receiver.active(); auto reply=receiver.receive(*packet,now());
            if(reply && reply->kind==scx2::Kind::ready && reply->hid_ready && !active) {
                if(known) {old_peer=peer;old_known=true;} peer=address;known=true;
            }
            send(reply,address);
        }
    }
    std::cerr<<"Loopback peer stopped (EOF or configured duration bound).\n";
    close(fd);close(foreign);return 6;
}

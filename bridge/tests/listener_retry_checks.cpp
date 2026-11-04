#include "listener_retry.hpp"
#include <cstdlib>
#include <iostream>
int main() {
    unsigned checks=0;
    auto check=[&](bool result,const char* message) { if(!result) { std::cerr<<message<<'\n'; std::exit(1); } ++checks; };
    scx2::ListenerStartRetry start;
    check(start.due(0),"first allocation is immediate");
    start.failed(10); // allocation failure
    check(!start.due(1'000'009) && start.due(1'000'010),"allocation failure waits its full retry delay");
    start.failed(1'000'010); // bind failure
    check(!start.faulted() && start.failures()==2,"bind failure does not permanently strand startup");
    start.succeeded();
    check(!start.due(100'000'000),"successful listener is not allocated again");
    start.reset();
    for(unsigned i=0;i<5;++i) start.failed(std::uint64_t(i)*1'000'000);
    check(start.faulted() && !start.due(100'000'000),"five failures latch a bounded startup fault");
    start.succeeded(); check(start.faulted(),"late success cannot clear a fault");
    start.reset(); check(start.due(100'000'000) && start.failures()==0,"USB reset allows a new bounded attempt series");
    start.failed(UINT64_MAX); check(start.faulted(),"retry deadline cannot wrap");
    std::cout<<"PASS: "<<checks<<" listener retry policy checks; no board/network/USB used.\n";
}

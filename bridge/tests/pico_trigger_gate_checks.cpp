#include "pico_trigger_output.hpp"
#include <iostream>
int main() {
    unsigned count=0,failures=0;
    auto require=[&](bool pass,const char* name){++count;if(!pass){++failures;std::cerr<<"FAIL: "<<name<<'\n';}};
    {
        fake_pico::reset(false);fake_pico::during_sleep=[](){fake_pico::level[21]=true;};
        PicoTriggerIO io;io.initialize();
        require(io.power_good() && !gpio_get(20),"Initial supervisor-low delay can recover before startup without a false edge fault");
        io.gate(true);require(gpio_get(20),"Healthy initialized gate accepts explicit enable");
        fake_pico::fall();
        require(!gpio_get(20) && !io.power_good() && fake_pico::callback_count==1,"Falling-edge ISR immediately clears GP20 and latches unavailable");
        fake_pico::level[21]=true;io.gate(true);
        require(!gpio_get(20) && !io.power_good(),"Recovered voltage cannot reconnect the previous software request");
        io.initialize();require(io.power_good() && !gpio_get(20),"Explicit initialization clears the latch while keeping output disconnected");
    }
    {
        fake_pico::reset();PicoTriggerIO io;io.initialize();io.gate(true);
        auto saved=save_and_disable_interrupts();fake_pico::fall();fake_pico::level[21]=true;
        require(!io.power_good(),"Pending falling-edge history is visible even before a delayed ISR executes");
        const auto positives=fake_pico::positive_writes;io.gate(true);
        require(!gpio_get(20) && fake_pico::positive_writes==positives && !fake_pico::interrupt_enabled,
                "Gate checks pending edge atomically and preserves an outer interrupt-disabled state");
        restore_interrupts(saved);require(!io.power_good() && !gpio_get(20),"Deferred ISR keeps the fault latched after interrupts resume");
    }
    {
        fake_pico::reset();PicoTriggerIO io;io.initialize();
        fake_pico::during_enable=[](){fake_pico::fall();};io.gate(true);
        require(!gpio_get(20) && !io.power_good() && fake_pico::callback_count==1,
                "An edge at the positive GPIO write is delivered after the critical section and cannot be overwritten by that write");
        fake_pico::level[21]=true;io.gate(true);
        require(!gpio_get(20),"The check/write race cannot rearm on subsequent good voltage");
    }
    {
        fake_pico::reset();PicoTriggerIO io;io.initialize();fake_pico::level[21]=false;io.gate(true);
        require(!gpio_get(20) && !io.power_good(),"Observed low level fails closed even without a captured edge");
        fake_pico::level[21]=true;require(!io.power_good(),"A directly observed low level remains latched after recovery");
    }
    if(failures)return 1;
    std::cout<<"PASS: "<<count<<" actual PicoTriggerIO gate/IRQ host-shim checks. No physical interrupt or electrical timing proof.\n";
}

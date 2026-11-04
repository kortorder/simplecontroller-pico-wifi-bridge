#pragma once
// Host SDK shim: tests execute the actual PicoTriggerIO methods, not a copied latch.
#include <array>
#include <cstdint>
using uint = unsigned;
constexpr unsigned GPIO_OUT=1,GPIO_IN=0,GPIO_FUNC_SPI=1,GPIO_IRQ_EDGE_FALL=4;
namespace fake_pico {
inline std::array<bool,32> level{};
inline bool interrupt_enabled=true, edge_enabled=false;
inline std::uint32_t pending=0;
inline std::uint64_t time=0;
inline void (*callback)(unsigned,std::uint32_t)=nullptr;
inline void (*during_enable)()=nullptr;
inline void (*during_sleep)()=nullptr;
inline unsigned positive_writes=0, callback_count=0;
inline void dispatch() {
    if(interrupt_enabled && edge_enabled && pending && callback) {
        auto events=pending;pending=0;++callback_count;callback(21,events);
    }
}
inline void fall() {level[21]=false;pending|=GPIO_IRQ_EDGE_FALL;dispatch();}
inline void reset(bool good=true) {
    level={};level[21]=good;interrupt_enabled=true;edge_enabled=false;pending=0;time=0;
    callback=nullptr;during_enable=nullptr;during_sleep=nullptr;positive_writes=0;callback_count=0;
}
}
inline void gpio_init(unsigned) {}
inline void gpio_set_dir(unsigned,unsigned) {}
inline void gpio_pull_down(unsigned) {}
inline void gpio_set_function(unsigned,unsigned) {}
inline bool gpio_get(unsigned pin) {return fake_pico::level[pin];}
inline void gpio_put(unsigned pin,bool value) {
    if(pin==20 && value) {
        ++fake_pico::positive_writes;
        if(fake_pico::during_enable) {auto fn=fake_pico::during_enable;fake_pico::during_enable=nullptr;fn();}
    }
    fake_pico::level[pin]=value;
}
inline void gpio_set_irq_enabled(unsigned,unsigned,bool value) {fake_pico::edge_enabled=value;}
inline void gpio_acknowledge_irq(unsigned,std::uint32_t events) {fake_pico::pending&=~events;}
inline std::uint32_t gpio_get_irq_event_mask(unsigned) {return fake_pico::pending;}
inline void gpio_set_irq_enabled_with_callback(unsigned,unsigned,bool value,void(*fn)(unsigned,std::uint32_t)) {
    fake_pico::callback=fn;fake_pico::edge_enabled=value;fake_pico::dispatch();
}
inline std::uint64_t time_us_64() {return fake_pico::time;}
inline void sleep_ms(unsigned ms) {fake_pico::time+=std::uint64_t(ms)*1000;if(fake_pico::during_sleep)fake_pico::during_sleep();}
inline void busy_wait_us_32(unsigned us) {fake_pico::time+=us;}

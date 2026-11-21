#include "../firmware/lwipopts.h"
#if defined(SC_CHAIR_RELAY) && SC_CHAIR_RELAY
static_assert(MEMP_NUM_UDP_PCB == 3, "Relay needs DHCP plus app and chair sockets");
#else
static_assert(MEMP_NUM_UDP_PCB == 2, "Default UDP resource budget must remain unchanged");
#endif
static_assert(LWIP_DHCP == 1 && LWIP_UDP == 1, "Expected real DHCP/UDP configuration");
int main() {}

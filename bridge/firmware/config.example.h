#pragma once
// Empty defaults build a USB-only diagnostic image: no radio association or UDP service.
// Copy to an ignored/private header and pass SC_BRIDGE_CONFIG to CMake for local setup.
// Credential-bearing headers, build folders and UF2 images must never be committed/shared.
#define SC_WIFI_SSID ""
#define SC_WIFI_PASSWORD ""
#define SC_WIFI_COUNTRY CYW43_COUNTRY_USA
#define SC_UDP_PORT 42736
#define SC_XAC_RIGHT_PORT 0

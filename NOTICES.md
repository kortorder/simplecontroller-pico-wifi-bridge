# Source and dependency notices

The SCX2 protocol implementation, test harness, fixtures, HID descriptor, board adapter and setup tools were written for this project. This source publication does not specify a license for the project's own code. Third-party license texts apply to their respective dependencies.

The repository does not bundle SDK sources, compiler executables, operating-system libraries, Wi-Fi firmware or a UF2 image. The verified build obtains these dependencies separately. Its sources and tool hashes are pinned in [the build manifest](bridge/firmware/build-manifest.json) and [executable checksum list](bridge/firmware/tool-binaries.json).

| Component | Source and license material |
| --- | --- |
| Raspberry Pi Pico SDK 2.3.1 and SDK components | [Pinned SDK](https://github.com/raspberrypi/pico-sdk/tree/079c6f39023649b154152db30f1d781e884879bc), [SDK notice](bridge/third_party_licenses/pico-sdk-LICENSE.txt), [printf notice](bridge/third_party_licenses/pico-printf-LICENSE.txt) |
| TinyUSB | [Pinned TinyUSB](https://github.com/hathach/tinyusb/tree/86ad6e56c1700e85f1c5678607a762cfe3aa2f47), [MIT notice](bridge/third_party_licenses/tinyusb-LICENSE.txt) |
| lwIP | [Pinned lwIP](https://github.com/lwip-tcpip/lwip/tree/77dcd25a72509eb83f72b033d219b1d40cd8eb95), [COPYING](bridge/third_party_licenses/lwip-COPYING.txt) |
| CYW43 driver and radio firmware | [Pinned driver](https://github.com/georgerobotics/cyw43-driver/tree/055d64274b014dd7b1c2fc94d26e8a18face7124), [driver license](bridge/third_party_licenses/cyw43-LICENSE.txt), [Raspberry Pi terms](bridge/third_party_licenses/cyw43-LICENSE.RP.txt), [radio firmware notes](https://github.com/georgerobotics/cyw43-driver/blob/055d64274b014dd7b1c2fc94d26e8a18face7124/firmware/README.md) |

The CYW43 Raspberry Pi terms are hardware-specific. A compiled image links SDK, network and USB components and embeds radio firmware. Its notices and redistribution terms must be evaluated for the actual artifact and hardware. This package distributes source only.

The USB mapping was implemented using Microsoft's public [Xbox Adaptive Controller input specification](https://assets.xboxservices.com/assets/b8/05/b8055a56-827c-48f8-b8ec-2c7aa66ef069.pdf?n=xbox-adaptive-controller-input-device-specification_1.6_Finalpdf.pdf). This project has no claimed Microsoft/Xbox certification or vendor-assigned product identity. The shared test USB ID has [separate, in-house-only terms](https://pid.codes/1209/0001/).

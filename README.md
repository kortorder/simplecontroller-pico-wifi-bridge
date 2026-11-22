# SimpleController Wi-Fi Pico bridge

[Download the complete phone bridge source ZIP](https://github.com/kortorder/simplecontroller-pico-wifi-bridge/raw/refs/heads/main/simplecontroller-pico-wifi-bridge-source.zip). Extract it before following the setup steps below.

Use a Raspberry Pi **Pico W (RP2040)** as a USB joystick for SimpleController. The phone sends controls over the local Wi-Fi network to the Pico. The Pico connects by USB to a computer for testing, or to a side USB port on an Xbox Adaptive Controller (XAC). The XAC connects to the Xbox. The bridge uses SCX2 on UDP port **42736**.

This repository contains source and build instructions. Build your own image with your local network settings. The prior USB-only Pico W has been tested with an iPhone, a Mac USB host and an Xbox Adaptive Controller. The 0.22 firmware has also passed a physical Mac USB test: LT and RT independently, both triggers with A and left-stick movement, and neutral output after stopping held triggers. After X1/X2 mapping in Xbox Accessories, the user also confirmed LT/RT press and release, simultaneous chair right-stick input and page changes without manual reconnect. Live gameplay and the optional physical analog circuit still need their own verification.

## Cronus Zen candidate (0.23)

An optional XInput USB build now carries adjustable LT/RT directly to a Cronus Zen candidate setup, with a separate external-chair right-stick option. The original XAC firmware remains the default and all existing XAC connection paths remain available. This route needs its own physical Zen/Xbox verification; it is not yet a proven plug-and-play release. See [connections, build choices, rollback and verification](docs/CRONUS_ZEN.md).

## Beginner help and companion apps

Both companion apps include an illustrated first-run guide and **Support → Learn / PC / Console**. See the [plain-text connection FAQ](https://github.com/kortorder/simplecontroller-ios/blob/main/FAQ.txt) and the [instructional videos](https://github.com/kortorder/simplecontroller-ios/releases). For Windows or Mac play, the apps use the free PC receiver software instead of a Pico; no bridge hardware is required. Windows provides a virtual Xbox controller; Mac translates controls to keyboard/mouse and does not provide analog gamepad output. [Windows and Mac receiver setup](https://github.com/kortorder/simplecontroller-ios/blob/main/receiver/README.md).

This XAC firmware does not connect directly to a PS5 or Sony Access controller. Experimental Zen/XInput source remains available, but a complete PlayStation route is not physically verified. Read the FAQ before purchasing hardware for that purpose.

## Choose the phone and build

For a phone Pico in the XAC's LEFT USB port and a chair Pico in RIGHT, the optional
[0.24 chair relay](docs/CHAIR_RIGHT_STICK_RELAY.md) lets either device supply the
right stick. It needs compatible firmware on both boards and the chair's Wi-Fi
address. This addition preserves the original XAC and Zen build options; the
two-board route passed real Mac USB and live phone/chair handoff checks. Complete combined Xbox acceptance remains a separate hardware check.

- **Android.** The community preview includes an installable APK and instructions. Select **Pico W / Xbox Adaptive Controller (SCX2)** in its output settings. Windows / Mac receiver, legacy ConsoleBridge and offline Practice are separate options. [Download the Android companion](https://github.com/kortorder/simplecontroller-android/releases).
- **iPhone.** [SimpleController for iPhone](https://github.com/kortorder/simplecontroller-ios) can connect to this bridge. USB trigger mode needs the matching 0.22 or later app and this firmware. This firmware repository contains no iOS app or App Store installation. An Android APK cannot be installed on an iPhone.
- **USB joystick.** Start with `--trigger-output none`. It handles two sticks, a D-pad, ten gamepad buttons and USB neutral on stop. New format 4 adds **full press/release LT and RT** using the spare X1/X2 button inputs. Map X1 to LT and X2 to RT in Xbox Accessories. Every positive trigger value becomes fully pressed; saved percentages remain intact. This needs no added circuit. USB does not carry proportional trigger pressure, Guide, Share, keyboard or mouse.
- **Adjustable LT/RT.** The optional `mcp4251` build supports pressure only with the separate supervised trigger circuit. A bare Pico has no proportional output for the XAC jacks. Build and verify the circuit on a simulated load before connecting it. [Analog parts, wiring and bench checks](docs/ANALOG_TRIGGER_HARDWARE.md). [Builder’s purchasing list](docs/Adjustable-trigger-shopping-list.txt).

## From source to first input

1. Use a Pico W, a Micro-USB **data** cable, a computer and a trusted 2.4 GHz WPA2 network. The phone and Pico must be able to reach each other on the LAN. Guest isolation or a VPN can prevent discovery. SCX2 session tokens reject stale traffic but do not authenticate network peers. Keep port 42736 off the public Internet.
2. Follow the [Mac build and local provisioning guide](bridge/firmware/README.md). It explains how to check the USB identity/power settings, privately enter Wi-Fi details and build the UF2. A privately configured UF2 contains the network credentials.
3. Disconnect the Pico from the XAC. Hold BOOTSEL while plugging the board into the computer. Release BOOTSEL when **RPI-RP2** appears. Copy only the privately configured **pico_w** UF2 to that drive. The Pico restarts. The device should appear as **SCX2 USB triggers 0.22**. If it stays in BOOTSEL, check the data cable and the file you copied. [Official Pico documentation](https://www.raspberrypi.com/documentation/microcontrollers/raspberry-pi-pico.html).
4. Leave the phone disconnected while running `python3 bridge/tools/discover.py` on a Mac on the same LAN. The helper sends a discovery HELLO, confirms the response by unicast and prints the board's IPv4 address. It sends no controller STATE. If needed, specify the LAN interface with `--interface` or consult the router's DHCP list.
5. Enter that IPv4 address and port **42736** in SimpleController. Select SCX2 and **USB full press/release** trigger mode in a matching 0.22 or later companion, allow local-network access and tap **Connect**. Select **Off** if you do not want trigger buttons. Do not select adjustable/analog mode without the complete verified analog circuit. A completed neutral USB transfer must precede live input. A USB host that never polls the joystick can prevent readiness.
6. On the computer, use a USB gamepad input viewer to observe the actual device. Check one A press and release, stick movement and center, then hold A and select **Release all**. Confirm the host returns to neutral. The app keeps the bridge connected across page/profile changes while releasing their old inputs. Explicit Disconnect ends the connection. Interrupting Wi-Fi should expire the lease and return the host to neutral.
7. For an XAC test, use the same side port selected in the build and check the current power arrangement. This image requests **500 mA**. Microsoft's specification allows **100 mA** per side port without the XAC rear DC supply and **500 mA** with it. Lowering the USB declaration does not lower the Pico's actual current. Use a suitable XAC supply for this configuration, one USB power source and current XAC firmware. In Xbox Accessories, map the phone Pico’s X1 to LT and X2 to RT. On the left side port these are Joy1/Joy2; on the right side port Joy7/Joy8. Verify both triggers separately and together, release and page changes before trying a game. [Microsoft XAC specification, §§10–10.2](https://assets.xboxservices.com/assets/b8/05/b8055a56-827c-48f8-b8ec-2c7aa66ef069.pdf?n=xbox-adaptive-controller-input-device-specification_1.6_Finalpdf.pdf).

## Source and verification

The receiver and HID encoder are in `bridge/src` and `bridge/include`. The board adapter and build wrapper are in `bridge/firmware`. The [wire contract](bridge/PROTOCOL.md) and [test vectors](bridge/fixtures/) document legacy format 2, proportional format 3 and USB trigger-button format 4. The [host checks](docs/TESTING.md) run without a Pico or Xbox.

Source and dependency notices are in [NOTICES.md](NOTICES.md).

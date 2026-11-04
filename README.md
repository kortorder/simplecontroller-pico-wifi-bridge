# SimpleController Wi-Fi Pico bridge

Use a Raspberry Pi **Pico W (RP2040)** as a USB joystick for SimpleController. The phone sends controls over the local Wi-Fi network to the Pico. The Pico connects by USB to a computer for testing, or to a side USB port on an Xbox Adaptive Controller (XAC). The XAC connects to the Xbox. The bridge uses SCX2 on UDP port **42736**.

This repository contains source and build instructions. Build your own image with your local network settings. A USB-only Pico W has been tested with an iPhone and a Mac USB host. The Mac observed button presses and releases, stick movement, centering and neutral output after a stop. XAC recognition, Xbox gameplay and the optional physical analog circuit still need verification.

## Choose the phone and build

- **Android.** The community preview includes an installable APK and instructions. Select **Pico W / Xbox Adaptive Controller (SCX2)** in its output settings. Windows receiver, legacy ConsoleBridge and offline Practice are separate options. [Download the Android companion](https://github.com/kortorder/simplecontroller-android/releases/tag/v0.17.0-preview1).
- **iPhone.** An installed SimpleController build with SCX2 support can connect to this bridge. This firmware repository contains no iOS app or App Store installation. An Android APK cannot be installed on an iPhone.
- **USB joystick.** Start with `--trigger-output none`. It handles two sticks, a D-pad, ten gamepad buttons and USB neutral on stop. The USB report carries no LT/RT pressure, Guide, Share, keyboard or mouse.
- **Adjustable LT/RT.** The optional `mcp4251` build supports pressure only with the separate supervised trigger circuit. A bare Pico has no proportional output for the XAC jacks. Build and verify the circuit on a simulated load before connecting it. [Analog parts, wiring and bench checks](docs/ANALOG_TRIGGER_HARDWARE.md).

## From source to first input

1. Use a Pico W, a Micro-USB **data** cable, a computer and a trusted 2.4 GHz WPA2 network. The phone and Pico must be able to reach each other on the LAN. Guest isolation or a VPN can prevent discovery. SCX2 session tokens reject stale traffic but do not authenticate network peers. Keep port 42736 off the public Internet.
2. Follow the [Mac build and local provisioning guide](bridge/firmware/README.md). It explains how to check the USB identity/power settings, privately enter Wi-Fi details and build the UF2. A privately configured UF2 contains the network credentials.
3. Disconnect the Pico from the XAC. Hold BOOTSEL while plugging the board into the computer. Release BOOTSEL when **RPI-RP2** appears. Copy only the privately configured **pico_w** UF2 to that drive. The Pico restarts. The device should appear as **SCX2 unverified XAC bridge**. If it stays in BOOTSEL, check the data cable and the file you copied. [Official Pico documentation](https://www.raspberrypi.com/documentation/microcontrollers/raspberry-pi-pico.html).
4. Leave the phone disconnected while running `python3 bridge/tools/discover.py` on a Mac on the same LAN. The helper sends a discovery HELLO, confirms the response by unicast and prints the board's IPv4 address. It sends no controller STATE. If needed, specify the LAN interface with `--interface` or consult the router's DHCP list.
5. Enter that IPv4 address and port **42736** in SimpleController. Select SCX2, leave **Adjustable LT and RT** off for the USB-only build, allow local-network access and tap **Connect**. A completed neutral USB transfer must precede live input. A USB host that never polls the joystick can prevent readiness.
6. On the computer, use a USB gamepad input viewer to observe the actual device. Check one A press and release, stick movement and center, then hold A and select **Release all**. Confirm the host returns to neutral. Reconnect explicitly after a stop. Interrupting Wi-Fi should expire the lease and return the host to neutral.
7. For an XAC test, use the same side port selected in the build and check the current power arrangement. This image requests **500 mA**. Microsoft's specification allows **100 mA** per side port without the XAC rear DC supply and **500 mA** with it. Lowering the USB declaration does not lower the Pico's actual current. Use a suitable XAC supply for this configuration, one USB power source and current XAC firmware. Verify the controls in Xbox Accessories before trying a game. [Microsoft XAC specification, §§10–10.2](https://assets.xboxservices.com/assets/b8/05/b8055a56-827c-48f8-b8ec-2c7aa66ef069.pdf?n=xbox-adaptive-controller-input-device-specification_1.6_Finalpdf.pdf).

## Source and verification

The receiver and HID encoder are in `bridge/src` and `bridge/include`. The board adapter and build wrapper are in `bridge/firmware`. The [wire contract](bridge/PROTOCOL.md) and [test vectors](bridge/fixtures/) document format 2 and the separately negotiated proportional format 3. The [host checks](docs/TESTING.md) run without a Pico or Xbox.

Source and dependency notices are in [NOTICES.md](NOTICES.md).

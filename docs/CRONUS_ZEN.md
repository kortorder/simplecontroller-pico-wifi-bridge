# Cronus Zen USB candidate, version 0.23

This adds a separate **XInput USB output** to the phone Pico. The existing Xbox Adaptive Controller (XAC) HID output remains the default and is still buildable. The XAC's USB-button triggers, optional MCP4251 analog circuit, original button mappings, and left/right USB-port mapping remain available.

**Status:** software and cross-build checks only until a specific Pico image has passed the Mac USB bench and Cronus Zen/Xbox tests. The app's profiles do not need to be rebuilt. No Cronus compatibility or Xbox authentication is inferred merely from a successful compile.

## Connections to verify

1. Phone → local Wi-Fi → phone Pico with the XInput firmware → **Zen A3**.
2. A genuine matching Xbox controller stays in **Zen A1** for the Zen's documented Xbox authentication setup. Zen's console USB connects to the Xbox.
3. Chair Pico stays in the XAC's **right USB port**; XAC remains connected to Xbox with its existing compatible rear supply and Xbox USB connection.
4. Use Xbox **Controller Assist** to combine the XAC and Zen into the same player's controls. This combination requires a physical test; it does not assume that Zen merges an XAC attached inside the Zen.

The new firmware does not authenticate directly to an Xbox console. It is intended as the input to a console adapter. It does not require a trigger circuit or GPIO wiring.

Official references: [Cronus USB crossover setup](https://guide.cronuszen.com/quick-start-and-downloads/step-3-controller-compatibility/usb), [Cronus controller compatibility](https://guide.cronuszen.com/supported-devices/controller), [Xbox Controller Assist](https://support.xbox.com/en-US/help/account-profile/accessibility/copilot).

## Build modes and rollback

Continue to use `build_private.py` with the existing pinned SDK/tools and private Wi-Fi configuration. It never flashes a board.

| Purpose | Additional arguments |
| --- | --- |
| Existing XAC USB controls and binary LT/RT | `--usb-output xac --trigger-output none` (defaults) |
| Existing XAC supervised analog trigger circuit | `--usb-output xac --trigger-output mcp4251` |
| Zen candidate, phone controls both sticks | `--usb-output xinput --xinput-right-stick phone --trigger-output none` |
| Zen candidate with separate chair right stick | `--usb-output xinput --xinput-right-stick external --trigger-output none` |

The external right-stick option sets only phone **RX/RY output to neutral** in the XInput encoder. Phone LT/RT, left stick, D-pad, face buttons, bumpers, Back/Menu and both stick-click buttons retain their mappings. The original phone right-stick values remain in the profile and protocol state. This avoids camera conflict with the chair when Controller Assist combines sources. The XAC firmware retains its original port-priority behavior.

The build refuses mixed XInput/MCP4251 configuration and refuses an external-right-stick flag with XAC HID. A separately retained XAC image is the rollback path; no current app profile or Xbox Accessories mapping needs to be removed. Switching back requires installing that XAC image and choosing the existing XAC trigger mode in the app.

### USB identity

USB VID/PID remain explicit private configuration fields. A Zen bench may require the widely used Xbox 360 compatibility identity `045e:028e`, as used by the referenced open-source XInput implementation. This is **protocol compatibility emulation**, not a claim that the Pico was manufactured or licensed by Microsoft. The product/manufacturer strings remain SimpleController's, and the genuine controller in Zen A1 supplies console authentication. Keep the XAC image's original identity/configuration unchanged. Do not distribute a Wi-Fi-configured private UF2.

The XInput configuration exposes one gamepad interface (class/subclass/protocol `ff/5d/01`), IN endpoint `81` and OUT endpoint `02`, with the standard 20-byte input report. It advertises the XUSB10 compatible ID. It drains LED/rumble OUT writes but does not provide vibration, headset audio or a counterfeit authentication response. A host that requires additional interfaces/requests may reject this candidate; test before relying on it.

## App and protocol

The discovery helper accepts `--usb-output xinput` to probe format5 without sending or arming any controller state. Its default remains the existing XAC format2 probe.

Select the new Cronus Zen adjustable USB trigger mode in a matching app. SCX2 **format 5**, HELLO/READY capability **4**, carries the same 80-byte state shape as proportional format 3. LT/RT retain independent unsigned 16-bit values through the Wi-Fi connection, then round to the nearest 8-bit XInput trigger value (0–255). For example, 50% becomes 128 and 70% becomes 179.

The XInput receiver offers only format 5. XAC firmware rejects format 5. Existing formats 2 (no triggers), 3 (external analog circuit) and 4 (full-press USB buttons) remain unchanged. There is no silent fallback that loses pressure or writes a different output route.

READY still waits for an actual neutral USB completion. Session arming, acknowledgements, STOPPED, replay protection, lease expiry, bounded output queue, failed-transfer handling and the loop watchdog remain in the output path. XInput USB suspend deliberately disconnects and reboots the Pico so a press queued before sleep cannot replay after resume or leave the endpoint busy indefinitely.

## Required hardware checks

- Verify the actual USB descriptor/report stream on a Mac-only bench, including 0%, 10%, 50%, 70% and 100% independent LT/RT, buttons, left stick and external-right-stick suppression.
- Verify neutral before READY, physical neutral on STOP and Wi-Fi lease expiry, USB suspend/unplug and reconnect without an old hold.
- Confirm Zen sees the candidate and passes its values through with a neutral/pass-through configuration. Disable scripts or mappings that alter triggers during this test.
- In Xbox Accessories, verify both triggers, button identities and all stick directions. Then combine with the chair XAC using Controller Assist and test simultaneous chair camera plus phone aim/fire.
- Verify the chair's four nudge actions are disabled separately in its existing maintenance settings, while gentle camera movement still centers correctly.
- Switch Walk/Drive/Extras and confirm that the bridge stays connected and old page actions release.

These checks are separate from the prior 0.22 XAC acceptance. The prior accepted XAC image remains available until the new route passes.

## Implementation references

XInput field layout and interface protocol were cross-checked against the MIT-licensed [GP2040-CE report/descriptor source](https://github.com/OpenStickCommunity/GP2040-CE/blob/main/headers/drivers/xinput/XInputDescriptors.h) and [driver source](https://github.com/OpenStickCommunity/GP2040-CE/blob/main/src/drivers/xinput/XInputDriver.cpp). The SCX2 receiver, report adapter and TinyUSB driver here are independently written for this bridge's ownership/acknowledgement model. GP2040-CE's [FAQ](https://gp2040-ce.info/faq/faq-general/) recommends XInput for PC games and third-party console adapters. Stock GP2040-CE is not a replacement for this Wi-Fi receiver.

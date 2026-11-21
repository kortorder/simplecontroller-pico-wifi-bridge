# Optional Wi-Fi relay for both right-stick controls

The XAC routes each side USB device's primary stick to that side. With devices
in both side ports, it ignores the secondary stick of a dual-stick device.
Therefore the phone Pico on LEFT can send RX/RY correctly while the chair Pico
on RIGHT prevents those phone axes from reaching the Xbox. This is specified on
pages15–16 of Microsoft's [XAC input specification1.6](https://assets.xboxservices.com/assets/b8/05/b8055a56-827c-48f8-b8ec-2c7aa66ef069.pdf?n=xbox-adaptive-controller-input-device-specification_1.6_Finalpdf.pdf).

Version0.24 adds an optional route: phone app → phone Pico → Wi-Fi → chair Pico
→ XAC RIGHT USB. The phone Pico still sends its original USB report to LEFT.
Profiles, mappings, left-stick controls, USB trigger buttons, and the XAC USB
backend remain available. No app/profile conversion or additional wiring is
needed. The original default firmware has no relay socket or relay traffic.

Both Picos need the compatible optional relay firmware. The chair must join
the same2.4GHz Wi-Fi as the phone Pico. Its fixed/configured LAN IPv4 is passed
to the phone firmware builder:

```sh
python3 bridge/firmware/build_private.py --mode compile-only \
  --chair-relay-address 192.0.2.20 --sdk /path/to/pinned-sdk \
  --tools /path/to/pinned-tools --output /private/tmp/new-build
```

The example address is for compilation, not a discovered device. For a private
build use the existing private configuration and the chair's actual address.
`--chair-relay-address` is optional and supports only the phone LEFT/XAC path.
XInput and the relay cannot be enabled together. The redacted build manifest
records whether the relay is enabled and its port, omitting the private address.
DHCP can change the address: reserve it at the router or rebuild the phone
firmware when the chair's wired diagnostic reports a different address. There
is no broadcast discovery or automatic selection of another person's chair.

The relay build requests `CYW43_NONE_PM` after enabling station mode so Wi-Fi
power-save buffering does not delay camera packets beyond the short lease.
This setting is confined to the optional relay build. If the SDK rejects it,
the relay stays unavailable while the phone's ordinary app connection and USB
outputs continue. Successful setup does not by itself prove reliable latency;
the two-board radio bench and Xbox checks are still required.

The chair gives a nonzero phone right stick priority. When the phone right
stick is centered, current chair motion supplies the right stick. Inputs are
not added together, avoiding cancellation or doubled camera motion. A phone
stick intentionally configured to stay in position continues to take priority
until centered or released. The existing chair nudge setting and Bluetooth
pairing must be retained by its additive update.

## Relay contract and fault behavior

- Dedicated UDP42737; SCX2 format2; no extra app-facing protocol mode.
- STATE contains only axes2/3. Buttons, hat, left stick and trigger fields are
  neutral. The chair rejects other formats and noncamera output fields.
- Each connection uses a fresh random session and the chair's boot/challenge.
  Its first STATE is neutral. Replies are accepted only from the configured
  address/port and matching session, boot, challenge and sequence.
- The phone copies only an armed upstream receiver's actual USB-completed
  state. A new upstream session or USB epoch invalidates the old source.
- Heartbeats are50ms. Missing ACK for250ms causes STOP and a fresh neutral arm.
  Old held camera input cannot resume until the phone source is seen centered.
- A neutral source update cannot be coalesced away by another press. Upstream
  retirement, suspend, radio stop and USB failure invalidate the relay and try
  to send STOP immediately. If transmission is impossible, the chair's250ms
  lease removes the phone source. Live physical-chair motion remains available.
- The ordinary phone receiver retains its1s lease. In the combined worst case
  where app packets stop and the later relay STOP is lost, camera source expiry
  is bounded by1.25s plus USB/loop service latency. Do not describe this as a1s
  end-to-end bound. USB faults retain the existing neutral/disconnect recovery.
- Relay ACK is produced by the chair only after the corresponding source
  generation is carried in an actual combined USB completion. The app's
  existing ACK still confirms the phone Pico's USB completion; its Connected
  indicator does **not** certify successful delivery to the second Pico.

Host tests cover the real codec/receiver, wrong/stale replies, release ordering,
timeout/rearm, source generations, short/default leases, and the real phone
main-loop integration. These tests and cross-builds do not establish Wi-Fi and
Omni2 Bluetooth coexistence latency or Xbox behavior. Those require the two
physical Picos and a final Xbox test. The optional Zen XInput path is preserved;
if used later with phone camera control, build `--xinput-right-stick phone`,
not the earlier exclusive-chair `external` variant.

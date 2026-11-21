# SCX2 wire contract: legacy 2, proportional 3 and USB trigger buttons 4

The Pico candidate listens on IPv4 UDP port **42736**, distinct from the existing SCX1 port 42735.

Formats 2 and 3 remain byte-for-byte compatible. Format 3 adds explicitly negotiated proportional LT/RT through a separate analog backend. Format 4 adds full press/release LT/RT through the XAC’s spare USB button inputs; it does not provide partial pressure. SCX2 is a separate binary protocol; the app also retains explicitly selected SCX1 JSON compatibility. This contract does not establish USB/Xbox compatibility or authentication. Do not expose the receiver to the Internet. Random tokens prevent stale-session reuse, not a malicious peer on the same network.

All datagrams have this 64-byte header. Multibyte integers use network (big-endian) byte order. UUIDs are the 16 bytes represented by their canonical hexadecimal spelling, ignoring hyphens; they must not be all zero. Do not serialize platform UUID struct memory or convert sequence integers through floating point.

| Bytes | Field |
| --- | --- |
| 0–3 | ASCII `SCX2` (`53 43 58 32`) |
| 4 | Format version `2` (legacy), `3` (proportional) or `4` (USB trigger buttons) |
| 5 | Kind: hello=1, ready=2, state=3, ack=4, stop=5, stopped=6 |
| 6 | Flags: bit 0 is `hidReady`; every other bit must be zero |
| 7 | Format 2: zero. Format 3 HELLO/usable READY: exactly `1`. Format 4 HELLO/usable READY: exactly `2`. Unavailable READY and every other kind: zero. Combined or mismatched capabilities are invalid. |
| 8–23 | Client session UUID |
| 24–39 | Receiver boot token, 128 bits |
| 40–55 | Offered-session challenge, 128 bits |
| 56–63 | Unsigned 64-bit sequence |

Format-2 state packets append a 12-byte body, making them exactly 76 bytes. Format-3 and format-4 state packets append that same body plus four trigger bytes, making them exactly 80 bytes. Every other kind is exactly 64 bytes. The format selects the exact length; a format-3 or format-4 state with only 76 bytes is invalid even if pressure would have been zero. Extra, missing, reserved, unsupported or out-of-range values reject the entire packet before it changes state.

| Bytes | Complete state body |
| --- | --- |
| 64–65 | Button mask: bit0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 View, 7 Menu, 8 LS click, 9 RS click; bits10–15 zero |
| 66 | HAT: 0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left, 7 up-left, 8 neutral |
| 67 | Reserved, zero |
| 68–69, 70–71 | Left X, Y, signed 16-bit two's complement |
| 72–73, 74–75 | Right X, Y, signed 16-bit two's complement |

Axes range −32767 through +32767; −32768 is invalid. Positive X is right, positive Y is up. Neutral is buttons=0, HAT=8, every axis=0. Format 2 deliberately has no triggers. None of the formats includes Guide, Share, keyboard, mouse, touchpad, layout or behavior fields. The digital button mask remains ten bits in all formats. Format-3 pressure must never be reinterpreted as a spare HID button or axis. Only the explicitly selected format-4 trigger fields map to spare USB buttons. The Swift client converts normalized axes with nearest-integer rounding and clamps only its own valid −1…1 input before encoding; a receiver rejects invalid wire values.

## Format-3 proportional pressure

The additional state fields are:

| Bytes | Field |
| --- | --- |
| 76–77 | LT, unsigned 16-bit big-endian |
| 78–79 | RT, unsigned 16-bit big-endian |

Zero is released; 65535 is full scale. The Swift client validates finite normalized `0...1` values, then rounds `value * 65535` to nearest, ties away from zero. Examples: `0.1 → 6554`, `0.5 → 32768`, `1 → 65535`. Maximum quantization error is half one step. The queue retains the full pressure-bearing snapshot; `RT:0.2 → RT:0.8 → RT:0` is three distinct output states even when all digital/stick values are unchanged. This is pressure transport, not an electrical voltage specification or a claim that XAC accepted the pressure.

Format-3 HELLO must request capability `1`. A usable format-3 READY must return capability `1`, `hidReady=true` and a nonzero challenge. If the pressure backend is absent, uninitialized, faulted or not neutral-confirmed—or USB is not ready—the response is format-3 READY with capability `0`, `hidReady=false` and zero challenge; no session is offered. There is no downgrade or binary conversion. Old receivers reject the format-3 HELLO and cannot accidentally arm it. Format-2 HELLO/READY and all eight original vectors remain unchanged.

All packets belonging to an offered session use its negotiated format. The receiver matches format as well as session/boot/challenge, so the same UUID cannot change format while live. ACK, STOP and STOPPED keep their 64-byte shape and zero capability byte in every format. A format-3 neutral first state includes both pressures equal to zero; no pressure is applied merely on receipt or before ACK1. Use [the separate format-3 vectors](fixtures/scx2-pressure-wire.json); their individual hex files are under `fixtures/pressure/`.

## Format-4 USB trigger buttons

Format 4 uses the same 80-byte STATE shape as format 3. Bytes 76–77 (LT) and 78–79 (RT) accept **only 0 or 65535**. Zero releases; 65535 presses. Any intermediate value rejects the complete packet. The app converts every positive requested pressure to 65535 when the user selects USB full press/release mode, retaining the original saved percentages for Practice or a future analog setup.

HELLO must request capability `2`. A usable READY returns capability `2`, `hidReady=true` and a nonzero challenge. An unavailable READY returns capability `0`, `hidReady=false` and zero challenge. The first STATE remains fully neutral and must complete USB transfer before ACK1. There is no silent downgrade: old firmware rejects format 4; format 3 still requests real proportional support and never substitutes digital buttons. The default `--trigger-output none` build supports formats 2 and 4 and advertises no proportional capability.

Format-4 LT and RT become the unused X1 and X2 USB button inputs. For the **left** XAC side port they are Joy1 and Joy2 (report bits 0 and 1); for the **right** port they are Joy7 and Joy8 (bits 6 and 7). The configured port must match the physical port. Map X1 to LT and X2 to RT in Xbox Accessories. The ten existing buttons, D-pad and axes keep their existing mappings. No Guide, Share, keyboard or mouse output is added. This mode needs no analog circuit and cannot deliver gentle acceleration, partial braking or partial trigger pressure.

A format-4 session does not apply analog pressure. Even with a failed optional analog backend, USB readiness depends on USB availability and confirmed neutral transfer, not proportional readiness; the failed analog output stays released. USB work stores its negotiated format through completion, GET_REPORT and idle repeats, so held LT/RT are not lost between reports. STOP, lease expiry, network loss, USB failure, queue overflow and stale callbacks follow the same neutral and token rules as the other formats.

Use [the format-4 vectors](fixtures/scx2-usb-trigger-wire.json); individual hex files are under `fixtures/usb-buttons/`. Host tests verify the bytes and USB reports. Actual Xbox Accessories mapping and console actions require hardware checks.

## Field rules

- **hello:** sequence 0, flags 0, boot/challenge zero. A client creates a fresh session UUID for each new handshake; an app may reconnect after a temporary interruption without replaying held actions.
- **ready:** sequence 0, nonzero boot. With `hidReady=true`, challenge is nonzero; with `false`, challenge is zero and no session is offered.
- **state:** both tokens nonzero, flags 0, sequence 1 through `UINT64_MAX−1`.
- **ack:** both tokens nonzero, flags 1, sequence 1 through `UINT64_MAX−1`. It confirms completion of that USB transfer; format 3 additionally requires the proportional backend to accept the matching pressure command. It never proves measured jack voltage or game/console acceptance.
- **stop / stopped:** both tokens nonzero, flags 0, sequence 1 through `UINT64_MAX`. The final value is reserved for ending an exhausted sequence space. Wrapping to zero is forbidden.

## Handshake and ownership

1. Boot starts neutral and unready. The adapter supplies a fresh, nonzero random boot token. USB mount/resume requires a neutral report to complete before ready can be true.
2. An idle, ready receiver offers a fresh random challenge for a hello. The offer expires after 1 second. Duplicate hello for that offered/live session returns the same tokens without extending its deadline; other sessions cannot take over.
3. First state must be sequence 1 and neutral. Its completed USB transfer returns ACK 1 and arms the session; format 3 additionally requires the configured proportional backend to accept zero before that ACK. Nonneutral traffic before that ACK is rejected. The client must keep its touch surface disabled until ACK 1.
4. After arming, a strictly newer state atomically replaces the desired snapshot and renews the one-second lease. UDP loss may skip sequences. Duplicate latest state may receive its already-completed ACK but never renews the lease or changes output. Older states are ignored.
5. Expiry is checked before packet processing, USB work and completion callbacks. At the deadline, the token is retired, queued states are discarded and neutral takes priority. Fresh hello/neutral confirmation is needed; old tokens cannot resume.
6. A matching newer stop retires the session immediately. Stopped is returned only after neutral USB completion. The most recent stop tuple permits harmless retries, including while another session is active. Older unrelated stop packets are ignored.

Every new offer gets a new challenge, including reuse of a client UUID. A challenge contains 8 random bytes followed by an 8-byte big-endian offer counter, incremented for every new offer and never wrapped. Thus challenges cannot repeat within one boot, even if entropy repeats. Every reboot gets a fresh random 128-bit boot token. This removes SCX1's need for an unbounded retired-session set. The embedded entropy adapter must be reviewed and tested; deterministic host-test tokens are not suitable firmware entropy. All-zero entropy or counter exhaustion faults neutral. Tokens are opaque bytes to the client.

## USB completion and bounded work

The receiver owns one in-flight report and at most 16 queued states. It does not silently coalesce a quick press/release. Queue overflow retires the session and requests neutral. USB completion tickets never wrap. Stop/expiry can invalidate an in-flight report but cannot retract bytes already on the bus; its completion produces no stale ACK, and the next report is neutral. A new offer is blocked until that neutral completes.

Endpoint-busy alone is not USB loss: leave queued work pending. Unmount, suspend or transfer failure retires the active token and disables output until the adapter reports availability and a new neutral report completes. The core does not claim it can release XAC output while unplugged, unpowered or crashed.

The host-test adapter passes monotonic microseconds. Backward time or a time too near `UINT64_MAX` to represent a complete lease faults neutral. A local watchdog must cover a stopped event loop separately. The board adapter handles UDP source-address binding, local credential provisioning, entropy and USB callbacks. Physical timing still needs measurement at the output.

See [format-2 vectors](fixtures/scx2-wire.json), [format-3 vectors](fixtures/scx2-pressure-wire.json) and [format-4 vectors](fixtures/scx2-usb-trigger-wire.json) for exact byte examples.

## Companion behavior

The companion waits for matching neutral ACK1 before enabling touch input. Changing a page or profile releases the app’s active inputs while keeping a healthy bridge session connected. When a session stops or expires, the next connection must complete a new handshake. Current companions may reconnect automatically after a short interruption; explicit Disconnect and an absence of at least five minutes require Connect again. All resumed input starts neutral. Stop is best effort, so the receiver also enforces the lease. A valid ACK confirms a completed USB transfer (plus acceptance by a negotiated proportional backend). Xbox/game actions and physical jack voltages require separate observation.

## Proportional backend and failure boundary

`TriggerOutputBackend` receives normalized 16-bit LT/RT commands, reports initialization readiness, and exposes immediate `release`. Inactive neutral confirmation uses `apply`; active output uses `apply_before` with the receiver's absolute monotonic-microsecond lease deadline. The latter returns `accepted`, `expired` or `fault`. An expired operation releases and retires the session without ACK or a sticky hardware fault; a fresh neutral handshake can reconnect. Calls must be bounded, nonblocking and non-reentrant; the backend must outlive the receiver. A real implementation must enforce the chosen circuit’s calibrated electrical limits and default to released during reset, Hi-Z and power loss. A dry-contact binary switch is not a proportional backend.

The receiver accepts zero before advertising proportional readiness and again before ACK1. Observing backend unavailability invalidates any prior neutral confirmation, including while idle, so reinitialization requires another successful zero command. Only a successfully completed, still-current USB ticket may apply nonzero pressure. It checks lease, format, session, boot, challenge and USB availability first. A backend rejection returns no ACK, latches a trigger-output fault, and releases immediately. That fault requires receiver reconstruction/reset; returning `ready=true` does not silently rearm it.

Stop, lease expiry, queue overflow, invalid time, USB loss/failure and backend readiness loss call `release` immediately, independently of queued/in-flight USB neutral reports. The receiver’s `accepted_trigger_output()` becomes zero. `completed_output()` may still describe a stale USB transfer and **must never be used as a GPIO/analog source**. A stale completion cannot apply pressure or ACK. STOPPED still waits for USB neutral; backend acceptance is command feedback, not physical contact/voltage feedback. CPU stalls and power faults additionally require the board watchdog and the real circuit’s released default; no physical timing bound is claimed here.

The default backend is absent. The default USB-only firmware therefore advertises **no proportional capability** and has no enabled trigger GPIOs. The explicitly enabled MCP4251 candidate uses supervised, isolated output gates; its driver checks the deadline before SPI transactions and immediately before enabling those gates. It does not cycle gates on unchanged held-pressure heartbeats. `UINT64_MAX` is an explicit unbounded sentinel for inactive neutral work, with no deadline arithmetic. The dedicated `bash bridge/run-mcp4251-checks.sh` suite passes 96 fake-I/O checks, including a lease crossing during successful SPI that cannot enable output or ACK and still permits a new neutral handshake. Actual failed transfers or readback mismatches remain hardware faults even when they also cross the deadline. See the [analog hardware guide](../docs/ANALOG_TRIGGER_HARDWARE.md) for the exact opt-in circuit and measurement gates. Neither driver readback nor a protocol ACK measures jack voltage, relay timing or XAC behavior. Physical proportional LT/RT remains a separate hardware acceptance step.


## Format 5: proportional triggers through XInput USB (0.23 candidate)

`format = 5`, HELLO/usable READY capability byte `4`. Other control packets have capability zero. STATE is exactly 80 bytes with the same big-endian fields as format3: LT at bytes76–77, RT at78–79, each0..65535. Format5 is exclusive to the XInput USB backend; XAC HID/circuit backends return unavailable for it. XInput returns unavailable for formats2/3/4 rather than reinterpret their outputs. No other format or saved profile encoding changes.

The existing receipt, neutral-first arming, completion acknowledgement, STOP, lease and stale-session rules apply unchanged. Final USB output is a20-byte XInput report with LT/RT rounded to0..255, four signed16-bit axes and the same logical buttons/hat. An external-right-stick build suppresses only RX/RY at the final encoder. All-neutral work retains its normal receiver/barrier semantics regardless of its stored format. Shared fixtures are `fixtures/scx2-usb-pressure-wire.json`.

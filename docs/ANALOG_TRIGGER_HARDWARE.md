# Optional proportional LT and RT prototype

The default USB firmware leaves trigger GPIOs unused. The separate format-3 build drives a supervised MCP4251 circuit connected to the XAC LT and RT jacks. This circuit requires assembly and measurement by an electronics builder. Physical operation on XAC is unverified. Keep the trigger leads disconnected until the simulated-load checks below pass.

## Parts for two channels

| Quantity | Component |
| --- | --- |
| 1 | Microchip MCP4251-103E/P, DIP14 dual 10 kΩ |
| 2 | Panasonic AQW212EH, DIP8 dual normally-open PhotoMOS |
| 1 each | MCP120-300DI/TO (D bondout), SN74HC08N (DIP14), onsemi 2N3904BU and 1N5817G |
| 1 | 100 µF polar capacitor rated ≥10 V and three 100 nF ceramic bypass capacitors |
| Resistors | Four 220 Ω, one 100 Ω, one 1 kΩ, four 10 kΩ and two 100 kΩ; 1%, ¼ W |
| 2 | Four-conductor 3.5 mm TRRS plugs with accessible contacts, such as Adafruit #2914 |
| Bench | Pico W with headers, data cable, simulated load, meter and oscilloscope |
| Final assembly | Insulated board, enclosure, strain relief and reviewed XAC accessory power |

Microsoft specifies 100 mA per side USB port without the XAC rear DC supply and 500 mA with the supply. The build requests 500 mA. Measure actual current with Wi-Fi and all four relay LEDs running. [Microsoft XAC specification, §10.1](https://assets.xboxservices.com/assets/b8/05/b8055a56-827c-48f8-b8ec-2c7aa66ef069.pdf?n=xbox-adaptive-controller-input-device-specification_1.6_Finalpdf.pdf)

## XAC connector mapping

For each LT/RT jack: **tip = input**, **ring 1 = unused**, **ring 2 = return through the XAC's internal 1 kΩ**, **sleeve = 1.8 V reference through its internal 1 kΩ**. The input has a 100 kΩ pull-down. Microsoft specifies a nominal 10 kΩ potentiometer (±3 kΩ), with its wiper feeding tip, and a maximum 1.8 V operating input. The normal potentiometer span is approximately 0.15–1.65 V. [Microsoft input-device specification, §§9.2–9.3](https://assets.xboxservices.com/assets/b8/05/b8055a56-827c-48f8-b8ec-2c7aa66ef069.pdf?n=xbox-adaptive-controller-input-device-specification_1.6_Finalpdf.pdf)

**Do not connect the XAC sleeve to Pico ground, 3V3 or 5V. Do not connect ring 2 directly to Pico ground.** The Adafruit adapter's audio label **GND actually denotes sleeve**; relabel it `XAC REF`. Its other labels map `blank → ring 2`, `Right → ring 1`, `Left → tip`; verify every plug with a continuity meter before wiring. [Adapter labeling](https://www.adafruit.com/product/2914)

During XAC operation, the Pico's USB ground must be the same system ground as the XAC. Do not power it from an unrelated computer/supply while connecting the trigger leads without a reviewed grounding arrangement. Bench testing initially uses a separate simulated XAC circuit, not the real XAC.

## Wiring

Use two named logic rails: `RAW_3V3` is Pico pin 36; `HELD_3V3` exists only after the diode/resistor below. Logic ground is Pico GND, for example physical pin 38. DIP pins are numbered from the package notch/dot, viewed from above.

### Power, supervisor and relay enable

1. `RAW_3V3 → 1N5817 anode`; diode's banded cathode → 100 Ω → `HELD_3V3`. Put 100 µF positive on `HELD_3V3`, negative on logic ground. Add the potentiometer's 100 nF there. The resistor bounds initial charging current to under 33 mA at 3.3 V, before diode drop.
2. MCP120 **D bondout**, flat face towards you/leads down: left `/RESET`, middle VDD=`RAW_3V3`, right VSS=ground. Add 100 nF across its supply. Pull `/RESET` up with 10 kΩ to `RAW_3V3`; this node is `POWER_GOOD` and connects to **GP21 / physical pin 27**. Do not substitute MCP130 or a different TO-92 bondout without changing the wiring. It trips between 2.85 and 3.00 V and can delay recovery up to 700 ms. [MCP120 datasheet, pp.1–2](https://ww1.microchip.com/downloads/en/DeviceDoc/11184d.pdf)
3. SN74HC08N: pin 14=`RAW_3V3`, pin 7=ground, 100 nF across them; pin 1=**GP20 / physical pin 26**, with 100 kΩ to ground; pin 2=`POWER_GOOD`. Pin 3 drives the transistor base through 1 kΩ. Tie unused inputs 4,5,9,10,12,13 to ground; leave unused outputs 6,8,11 unconnected. [TI pinout/datasheet](https://www.ti.com/lit/ds/symlink/sn74hc08.pdf)
4. onsemi 2N3904BU: pin 1 emitter to ground, pin 3 collector to all four PhotoMOS LED cathodes; pin 2 base has 100 kΩ to emitter. Confirm package orientation against the supplied manufacturer's drawing. Each LED anode receives `RAW_3V3` through its own 220 Ω. This AND gate makes hardware enable require both software request and healthy supply. [Transistor datasheet](https://www.onsemi.com/pdf/datasheet/2n3904-d.pdf)

PhotoMOS package U2 serves LT and U3 serves RT. In each package, LED anodes are pins 1 and 3; cathodes are pins 2 and 4. The corresponding isolated contact pairs are **8–7** and **6–5**. Use 8–7 for sleeve-to-pot-A, 6–5 for pot-W-to-tip. Contacts are bidirectional. The selected part has maximum 4 ms turn-on and 1 ms turn-off at the specified test conditions. [Panasonic circuit and specifications](https://industry.panasonic.com/ac/cdn/e/control/relay/photomos/catalog/semi_eng_ge2a_aqw21_e.pdf)

### Potentiometer and SPI

| MCP4251 DIP14 pin | Connection |
| --- | --- |
| 1 /CS | GP17 / Pico physical 22; 10 kΩ pull-up to `HELD_3V3` |
| 2 SCK | GP18 / physical 24 |
| 3 SDI | GP19 / physical 25 |
| 4 VSS | Logic ground |
| 5 P1B | RT ring 2 only |
| 6 P1W | U3 pin 6; U3 pin 5 → RT tip |
| 7 P1A | U3 pin 7; U3 pin 8 → RT sleeve |
| 8 P0A | U2 pin 7; U2 pin 8 → LT sleeve |
| 9 P0W | U2 pin 6; U2 pin 5 → LT tip |
| 10 P0B | LT ring 2 only |
| 11 NC | Unconnected |
| 12 /SHDN | 10 kΩ to `HELD_3V3` |
| 13 SDO | Through 10 kΩ to GP16 / physical 21 |
| 14 VDD | `HELD_3V3` |

Both ring-1 contacts stay unconnected. MCP4251 has 257 wiper positions and starts at midscale; it is not inherently fail-neutral. Its analog terminals must remain between VSS and VDD in operation; the absolute limit VDD+0.3 V is not a permissible design target. Supply it before connecting either XAC reference. [Microchip datasheet, pp.2–5,9,31](https://ww1.microchip.com/downloads/en/DeviceDoc/22060b.pdf)

The 10 kΩ SDO resistor limits transient injection into an unpowered Pico; it does **not** provide galvanic isolation or eliminate back-power. Start SPI slowly (for example 100 kHz), and verify read timing with the installed wiring. CS/SCK/SDI are high-voltage-tolerant inputs, but the held supply's weak pull-ups and CS pull-up still contribute to discharge/loading during power loss.

## Firmware agreement and bench gates

Reserved GPIOs are **16–21** as above. No additional /SHDN GPIO is required. The pressure backend must remain an explicit build option until the assembled circuit passes review.

At boot/reset keep GP20 low. Wait at least one second, require GP21 high, initialize both wipers to zero and TCON to the intended all-connected state, then read back both wipers and TCON. A healthy readback is necessary but cannot prove physical contacts closed or trigger voltage correct. Enable only for a valid current session after successful checks. Program both pressure values before enabling the shared relay bank. If one trigger is zero while the other is active, its zero wiper remains connected and must measure unpressed.

Release all/STOP/session expiry/disconnection/SPI verification failure must lower GP20 and clear pressure state. A POWER_GOOD falling edge must latch a fault, not merely resume an old request when the supply recovers; require fresh initialization/session. The firmware falling-edge interrupt immediately requests GP20 low and retains the fault; gate enabling also checks a pending edge to avoid a check/write race. This software latch does not certify release during an interrupt-delayed or stalled CPU. Hardware supervisor/AND gating, watchdog coverage and the held rail/relay timing still require oscilloscope proof. Hardware supervisor plus AND gate can open contacts independently of the CPU. The firmware uses a watchdog to reset the CPU on a stall. Do not silently resume held pressure after reset.

**Unresolved electrical validation is material:** capacitor sizing and the four-LED driver have not been measured. At 2.85 V, assuming a 1.5 V LED and 0.2 V saturated transistor, 220 Ω gives about 5.2 mA; four channels can approach 40 mA at normal/high supply. This assumes adequate transistor/logic drive. Measure all four LED currents and transistor voltage at the lowest allowed supply; do not infer saturation from the calculation. Reassess the existing USB power budget with Wi-Fi and this added load.

The hold-up capacitor must keep the potentiometer rail above every still-connected analog terminal until both reference contacts open, on abrupt unplug, slow brownout and reset. A nominal 100 µF at 1 mA loses only 10 mV/ms, but diode leakage, GPIO paths, capacitor tolerance and actual load change that. The supervisor's falling-edge delay is specified typically, not as a guaranteed maximum; the relay timing also has test conditions. **An oscilloscope measurement is required before this design is called power-loss safe.** If the margin fails, revise the power/isolation circuit; a current-limiting resistor alone does not make an out-of-rail analog pin compliant. [Diode characteristics](https://www.onsemi.com/download/data-sheet/pdf/1n5817-d.pdf)

The helper's initial test load should reproduce each XAC channel using a regulated 1.8 V reference, two 1 kΩ series resistors and a 100 kΩ tip pull-down. Check neutral, independent 10–100% steps, full scale, both channels together, missing SPI device, loss of packets, reset, brownout, unplug and reconnection. Confirm tip never exceeds 1.8 V and neutral returns without software. Then test on the XAC using its calibration behavior; do not automatically sweep to full pressure at startup. Numeric pressure is a requested normalized value, not proof of exact mechanical/game response.

Opening the relay bank on release introduces turn-on/off latency; extremely short pulses may not reproduce the app's timing. Measure the minimum reliable pulse and document it. Existing switches on other XAC jacks can remain, but **do not Y-split another trigger source onto these LT/RT outputs**. If an existing switch must share LT or RT, an explicit electrical override/merge design is additional work; it is not supplied by this adapter.

Digital potentiometers were selected because their output follows the XAC's own reference. A bare 3.3 V DAC or PWM pin is not a substitute: it exceeds the allowed input and would require a separately reviewed limit/filter/disconnect circuit. This design still does not prove Xbox recognition, mapping, game behavior or pulse fidelity; those remain physical acceptance tests after assembly.

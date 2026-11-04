# Verify the source

The host checks use Python 3 and Clang with address/undefined-behavior sanitizers. Run from the repository root on macOS. The discovery and configuration-form checks bind only to local loopback and use synthetic data. These commands do not flash a board or send traffic to one.

```sh
set -e
export PYTHONDONTWRITEBYTECODE=1
bash bridge/run-checks.sh
bash bridge/run-listener-checks.sh
bash bridge/run-radio-startup-checks.sh
bash bridge/run-mcp4251-checks.sh
python3 bridge/tests/private_build_checks.py
python3 bridge/tests/configure_wifi_checks.py
bash bridge/run-discovery-checks.sh
```

| Suite | Result from this standalone source |
| --- | --- |
| SCX2 codec, receiver, USB lifecycle and HID | 59 checks |
| Proportional transport and fake backend | 50 checks |
| UDP listener retry | 8 checks |
| Startup, live stall and retirement stall | 18 adapter assertions in each of two scenarios |
| MCP4251 SPI/lease and gate/IRQ shim | 96 driver and 12 adapter checks |
| Build validation and local Wi-Fi form | 14 tests each |
| Discovery | 10 tests, including local UDP receiver exchange |

The pinned Apple Silicon toolchain also cross-built the RP2040 USB-only and MCP4251 variants from the public source with synthetic compile-only configuration. The tests can inspect a requested pressure or simulated hardware operation. Actual pressure at the XAC jack, XAC button mapping and live Xbox gameplay need their own physical checks.

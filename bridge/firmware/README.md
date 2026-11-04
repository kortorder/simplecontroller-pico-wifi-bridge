# Build a Pico W image on a Mac

The verified wrapper targets Apple Silicon macOS, with Python 3.8+, Git, command-line build tools and the pinned ARM toolchain. Run these commands from this repository's root. Store external dependencies outside the repository. The wrapper verifies the archives, executables, SDK and submodule revisions before compiling. It never flashes hardware.

## 1. Prepare the pinned SDK and tools

Choose a new local directory. Obtain the SDK from its official repository and check out the exact revision. Use the original board target `pico_w` for a Pico W marked RP2040.

```sh
export SC_DEPS="$HOME/simplecontroller-pico-deps"
mkdir -p "$SC_DEPS/tools"
git clone --no-checkout https://github.com/raspberrypi/pico-sdk.git "$SC_DEPS/sdk"
git -C "$SC_DEPS/sdk" checkout --detach 079c6f39023649b154152db30f1d781e884879bc
git -C "$SC_DEPS/sdk" submodule update --init lib/tinyusb lib/lwip lib/cyw43-driver
```

The four official tool archives and SHA-256 values are in [build-manifest.json](build-manifest.json). The following command downloads missing archives to the external tools directory and checks each file before extraction. Keep the archives there because every build checks them again. No tools are installed system-wide.

```sh
python3 - <<'PY'
from hashlib import sha256
from pathlib import Path
from urllib.request import urlopen
import json, os, shutil
tools = Path(os.environ["SC_DEPS"]) / "tools"
for entry in json.loads(Path("bridge/firmware/build-manifest.json").read_text())["tools"]:
    archive = tools / entry["name"]
    if not archive.exists():
        part = archive.with_name(archive.name + ".part")
        with urlopen(entry["url"], timeout=120) as src, part.open("wb") as dst:
            shutil.copyfileobj(src, dst)
        if sha256(part.read_bytes()).hexdigest() != entry["sha256"]:
            raise SystemExit("Archive checksum failed: " + archive.name)
        part.replace(archive)
    if sha256(archive.read_bytes()).hexdigest() != entry["sha256"]:
        raise SystemExit("Archive checksum failed: " + archive.name)
    print("Verified", archive.name)
PY
```

After all four checks succeed, extract the tools into the layout expected by the wrapper.

```sh
(
  set -eu
  cd "$SC_DEPS/tools"
  tar -xf cmake-4.3.4-macos-universal.tar.gz
  tar -xf arm-gnu-toolchain-15.2.rel1-darwin-arm64-arm-none-eabi.tar.xz
  mkdir -p ninja picotool
  unzip -oq ninja-mac.zip -d ninja
  unzip -oq picotool-2.3.1-mac.zip -d picotool
)
```

## 2. Verify or compile without Wi-Fi credentials

```sh
python3 bridge/firmware/build_private.py --sdk "$SC_DEPS/sdk" --tools "$SC_DEPS/tools" \
  --board pico_w --mode compile-only --trigger-output none --validate-only
```

Remove `--validate-only` to build the USB-only candidate. Select `--trigger-output mcp4251` to cross-build the optional analog candidate. Compile-only images use placeholder network information and USB identity. Do not flash or distribute them.

## 3. Prepare one private USB-only image

1. Create an owner-only configuration using the public example. These instructions refuse to overwrite an existing local file.

   ```sh
   (set -C; umask 077; cat bridge/firmware/config.example.json > bridge/firmware/local_config.json)
   ```

2. In the local JSON, set the USB vendor/product IDs you may legitimately use, the reviewed power declaration and the XAC port (`left` or `right`). Set `identity_reviewed` and `power_reviewed` to true only after checking those details. For a first computer bench, use a USB port that supports the declared 500 mA. For XAC, review its rear DC supply before plugging into either side USB port.

   For an **in-house prototype** the allocator allows the shared pair `0x1209` / `0x0001`. Check there is no other device with the same pair on the test host. A distributed/sold/manufactured device needs an appropriate assigned identity. [Allocator's test-ID terms](https://pid.codes/1209/0001/).
3. Run the local setup form and open the random loopback URL it prints. Enter the **2.4 GHz WPA2** network name, passphrase and actual regulatory country. Saving the form updates only the Wi-Fi fields.

   ```sh
   python3 bridge/tools/configure_wifi.py --config bridge/firmware/local_config.json
   ```

4. Build locally. Both the private configuration and the UF2 can contain Wi-Fi credentials. Keep the private build directory outside the repository.

   ```sh
   python3 bridge/firmware/build_private.py --sdk "$SC_DEPS/sdk" --tools "$SC_DEPS/tools" \
     --board pico_w --mode private --trigger-output none --config bridge/firmware/local_config.json
   ```

5. The command prints a new private build directory. Its `build/simplecontroller_bridge.uf2` is the file for the **Mac-first** BOOTSEL test. Check the redacted manifest reports `pico_w`, `rp2040`, `private` and `trigger_output: none`. Keep the UF2, ELF, private header and build log local. Return to the [USB setup steps](../../README.md#from-source-to-first-input).

The optional pressure image needs the complete [trigger interface](../../docs/ANALOG_TRIGGER_HARDWARE.md), including independent power-fault protection and simulated-load validation.

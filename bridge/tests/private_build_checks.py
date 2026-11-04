#!/usr/bin/env python3
"""No credentials, SDK execution or network: tests the private build's validation boundary."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

MODULE_PATH = Path(__file__).resolve().parents[1] / "firmware/build_private.py"
spec = importlib.util.spec_from_file_location("private_build", MODULE_PATH)
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

class PrivateBuildChecks(unittest.TestCase):
    def config(self):
        return {"wifi": {"ssid": "LOCAL TEST ONLY", "password": "fake-secret-value", "country": "USA"},
                "usb": {"vid": "0x1234", "pid": "0x5678", "power_ma": 500, "identity_reviewed": True, "power_reviewed": True}, "xac_port": "left"}
    def test_valid_local_config_and_redaction(self):
        config = build.config_values(self.config(), {"USA"})
        redacted = json.dumps(build.redacted_config(config, "private"))
        self.assertNotIn(config["wifi"]["password"], redacted)
        self.assertNotIn(config["wifi"]["ssid"], redacted)
    def test_unknown_keys_rejected(self):
        config = self.config(); config["flash"] = True
        self.assertRaises(build.BuildError, build.config_values, config, {"USA"})
    def test_placeholder_and_unreviewed_identity_rejected(self):
        for key,value in [("vid","0xffff"),("pid","0x0000"),("identity_reviewed",False),("power_reviewed",False)]:
            config=self.config();config["usb"][key]=value
            self.assertRaises(build.BuildError,build.config_values,config,{"USA"})
    def test_power_never_inferred_from_smaller_number(self):
        for value in [True,1,501,101,"100"]:
            config=self.config();config["usb"]["power_ma"]=value
            self.assertRaises(build.BuildError,build.config_values,config,{"USA"})
    def test_utf8_ssid_byte_bound_and_controls(self):
        for value in ["", "é"*17,"bad\nname"]:
            config=self.config();config["wifi"]["ssid"]=value
            self.assertRaises(build.BuildError,build.config_values,config,{"USA"})
    def test_country_and_auth_validation(self):
        for value in ["USA\n#error", [], "UNKNOWN"]:
            config=self.config();config["wifi"]["country"]=value
            self.assertRaises(build.BuildError,build.config_values,config,{"USA"})
        for value in ["", "short", "A"*65]:
            config=self.config();config["wifi"]["password"]=value
            self.assertRaises(build.BuildError,build.config_values,config,{"USA"})
    def test_private_permissions_and_size_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"local.json";path.write_text(json.dumps(self.config()));path.chmod(0o644)
            self.assertRaises(build.BuildError,build.load_config,path,{"USA"})
            path.chmod(0o600);self.assertEqual(build.load_config(path,{"USA"}),self.config())
            path.write_text(" "*8193);self.assertRaises(build.BuildError,build.load_config,path,{"USA"})
    def test_header_escapes_bytes_without_source_injection(self):
        config=self.config();config["wifi"]["ssid"]='test"\\xFF'
        text=build.header(build.config_values(config,{"USA"}))
        self.assertNotIn('test"',text);self.assertIn('\\042',text);self.assertIn('\\134',text)
        self.assertEqual(text.count("#define"),5)
    def test_deployment_rejected_before_tools_or_credentials(self):
        with patch.object(build,"check_dependencies",side_effect=AssertionError("must not run")):
            self.assertRaises(build.BuildError,build.main,["--deploy"])
    def test_compile_mode_refuses_credentials_without_reading(self):
        with patch.object(build,"check_dependencies",side_effect=AssertionError("must not run")):
            self.assertRaises(build.BuildError,build.main,["--config","/does/not/exist"])
            self.assertRaises(build.BuildError,build.main,["--mode","private"])
    def test_concurrent_source_change_refuses_success_and_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); source=root/"source.cpp"; source.write_text("before")
            country=root/"sdk/lib/cyw43-driver/src/cyw43_country.h";country.parent.mkdir(parents=True);country.write_text("#define CYW43_COUNTRY_USA CYW43_COUNTRY('U','S',0)")
            output=root/"output"
            def changed_build(*args,**kwargs):
                source.write_text("after")
                return type("Result",(),{"returncode":0})()
            with patch.object(build,"ROOT",root), patch.object(build,"public_sources",return_value=[source]), \
                 patch.object(build,"check_dependencies",return_value={}), patch.object(build.subprocess,"run",side_effect=changed_build):
                # Choose output outside the mocked source root to retain the real safety check.
                with tempfile.TemporaryDirectory() as external:
                    output=Path(external)/"fresh"
                    with self.assertRaisesRegex(build.BuildError,"inputs changed"):
                        build.main(["--sdk",str(root/"sdk"),"--output",str(output)])
                    self.assertFalse((output/"redacted-manifest.json").exists())

    def test_board_selection_reaches_cmake_and_manifest(self):
        for arguments,board,platform in [([],"pico_w","rp2040"),(["--board","pico2_w"],"pico2_w","rp2350-arm-s")]:
            with tempfile.TemporaryDirectory() as directory:
                root=Path(directory);country=root/"sdk/lib/cyw43-driver/src/cyw43_country.h"
                country.parent.mkdir(parents=True);country.write_text("#define CYW43_COUNTRY_USA CYW43_COUNTRY('U','S',0)")
                output=root/"fresh"; commands=[]
                def fake_build(command,**kwargs):
                    commands.append(command)
                    product=output/"build";product.mkdir(exist_ok=True)
                    for suffix in ["elf","bin","uf2"]:(product/("simplecontroller_bridge."+suffix)).write_bytes(b"TEST ONLY")
                    return type("Result",(),{"returncode":0})()
                with patch.object(build,"check_dependencies",return_value={"sdk":{},"submodules":{},"tools":[]}), \
                     patch.object(build,"public_sources",return_value=[]), patch.object(build.subprocess,"run",side_effect=fake_build), contextlib.redirect_stdout(io.StringIO()):
                    build.main(arguments+["--sdk",str(root/"sdk"),"--output",str(output)])
                result=json.loads((output/"redacted-manifest.json").read_text())
                self.assertIn("-DPICO_BOARD="+board,commands[0])
                self.assertEqual((result["board"],result["platform"]),(board,platform))
                self.assertFalse(result["deployable"])
        with patch.object(build,"check_dependencies",side_effect=AssertionError("must not run")), contextlib.redirect_stderr(io.StringIO()):
            self.assertRaises(SystemExit,build.main,["--board","pico"])

    def test_shared_test_identity_requires_explicit_compile_only_choice(self):
        normal=build.public_compile_config();bench=build.public_compile_config(True)
        self.assertEqual((normal["usb"]["vid"],normal["usb"]["pid"]),("0xffff","0xffff"))
        self.assertEqual((bench["usb"]["vid"],bench["usb"]["pid"]),("0x1209","0x0001"))
        self.assertFalse(bench["usb"]["power_reviewed"])
        with patch.object(build,"check_dependencies",side_effect=AssertionError("must not run")):
            self.assertRaises(build.BuildError,build.main,["--mode","private","--config","/unread","--test-usb-identity"])

    def test_failed_validation_does_not_create_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output=Path(directory)/"never-created"
            with patch.object(build,"check_dependencies",side_effect=build.BuildError("wrong pin")):
                self.assertRaises(build.BuildError,build.main,["--output",str(output)])
            self.assertFalse(output.exists())

if __name__ == "__main__":
    unittest.main(verbosity=2)

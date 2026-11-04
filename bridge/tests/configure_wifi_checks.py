#!/usr/bin/env python3
"""Synthetic credentials only; requests stay on loopback and no firmware is built."""
import contextlib
import http.client
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.parse import urlencode, urlsplit
from unittest.mock import patch

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools/configure_wifi.py"
spec = importlib.util.spec_from_file_location("configure_wifi", MODULE_PATH)
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


def configuration():
    return {"wifi": {"ssid": "OLD SYNTHETIC NETWORK", "password": "old-fake-password", "country": "USA"},
            "usb": {"vid": "0x1209", "pid": "0x0001", "power_ma": 500,
                    "identity_reviewed": True, "power_reviewed": False}, "xac_port": "right"}


class FileChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "local_config.json"
        self.path.write_text(json.dumps(configuration()))
        self.path.chmod(0o600)

    def tearDown(self):
        self.temp.cleanup()

    def test_atomic_private_update_preserves_every_non_wifi_value(self):
        old = configuration()
        before_inode = self.path.stat().st_ino
        store = setup.ConfigFile(self.path)
        wifi = setup.validate_wifi("NEW SYNTHETIC NETWORK", "new-fake-password", "US")
        store.save_wifi(wifi)
        result = json.loads(self.path.read_text())
        self.assertEqual(result, {**old, "wifi": wifi})
        self.assertFalse(result["usb"]["power_reviewed"])
        self.assertEqual(result["wifi"]["country"], "USA")
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        self.assertNotEqual(self.path.stat().st_ino, before_inode)
        self.assertEqual(list(self.path.parent.glob(".wifi-setup-*")), [])

    def test_missing_readonly_public_linked_and_unowned_file_refused(self):
        missing = self.path.parent / "missing.json"
        self.assertRaises(setup.SetupError, setup.ConfigFile, missing)
        self.assertFalse(missing.exists())
        for mode in (0o400, 0o644, 0o660, 0o700):
            self.path.chmod(mode)
            self.assertRaises(setup.SetupError, setup.ConfigFile, self.path)
        self.path.chmod(0o600)
        link = self.path.parent / "alias.json"; link.symlink_to(self.path)
        self.assertRaises(setup.SetupError, setup.ConfigFile, link)
        link.unlink(); os.link(self.path, link)
        self.assertRaises(setup.SetupError, setup.ConfigFile, self.path)
        link.unlink()
        with patch.object(setup.os, "getuid", return_value=os.getuid()+1):
            self.assertRaises(setup.SetupError, setup.ConfigFile, self.path)

    def test_schema_duplicate_keys_and_oversize_refused_without_rewrite(self):
        for text in ('{"wifi":{},"wifi":{}}', '{"wifi":[]}', '{', " " * 8193):
            self.path.write_text(text)
            self.assertRaises(setup.SetupError, setup.ConfigFile, self.path)
            self.assertEqual(self.path.read_text(), text)
        value = configuration(); value["surprise"] = "preserve by refusing"
        self.path.write_text(json.dumps(value))
        self.assertRaises(setup.SetupError, setup.ConfigFile, self.path)

    def test_changed_file_and_lost_write_permission_are_not_overwritten(self):
        store = setup.ConfigFile(self.path)
        updated = configuration(); updated["usb"]["power_ma"] = 100
        self.path.write_text(json.dumps(updated))
        self.assertRaises(setup.SetupError, store.save_wifi, setup.validate_wifi("FAKE", "fake-password", "US"))
        self.assertEqual(json.loads(self.path.read_text()), updated)
        store = setup.ConfigFile(self.path); self.path.chmod(0o400)
        self.assertRaises(setup.SetupError, store.save_wifi, setup.validate_wifi("FAKE", "fake-password", "US"))
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o400)

    def test_atomic_replace_failure_preserves_old_data_and_removes_temp(self):
        store = setup.ConfigFile(self.path); before = self.path.read_bytes()
        with patch.object(setup.os, "replace", side_effect=OSError("synthetic failure")):
            self.assertRaises(setup.SetupError, store.save_wifi, setup.validate_wifi("FAKE", "fake-password", "US"))
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(list(self.path.parent.glob(".wifi-setup-*")), [])

    def test_repository_credential_destination_is_restricted(self):
        with patch.object(setup, "ROOT", self.path.parent.resolve()):
            self.assertRaises(setup.SetupError, setup.ConfigFile, self.path)

    def test_wifi_byte_bounds_controls_country_and_hex_key(self):
        for ssid,password,country in [("", "fake-password", "US"), ("é"*17,"fake-password","US"),
                                      ("FAKE\n", "fake-password","US"), ("FAKE","short","US"),
                                      ("FAKE","é"*32,"US"), ("FAKE","A"*65,"US"),
                                      ("FAKE","fake\x7fpassword","US"), ("FAKE","fake-password","XX"),
                                      ("FAKE","fake-password","USA")]:
            self.assertRaises(setup.SetupError, setup.validate_wifi, ssid,password,country)
        self.assertEqual(setup.validate_wifi("é"*16,"F"*64,"CA")["country"], "CANADA")


class HTTPChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "local_config.json"
        self.path.write_text(json.dumps(configuration())); self.path.chmod(0o600)
        self.before = self.path.read_bytes()
        self.server = setup.SetupServer(setup.ConfigFile(self.path))
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval":0.01}, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join(timeout=2)
        self.temp.cleanup()

    def fields(self, **changes):
        return {"csrf":self.server.csrf,"ssid":"SYNTHETIC ONLY","password":"synthetic-test-password","country":"US",**changes}

    def request(self, method="GET", body=None, headers=None, path=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        values = [("Host",self.server.host)]
        if method == "POST":
            values += [("Origin",self.server.origin),("Content-Type","application/x-www-form-urlencoded"),
                       ("Content-Length",str(len(body or b"")))]
        if headers is not None:
            replaced = {key.lower() for key,value in headers}
            values = [(key,value) for key,value in values if key.lower() not in replaced] + headers
        connection.putrequest(method, path or self.server.route, skip_host=True)
        for key,value in values: connection.putheader(key,value)
        connection.endheaders(body)
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), response.read().decode()
        connection.close()
        return result

    def post(self, **changes):
        return self.request("POST", urlencode(self.fields(**changes)).encode())

    def test_get_empty_masked_form_and_private_headers_no_secrets_or_logging(self):
        stdout,stderr=io.StringIO(),io.StringIO()
        with contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
            status,headers,page=self.request()
        self.assertEqual(status,200)
        self.assertIn('type="password"',page)
        self.assertIn('value="US" selected',page)
        self.assertNotIn("OLD SYNTHETIC NETWORK",page)
        self.assertNotIn("old-fake-password",page)
        self.assertEqual(headers["Cache-Control"],"no-store")
        self.assertIn("default-src 'none'",headers["Content-Security-Policy"])
        self.assertEqual(headers["Referrer-Policy"],"same-origin")
        self.assertEqual(stdout.getvalue()+stderr.getvalue(),"")
        self.assertEqual(self.path.read_bytes(),self.before)
        self.assertEqual(self.server.server_address[0],"127.0.0.1")
        self.assertNotEqual(self.server.route[1:],self.server.csrf)

    def test_csrf_origin_host_and_path_rejections_do_not_write(self):
        self.assertEqual(self.post(csrf="wrong")[0],403)
        body=urlencode(self.fields()).encode()
        for headers in [[("Origin","http://example.invalid")],[("Origin","null")],[("Origin","")],
                        [("Host","example.invalid")],[("Host",self.server.host),("Host",self.server.host)],
                        [("Origin",self.server.origin),("Origin",self.server.origin)]]:
            self.assertEqual(self.request("POST",body,headers)[0],403)
        self.assertEqual(self.request(path="/wrong")[0],404)
        self.assertEqual(self.request(path=self.server.route+"?extra=yes")[0],404)
        self.assertEqual(self.request(headers=[("Origin","http://example.invalid")])[0],403)
        self.assertEqual(self.path.read_bytes(),self.before)

    def test_bounded_invalid_and_duplicate_form_input_does_not_write_or_echo(self):
        invalid=[b"a="+b"X"*4096, b"ssid=%FF", b"ssid=%XZ", b"csrf=one&csrf=two&ssid=a&password=b&country=US"]
        for body in invalid:
            status,_,page=self.request("POST",body)
            self.assertIn(status,(400,413)); self.assertNotIn("XXXXX",page)
        for changes in [{"ssid":"é"*17},{"password":"short"},{"country":"UNSUPPORTED"}]:
            status,_,page=self.post(**changes)
            self.assertEqual(status,400)
            self.assertNotIn("SYNTHETIC ONLY",page)
            self.assertNotIn("synthetic-test-password",page)
        body=urlencode(self.fields()).encode()
        self.assertEqual(self.request("POST",body,[("Content-Type","text/plain")])[0],415)
        self.assertEqual(self.request("POST",body,[("Transfer-Encoding","chunked")])[0],400)
        self.assertEqual(self.request("POST",body,[("Content-Length","-1")])[0],400)
        self.assertEqual(self.path.read_bytes(),self.before)

    def test_success_is_single_use_redacted_and_preserves_usb(self):
        stdout,stderr=io.StringIO(),io.StringIO()
        with contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
            status,headers,page=self.post()
        self.assertEqual(status,200);self.assertTrue(self.server.completed)
        self.assertNotIn("SYNTHETIC ONLY",page);self.assertNotIn("synthetic-test-password",page)
        self.assertEqual(stdout.getvalue()+stderr.getvalue(),"")
        saved=json.loads(self.path.read_text())
        self.assertEqual(saved["usb"],configuration()["usb"])
        self.assertEqual(saved["xac_port"],"right")
        self.assertEqual(saved["wifi"]["country"],"USA")
        before=self.path.read_bytes()
        self.assertEqual(self.post(ssid="SECOND")[0],409)
        self.assertEqual(self.path.read_bytes(),before)

    def test_changed_file_returns_safe_error_without_overwrite(self):
        changed=configuration();changed["xac_port"]="left";self.path.write_text(json.dumps(changed))
        status,_,page=self.post()
        self.assertEqual(status,400);self.assertIn("configuration changed",page)
        self.assertEqual(json.loads(self.path.read_text()),changed)
        self.assertFalse(self.server.completed)

    def test_oversized_headers_and_ambiguous_length_are_rejected(self):
        self.assertEqual(self.request(headers=[("X-Extra","X"*8200)])[0],431)
        body=urlencode(self.fields()).encode()
        self.assertEqual(self.request("POST",body,[("Content-Length",str(len(body))),
                                                  ("Content-Length",str(len(body)))])[0],400)
        self.assertEqual(self.path.read_bytes(),self.before)


class CLIChecks(unittest.TestCase):
    def test_cli_only_prints_ready_url_and_completion_with_synthetic_submission(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"local_config.json"
            path.write_text(json.dumps(configuration()));path.chmod(0o600)
            process=subprocess.Popen([sys.executable,str(MODULE_PATH),"--config",str(path)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                url=process.stdout.readline().strip();parsed=urlsplit(url)
                self.assertEqual(parsed.hostname,"127.0.0.1")
                connection=http.client.HTTPConnection(parsed.hostname,parsed.port,timeout=3)
                connection.request("GET",parsed.path)
                page=connection.getresponse().read().decode();connection.close()
                import re
                token=re.search(r'name="csrf" value="([^"]+)"',page).group(1)
                body=urlencode({"csrf":token,"ssid":"CLI SYNTHETIC","password":"synthetic-password","country":"US"})
                connection=http.client.HTTPConnection(parsed.hostname,parsed.port,timeout=3)
                connection.request("POST",parsed.path,body,{"Origin":f"http://{parsed.netloc}","Content-Type":"application/x-www-form-urlencoded"})
                response=connection.getresponse();self.assertEqual(response.status,200);response.read();connection.close()
                rest,errors=process.communicate(timeout=5)
                self.assertEqual(process.returncode,0);self.assertEqual(errors,"")
                self.assertEqual(rest,"Wi-Fi settings saved locally. No build or flash performed.\n")
            finally:
                if process.poll() is None:process.kill();process.communicate()


if __name__ == "__main__":
    unittest.main(verbosity=2)

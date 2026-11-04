#!/usr/bin/env python3
"""One-use localhost form for an existing private Pico Wi-Fi configuration."""
import argparse
import html
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import stat
import sys
import tempfile
from urllib.parse import parse_qs

ROOT = Path(__file__).resolve().parents[2]
MAX_CONFIG_BYTES = 8192
MAX_FORM_BYTES = 4096
# ISO codes and enum names from the pinned Pico SDK 2.3.1 cyw43_country.h.
# WORLDWIDE is deliberately not a substitute for the user's actual location.
COUNTRIES = dict(item.split(":") for item in """
AU:AUSTRALIA AT:AUSTRIA BE:BELGIUM BR:BRAZIL CA:CANADA CL:CHILE CN:CHINA
CO:COLOMBIA CZ:CZECH_REPUBLIC DK:DENMARK EE:ESTONIA FI:FINLAND FR:FRANCE
DE:GERMANY GR:GREECE HK:HONG_KONG HU:HUNGARY IS:ICELAND IN:INDIA IL:ISRAEL
IT:ITALY JP:JAPAN KE:KENYA LV:LATVIA LI:LIECHTENSTEIN LT:LITHUANIA
LU:LUXEMBOURG MY:MALAYSIA MT:MALTA MX:MEXICO NL:NETHERLANDS NZ:NEW_ZEALAND
NG:NIGERIA NO:NORWAY PE:PERU PH:PHILIPPINES PL:POLAND PT:PORTUGAL SG:SINGAPORE
SK:SLOVAKIA SI:SLOVENIA ZA:SOUTH_AFRICA KR:SOUTH_KOREA ES:SPAIN SE:SWEDEN
CH:SWITZERLAND TW:TAIWAN TH:THAILAND TR:TURKEY GB:UK US:USA
""".split())


class SetupError(Exception):
    """Only fixed, non-secret messages may be used here."""


def exact_keys(value, keys):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise SetupError("The existing configuration does not match the supported schema.")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise SetupError("The existing configuration contains duplicate fields.")
        result[key] = value
    return result


def config_schema(raw):
    exact_keys(raw, ("wifi", "usb", "xac_port"))
    exact_keys(raw["wifi"], ("ssid", "password", "country"))
    exact_keys(raw["usb"], ("vid", "pid", "power_ma", "identity_reviewed", "power_reviewed"))
    if not all(isinstance(raw["wifi"][key], str) for key in ("ssid", "password", "country")):
        raise SetupError("The existing Wi-Fi fields must be text.")
    usb = raw["usb"]
    if (not all(isinstance(usb[key], str) and re.fullmatch(r"0x[0-9A-Fa-f]{4}", usb[key]) for key in ("vid", "pid"))
            or type(usb["power_ma"]) is not int or not 2 <= usb["power_ma"] <= 500 or usb["power_ma"] % 2
            or any(type(usb[key]) is not bool for key in ("identity_reviewed", "power_reviewed"))
            or raw["xac_port"] not in ("left", "right")):
        raise SetupError("The existing USB or port fields do not match the supported schema.")
    # Setup does not approve, infer or change identity/power reviews or the chosen port.
    return raw


def validate_wifi(ssid, password, country):
    if any(any(ord(c) < 32 or ord(c) == 127 for c in value) for value in (ssid, password)):
        raise SetupError("Network name and password cannot contain control characters.")
    if not 1 <= len(ssid.encode("utf-8")) <= 32:
        raise SetupError("Enter a network name of 1–32 UTF-8 bytes.")
    if not (8 <= len(password.encode("utf-8")) <= 63 or re.fullmatch(r"[0-9A-Fa-f]{64}", password)):
        raise SetupError("Enter a WPA2 password of 8–63 UTF-8 bytes, or a 64-digit hexadecimal key.")
    if country not in COUNTRIES:
        raise SetupError("Choose your actual country from the supported list.")
    return {"ssid": ssid, "password": password, "country": COUNTRIES[country]}


class ConfigFile:
    def __init__(self, path):
        requested = Path(path).expanduser().absolute()
        if requested.is_symlink():
            raise SetupError("Use the existing private configuration file, not a symbolic link.")
        self.path = requested.resolve()
        if (self.path == ROOT or ROOT in self.path.parents) and self.path != ROOT / "bridge/firmware/local_config.json":
            raise SetupError("Inside this repository, use only ignored bridge/firmware/local_config.json.")
        self.original, self.identity = self.read()
        try:
            self.value = config_schema(json.loads(self.original.decode("utf-8"), object_pairs_hook=unique_object))
        except (UnicodeError, ValueError, RecursionError):
            raise SetupError("The existing configuration must be bounded UTF-8 JSON.") from None

    def read(self):
        try:
            fd = os.open(self.path, os.O_RDONLY | os.O_NOFOLLOW)
        except OSError:
            raise SetupError("The configuration must already exist as a readable private file.") from None
        with os.fdopen(fd, "rb") as file:
            info = os.fstat(file.fileno())
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                    or stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1):
                raise SetupError("The configuration must be owned by you, with permissions 600 and no file links.")
            data = file.read(MAX_CONFIG_BYTES + 1)
            if len(data) > MAX_CONFIG_BYTES:
                raise SetupError("The configuration exceeds its 8 KiB limit.")
            return data, (info.st_dev, info.st_ino, info.st_mtime_ns)

    def save_wifi(self, wifi):
        candidate = dict(self.value)
        candidate["wifi"] = wifi
        data = (json.dumps(candidate, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        if len(data) > MAX_CONFIG_BYTES:
            raise SetupError("The updated configuration exceeds its size limit.")
        if self.read() != (self.original, self.identity):
            raise SetupError("The configuration changed. Close this form and start setup again.")
        name = None
        try:
            fd, name = tempfile.mkstemp(prefix=".wifi-setup-", dir=self.path.parent)
            with os.fdopen(fd, "wb") as file:
                os.fchmod(file.fileno(), 0o600)
                file.write(data)
                file.flush()
                os.fsync(file.fileno())
            if self.read() != (self.original, self.identity):
                raise SetupError("The configuration changed. Close this form and start setup again.")
            os.replace(name, self.path)
            name = None
        except OSError:
            raise SetupError("The private configuration could not be saved. No setup completion was recorded.") from None
        finally:
            if name is not None:
                try:
                    os.unlink(name)
                except OSError:
                    pass


class SetupServer(HTTPServer):
    def __init__(self, config):
        self.config = config
        self.route = "/" + secrets.token_urlsafe(32)
        self.csrf = secrets.token_urlsafe(32)
        self.completed = False
        super().__init__(("127.0.0.1", 0), SetupHandler)
        self.host = "127.0.0.1:" + str(self.server_port)
        self.origin = "http://" + self.host
        self.url = self.origin + self.route
        self.timeout = 1

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(5)
        return connection, address

    def handle_error(self, request, client_address):
        pass  # Never log a request, form body or exception containing submitted data.


class SetupHandler(BaseHTTPRequestHandler):
    server_version = "LocalSetup"
    sys_version = ""

    def log_message(self, *_):
        pass

    def send_error(self, code, message=None, explain=None):
        self.respond(code, "Request refused.")

    def respond(self, status, content):
        data = ("<!doctype html><html lang=en><meta charset=utf-8>"
                "<meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>Local Wi-Fi setup</title><body>" + content + "</body></html>").encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Pragma", "no-cache")
        self.send_header("Content-Security-Policy", "default-src 'none'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
        # Browsers need to retain Origin on a same-origin form POST for our CSRF check.
        self.send_header("Referrer-Policy", "same-origin")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(data)

    def allowed_request(self, post=False):
        if sum(len(k) + len(v) for k, v in self.headers.items()) > 8192:
            self.respond(431, "Request headers are too large."); return False
        if self.headers.get_all("Host") != [self.server.host]:
            self.respond(403, "This form is available only at its local address."); return False
        origins = self.headers.get_all("Origin", [])
        if (post and origins != [self.server.origin]) or (not post and origins and origins != [self.server.origin]):
            self.respond(403, "Open the original local setup link and try again."); return False
        if self.path != self.server.route:
            self.respond(404, "Open the original local setup link."); return False
        if self.server.completed:
            self.respond(409, "Setup is already complete. Close this tab."); return False
        return True

    def form(self, message=""):
        options = []
        for code, name in sorted(COUNTRIES.items(), key=lambda item: item[1]):
            label = {"US": "United States", "GB": "United Kingdom"}.get(code, name.replace("_", " ").title())
            options.append('<option value="' + code + '"' + (' selected' if code == "US" else '') + '>' + label + '</option>')
        return ('<h1>Set up the Pico Wi-Fi</h1>'
                '<p>Use a 2.4 GHz WPA2 network that your iPhone also joins. '
                'Enter the password here on this Mac, not in chat. It is saved only in the local private configuration.</p>'
                '<p>' + html.escape(message) + '</p>'
                '<form method="post" action="' + self.server.route + '" autocomplete="off">'
                '<input type="hidden" name="csrf" value="' + self.server.csrf + '">'
                '<p><label>Network name (SSID)<br><input name="ssid" required maxlength="32" autocomplete="off" autocapitalize="none" spellcheck="false"></label></p>'
                '<p><label>Wi-Fi password<br><input type="password" name="password" required maxlength="64" autocomplete="new-password"></label></p>'
                '<p><label>Country where the Pico will be used<br><select name="country">' + ''.join(options) + '</select></label></p>'
                '<button type="submit">Save Wi-Fi settings</button></form>'
                '<p>This saves Wi-Fi settings only. It does not build or flash the Pico.</p>')

    def do_GET(self):
        if self.allowed_request():
            self.respond(200, self.form())

    def do_POST(self):
        if not self.allowed_request(post=True):
            return
        lengths = self.headers.get_all("Content-Length", [])
        if self.headers.get_all("Transfer-Encoding") or len(lengths) != 1 or not re.fullmatch(r"[0-9]{1,5}", lengths[0]):
            self.respond(400, "A bounded form submission is required."); return
        length = int(lengths[0])
        if length > MAX_FORM_BYTES:
            self.respond(413, "The submitted form is too large."); return
        if self.headers.get_all("Content-Type") != ["application/x-www-form-urlencoded"]:
            self.respond(415, "Submit using the local setup form."); return
        body = self.rfile.read(length)
        if len(body) != length:
            self.respond(400, "The form was incomplete. Please try again."); return
        try:
            encoded = body.decode("utf-8")
            if re.search(r"%(?![0-9A-Fa-f]{2})", encoded):
                raise ValueError()
            fields = parse_qs(encoded, keep_blank_values=True, strict_parsing=True, encoding="utf-8", errors="strict", max_num_fields=4)
            if set(fields) != {"csrf", "ssid", "password", "country"} or any(len(v) != 1 for v in fields.values()):
                raise ValueError()
        except (ValueError, UnicodeError):
            self.respond(400, "The submitted form is invalid."); return
        if not secrets.compare_digest(fields["csrf"][0].encode("utf-8"), self.server.csrf.encode("ascii")):
            self.respond(403, "Open the original local setup link and try again."); return
        try:
            wifi = validate_wifi(fields["ssid"][0], fields["password"][0], fields["country"][0])
            self.server.config.save_wifi(wifi)
        except SetupError as error:
            self.respond(400, self.form(str(error))); return
        self.server.completed = True
        self.respond(200, "<h1>Wi-Fi settings saved</h1><p>You can close this tab. No firmware has been built or flashed.</p>")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True, help="Existing private JSON file; never a new destination")
    args = parser.parse_args(argv)
    try:
        config = ConfigFile(args.config)
        with SetupServer(config) as server:
            print(server.url, flush=True)
            while not server.completed:
                server.handle_request()
        print("Wi-Fi settings saved locally. No build or flash performed.", flush=True)
        return 0
    except KeyboardInterrupt:
        return 130
    except (SetupError, OSError) as error:
        message = str(error) if isinstance(error, SetupError) else "Local setup could not start."
        print(message, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""PC Host for the PSVietHoa AutoLoader (fork of ps5-webkit-autoloader).

A zero-dependency DNS + HTTP server that:
  - Spoofs ``manuals.playstation.net`` (or any --target) to this PC's IP.
  - Returns NXDOMAIN for every other domain (blocks telemetry/internet).
  - Serves files over HTTPS on port 443 (required) and optionally over HTTP
    (--http-port, e.g. for browser testing), checking the overrides/
    directory first, the embedded frontend archive second, and the base
    frontend directory last. The built ``psviethoa-webkit-autoloader-host.py``
    (from tools/build_host.py) carries the frontend/autoloader files inside the
    script itself, so it is fully portable and serves them straight from
    memory.

Usage:
    python3 host.py                       # guided mode (default)
    python3 host.py --verbose             # detailed per-request logging
    python3 host.py --http-port 8080      # also serve plain HTTP (dev/testing)
    python3 host.py --target manuals.playstation.net --base ../frontend/autoloader
"""

import argparse
import base64
import binascii
import datetime
import errno
import io
import json
import mimetypes
import os
import posixpath
import re
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
import zipfile
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_TARGET = "manuals.playstation.net"
DEFAULT_OVERRIDES = "./overrides"
DEFAULT_TTL = 300

APP_NAME = "psviethoa-webkit-autoloader PC host"

# [[VERSION_PLACEHOLDER]]
VERSION = "dev"

# [[BUILD_TIME_PLACEHOLDER]]
BUILD_TIME = "dev"

# ANSI colors (enabled only when output is a real terminal)
def _init_console():
    """Enable ANSI/VT processing on Windows consoles; returns True when
    color output is supported."""
    if not sys.stdout.isatty():
        return False
    if os.name == "nt":
        import ctypes

        kernel32 = ctypes.windll.kernel32
        ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
        for handle_id in (-11, -12):  # stdout, stderr
            handle = kernel32.GetStdHandle(handle_id)
            mode = ctypes.c_ulong()
            if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
                kernel32.SetConsoleMode(handle, mode.value | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
    return True


COLOR_ENABLED = _init_console()


def _style(text, *codes):
    """Apply ANSI SGR color codes to text; no-op when not a terminal."""
    if not COLOR_ENABLED:
        return text
    return "\033[" + ";".join(str(c) for c in codes) + "m" + text + "\033[0m"


def tag(text):
    """Recolor the [..] markers in a message ([+] green, [-] red)."""
    return text.replace("[+]", _style("[+]", 32)).replace("[-]", _style("[-]", 31))


def build_banner():
    """ASCII banner; the installed version and build time are shown on rows 2-3."""
    width = 46
    row = lambda text: "   │" + text.center(width) + "│"
    banner = "\n".join(
        [
            "",
            "   ┌" + "─" * width + "┐",
            row("PSVIETHOA-AUTOLOADER"),
            row(f"INSTALLER-HOST v{VERSION}"),
            row(f"fork of itsPLK (built {BUILD_TIME})"),
            "   └" + "─" * width + "┘",
        ]
    )
    return _style(banner, 36)


def build_credits():
    """ASCII box shown on the splash screen."""
    width = 46
    row = lambda text: "   │" + text.center(width) + "│"
    box = "\n".join(
        [
            "",
            "   ┌" + "─" * width + "┐",
            row("THIS PROJECT IS FREE & OPEN SOURCE"),
            row("github.com/itsPLK/ps5-webkit-autoloader"),
            "   └" + "─" * width + "┘",
        ]
    )
    return _style(box, 32)


def clear_screen():
    """Clear the terminal; no-op when output is not a terminal."""
    if not sys.stdout.isatty():
        return
    os.system("cls" if os.name == "nt" else "clear")


def show_splash():
    """Show the branding splash (credits box) briefly, then clear."""
    clear_screen()
    print(build_credits())
    time.sleep(2)
    clear_screen()


class UpdateChecker:
    """Background check for a newer release on GitHub.

    Runs in its own daemon thread so the HTTP request overlaps the splash
    screen. All failures are silent — the notice is best-effort only.
    """

    # TODO(PSVietHoa): point these at the fork's repo once it is published.
    API_URL = "https://api.github.com/repos/itsPLK/ps5-webkit-autoloader/releases/latest"
    RELEASES_URL = "https://github.com/itsPLK/ps5-webkit-autoloader/releases"
    USER_AGENT = "psviethoa-webkit-autoloader-host"
    TIMEOUT = 3

    def __init__(self, version):
        self.current = self._base_version(version)
        self.latest = None
        self.done = False
        self._lock = threading.Lock()
        self.thread = None

    def start(self):
        """Start the background check in a daemon thread."""
        self.thread = threading.Thread(target=self.check, daemon=True)
        self.thread.start()

    @staticmethod
    def _base_version(version):
        """Extract the (major, minor, patch) base from a version string.

        Handles stable ("0.1.2") and dev ("0.1.2-dev-<suffix>") builds alike.
        Returns None when the version is not a semver-like string (e.g. the
        plain "dev" used when running straight from source), which disables
        the check.
        """
        m = re.match(r"(\d+)\.(\d+)\.(\d+)", version or "")
        return tuple(int(x) for x in m.groups()) if m else None

    def check(self):
        try:
            req = urllib.request.Request(
                self.API_URL, headers={"User-Agent": self.USER_AGENT}
            )
            with urllib.request.urlopen(req, timeout=self.TIMEOUT) as resp:
                if resp.status != 200:
                    return
                tag = json.load(resp).get("tag_name", "")
            self.latest = self._base_version(tag.lstrip("v"))
        except Exception:
            pass
        finally:
            with self._lock:
                self.done = True

    def has_update(self):
        with self._lock:
            return bool(self.current and self.latest and self.latest > self.current)

    def notice(self):
        """Return the formatted update notice, or None when up to date."""
        if not self.has_update():
            return None
        latest = ".".join(str(x) for x in self.latest)
        return tag(f"[+] A newer version (v{latest}) is available — {self.RELEASES_URL}")

# The HTTPS server needs a self-signed cert for the spoofed target domain.
# tools/build_host.py generates a fresh pair at build time and injects it into
# the placeholders below. When running straight from source, get_server_cert()
# generates a fresh pair on every run with openssl.
# [[SSL_CERT_PLACEHOLDER]]
SSL_CERT_PEM = ""

# [[SSL_KEY_PLACEHOLDER]]
SSL_KEY_PEM = ""

# Embedded frontend payload: tools/build_host.py replaces the block below
# with a base64-encoded (deflated) zip of frontend/autoloader, so the built
# psviethoa-webkit-autoloader-host.py can serve the frontend entirely from memory.
# [[EMBEDDED_ZIP]]
EMBEDDED_ZIP_B64 = ""

_embedded_zip_cache = None
_embedded_zip_loaded = False


def get_embedded_zip():
    """Lazily decode EMBEDDED_ZIP_B64 into an in-memory zipfile.ZipFile.

    Returns None when the script was not built by tools/build_host.py (or the
    payload is corrupt), in which case only the filesystem sources are used.
    """
    global _embedded_zip_cache, _embedded_zip_loaded
    if not _embedded_zip_loaded:
        _embedded_zip_loaded = True
        if EMBEDDED_ZIP_B64:
            try:
                _embedded_zip_cache = zipfile.ZipFile(
                    io.BytesIO(base64.b64decode(EMBEDDED_ZIP_B64))
                )
            except (binascii.Error, zipfile.BadZipFile):
                _embedded_zip_cache = None
    return _embedded_zip_cache


def generate_server_cert(cert_path, key_path):
    """Generate a self-signed certificate for the spoofed target domain."""
    subprocess.run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-nodes", "-days", "3650", "-sha256",
            "-keyout", key_path, "-out", cert_path,
            "-subj", "/CN=" + DEFAULT_TARGET,
        ],
        check=True,
        capture_output=True,
    )


def get_server_cert():
    """Return (cert_pem, key_pem) for the HTTPS server.

    The built script carries a pair generated at build time by
    tools/build_host.py. When running from source, a fresh pair is generated
    on every run with openssl. Returns (None, None) when neither is available.
    """
    if SSL_CERT_PEM and SSL_KEY_PEM:
        return SSL_CERT_PEM, SSL_KEY_PEM
    tmpdir = tempfile.mkdtemp(prefix="ps5-wkal-")
    cert_path = os.path.join(tmpdir, "cert.pem")
    key_path = os.path.join(tmpdir, "key.pem")
    try:
        generate_server_cert(cert_path, key_path)
    except (OSError, subprocess.CalledProcessError):
        return None, None
    with open(cert_path) as f:
        cert = f.read()
    with open(key_path) as f:
        key = f.read()
    return cert, key


def detect_local_ip():
    """Best-effort local IP detection (no traffic is actually sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def validate_ip(value):
    socket.inet_aton(value)
    return value


def _bind_hint(service, port, exc):
    """Return a user-facing hint for a bind failure, or '' when unknown."""
    if isinstance(exc, PermissionError):
        return (
            f"    {service} port {port} needs elevated privileges: on Linux, ports below "
            "1024 require root (e.g. 'sudo python3 host.py'); on Windows the port may be "
            "reserved by a system service."
        )
    if getattr(exc, "errno", None) == errno.EADDRINUSE:
        return (
            f"    Port {port} is already in use — stop the process holding it (on Windows, "
            f"the DNS Client service can hold port 53), or pick another port "
            f"(e.g. --dns-port 1053 / --https-port 8443)."
        )
    return ""


def parse_query(data):
    """Parse the first question of a DNS query.

    Returns ``(qid, question_bytes, name)`` where ``question_bytes`` is the
    raw question section to echo back in the response, or ``None`` on failure.
    """
    if len(data) < 12:
        return None
    qid, _flags, qdcount, _, _, _ = struct.unpack(">HHHHHH", data[:12])
    if qdcount < 1:
        return None

    offset = 12
    labels = []
    while offset < len(data):
        length = data[offset]
        if length == 0:
            offset += 1
            break
        if length & 0xC0 == 0xC0:  # compressed pointer: name continues elsewhere
            return None
        offset += 1
        if offset + length > len(data):
            return None
        labels.append(data[offset : offset + length].decode("ascii", "replace"))
        offset += length
    else:
        return None

    if offset + 4 > len(data):  # QTYPE + QCLASS
        return None
    end = offset + 4
    name = ".".join(labels).lower()
    return qid, data[12:end], name


def build_response(qid, question_bytes, ip=None):
    """Build a DNS response. With ``ip`` -> answer with an A record,
    without -> NXDOMAIN."""
    if ip is None:
        flags = 0x8183  # QR + RD + RA + NXDOMAIN
        answer = b""
    else:
        flags = 0x8180  # QR + RD + RA
        answer = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, DEFAULT_TTL, 4) + socket.inet_aton(ip)
    header = struct.pack(">HHHHHH", qid, flags, 1, 1 if ip else 0, 0, 0)
    return header + question_bytes + answer


class GuideStatus:
    """One-shot status milestones: prints a message when the PS5 first
    contacts the server, and another when the PSVietHoa AutoLoader
    Installer has been served (a /document/ request)."""

    def __init__(self, logger=print):
        self.logger = logger
        self.connected = False
        self.served = False
        self._lock = threading.Lock()

    def on_connection(self):
        with self._lock:
            if self.connected:
                return
            self.connected = True
        self.logger(
            tag("\n[+] PS5 connection detected.\n"
                "    Now open the User's Guide on your PS5\n"
                "    (Settings -> User's Guide) to install the\n"
                "    PSVietHoa AutoLoader.\n")
        )

    def on_document_served(self):
        with self._lock:
            if self.served:
                return
            self.served = True
        self.logger(
            tag("\n[+] Installer served.\n"
                "    If the exploit succeeded and the PSVietHoa AutoLoader has\n"
                "    been installed, you can now close this host and change\n"
                "    your PS5 DNS back — otherwise your PS5 won't be able to\n"
                "    access the Internet.\n")
        )


class DNSHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        parsed = parse_query(data)
        if parsed is None:
            return
        qid, question_bytes, name = parsed
        server = self.server
        server.guide.on_connection()
        if name == server.target.lower():
            server.log(_style(f"[DNS]  {name} -> {server.ip}", 36))
            sock.sendto(build_response(qid, question_bytes, server.ip), self.client_address)
        else:
            server.log(_style(f"[DNS]  {name} -> BLOCKED (NXDOMAIN)", 33))
            sock.sendto(build_response(qid, question_bytes), self.client_address)


class DNSServer(socketserver.ThreadingUDPServer):
    allow_reuse_address = True

    def __init__(self, address, target, ip, logger=print, guide=None):
        self.target = target
        self.ip = ip
        self.log = logger
        self.guide = guide or GuideStatus()
        super().__init__(address, DNSHandler)


class DualDirHandler(SimpleHTTPRequestHandler):
    """Serves files from overrides/ first, the embedded zip archive second,
    and base_dir last."""

    def __init__(self, *args, base_dir, overrides_dir, allowed_host, embedded_zip=None, **kwargs):
        self.base_dir = os.path.abspath(base_dir) if base_dir else None
        self.overrides_dir = os.path.abspath(overrides_dir)
        self.allowed_host = allowed_host.lower() if allowed_host else None
        self.embedded_zip = embedded_zip
        super().__init__(*args, directory=self.base_dir, **kwargs)

    def log_message(self, fmt, *args):  # silence default per-request logging
        pass

    def _log_http(self, message):
        if getattr(self.server, "quiet", False):
            return
        if "Not Found" in message or "REJECTED" in message:
            print(_style(f"[HTTP] {message}", 31))
        else:
            print(_style(f"[HTTP] {message}", 34))

    def _relative_path(self):
        """Normalize the request path into a docroot-relative path ('' for
        root), skipping unsafe words — mirrors SimpleHTTPRequestHandler."""
        path = self.path.split("?", 1)[0].split("#", 1)[0]
        
        # Intercept PS5 User's Guide URLs (e.g. /document/en/ps5/index.html)
        if path.startswith("/document/") and "/ps5/" in path:
            path = "/" + path.split("/ps5/", 1)[1]
            
        # The autoloader HTML hardcodes /app/ for the ELF cache structure.
        # Map /app/ back to the root so the standalone files resolve properly.
        if path.startswith("/app/"):
            path = path[4:]
            
        try:
            path = urllib.parse.unquote(path, errors="surrogatepass")
        except UnicodeDecodeError:
            path = urllib.parse.unquote(path)
        path = posixpath.normpath(path)
        words = [
            word for word in path.split("/")
            if word and not (os.path.dirname(word) or word in (os.curdir, os.pardir))
        ]
        return "/".join(words)

    def _resolve_source(self):
        """Resolve self.path to (data, content_type, mtime, source, shown)
        where source is 'overrides', 'embedded' or 'base', or None if the
        file cannot be found anywhere."""
        rel = self._relative_path()
        candidates = [rel]
        if not rel or rel.endswith("/"):
            candidates = [rel + name for name in ("index.html", "index.htm")]

        # If running as a fat binary, serve STRICTLY from the embedded zip
        if self.embedded_zip is not None:
            for candidate in candidates:
                if candidate in self.embedded_zip.namelist():
                    info = self.embedded_zip.getinfo(candidate)
                    mtime = datetime.datetime(*info.date_time[:6]).timestamp()
                    return self.embedded_zip.read(info), self._content_type(candidate), mtime, "embedded", candidate
            return None

        # 1. Overrides directory (local development)
        for candidate in candidates:
            override_path = os.path.join(self.overrides_dir, candidate)
            if os.path.isfile(override_path):
                with open(override_path, "rb") as f:
                    return f.read(), self._content_type(candidate), os.path.getmtime(override_path), "overrides", candidate

        # 2. Base directory (local development)
        for candidate in candidates:
            base_path = os.path.join(self.base_dir, candidate)
            if os.path.isfile(base_path):
                with open(base_path, "rb") as f:
                    return f.read(), self._content_type(candidate), os.path.getmtime(base_path), "base", candidate

        return None

    @staticmethod
    def _content_type(path):
        return mimetypes.guess_type(path)[0] or "application/octet-stream"

    def host_allowed(self):
        if self.allowed_host is None:
            return True
        host = self.headers.get("Host", "").split(":", 1)[0].strip().lower()
        return host == self.allowed_host

    def send_head(self):
        raw_path = self.path.split("?", 1)[0].split("#", 1)[0]
        self.server.guide.on_connection()
        if not self.host_allowed():
            self.send_error(403, "Host not allowed")
            self._log_http(f"{self.command} {raw_path} -> REJECTED (Host header mismatch)")
            return None

        resolved = self._resolve_source()
        if resolved is None:
            self.send_error(404, "File not found")
            self._log_http(f"{self.command} {raw_path} -> Not Found")
            return None

        data, content_type, mtime, source, shown = resolved
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Last-Modified", self.date_time_string(mtime))
        self.end_headers()
        self._log_http(f"{self.command} {raw_path} -> Served from {source} ({shown})")
        if raw_path.startswith("/document/"):
            self.server.guide.on_document_served()
        return io.BytesIO(data)


def build_http_server(host, port, base, overrides, allowed_host, embedded_zip=None, logger=print, quiet=False, guide=None):
    handler = lambda *args, **kwargs: DualDirHandler(
        *args, base_dir=base, overrides_dir=overrides, allowed_host=allowed_host,
        embedded_zip=embedded_zip, **kwargs
    )
    httpd = ThreadingHTTPServer((host, port), handler)
    httpd.daemon_threads = True
    httpd.log = logger
    httpd.quiet = quiet
    httpd.guide = guide or GuideStatus()
    return httpd


def resolve_dir(path):
    """Resolve a path argument; relative paths resolve against the current
    working directory. Returns None when no path is given."""
    if not path:
        return None
    return os.path.abspath(path)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        prog="host.py",
        description="DNS spoofer + dual-directory HTTP server for the PSVietHoa AutoLoader.",
        epilog="Examples:\n"
        "  python3 host.py\n"
        "  python3 host.py --target manuals.playstation.net --base ../frontend/autoloader\n"
        "  python3 host.py --no-dns --http-port 8080",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--ip", type=validate_ip, default=None,
                        help="IP to point the spoofed domain at (default: auto-detect).")
    parser.add_argument("--target", default=DEFAULT_TARGET,
                        help=f"Domain to spoof (default: {DEFAULT_TARGET}).")
    parser.add_argument("--base", default=None,
                        help="Base frontend directory to serve from disk (optional; "
                             "served only when the embedded archive is absent).")
    parser.add_argument("--overrides", default=DEFAULT_OVERRIDES,
                        help=f"Overrides directory (default: {DEFAULT_OVERRIDES}).")
    parser.add_argument("--dns-port", type=int, default=53, help="DNS UDP port (default: 53).")
    parser.add_argument("--http-port", type=int, default=None,
                        help="Optional HTTP TCP port (default: disabled). Set it to serve files "
                             "over plain HTTP, e.g. --http-port 8080 for browser testing.")
    parser.add_argument("--https-port", type=int, default=443, help="HTTPS TCP port (default: 443).")
    parser.add_argument("--strict-host", action="store_true",
                        help="Only serve requests whose Host header matches --target.")
    parser.add_argument("--no-dns", action="store_true", help="Disable the DNS server.")
    parser.add_argument("--no-http", action="store_true", help="Disable the HTTP server.")
    parser.add_argument("--no-https", action="store_true", help="Disable the HTTPS server.")
    parser.add_argument("--verbose", action="store_true",
                        help="Detailed per-request DNS/HTTP logging (default: guided mode "
                             "with one-time status messages).")
    parser.add_argument("--no-color", action="store_true",
                        help="Disable ANSI colors in the terminal output.")
    parser.add_argument("--no-update-check", action="store_true",
                        help="Disable the GitHub update check at startup.")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    if args.no_color:
        global COLOR_ENABLED
        COLOR_ENABLED = False
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    # Start the update check before the splash so the HTTP request overlaps
    # the splash sleep; the notice is printed once the main screen is up.
    checker = None
    if not args.no_update_check:
        checker = UpdateChecker(VERSION)
        checker.start()

    show_splash()

    if args.no_dns and args.no_http and args.no_https:
        print(tag("[-] All servers disabled, nothing to do."))
        return 1

    ip = args.ip or detect_local_ip()
    embedded = get_embedded_zip()
    base_dir = resolve_dir(args.base)
    overrides_dir = resolve_dir(args.overrides)
    if (not base_dir or not os.path.isdir(base_dir)) and embedded is None:
        print(tag("[-] No base directory specified and no embedded frontend archive."))
        print(_style("    Pass --base <dir> to serve from disk, or rebuild with tools/build_host.py.", 2))
        return 1

    print(build_banner())
    print(tag(f"[+] Spoofing '{args.target}' -> {ip}"))
    print(_style("    -> Blocking all other domains", 2))
    if args.verbose:
        print(tag("[+] Verbose mode: per-request DNS/HTTP logging enabled."))

    guide = GuideStatus(logger=print if not args.verbose else (lambda *a, **k: None))
    dns_logger = print if args.verbose else (lambda *a, **k: None)

    dns = None
    if not args.no_dns:
        try:
            dns = DNSServer(("0.0.0.0", args.dns_port), args.target, ip, logger=dns_logger, guide=guide)
        except OSError as exc:
            print(tag(f"[-] Could not bind DNS port {args.dns_port} (required): {exc}"))
            hint = _bind_hint("DNS", args.dns_port, exc)
            if hint:
                print(_style(hint, 2))
            return 1
        threading.Thread(target=dns.serve_forever, daemon=True).start()
        print(tag(f"[+] DNS server on UDP {args.dns_port}"))

    httpd = None
    if args.http_port is not None and not args.no_http:
        try:
            httpd = build_http_server(
                "0.0.0.0", args.http_port, base_dir, overrides_dir,
                args.target if args.strict_host else None,
                embedded_zip=embedded,
                quiet=not args.verbose,
                guide=guide,
            )
        except OSError as exc:
            print(tag(f"[-] Could not bind HTTP port {args.http_port}: {exc}"))
            hint = _bind_hint("HTTP", args.http_port, exc)
            if hint:
                print(_style(hint, 2))
            print(_style("    Continuing without HTTP.", 2))
        if httpd is not None:
            threading.Thread(target=httpd.serve_forever, daemon=True).start()
            print(tag(f"[+] HTTP server on TCP {args.http_port}"))

    httpsd = None
    if not args.no_https:
        try:
            httpsd = build_http_server(
                "0.0.0.0", args.https_port, base_dir, overrides_dir,
                args.target if args.strict_host else None,
                embedded_zip=embedded,
                quiet=not args.verbose,
                guide=guide,
            )
            cert_pem, key_pem = get_server_cert()
            if cert_pem is None:
                print(tag("[-] Could not obtain an HTTPS certificate for the server."))
                print(_style("    Run the build (make host) or install the 'openssl' command.", 2))
                return 1
            cert_file = tempfile.NamedTemporaryFile(delete=False, suffix=".pem")
            cert_file.write(cert_pem.encode("ascii"))
            cert_file.close()

            key_file = tempfile.NamedTemporaryFile(delete=False, suffix=".pem")
            key_file.write(key_pem.encode("ascii"))
            key_file.close()

            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(certfile=cert_file.name, keyfile=key_file.name)
            httpsd.socket = context.wrap_socket(httpsd.socket, server_side=True)

            # Best-effort cleanup: on Windows, antivirus/indexing scanners may
            # briefly hold the files open and make os.unlink fail. The cert is
            # already loaded into the SSL context, so leaking the temp file is
            # harmless — only the warning matters.
            for fname in (cert_file.name, key_file.name):
                try:
                    os.unlink(fname)
                except OSError as exc:
                    print(tag(f"[-] Could not remove temp {fname}: {exc}"))
        except OSError as exc:
            print(tag(f"[-] Could not bind HTTPS port {args.https_port} (required): {exc}"))
            hint = _bind_hint("HTTPS", args.https_port, exc)
            if hint:
                print(_style(hint, 2))
            return 1
        threading.Thread(target=httpsd.serve_forever, daemon=True).start()
        print(tag(f"[+] HTTPS server on TCP {args.https_port}"))

    if dns is None and httpd is None and httpsd is None:
        print(tag("[-] No servers could be started (DNS and HTTPS are required)."))
        return 1

    if httpd is not None or httpsd is not None:
        if embedded is not None:
            if len(embedded.namelist()) == 0:
                print(tag("[-] Embedded archive is empty (0 files) — nothing to serve."))
        elif os.path.isdir(base_dir):
            print(tag(f"[+] Serving files from {os.path.normpath(base_dir)}"))
        if args.strict_host:
            print(tag(f"[+] Host header restriction: only '{args.target}'"))

    # Bounded wait for the update check; if it hasn't finished by now the
    # notice is skipped silently (best-effort only).
    if checker is not None:
        checker.thread.join(1.0)
        notice = checker.notice()
        if notice:
            print(notice)

    print(tag(f"\n[+] Set your PS5 DNS to {_style(ip, 1, 33)}."))
    print(tag("[+] Waiting for PS5 connection...\n"))
    try:
        while True:
            threading.Event().wait(3600)
    except KeyboardInterrupt:
        print(tag("\n[-] Shutting down."))
        if dns:
            dns.shutdown()
            dns.server_close()
        if httpd:
            httpd.shutdown()
            httpd.server_close()
        if httpsd:
            httpsd.shutdown()
            httpsd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

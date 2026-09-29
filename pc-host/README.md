# PC Host (PSVietHoa-Webkit-AutoLoader)

A zero-dependency Python script that turns your PC into a local DNS + HTTPS
server for the PSVietHoa AutoLoader. No `pip install` required — only the
Python 3 standard library.

## What it does

- **DNS (UDP 53):** resolves `manuals.playstation.net` (or any `--target`)
  to your PC's local IP so the PS5's User's Guide page is served by you.
  Every other domain gets `NXDOMAIN`, which blocks PS5 telemetry and internet
  access entirely.
- **HTTPS (TCP 443):** serves files from the overrides directory first, the
  embedded frontend archive second, and the base frontend directory last, so
  you can hot-swap individual files without touching the frontend source.
  The HTTPS server carries a self-signed certificate for the spoofed domain
  (generated at build time and embedded into the script).
- **HTTP (optional, `--http-port`):** same content over plain HTTP, mainly
  for browser-side testing without a cert.

The built `psviethoa-webkit-autoloader-host.py` (from `tools/build_host.py`) carries the
frontend/autoloader files inside the script itself, so it is fully portable
and serves them straight from memory.

## Usage

```bash
cd pc-host
python3 host.py
```

Options:

```text
--ip 1.2.3.4              IP to point the spoofed domain at (default: auto-detect)
--target domain           Domain to spoof (default: manuals.playstation.net)
--base <dir>              Base frontend directory (optional; served from disk only
                          when the embedded archive is absent, e.g. --base ../frontend/autoloader)
--overrides ./overrides   Overrides directory
--dns-port 53             DNS UDP port
--https-port 443          HTTPS TCP port
--http-port 8080          Also serve plain HTTP on this port (optional)
--strict-host             Only serve requests with Host: <target>
--no-dns                  Disable the DNS server
--no-http                 Disable the HTTP server
--no-https                Disable the HTTPS server
--no-update-check         Skip the GitHub update check at startup
```

## Overrides

Drop any file into `pc-host/overrides/` mirroring the base directory layout.
Requests check overrides first, so this wins over the base file:

```
overrides/
└── app/
    └── index.html   # served instead of base/app/index.html
```

# PSVietHoa AutoLoader: Architecture

A persistent entry point for PS5 payloads that runs a WebKit/kernel exploit chain
and autoloads your payloads fully offline. Two exploit chains are bundled and
selected by firmware:

- **umtx2** (FW 1.00–5.50) — idlesauce umtx2 chain (`umtx2/`).
- **relapse** (FW 7.00–13.60) — ntfargo's relapse chain (`relapse/`), the only
  chain for the whole high-firmware range (the fork's headline chain).

Both chains converge on the same result: a `PSVH00001` homescreen app that runs the
exploit, boots elfldr, and autoloads your payload through it.

## Repository layout

| Path | Purpose |
|---|---|
| `frontend/autoloader/` | The autoloader UI, served by both the installer and the PC host |
| `frontend/installer-page/` | Wrapper page that drives the one-time AppCache caching |
| `frontend/pointer/` | Stable `/app/index.html` entry that verifies the cache and points into the versioned app dir |
| `pc-host/` | The PC host script (`host.py`) + overrides for the bootstrap flow |
| `src/` | Native installer ELF (HTTP server, app installer, browser launcher) |
| `include/` | Headers, incl. generated `wkali_version.h` and `file_registry.{h,c}` |
| `patches/` | The umtx2 and relapse autoloader patch files |
| `tools/` | Build, version, icon, registry scripts, and dependency downloader |
| `assets/` | Icon source and PS5 app metadata templates |
| `third_party/` | `umtx2`, `relapse` and `ps5-unified-autoloader` sources (vendored snapshots; see `third_party/VENDORED.md`). The `slopkit`/`ps5-elfldr` snapshots remain on disk as legacy archives, no longer built. |

## Two setup flows

**Installer ELF (already jailbroken).** Send `psviethoa-webkit-autoloader-installer_v*.elf` to the
console (elfldr or Payload Manager). It opens the browser once to cache the frontend via AppCache,
creates the `PSVH00001` app only after that cache succeeds, then exits. From then on the app
runs the selected chain (relapse by default on 7.00–13.60) offline from the cache.

**PC host (not jailbroken).** Run `psviethoa-webkit-autoloader-host_v*.py` / `.exe` on a PC, point
the console's DNS at it, and open the User's Guide. The host spoofs `manuals.playstation.net`
(DNS + self-signed HTTPS) and serves the same frontend, but autoloads the **installer ELF**
instead of the unified-autoloader — so this flow installs the homescreen app.

## Frontend (`frontend/autoloader/`)

- A splash screen, a log terminal and a progress bar. The exploit runs in a **hidden**
  same-origin iframe. On load, `app.js` picks the chain from the firmware in
  the user-agent (`PlayStation 5/x.xx`): **umtx2** for 1.00–5.50 and **relapse**
  for 7.00–13.60 (its full supported list).
- A `FORCE_EXPLOIT` build-time override (`auto | umtx2 | relapse`; or a `?force=`
  query) bypasses the table so a specific chain can be exercised on any
  firmware; the exploit's own firmware guard still applies.
- The relapse chain logs to `#console` (same shape as umtx2), so `mirrorRelapse()`
  streams its lines into the UI and drives coarse milestones from its key log lines
  ("Starting WebKit exploit", "ARW ready", "elfldr is listening…", "autoloaded …").
  Its offsets are loaded with a plain relative URL (no dynamic cache-buster) so the
  AppCache manifest entry matches offline.
- umtx2 auto-runs its chain via the `on_load_autorun` sessionStorage key (set by
  `app.js` before arming); relapse auto-runs on load and reads `?autoload=<name>`
  from its iframe URL.
- On `window.load` the iframe is armed; at script parse it is blanked to
  `about:blank` so a WebProcess-crash page restore never auto-runs the chain.
- `app.js` mirrors the selected chain's output into the log (relapse console lines,
  stage changes and errors) and receives the `?autoload` result via `postMessage`.
  The relapse chain additionally drives coarse progress milestones from its key
  log lines.

`payload.elf` is a virtual name: the PC host serves the installer ELF there, the homescreen app
serves the real unified-autoloader. All exploits autoload the same `payload.elf`. umtx2 (FW
1.00–5.50) boots its **own bundled elfldr** (`/app/<version>/umtx2/payloads/elfldr-ps5.elf`, kept
from the umtx2 sources like stock umtx2); relapse (7.00–13.60) boots its **own bundled elfldr**
(`/app/<version>/relapse/payloads/elfldr-ps5-1360.elf`, exactly as upstream ships it).

## Native installer (`src/`)

A PS5 payload running a `libmicrohttpd` server on port **18182**:

1. Serves the staged frontend: installer-page at `/`, the pointer at `/app/index.html`
   and the versioned autoloader at `/app/<version>/`.
2. Frontend files are embedded **compressed** (raw DEFLATE via `src/inflate.c`, the vendored
   puff) and inflated on demand.
3. The browser caches everything through `cache.appcache`, then hits `/install`. The ELF
   installs/updates the `PSVH00001` homescreen app and shuts down only after the cache is
   confirmed complete. If the user closes the browser mid-load, no `/install` is ever hit,
   so no shortcut is created/updated and the previously-installed version stays untouched (the
   installer process simply keeps running until a subsequent run kills it). The master URL
   carries `?v=<version>` so stale cached entries are avoided.
4. The app's `deeplinkUri` is the stable pointer `http://127.0.0.1:18182/app/index.html`.

### Cache layout and partial-cache protection

The staged autoloader lives under `/app/<full-version>/` (see the Makefile staging rule), so
every build's assets have version-unique URLs. This, plus two generated extras, protects the
cache against a user closing the browser mid-download:

- **Versioned app dir** (`/app/<version>/…`): an interrupted *update* can never clobber the
  files the currently-installed version points at — they live at their own URLs. An
  interrupted first cache never creates a shortcut at all, because the app is only installed
  after `/install` fires.
- **Pointer page** (`/app/index.html`, generated from `frontend/pointer/`): the stable
  deeplink target. It fetches the version's `__complete__` marker and only then redirects
  into `/app/<version>/index.html`; on a mismatch it shows "Cache incomplete" instead of
  loading a broken chain.
- **`__complete__` marker** (`/app/<version>/__complete__`, content = the full version): the
  **last** entry of the AppCache `CACHE:` section. If the marker is cached (with matching
  content), every file listed before it was downloaded — so the pointer can only ever point
  at a fully-cached directory.

Because the exploit iframe URLs, `payloads/` and the app
entry page's own `style.css`/`app.js`/`logo.svg`/`favicon.svg` references are all relative
(never `/app/...` absolute), `app.js`, the exploit patches and the app pages are untouched by
the versioned layout and resolve correctly under `/app/<version>/`, on the PC host and in the
dev server.

## PC host (`pc-host/host.py`)

- Binds DNS port 53 and HTTPS port 443 (both required). A self-signed certificate is generated
  on the fly with `openssl`.
- Redirects `manuals.playstation.net` to the PC and blocks other telemetry domains; the User's
  Guide URL is mapped to the served frontend.
- The frontend (+ filtered chain assets) is embedded in the script as a base64 zip and served
  from memory. `HOST_PAYLOAD` replaces the autoload payload with the installer ELF.

## Build system

- `make`: `all` (ELF), `host` (standalone host script), `dev` (local preview server),
  `umtx2-prepare`, `relapse-prepare`, `payload-deps`, `version`, `icons`,
  `clean`.
- `tools/gen_file_registry.py` walks the staged `frontend/dist/`, compresses each file (raw
  DEFLATE) and emits the C registry + the AppCache manifest. It pins the version from the
  staging handoff (`dist/VERSION`), writes the `__complete__` marker, substitutes the tokens
  in the pointer page and the app's versioned `index.html`, lists the pointer and marker LAST
  in the manifest, and replaces the `[[EXPLOIT_MODE]]` token in `app.js` from the
  `FORCE_EXPLOIT` env (`auto | umtx2 | relapse`, default `auto`). Unused
  exploit payloads and assets are filtered out (each chain keeps only what it boots plus
  its kexp — relapse drops its three optional payload ELFs).
- `build_release.sh` builds the ELF in a Dockerized SDK and the host script; CI
  (`.github/workflows/release.yml`) produces the versioned artifacts and the Windows `.exe`.
  `FORCE_EXPLOIT` is forwarded into the Docker build explicitly.

## Relapse integration

`relapse` is a pristine vendored snapshot in `third_party/relapse` (see
`third_party/VENDORED.md`; submodule-compatible). The build copies it to the gitignored
`frontend/autoloader/relapse/` and applies `patches/relapse-autoload.patch` there
(`tools/apply_relapse_patch.sh`, run automatically by the Makefile).

The patch (`relapse/src/main.js`, `relapse/src/kexp.js`, `relapse/src/site.js`):

- Adds `?autoload=<name>`: after the kernel chain finishes and elfldr is up on port 9021,
  the named payload is fetched from the shared payloads dir (`../payloads/`, i.e.
  `/app/<version>/payloads/`), mapped into kernel memory and pushed through the elfldr via
  the chain's own socket transport (`loadAutoloadPayload` in `kexp.js` — the same mechanism
  upstream uses for its optional payloads, just pointed at the autoloader's file).
- Reports the result to the parent page: `{type:"wkal", kind:"autoload", ok:true, bytes}`
  (or `{ok:false, why}`), plus a chain-level failure message from `site.js`. Without the
  `autoload` query key the standalone "press R2" flow is unchanged (and the R2 listener is
  skipped entirely when autoloading).
- Loads the offsets file with a plain relative URL (`offsets/<fw>.js`) instead of the
  upstream `?v=<Date.now()>` cache-buster — AppCache matches URLs exactly, so the manifest
  entry must be stable to serve the offsets offline. All other relapse asset paths are
  already plain relative ones.
- The chain boots its **own** bundled `payloads/elfldr-ps5-1360.elf` + `kexp_2026_05_25.bin`;
  the optional `kstuff.elf` / `shadowmountplus.elf` / `etaHEN.elf` stay in the repo but are
  filtered out of the autoloader bundles (the autoloader sends the shared `payload.elf`
  instead — see `include_in_registry()` in `tools/gen_file_registry.py`).

The iframe URL is the canonical `relapse/index.html?autoload=payload.elf&v=1`, listed
verbatim in the AppCache manifest (`relapse_iframe_url()` in `tools/gen_file_registry.py`;
keep in sync with `RELAPSE_URL` in `app.js`).

To update relapse: refresh `third_party/relapse` (see `third_party/VENDORED.md`), re-run
`tools/apply_relapse_patch.sh`, and regenerate `patches/relapse-autoload.patch` if it no
longer applies (the script verifies integration markers after applying).

## Umtx2 integration

`umtx2` is a pinned, **pristine** submodule. The build copies its `document/en/ps5` directory
(flattened to the copy root, so the exploit serves at `/app/umtx2/`) to the gitignored
`frontend/autoloader/umtx2/` and applies `patches/umtx2-autoload.patch` there
(`tools/apply_umtx2_patch.sh`, run automatically by the Makefile). The bundled payloads dir is
pruned down to `elfldr-ps5.elf` — umtx2 boots its **own** elfldr, exactly like stock umtx2.

The patch (in `umtx2/main.js`):

- Keeps `load_local_elf("elfldr-ps5.elf")` loading umtx2's own `payloads/elfldr-ps5.elf` (stock
  behavior).
- Adds an optional `base` parameter to `load_payload_into_elf_store_from_local_file` and
  `load_local_elf` so different base paths can be used without duplicating the fetch logic.
- Threads `payload_info.wkalBase` through the main loop's fetch call, so the payload is
  always fetched via the same code path as a regular button press.
- `?autoload=<name>`: after elfldr loads and `switchPage("payloads-view")` completes,
  waits 4 s (it must bind 9021), then fires a synthetic `MAINLOOP_EXECUTE_PAYLOAD_REQUEST`
  event with `{fileName, wkalBase: "../payloads/", toPort: 9021, wkalAutoload: true}`.
  The main loop processes it identically to a button press.
- The main loop's success and error handlers post `{type:"wkal", kind:"autoload", ok, bytes}`
  (or `{ok:false, why}`) to the parent page when `payload_info.wkalAutoload` is set.
- Neutralizes the `confirm()` dialogs around the elfldr probe so the chain runs unattended.

To update umtx2: `git -C third_party/umtx2 fetch && git -C third_party/umtx2 checkout <commit>`,
re-run the script, and regenerate `patches/umtx2-autoload.patch` if it no longer applies. Keep the
`?v=` cache-buster on `UMTX2_IFRAME_URL` in `app.js`/`gen_file_registry.py` in sync.

## Payload dependency

`tools/download_deps.sh` (the Makefile's `payload-deps` target) downloads
`frontend/autoloader/payloads/payload.elf` from the `ps5-unified-autoloader` submodule's pinned
GitHub release, sha256-verifies it, and caches the digest in a `.sha256` sidecar so offline
rebuilds work. Bump the submodule to pick up a newer release.

## Versioning

The base version lives in `include/wkali.h` (`WKAL_VERSION`). `tools/gen_version.py` produces
the full version — `<base>` for stable (`BUILD_TYPE=stable`) or `<base>-dev-<suffix>` for dev —
and regenerates `include/wkali_version.h`, `assets/param.json` and the version placeholders in
the pages. The full version also names the staged app directory (`/app/<version>/`), the
pointer page's redirect/marker targets and the `__complete__` content, so the whole cache
layout is version-keyed. It ends up in the installer ELF, the host banner and the artifact
names.

## Conventions

- The ELF serves the autoloader under `/app/<version>/`; the PC host maps `/app/` to its root.
- Never put `manifest="..."` on the autoloader page — caching is the installer page's job.
- Never edit `third_party/umtx2/` or `third_party/relapse/`, nor the generated
  `frontend/autoloader/umtx2/` / `frontend/autoloader/relapse/` — edit the patch files instead.
- The pointer page (`frontend/pointer/`) and the manifest ordering are the partial-cache guard:
  keep `__complete__` the LAST cache entry and never reorder it before the pointer.
- After changing `host.py`, rebuild the host (`make host` / `build_release.sh`).

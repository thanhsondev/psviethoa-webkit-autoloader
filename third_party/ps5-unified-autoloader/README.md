# ps5-unified-autoloader

A standalone PS5 ELF payload that automates loading payloads. This is intended for integration into jailbreak chains rather than direct end-user usage.

## What it does

When loaded via elfldr (e.g. as part of your jailbreak chain), `autoloader.elf`:

1. **Kills the entry point app** (YouTube `PPSA01650`/`01651`/`01652` or Artemis Lua games like Aerial Life, Aibeya, etc.) if it is running
2. **Kills BD Disc Player** (NPXS40140) if it is running, using a careful suspend→wait→kill sequence
3. **Waits for elfldr** to be ready on port 9021 (up to 10 seconds)
4. **Looks for** the `autoload.txt` configuration file in the following order (highest priority first):
   - **App-specific directories on USB** (`/mnt/usb[0-7]/ps5_autoloader_<app>/autoload.txt`, where `<app>` is `bdjb` for BD Disc Player or the Title ID of the entry point app, e.g. `PPSA01650` for YouTube)
   - **App-specific directory in `/data`** (`/data/ps5_autoloader_<app>/autoload.txt`)
   - **Generic directories on USB** (`/mnt/usb[0-7]/ps5_autoloader/autoload.txt`)
   - **Generic directory in `/data`** (`/data/ps5_autoloader/autoload.txt`)
5. **If found**: launches each payload listed in the config via elfldr
6. **If not found**: automatically starts the bundled **Payload Manager**

## autoload.txt format

```
# This is a comment — ignored
@sync                  # move config directory to /data if all loads succeed
mypayload.elf          # loaded from the same directory as this autoload.txt
anotherpayload.elf
!1000                  # sleep 1000 ms before next entry
third_payload.elf
```

- One entry per line
- Filenames are resolved **relative to the directory containing autoload.txt**
  (e.g. if config is on `/mnt/usb0/ps5_autoloader/autoload.txt`, then
  `mypayload.elf` resolves to `/mnt/usb0/ps5_autoloader/mypayload.elf`)
- Absolute paths (starting with `/`) are used as-is
- Lines starting with `#` are comments
- Lines starting with `!` are sleep commands: `!<ms>` sleeps for that many milliseconds
- Lines starting with `@` are directives:
  - `@sync`: If loaded from a USB drive, this moves the entire active configuration directory (including all payloads and `autoload.txt`) to the internal `/data` partition on the PS5. The process runs only after all payloads have loaded successfully with zero errors. It clears the target internal directory, copies the files, verifies them byte-by-byte, and then deletes the folder from the USB drive so that subsequent boots run locally without needing the USB.

## Building

### Requirements
- Docker
- git (with submodules, only needed for `-b` builds)

### Clone
```bash
git clone https://github.com/itsPLK/ps5-unified-autoloader.git
cd ps5-unified-autoloader
```

### Build (download pre-built pldmgr — recommended)
```bash
./build_release.sh
# or explicitly:
./build_release.sh -d
```

### Build (compile pldmgr from source)
```bash
git submodule update --init --recursive
./build_release.sh -b
```

This uses pldmgr's own Docker image (which includes libmicrohttpd, mbedTLS, libcurl)
to build pldmgr, then uses a separate lean SDK image to build the autoloader.

### Output
```
autoloader_v0.1.0_abc1234.elf
```

## Structure

```
autoloader.elf          ← load this via elfldr
  └─ pldmgr.elf         ← embedded fallback (launched if no autoload.txt found)
```
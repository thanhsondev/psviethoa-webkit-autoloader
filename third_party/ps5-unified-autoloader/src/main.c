/*
 * ps5-autoloader — main entry point
 *
 * Execution flow:
 *   1. Handle WebKit browser (if running, return to Home screen)
 *   2. Kill entry app (YouTube / Artemis Lua / Yarpe / mast1c0re if running)
 *   3. Kill BD Disc Player (if running, with proper timing)
 *   4. Wait for elfldr to be ready on port 9021
 *   5. Scan USB0-7, then /data for autoload.txt
 *   6a. If found: iterate lines, launch each .elf from the config directory
 *   6b. If not found: send embedded pldmgr.elf to elfldr (fallback)
 */

#include "autoloader.h"
#include "app_killer.h"
#include "launcher.h"
#include "notification.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sync.h"

/*
 * pldmgr.elf is embedded at build time via:
 *   xxd -i pldmgr.elf > pldmgr_elf.c
 * which generates:
 *   unsigned char pldmgr_elf[] = { ... };
 *   unsigned int  pldmgr_elf_len = ...;
 */
#include "pldmgr_elf.c"

/* -----------------------------------------------------------------------
 * find_autoload_config
 *
 * Searches for ps5_autoloader/autoload.txt in order:
 *   /mnt/usb0 .. /mnt/usb7  (highest priority)
 *   /data                   (fallback)
 *
 * Writes the found path into out_buf (size out_size).
 * Returns 0 on success (found), -1 if not found.
 * ----------------------------------------------------------------------- */
static int find_autoload_config(char *out_buf, size_t out_size) {
    struct stat st;

    /* Check Title ID specific paths first if we have a title ID */
    if (g_entry_point_id[0] != '\0') {
        for (int i = 0; i < USB_COUNT; i++) {
            snprintf(out_buf, out_size, "%s/ps5_autoloader_%s/autoload.txt", USB_BASES[i], g_entry_point_id);
            if (stat(out_buf, &st) == 0) {
                printf("[autoloader] Found config on USB%d: %s\n", i, out_buf);
                fflush(stdout);
                return 0;
            }
        }

        snprintf(out_buf, out_size, "%s/ps5_autoloader_%s/autoload.txt", DATA_BASE, g_entry_point_id);
        if (stat(out_buf, &st) == 0) {
            printf("[autoloader] Found config in /data: %s\n", out_buf);
            fflush(stdout);
            return 0;
        }
    }

    /* Check each USB base in priority order */
    for (int i = 0; i < USB_COUNT; i++) {
        snprintf(out_buf, out_size, "%s/%s", USB_BASES[i], AUTOLOAD_CONFIG_SUBPATH);
        if (stat(out_buf, &st) == 0) {
            printf("[autoloader] Found config on USB%d: %s\n", i, out_buf);
            fflush(stdout);
            return 0;
        }
    }

    /* Fallback: /data */
    snprintf(out_buf, out_size, "%s/%s", DATA_BASE, AUTOLOAD_CONFIG_SUBPATH);
    if (stat(out_buf, &st) == 0) {
        printf("[autoloader] Found config in /data: %s\n", out_buf);
        fflush(stdout);
        return 0;
    }

    out_buf[0] = '\0';
    return -1;
}


/* -----------------------------------------------------------------------
 * run_autoload_sequence
 *
 * Opens config_path and processes each line:
 *   - Empty lines and '#' comments are skipped
 *   - Lines starting with '@' are directives (e.g. @sync)
 *   - Lines starting with '!' are sleep commands: !<ms>
 *   - Everything else is treated as a filename relative to config_dir
 * ----------------------------------------------------------------------- */
static void run_autoload_sequence(const char *config_path) {
    /* Derive the directory containing the config file */
    char config_dir[512];
    strncpy(config_dir, config_path, sizeof(config_dir) - 1);
    config_dir[sizeof(config_dir) - 1] = '\0';

    char *last_slash = strrchr(config_dir, '/');
    if (last_slash) {
        *(last_slash + 1) = '\0'; /* keep trailing slash */
    } else {
        config_dir[0] = '\0';
    }

    printf("[autoloader] Config dir: %s\n", config_dir);
    fflush(stdout);

    FILE *f = fopen(config_path, "r");
    if (!f) {
        printf("[autoloader] Failed to open config: %s\n", config_path);
        fflush(stdout);
        autoloader_notify("Autoload: failed to open config");
        return;
    }

    char line[512];
    int launched = 0;
    int errors = 0;
    int should_sync = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip comments if any */
        char *comment = strchr(line, '#');
        if (comment) {
            *comment = '\0';
        }

        /* Strip trailing newline / carriage return */
        line[strcspn(line, "\r\n")] = '\0';

        /* Strip trailing whitespace */
        int len = strlen(line);
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }

        /* Skip empty lines */
        if (line[0] == '\0')
            continue;

        printf("[autoloader] Processing: %s\n", line);
        fflush(stdout);

        if (line[0] == '@') {
            /* Directive */
            if (strcmp(line, "@sync") == 0) {
                should_sync = 1;
            }
            continue;
        } else if (line[0] == '!') {
            /* Sleep command: !<ms> */
            int ms = atoi(line + 1);
            if (ms > 0) {
                printf("[autoloader] Sleeping %d ms\n", ms);
                fflush(stdout);
                usleep((useconds_t)ms * 1000);
            }
        } else {
            /* ELF/BIN payload — resolve relative to config directory */
            char full_path[1024];
            if (line[0] == '/') {
                /* Absolute path — use as-is */
                snprintf(full_path, sizeof(full_path), "%s", line);
            } else {
                snprintf(full_path, sizeof(full_path), "%s%s", config_dir, line);
            }

            struct stat st;
            if (stat(full_path, &st) == 0) {
                autoloader_notify("Launching: %s", line);
                if (launch_elf_from_file(full_path) != 0) {
                    autoloader_notify("Failed to launch: %s", line);
                    errors++;
                } else {
                    launched++;
                    /* Small delay to give elfldr time to receive and process */
                    usleep(500000); /* 500 ms */
                }
            } else {
                printf("[autoloader] Not found: %s\n", full_path);
                fflush(stdout);
                autoloader_notify("Not found: %s", line);
                errors++;
            }
        }
    }

    fclose(f);
    printf("[autoloader] Autoload sequence complete. Launched %d payload(s).\n", launched);
    fflush(stdout);
    autoloader_notify("Autoload complete (%d payload(s) launched)", launched);

    if (should_sync && errors == 0) {
        try_sync_usb_to_data(config_path);
    }
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void) {
    printf("[autoloader] ps5-autoloader v" AUTOLOADER_VERSION " (" __DATE__ " " __TIME__ ") starting\n");
    fflush(stdout);

    /* Step 1: handle WebKit browser if running (navigate to Home) */
    handle_browser_app();

    /* Step 2: kill entry point app (YouTube or Artemis) if running (simple SIGKILL) */
    kill_entry_app();

    /* Step 3: kill BD Disc Player if running (suspend → wait → SIGKILL → LncKill) */
    kill_disc_player();

    /* Step 4: wait for elfldr to be ready on port 9021 */
    if (wait_for_elfldr() != 0) {
        autoloader_notify("ERROR: elfldr not available. Aborting.");
        printf("[autoloader] ERROR: elfldr did not become available. Aborting.\n");
        fflush(stdout);
        return -1;
    }

    /* Step 5: locate autoload.txt */
    char config_path[512];
    int found = (find_autoload_config(config_path, sizeof(config_path)) == 0);

    if (found) {
        /* Step 6a: run the autoload sequence from config */
        autoloader_notify("Found autoload config:\n%s", config_path);
        run_autoload_sequence(config_path);
    } else {
        /* Step 6b: no config — fall back to embedded pldmgr */
        printf("[autoloader] No autoload config found. Starting Payload Manager...\n");
        fflush(stdout);
        if (launch_elf_from_memory(pldmgr_elf, pldmgr_elf_len) != 0) {
            autoloader_notify("ERROR: failed to launch Payload Manager");
            printf("[autoloader] ERROR: failed to launch pldmgr\n");
            fflush(stdout);
            return -1;
        }
    }

    return 0;
}

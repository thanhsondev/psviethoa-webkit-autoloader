#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "wkali.h"
#include "webkit_cleaner.h"

#define USER_HOME_BASE "/user/home"

/* Recursively delete all contents of dir_path (files + subdirs),
 * but preserve dir_path itself. Returns 0 on success, -1 on error. */
static int rm_rf_contents(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return -1;

    struct dirent *entry;
    int ret = 0;

    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) { ret = -1; continue; }

        if (S_ISDIR(st.st_mode)) {
            if (rm_rf_contents(path) != 0) ret = -1;
            if (rmdir(path) != 0) ret = -1;
        } else {
            if (unlink(path) != 0) ret = -1;
        }
    }

    closedir(d);
    return ret;
}

int wkali_clear_webkit_data(void) {
    /* Only the foreground (signed-in) user's data is cleared. */
    extern int sceUserServiceGetForegroundUser(int *);
    int user_id = -1;
    if (sceUserServiceGetForegroundUser(&user_id) != 0 || user_id <= 0) {
        wkali_log("[WKALI] Cannot determine current user\n");
        return -1;
    }

    char ws_path[512];
    snprintf(ws_path, sizeof(ws_path), "%s/%08x/webkit/shell",
             USER_HOME_BASE, (unsigned int)user_id);

    struct stat st;
    if (stat(ws_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        wkali_log("[WKALI] No webkit/shell for current user %08x, nothing to clear\n",
                  (unsigned int)user_id);
        return 0;
    }

    wkali_log("[WKALI] Clearing %s ...\n", ws_path);
    if (rm_rf_contents(ws_path) == 0) {
        wkali_log("[WKALI] Cleared webkit/shell for current user %08x\n",
                  (unsigned int)user_id);
        return 0;
    }

    wkali_log("[WKALI] Errors clearing %s\n", ws_path);
    return -1;
}

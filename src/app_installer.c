/*
 * PS5 Homescreen App Installer for the PSVietHoa AutoLoader Installer.
 * Based on the original implementation in ftpsrv by John Törnblom
 * and Payload Manager by PLK.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_installer.h"
#include "wkali.h"
#include <ps5/kernel.h>

#define INCASSET(name, file)                                                   \
  __asm__(".section .rodata\n"                                                 \
          ".global " #name "\n"                                                \
          ".global " #name "_end\n"                                            \
          ".global " #name "_size\n"                                           \
          ".align 16\n" #name ":\n"                                            \
          ".incbin \"" file "\"\n" #name "_end:\n" #name "_size:\n"            \
          ".quad " #name "_end - " #name "\n"                                  \
          ".previous\n");                                                      \
  extern const uint8_t name[];                                                 \
  extern const size_t name##_size;

INCASSET(param_json, "assets/param.json");
INCASSET(icon0_png, "assets/icon0.png");

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);

/* Path buffers below are built as /user/app/<title_id>/... — title IDs are
 * fixed 9-char strings, so 256 bytes can never truncate. This guard keeps
 * it that way if WKAL_TITLE_ID is ever changed. */
_Static_assert(sizeof(WKAL_TITLE_ID) <= 16, "WKAL_TITLE_ID too long for path buffers");

static int mkdir_p(const char *path, mode_t mode) {
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t len = strlen(tmp);
  if (len == 0)
    return 0;
  if (tmp[len - 1] == '/')
    tmp[len - 1] = '\0';
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
    return -1;
  }
  return 0;
}

static int install_file(const char *path, const uint8_t *data, size_t size) {
  FILE *f;
  if (!(f = fopen(path, "wb"))) {
    return -1;
  }
  if (fwrite(data, size, 1, f) != 1) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int install_app(const char *title_id, const char *dir) {
  int (*sceAppInstUtilAppInstallTitleDir)(const char *, const char *, void *) =
      0;
  const char *nid = "Wudg3Xe3heE";
  uint32_t handle;

  if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
    sceAppInstUtilAppInstallTitleDir =
        (void *)kernel_dynlib_resolve(-1, handle, nid);
  }

  if (sceAppInstUtilAppInstallTitleDir) {
    return sceAppInstUtilAppInstallTitleDir(title_id, dir, 0);
  }

  return sceAppInstUtilAppInstallAll(0);
}

static int needs_update(const char *path, const uint8_t *expected_data,
                        size_t expected_size) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 1;
  if ((size_t)st.st_size != expected_size)
    return 1;

  FILE *f = fopen(path, "rb");
  if (!f)
    return 1;

  uint8_t *buf = malloc(expected_size);
  if (!buf) {
    fclose(f);
    return 1;
  }

  if (fread(buf, 1, expected_size, f) != expected_size) {
    free(buf);
    fclose(f);
    return 1;
  }
  fclose(f);

  int mismatch = memcmp(buf, expected_data, expected_size);
  free(buf);

  return mismatch != 0;
}

int wkali_install_app_if_needed(void) {
  const char *title_id = WKAL_TITLE_ID;
  char base_dir[256];
  char param_path[256];
  char icon_path[256];

  snprintf(base_dir, sizeof(base_dir), "/user/app/%s", title_id);
  snprintf(param_path, sizeof(param_path), "/user/app/%s/sce_sys/param.json",
           title_id);
  snprintf(icon_path, sizeof(icon_path), "/user/app/%s/sce_sys/icon0.png",
           title_id);

  int update_needed = 0;
  struct stat st;
  if (stat(base_dir, &st) != 0) {
    update_needed = 1;
  } else {
    if (needs_update(param_path, param_json, param_json_size))
      update_needed = 1;
    if (needs_update(icon_path, icon0_png, icon0_png_size))
      update_needed = 1;
  }

  if (!update_needed) {
    return 0; /* Already installed and up to date */
  }

  if (stat(base_dir, &st) == 0) {
    wkali_log("[WKALI] Updating existing app launcher (%s)...\n", title_id);
    wkali_notify("Updating PSVietHoa AutoLoader App...");
  } else {
    wkali_log("[WKALI] Installing browser launcher app (%s)...\n", title_id);
    wkali_notify("Installing PSVietHoa AutoLoader App...");
  }

  int err;
  if ((err = sceAppInstUtilInitialize())) {
    wkali_log("[WKALI] sceAppInstUtilInitialize: error 0x%08X\n", err);
    return -1;
  }

  char sce_sys_dir[256];
  snprintf(sce_sys_dir, sizeof(sce_sys_dir), "/user/app/%s/sce_sys", title_id);
  if (mkdir_p(sce_sys_dir, 0755) != 0) {
    wkali_log("[WKALI] Failed to create app dir: %s (errno: %d)\n",
              sce_sys_dir, errno);
    sceAppInstUtilTerminate();
    return -1;
  }

  if (install_file(param_path, param_json, param_json_size)) {
    wkali_log("[WKALI] Failed to install param.json\n");
    sceAppInstUtilTerminate();
    return -1;
  }

  if (install_file(icon_path, icon0_png, icon0_png_size)) {
    wkali_log("[WKALI] Failed to install icon0.png\n");
    sceAppInstUtilTerminate();
    return -1;
  }

  if ((err = install_app(title_id, "/user/app/"))) {
    wkali_log("[WKALI] install_app: error 0x%08X\n", err);
    sceAppInstUtilTerminate();
    return -1;
  }

  wkali_log("[WKALI] Launcher app installed successfully.\n");
  wkali_notify("PSVietHoa AutoLoader App Ready!");

  sceAppInstUtilTerminate();
  return 0;
}

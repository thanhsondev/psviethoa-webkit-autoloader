#include "app_killer.h"
#include "autoloader.h"
#include "notification.h"

#include <ps5/kernel.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

char g_entry_point_id[16] = {0};

/* PS5 SDK: get per-process app info (title ID, app ID) */
typedef struct app_info {
    uint32_t app_id;
    uint64_t unknown1;
    char     title_id[14];
    char     unknown2[0x3c];
} app_info_t;

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

/* LncUtil externs — used for graceful app teardown */
extern int sceLncUtilGetAppIdOfRunningBigApp(void);
extern int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);
extern int sceLncUtilSuspendApp(uint32_t app_id);
extern int sceLncUtilKillApp(uint32_t app_id);

/* ShellUI / UserService externs */
typedef struct {
    unsigned int size;
    uint32_t userId;
} SceShellUIUtilLaunchByUriParam;

extern int sceKernelLoadStartModule(const char *path, size_t args, const void *argp,
                                    uint32_t flags, const void *pOpt, int *pRes);
extern int sceUserServiceInitialize(const void *params);
extern int sceUserServiceGetForegroundUser(int *userId);
extern int sceUserServiceGetInitialUser(int *userId);

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/**
 * Find the PID of a process by comm name.
 * Returns the PID on success, -1 if not found.
 */
static pid_t get_pid_by_name(const char *name) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t buf_size = 0;

    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0))
        return -1;

    void *buf = malloc(buf_size);
    if (!buf)
        return -1;

    if (sysctl(mib, 4, buf, &buf_size, NULL, 0)) {
        free(buf);
        return -1;
    }

    pid_t pid = -1;
    for (void *ptr = buf; ptr < (buf + buf_size);) {
        struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
        if (ki->ki_structsize < (int)sizeof(struct kinfo_proc))
            break;
        if (strncmp(ki->ki_comm, name, sizeof(ki->ki_comm)) == 0) {
            pid = ki->ki_pid;
            break;
        }
        ptr += ki->ki_structsize;
    }

    free(buf);
    return pid;
}

/* -----------------------------------------------------------------------
 * kill_entry_app
 * ----------------------------------------------------------------------- */

/**
 * Kill the entry point app (YouTube or Artemis Lua game) if running.
 *
 * It does NOT require suspending first — a direct SIGKILL to its
 * main process (eboot.bin) is sufficient and fully terminates it.
 */
int kill_entry_app(void) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t buf_size = 0;

    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) < 0) {
        printf("[autoloader] kill_entry_app: sysctl size failed\n");
        return -1;
    }

    void *buf = malloc(buf_size);
    if (!buf) {
        printf("[autoloader] kill_entry_app: malloc failed\n");
        return -1;
    }

    if (sysctl(mib, 4, buf, &buf_size, NULL, 0) < 0) {
        printf("[autoloader] kill_entry_app: sysctl query failed\n");
        free(buf);
        return -1;
    }

    int found = 0;
    int ret = 0;

    for (void *ptr = buf; ptr < (buf + buf_size);) {
        struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
        if (ki->ki_structsize < (int)sizeof(struct kinfo_proc))
            break;

        app_info_t appinfo;
        memset(&appinfo, 0, sizeof(appinfo));

        if (sceKernelGetAppInfo(ki->ki_pid, &appinfo) == 0) {
            /* Check: title ID is one of the target IDs AND comm is eboot.bin */
            int is_match = 0;
            for (int i = 0; i < AUTOKILL_EBOOT_TITLE_ID_COUNT; i++) {
                if (strcmp(appinfo.title_id, AUTOKILL_EBOOT_TITLE_IDS[i]) == 0) {
                    is_match = 1;
                    break;
                }
            }

            if (is_match && strcmp(ki->ki_comm, "eboot.bin") == 0) {
                /* Save the title ID so the autoloader can look for the specific ps5_autoloader_<title_id> config dir */
                strncpy(g_entry_point_id, appinfo.title_id, sizeof(g_entry_point_id) - 1);
                g_entry_point_id[sizeof(g_entry_point_id) - 1] = '\0';

                printf("[autoloader] kill_entry_app: found %s (PID %d, AppID 0x%04x)\n",
                       ki->ki_comm, ki->ki_pid, appinfo.app_id);
                fflush(stdout);
                found = 1;

                if (kill(ki->ki_pid, SIGKILL) == 0) {
                    printf("[autoloader] kill_entry_app: SIGKILL sent to PID %d\n", ki->ki_pid);
                    /* Give it 1.5 seconds to fully terminate and free resources */
                    usleep(1500000);
                } else {
                    printf("[autoloader] kill_entry_app: SIGKILL failed for PID %d\n", ki->ki_pid);
                    autoloader_notify("Failed to terminate %s", appinfo.title_id);
                    ret = -1;
                }
                fflush(stdout);
                break;
            }
        }

        ptr += ki->ki_structsize;
    }

    free(buf);

    if (!found) {
        printf("[autoloader] kill_entry_app: no supported eboot process found (not running)\n");
        fflush(stdout);
    }

    return ret;
}

/* -----------------------------------------------------------------------
 * kill_disc_player
 * ----------------------------------------------------------------------- */

/**
 * Kill the BD Disc Player (NPXS40140) if running.
 *
 * The disc player requires a careful teardown sequence with specific delays
 * to allow the home screen to become stable before the kill:
 *   1. sceLncUtilSuspendApp
 *   2. sleep(2)  — home screen transition stability
 *   3. SIGKILL on "SceDiscPlayer" process
 *   4. sleep(1)
 *   5. sceLncUtilKillApp
 */
int kill_disc_player(void) {
    /* Step 1: get the currently running big app */
    uint32_t app_id = (uint32_t)sceLncUtilGetAppIdOfRunningBigApp();
    if (app_id == 0xffffffff) {
        printf("[autoloader] kill_disc_player: no big app running\n");
        fflush(stdout);
        return 0; /* Nothing to do */
    }

    /* Step 2: verify it is the disc player by title ID */
    char title_id[16] = {0};
    if (sceLncUtilGetAppTitleId(app_id, title_id) != 0) {
        printf("[autoloader] kill_disc_player: could not get title ID\n");
        fflush(stdout);
        return 0;
    }

    if (strcmp(title_id, DISC_PLAYER_TITLE_ID) != 0) {
        printf("[autoloader] kill_disc_player: running app is %s, not disc player\n", title_id);
        fflush(stdout);
        return 0; /* Different app — leave it alone */
    }

    /* Set the killed title ID to 'bdjb' so the autoloader looks for ps5_autoloader_bdjb config dir */
    strncpy(g_entry_point_id, "bdjb", sizeof(g_entry_point_id) - 1);
    g_entry_point_id[sizeof(g_entry_point_id) - 1] = '\0';

    printf("[autoloader] kill_disc_player: disc player detected (AppID 0x%04x)\n", app_id);
    fflush(stdout);
    autoloader_notify("Disc Player detected. Terminating...");

    /* Step 3: suspend the app */
    if (sceLncUtilSuspendApp(app_id) != 0) {
        autoloader_notify("Failed to suspend Disc Player");
        printf("[autoloader] kill_disc_player: suspend failed\n");
        fflush(stdout);
        return -1;
    }
    printf("[autoloader] kill_disc_player: suspended. Waiting for home screen...\n");
    fflush(stdout);

    /* Wait for home screen transition stability */
    sleep(2);

    /* Step 4: SIGKILL on the disc player process */
    pid_t pid = get_pid_by_name(DISC_PLAYER_PROCESS);
    if (pid != -1) {
        printf("[autoloader] kill_disc_player: sending SIGKILL to %s (PID %d)\n",
               DISC_PLAYER_PROCESS, pid);
        fflush(stdout);
        if (kill(pid, SIGKILL) == 0) {
            printf("[autoloader] kill_disc_player: SIGKILL sent\n");
        } else {
            printf("[autoloader] kill_disc_player: warning — SIGKILL failed\n");
            autoloader_notify("Warning: SIGKILL to Disc Player failed");
        }
        fflush(stdout);
        sleep(1);
    } else {
        printf("[autoloader] kill_disc_player: %s process not found (may already be gone)\n",
               DISC_PLAYER_PROCESS);
        fflush(stdout);
    }

    /* Step 5: LncUtil kill */
    int result = sceLncUtilKillApp(app_id);
    if (result == 0) {
        printf("[autoloader] kill_disc_player: Disc Player fully closed\n");
        fflush(stdout);
    } else {
        /* Check if it already disappeared */
        if ((uint32_t)sceLncUtilGetAppIdOfRunningBigApp() == 0xffffffff) {
            printf("[autoloader] kill_disc_player: Disc Player closed (already gone)\n");
            fflush(stdout);
        } else {
            printf("[autoloader] kill_disc_player: LncUtil kill failed (result=%d)\n", result);
            fflush(stdout);
            autoloader_notify("Failed to kill Disc Player (result=%d)", result);
            return -1;
        }
    }

    /* Small 0.2s delay just in case to let OS clean up completely */
    usleep(200000);

    return 0;
}

/* -----------------------------------------------------------------------
 * handle_browser_app
 * ----------------------------------------------------------------------- */

/**
 * Navigate to PS5 Home Screen via libSceShellUIUtil.sprx URI launch.
 * Uses: pshomeui:navigateToHome?bootCondition=psButton
 * Based on the Return-to-Home mechanism by LightningMods / etaHEN.
 */
static int return_to_home_by_uri(void) {
    int (*p_sceShellUIUtilInitialize)(void) = NULL;
    int (*p_sceShellUIUtilLaunchByUri)(const char *uri, SceShellUIUtilLaunchByUriParam *param) = NULL;

    /* Initialize User Service so user queries succeed */
    int user_prio = 256;
    int u_init = sceUserServiceInitialize(&user_prio);
    printf("[autoloader] sceUserServiceInitialize returned: 0x%08x (%d)\n", u_init, u_init);

    int mod = sceKernelLoadStartModule("/system_ex/common_ex/lib/libSceShellUIUtil.sprx", 0, 0, 0, 0, 0);
    if (mod < 0) {
        printf("[autoloader] return_to_home_by_uri: sceKernelLoadStartModule failed: 0x%08x (%d)\n", mod, mod);
        fflush(stdout);
        return -1;
    }

    /* Resolve symbols directly via kernel_dynlib_dlsym as in etaHEN */
    p_sceShellUIUtilInitialize = (void *)kernel_dynlib_dlsym(-1, (uint32_t)mod, "sceShellUIUtilInitialize");
    p_sceShellUIUtilLaunchByUri = (void *)kernel_dynlib_dlsym(-1, (uint32_t)mod, "sceShellUIUtilLaunchByUri");

    if (!p_sceShellUIUtilInitialize || !p_sceShellUIUtilLaunchByUri) {
        printf("[autoloader] return_to_home_by_uri: failed to resolve libSceShellUIUtil symbols\n");
        fflush(stdout);
        return -1;
    }

    SceShellUIUtilLaunchByUriParam param;
    memset(&param, 0, sizeof(param));
    param.size = sizeof(SceShellUIUtilLaunchByUriParam);

    int ui_init = p_sceShellUIUtilInitialize();
    printf("[autoloader] sceShellUIUtilInitialize returned: 0x%08x (%d)\n", ui_init, ui_init);

    int u_fg = sceUserServiceGetForegroundUser((int *)&param.userId);
    printf("[autoloader] sceUserServiceGetForegroundUser returned: 0x%08x (%d), userId=0x%08x\n",
           u_fg, u_fg, param.userId);

    if (param.userId == 0 || param.userId == (uint32_t)-1) {
        int u_init_u = sceUserServiceGetInitialUser((int *)&param.userId);
        printf("[autoloader] sceUserServiceGetInitialUser returned: 0x%08x (%d), userId=0x%08x\n",
               u_init_u, u_init_u, param.userId);
    }

    printf("[autoloader] return_to_home_by_uri: launching pshomeui with userId=0x%08x\n", param.userId);
    fflush(stdout);

    int res = p_sceShellUIUtilLaunchByUri("pshomeui:navigateToHome?bootCondition=psButton", &param);
    printf("[autoloader] return_to_home_by_uri: sceShellUIUtilLaunchByUri returned: 0x%08x (%d)\n", res, res);
    fflush(stdout);

    return res;
}

/**
 * Handle WebKit Browser (SceNKWebProcess) if running.
 *
 * SceNKWebProcess only runs when a WebKit browser webpage is actively open.
 *
 * Rather than killing the browser process (which triggers an OS error dialog
 * and reload), this requests the system to navigate back to the home screen
 * via libSceShellUIUtil URI launch. The OS automatically closes SceNKWebProcess
 * upon returning to the home screen.
 */
int handle_browser_app(void) {
    pid_t pid = get_pid_by_name(BROWSER_PROCESS);
    if (pid <= 0) {
        printf("[autoloader] handle_browser_app: WebKit browser (%s) not running\n", BROWSER_PROCESS);
        fflush(stdout);
        return 0;
    }

    /* Set entry point ID to 'webkit' so autoloader searches ps5_autoloader_webkit */
    strncpy(g_entry_point_id, "webkit", sizeof(g_entry_point_id) - 1);
    g_entry_point_id[sizeof(g_entry_point_id) - 1] = '\0';

    printf("[autoloader] handle_browser_app: WebKit browser (%s) detected (PID: %d)\n", BROWSER_PROCESS, pid);
    printf("[autoloader] handle_browser_app: Returning to Home via sceShellUIUtilLaunchByUri...\n");
    fflush(stdout);

    int ret = return_to_home_by_uri();
    if (ret != 0) {
        autoloader_notify("Warning: Return to Home returned 0x%08x", ret);
    }

    /* Small 0.1s delay before continuing */
    usleep(100000);

    return 0;
}

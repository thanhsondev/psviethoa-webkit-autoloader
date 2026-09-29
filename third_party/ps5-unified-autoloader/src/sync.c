#include "sync.h"
#include "autoloader.h"
#include "notification.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

static void clear_directory(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char file_path[1024];
        snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        if (stat(file_path, &st) == 0 && S_ISREG(st.st_mode)) {
            unlink(file_path);
        }
    }
    closedir(dir);
}

static int mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return mkdir(tmp, 0777);
}

static int copy_file(const char *src, const char *dst) {
    int fd_in = open(src, O_RDONLY);
    if (fd_in < 0) return -1;
    int fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd_out < 0) { close(fd_in); return -1; }
    char buf[16384];
    ssize_t n;
    int error = 0;
    while ((n = read(fd_in, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(fd_out, buf + written, n - written);
            if (w <= 0) { error = 1; break; }
            written += w;
        }
        if (error) break;
    }
    if (n < 0) error = 1;
    close(fd_in);
    close(fd_out);
    return error ? -1 : 0;
}

static int compare_files(const char *f1, const char *f2) {
    int fd1 = open(f1, O_RDONLY);
    if (fd1 < 0) return -1;
    int fd2 = open(f2, O_RDONLY);
    if (fd2 < 0) { close(fd1); return -1; }
    
    char buf1[8192];
    char buf2[8192];
    int error = 0;
    while (1) {
        ssize_t n1 = read(fd1, buf1, sizeof(buf1));
        ssize_t n2 = read(fd2, buf2, sizeof(buf2));
        if (n1 < 0 || n2 < 0 || n1 != n2) {
            error = 1;
            break;
        }
        if (n1 == 0) break; /* EOF */
        if (memcmp(buf1, buf2, n1) != 0) {
            error = 1;
            break;
        }
    }
    close(fd1);
    close(fd2);
    return error ? -1 : 0;
}

static void remove_directory(const char *dir_path) {
    clear_directory(dir_path);
    rmdir(dir_path);
}

static int sync_directory(const char *src_dir, const char *dst_dir) {
    DIR *dir = opendir(src_dir);
    if (!dir) return -1;
    mkdir_p(dst_dir);
    struct dirent *entry;
    int copied_count = 0;
    int error_count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char src_path[1024];
        char dst_path[1024];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name);
        struct stat st;
        if (stat(src_path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (copy_file(src_path, dst_path) == 0) {
                if (compare_files(src_path, dst_path) == 0) {
                    copied_count++;
                } else {
                    error_count++;
                }
            } else {
                error_count++;
            }
        }
    }
    closedir(dir);
    return error_count == 0 ? copied_count : -1;
}

void try_sync_usb_to_data(const char *config_path) {
    int is_usb = (strncmp(config_path, "/mnt/usb", 8) == 0);
    if (!is_usb) return;

    char usb_dir[512];
    char data_dir[512];
    strncpy(usb_dir, config_path, sizeof(usb_dir) - 1);
    usb_dir[sizeof(usb_dir) - 1] = '\0';
    char *last_slash = strrchr(usb_dir, '/');
    if (last_slash) *last_slash = '\0';
    else return;

    const char *rel_dir = strstr(usb_dir, "ps5_autoloader");
    if (!rel_dir) return;

    snprintf(data_dir, sizeof(data_dir), "%s/%s", DATA_BASE, rel_dir);

    printf("[autoloader] Syncing %s to %s\n", usb_dir, data_dir);
    fflush(stdout);

    clear_directory(data_dir);
    int synced = sync_directory(usb_dir, data_dir);
    if (synced >= 0) {
        printf("[autoloader] Sync complete and verified. Removing USB dir.\n");
        fflush(stdout);
        remove_directory(usb_dir);
        autoloader_notify("Sync complete & verified (%d files).\nUSB directory removed.", synced);
    } else {
        printf("[autoloader] Sync failed or verification error.\n");
        fflush(stdout);
        autoloader_notify("Sync failed! Check logs.");
    }
}

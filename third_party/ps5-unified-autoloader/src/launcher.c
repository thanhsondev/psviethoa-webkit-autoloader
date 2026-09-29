#include "launcher.h"
#include "autoloader.h"
#include "notification.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* -----------------------------------------------------------------------
 * Internal: open a TCP connection to 127.0.0.1:ELFLDR_PORT
 * Returns the connected socket fd on success, -1 on failure.
 * ----------------------------------------------------------------------- */
static int connect_to_elfldr(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("[autoloader] launcher: socket() failed\n");
        fflush(stdout);
        return -1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(ELFLDR_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

/* -----------------------------------------------------------------------
 * Internal: stream 'len' bytes from 'data' to 'sock'
 * Returns 0 on success, -1 on error.
 * ----------------------------------------------------------------------- */
static int stream_bytes(int sock, const uint8_t *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        ssize_t sent = send(sock, data + offset, len - offset, 0);
        if (sent <= 0) {
            printf("[autoloader] launcher: send() failed after %zu bytes\n", offset);
            fflush(stdout);
            return -1;
        }
        offset += (size_t)sent;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * wait_for_elfldr
 * ----------------------------------------------------------------------- */
int wait_for_elfldr(void) {
    printf("[autoloader] Waiting for elfldr on port %d...\n", ELFLDR_PORT);
    fflush(stdout);

    for (int i = 0; i < ELFLDR_WAIT_RETRIES; i++) {
        int sock = connect_to_elfldr();
        if (sock >= 0) {
            close(sock);
            printf("[autoloader] elfldr ready (attempt %d)\n", i + 1);
            fflush(stdout);
            return 0;
        }
        usleep(ELFLDR_WAIT_DELAY_US);
    }

    printf("[autoloader] elfldr did not become ready in time\n");
    fflush(stdout);
    return -1;
}

/* -----------------------------------------------------------------------
 * launch_elf_from_file
 * ----------------------------------------------------------------------- */
int launch_elf_from_file(const char *path) {
    printf("[autoloader] Launching ELF from file: %s\n", path);
    fflush(stdout);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("[autoloader] launch_file: open() failed for: %s\n", path);
        fflush(stdout);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        printf("[autoloader] launch_file: fstat() failed\n");
        fflush(stdout);
        close(fd);
        return -1;
    }

    int sock = connect_to_elfldr();
    if (sock < 0) {
        printf("[autoloader] launch_file: could not connect to elfldr\n");
        fflush(stdout);
        close(fd);
        return -1;
    }

    /* Stream file in chunks */
    uint8_t buf[8192];
    ssize_t n;
    size_t total = 0;
    int error = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (stream_bytes(sock, buf, (size_t)n) != 0) {
            error = 1;
            break;
        }
        total += (size_t)n;
    }

    if (n < 0) {
        printf("[autoloader] launch_file: read() error\n");
        fflush(stdout);
        error = 1;
    }

    printf("[autoloader] launch_file: sent %zu bytes%s\n",
           total, error ? " (with errors)" : "");
    fflush(stdout);

    close(sock);
    close(fd);
    return error ? -1 : 0;
}

/* -----------------------------------------------------------------------
 * launch_elf_from_memory
 * ----------------------------------------------------------------------- */
int launch_elf_from_memory(const uint8_t *data, size_t len) {
    printf("[autoloader] Launching ELF from memory (%zu bytes)\n", len);
    fflush(stdout);

    int sock = connect_to_elfldr();
    if (sock < 0) {
        printf("[autoloader] launch_mem: could not connect to elfldr\n");
        fflush(stdout);
        return -1;
    }

    int ret = stream_bytes(sock, data, len);
    close(sock);

    if (ret == 0) {
        printf("[autoloader] launch_mem: sent %zu bytes OK\n", len);
    } else {
        printf("[autoloader] launch_mem: send failed\n");
    }
    fflush(stdout);

    return ret;
}

#pragma once

#include <stddef.h>

/* PS5 kernel notification request structure */
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

/**
 * Send a formatted notification to the PS5 system notification bar.
 * Also prints to stdout for log visibility.
 */
void autoloader_notify(const char *fmt, ...);

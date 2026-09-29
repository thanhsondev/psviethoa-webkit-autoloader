#include "notification.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* PS5 SDK function — sends a notification to the system notification bar */
int sceKernelSendNotificationRequest(int device, notify_request_t *request,
                                     size_t size, int unused);

void autoloader_notify(const char *fmt, ...) {
    notify_request_t req;
    va_list args;

    memset(&req, 0, sizeof(req));
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, args);
    va_end(args);

    /* Print to stdout so it shows in any connected log client */
    printf("[autoloader] %s\n", req.message);
    fflush(stdout);

    /* Send to PS5 system notification bar */
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

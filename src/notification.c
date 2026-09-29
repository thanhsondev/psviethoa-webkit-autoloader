#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "wkali.h"
#include "notification.h"

/* PS5 SDK Function */
int sceKernelSendNotificationRequest(int device, notify_request_t *request,
                                     size_t size, int unused);

void wkali_notify(const char *fmt, ...) {
  notify_request_t req;
  va_list args;

  memset(&req, 0, sizeof(req));
  va_start(args, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, args);
  va_end(args);

  /* Send to PS5 notification system */
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);

  /* Also write to log */
  wkali_log("[Notify] %s\n", req.message);
}

#pragma once

#include <stddef.h>

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

#ifdef __cplusplus
extern "C" {
#endif

void wkali_notify(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

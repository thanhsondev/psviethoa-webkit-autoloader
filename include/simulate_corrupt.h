#pragma once

#include <microhttpd.h>
#include <stddef.h>

#if defined(WKALI_SIMULATE_CACHE_CORRUPTION) && WKALI_SIMULATE_CACHE_CORRUPTION > 0

void simulate_corrupt_manifest(const char *url, void **payload, size_t *payload_size,
                               enum MHD_ResponseMemoryMode *mem_mode);
void simulate_on_clear_success(void);

#else

static inline void simulate_corrupt_manifest(const char *url, void **payload,
                                             size_t *payload_size,
                                             enum MHD_ResponseMemoryMode *mem_mode) {
    (void)url;
    (void)payload;
    (void)payload_size;
    (void)mem_mode;
}

static inline void simulate_on_clear_success(void) {}

#endif

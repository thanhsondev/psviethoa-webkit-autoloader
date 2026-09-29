#include "simulate_corrupt.h"

#if defined(WKALI_SIMULATE_CACHE_CORRUPTION) && WKALI_SIMULATE_CACHE_CORRUPTION > 0

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "wkali.h"

/* Test-only: set to 1 once /clear-webkit-data succeeds. Used by the one-shot
 * (mode 1) simulation so the post-clear retry download runs a clean manifest,
 * mirroring a wipe actually fixing a corrupted on-disk cache. */
static atomic_int simulate_healed = 0;

void simulate_on_clear_success(void) {
#if WKALI_SIMULATE_CACHE_CORRUPTION == 1
    atomic_store(&simulate_healed, 1);
#endif
}

void simulate_corrupt_manifest(const char *url, void **payload, size_t *payload_size,
                               enum MHD_ResponseMemoryMode *mem_mode) {
    if (strcmp(url, ROUTE_CACHE_MANIFEST) != 0)
        return;

    if (WKALI_SIMULATE_CACHE_CORRUPTION == 1 && atomic_load(&simulate_healed))
        return;

    static const char fake[] = "simulate_corrupted.bin\n";
    size_t fake_len = sizeof(fake) - 1;
    const char *src = (const char *)*payload;
    size_t src_len = *payload_size;

    /* Insert after the CACHE: header so the fake entry is treated as a regular
     * cache member (and 404s). Fall back to the top of the file if the header
     * is somehow missing. */
    const char *marker = NULL;
    for (const char *p = src; p + 6 <= src + src_len; p++) {
        if (memcmp(p, "CACHE:", 6) == 0) {
            marker = p;
            break;
        }
    }
    const char *insert_at = src;
    if (marker) {
        const char *nl = memchr(marker, '\n', (src + src_len) - marker);
        if (nl) insert_at = nl + 1;
    }

    size_t off = (size_t)(insert_at - src);
    char *out = malloc(src_len + fake_len + 1);
    if (!out) return;
    memcpy(out, src, off);
    memcpy(out + off, fake, fake_len);
    memcpy(out + off + fake_len, src + off, src_len - off);
    out[src_len + fake_len] = '\0';

    if (*mem_mode == MHD_RESPMEM_MUST_FREE) free((void *)src);
    *payload = out;
    *payload_size = src_len + fake_len;
    *mem_mode = MHD_RESPMEM_MUST_FREE;

    wkali_log("[WKALI] SIMULATED corrupted cache: manifest now includes simulate_corrupted.bin (mode %d)\n",
              WKALI_SIMULATE_CACHE_CORRUPTION);
}

#endif

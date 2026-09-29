#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>

#include "wkali.h"

#define LOG_BUFFER_SIZE (128 * 1024)

static char log_buffer[LOG_BUFFER_SIZE];
static size_t log_head = 0;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t log_cond = PTHREAD_COND_INITIALIZER;

void wkali_log(const char *fmt, ...) {
    char temp[512];
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    va_start(args, fmt);
    int len = vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);

    if (len > 0) {
        pthread_mutex_lock(&log_mutex);
        if (log_head + len < LOG_BUFFER_SIZE) {
            memcpy(log_buffer + log_head, temp, len);
            log_head += len;
            pthread_cond_broadcast(&log_cond);
        }
        pthread_mutex_unlock(&log_mutex);
    }
}

/* Wait for new logs starting from *pos. Copies up to max_len into out_buf. 
 * Returns the number of bytes copied. Blocks up to 1 second. */
size_t wkali_wait_logs(size_t *pos, char *out_buf, size_t max_len) {
    pthread_mutex_lock(&log_mutex);
    
    struct timeval tv;
    struct timespec ts;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + 1;
    ts.tv_nsec = tv.tv_usec * 1000;

    while (*pos >= log_head) {
        int rc = pthread_cond_timedwait(&log_cond, &log_mutex, &ts);
        if (rc == ETIMEDOUT) {
            break;
        }
    }

    size_t copied = 0;
    if (*pos < log_head) {
        copied = log_head - *pos;
        if (copied > max_len) copied = max_len;
        memcpy(out_buf, log_buffer + *pos, copied);
        *pos += copied;
    }

    pthread_mutex_unlock(&log_mutex);
    return copied;
}

void wkali_log_wakeup(void) {
    pthread_mutex_lock(&log_mutex);
    pthread_cond_broadcast(&log_cond);
    pthread_mutex_unlock(&log_mutex);
}

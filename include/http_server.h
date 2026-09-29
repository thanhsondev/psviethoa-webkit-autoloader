#pragma once

#include <microhttpd.h>
#include <stdatomic.h>

/* Shared flag — set to 0 by a successful /install or /exit, read by the main loop. */
extern atomic_int http_keep_running;

/* Set to 1 only when /install succeeds so shutdown knows to notify success. */
extern atomic_int install_completed;

/* Set to 1 by /clear-webkit-data, read by main loop to re-launch browser. */
extern atomic_int webkit_data_cleared;

/* MHD request handler callback — dispatches all routes. */
enum MHD_Result http_on_request(void *cls, struct MHD_Connection *conn,
                                const char *url, const char *method,
                                const char *version, const char *upload_data,
                                size_t *upload_data_size, void **con_cls);

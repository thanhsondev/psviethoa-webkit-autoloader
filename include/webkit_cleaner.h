#pragma once

/* Recursively deletes all contents of the webkit/shell folder under the
 * current (foreground) user's home directory. Returns 0 on success, -1 on
 * error. */
int wkali_clear_webkit_data(void);

#pragma once

/**
 * Checks if the provided config path is on a USB drive.
 * If so, clears the corresponding internal storage directory,
 * synchronizes the contents from USB, verifies them byte-by-byte,
 * and removes the USB directory to prevent repeated loads.
 */
void try_sync_usb_to_data(const char *config_path);

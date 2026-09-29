#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Send an ELF file from the filesystem to the local elfldr socket (port 9021).
 * @param path  Full path to the .elf or .bin file.
 * @return 0 on success, -1 on failure.
 */
int launch_elf_from_file(const char *path);

/**
 * Send an in-memory ELF buffer to the local elfldr socket (port 9021).
 * Used for the embedded pldmgr.elf fallback.
 * @param data  Pointer to the ELF bytes.
 * @param len   Length of the buffer in bytes.
 * @return 0 on success, -1 on failure.
 */
int launch_elf_from_memory(const uint8_t *data, size_t len);

/**
 * Poll port 9021 until elfldr is ready to accept connections.
 * Tries ELFLDR_WAIT_RETRIES times with ELFLDR_WAIT_DELAY_US between each.
 * @return 0 if elfldr is ready, -1 if timed out.
 */
int wait_for_elfldr(void);

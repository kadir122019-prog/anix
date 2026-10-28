#ifndef ANIX_STAT_H
#define ANIX_STAT_H

#include <stdint.h>

enum anix_file_type {
    ANIX_FILE_REGULAR = 1,
    ANIX_FILE_DIRECTORY = 2
};

enum anix_stat_flags {
    ANIX_STAT_WRITABLE = 1u,
    ANIX_STAT_PERSISTENT = 2u
};

struct anix_stat {
    uint64_t size;       /* File length in bytes; zero for directories. */
    uint32_t type;       /* One of enum anix_file_type. */
    uint32_t flags;      /* Read-only metadata flags above. */
};

#endif

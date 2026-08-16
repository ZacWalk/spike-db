/*============================================================================
 * spike_db_internal.h — test-only entry points into the implementation
 *
 * NOT part of the public API and not covered by any compatibility promise.
 * Included by src/spike_db.c and src/test_spike_db.c only; nothing a user
 * links against needs it.
 *
 * See docs/testing.md §3 (injectable I/O) and §5 (invariant auditing).
 *============================================================================*/

#ifndef SPIKE_DB_INTERNAL_H
#define SPIKE_DB_INTERNAL_H

#include "spike_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/*----------------------------------------------------------------------------
 * Checksum
 *
 * CRC-32C has a hardware path and a software fallback that must produce
 * identical results, or a file written on one machine is unreadable on
 * another. This lets one process compare them.
 *--------------------------------------------------------------------------*/

uint32_t spike_db_internal_crc32c(const void* data, size_t len,
                                  int force_software);

/*----------------------------------------------------------------------------
 * Injectable I/O
 *
 * Every page read, page write, fsync and file lock goes through this table,
 * which defaults to `spike_db_internal_io_os`. Installing a wrapper is how
 * the suite reaches the error-handling half of the codebase: a wrapper
 * delegates to the default table and fails the Nth call.
 *
 * The table is per-handle and must be installed before the handle is used
 * concurrently with anything else. `ctx` is untouched by the library.
 *--------------------------------------------------------------------------*/

typedef struct SpikeDB_IoOps {
    SpikeDB_Status (*read  )(SpikeDB* db, uint32_t page_id, void* buf);
    SpikeDB_Status (*write )(SpikeDB* db, uint32_t page_id, const void* buf);
    SpikeDB_Status (*fsync )(SpikeDB* db);
    SpikeDB_Status (*lock  )(SpikeDB* db, bool exclusive);
    void           (*unlock)(SpikeDB* db);
    void*          ctx;
} SpikeDB_IoOps;

/* The real OS implementations, for a wrapper to delegate to. */
extern const SpikeDB_IoOps spike_db_internal_io_os;

/* Install `ops` (NULL restores the default) and return the previous table. */
const SpikeDB_IoOps* spike_db_internal_set_io(SpikeDB* db,
                                              const SpikeDB_IoOps* ops);

/* The `ctx` of the currently installed table, so a wrapper can find its
 * own state from the SpikeDB* the library hands it. */
void* spike_db_internal_io_ctx(SpikeDB* db);

/*----------------------------------------------------------------------------
 * Structural audit
 *
 * `spike_db_verify` walks the file. This walks the parts of the handle that
 * never reach disk — page-cache slots, the page-id hash table, pin counts,
 * transaction flags — where a leak shows up much later as an unrelated
 * SPIKEDB_FULL. Call it only when no operation is in flight.
 *
 * Returns the number of problems found (0 = clean) and copies the first
 * into `msg` when `msg` is non-NULL.
 *--------------------------------------------------------------------------*/

int spike_db_internal_check(SpikeDB* db, char* msg, size_t msg_len);

#ifdef __cplusplus
}
#endif

#endif /* SPIKE_DB_INTERNAL_H */

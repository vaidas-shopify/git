#ifndef PACK_BASE_STRATUM_H
#define PACK_BASE_STRATUM_H

#include "hash.h"

#define BASE_STRATUM_SIGNATURE 0x53545241 /* "STRA" */
#define BASE_STRATUM_VERSION 1

struct packed_git;

struct base_stratum_data {
	struct object_id anchor_commit;
	char *anchor_ref;
	uint32_t stratified_timestamp;
};

/*
 * Loads the .base-stratum file corresponding to "p", if any, returning
 * zero on success.
 */
int load_pack_base_stratum(struct packed_git *p, struct base_stratum_data *data);

/*
 * Writes a .base-stratum file for pack "p" with the given anchor
 * information. Returns zero on success.
 */
int write_pack_base_stratum(struct packed_git *p,
			const struct object_id *anchor_commit,
			const char *anchor_ref,
			uint32_t stratified_timestamp);

/*
 * Removes the .base-stratum file for pack "p", effectively demoting
 * the pack from stratify to regular. Returns zero on success.
 */
int remove_pack_base_stratum(struct packed_git *p);

void clear_base_stratum_data(struct base_stratum_data *data);

#endif

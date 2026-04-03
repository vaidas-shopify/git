#ifndef PACK_ANCHORED_H
#define PACK_ANCHORED_H

#include "hash.h"

#define ANCHORED_SIGNATURE 0x414e4348 /* "ANCH" */
#define ANCHORED_VERSION 1

struct packed_git;

struct anchored_data {
	struct object_id anchor_commit;
	char *anchor_ref;
	uint32_t pinned_timestamp;
};

/*
 * Loads the .anchored file corresponding to "p", if any, returning
 * zero on success.
 */
int load_pack_anchored(struct packed_git *p, struct anchored_data *data);

/*
 * Writes a .anchored file for pack "p" with the given anchor
 * information. Returns zero on success.
 */
int write_pack_anchored(struct packed_git *p,
			const struct object_id *anchor_commit,
			const char *anchor_ref,
			uint32_t pinned_timestamp);

/*
 * Removes the .anchored file for pack "p", effectively demoting
 * the pack from anti-cruft to regular. Returns zero on success.
 */
int remove_pack_anchored(struct packed_git *p);

void clear_anchored_data(struct anchored_data *data);

#endif

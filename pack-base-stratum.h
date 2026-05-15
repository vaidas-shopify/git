#ifndef PACK_BASE_STRATUM_H
#define PACK_BASE_STRATUM_H

#include "hash.h"

#define BASE_STRATUM_SIGNATURE 0x53545241 /* "STRA" */
#define BASE_STRATUM_VERSION 1

struct packed_git;
struct repository;
struct strbuf;

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

/*
 * Append the pack basename to write base-stratum packs for "anchor_ref"
 * to "out", in the form
 *
 *   <objects-source>/pack/base-stratum-<anchor-digest>
 *
 * pack-objects appends "-<pack-hash>.pack" to produce the final pack
 * filename. The anchor digest is a stable short hash of "anchor_ref"
 * that gives each anchor its own filename namespace, so two anchors
 * whose reachable object sets are identical do not collide on the same
 * pack and overwrite each other's .base-stratum sidecar.
 */
void format_base_stratum_pack_basename(struct strbuf *out,
				       struct repository *r,
				       const char *anchor_ref);

#endif

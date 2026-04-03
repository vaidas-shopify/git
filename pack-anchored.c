#include "git-compat-util.h"
#include "gettext.h"
#include "hash.h"
#include "chunk-format.h"
#include "csum-file.h"
#include "pack-anchored.h"
#include "packfile.h"
#include "strbuf.h"
#include "wrapper.h"

static char *pack_anchored_filename(struct packed_git *p)
{
	size_t len;
	if (!strip_suffix(p->pack_name, ".pack", &len))
		BUG("pack_name does not end in .pack");
	return xstrfmt("%.*s.anchored", (int)len, p->pack_name);
}

/*
 * .anchored file layout (version 1):
 *
 *   - 4 bytes: signature (0x414e4348 = "ANCH", network byte order)
 *   - 4 bytes: version (1, network byte order)
 *   - 4 bytes: hash_id (1=SHA1, 2=SHA256, network byte order)
 *   - N bytes: anchor_commit OID (raw, 20 or 32 bytes)
 *   - 4 bytes: pinned_timestamp (network byte order)
 *   - NUL-terminated string: anchor_ref name
 *   - N bytes: trailing checksum of all preceding data
 */
#define ANCHORED_HEADER_SIZE (12)

int load_pack_anchored(struct packed_git *p, struct anchored_data *data)
{
	char *anchored_name = NULL;
	int fd, ret = 0;
	struct stat st;
	unsigned char *buf = NULL;
	size_t file_size, pos;
	uint32_t signature, version, hash_id;
	const struct git_hash_algo *algo;
	size_t hash_len;

	if (!p->is_anchored)
		return -1;

	anchored_name = pack_anchored_filename(p);
	fd = git_open(anchored_name);
	if (fd < 0) {
		ret = -1;
		goto cleanup;
	}

	if (fstat(fd, &st)) {
		ret = error_errno(_("failed to read %s"), anchored_name);
		goto cleanup;
	}

	file_size = xsize_t(st.st_size);
	if (file_size < ANCHORED_HEADER_SIZE) {
		ret = error(_("anchored file %s is too small"), anchored_name);
		goto cleanup;
	}

	buf = xmmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);

	signature = get_be32(buf);
	version = get_be32(buf + 4);
	hash_id = get_be32(buf + 8);

	if (signature != ANCHORED_SIGNATURE) {
		ret = error(_("anchored file %s has unknown signature"),
			    anchored_name);
		goto cleanup;
	}

	if (version != ANCHORED_VERSION) {
		ret = error(_("anchored file %s has unsupported version %"PRIu32),
			    anchored_name, version);
		goto cleanup;
	}

	if (hash_id == 1)
		algo = &hash_algos[GIT_HASH_SHA1];
	else if (hash_id == 2)
		algo = &hash_algos[GIT_HASH_SHA256];
	else {
		ret = error(_("anchored file %s has unsupported hash id %"PRIu32),
			    anchored_name, hash_id);
		goto cleanup;
	}

	/* Verify hash algorithm matches the repository */
	if (algo != p->repo->hash_algo) {
		ret = error(_("anchored file %s uses %s, but repository uses %s"),
			    anchored_name, algo->name, p->repo->hash_algo->name);
		goto cleanup;
	}
	hash_len = algo->rawsz;

	pos = ANCHORED_HEADER_SIZE;

	/* anchor commit OID */
	if (file_size < pos + hash_len + 4 + 1 + hash_len) {
		ret = error(_("anchored file %s is corrupt"), anchored_name);
		goto cleanup;
	}

	oidread(&data->anchor_commit, buf + pos, algo);
	pos += hash_len;

	/* pinned timestamp */
	data->pinned_timestamp = get_be32(buf + pos);
	pos += 4;

	/* anchor ref name (NUL-terminated) */
	{
		const char *ref_start = (const char *)buf + pos;
		size_t max_ref_len = file_size - pos - hash_len;
		const char *nul = memchr(ref_start, '\0', max_ref_len);
		if (!nul) {
			ret = error(_("anchored file %s has unterminated ref name"),
				    anchored_name);
			goto cleanup;
		}
		data->anchor_ref = xstrdup(ref_start);
		pos += (nul - ref_start) + 1;
	}

	/* trailing checksum — verify size and contents */
	if (pos + hash_len != file_size) {
		ret = error(_("anchored file %s is corrupt"), anchored_name);
		free(data->anchor_ref);
		data->anchor_ref = NULL;
		goto cleanup;
	}
	{
		struct git_hash_ctx ctx;
		unsigned char computed[GIT_MAX_RAWSZ];

		algo->init_fn(&ctx);
		algo->update_fn(&ctx, buf, pos);
		algo->final_fn(computed, &ctx);

		if (hashcmp(computed, buf + pos, algo)) {
			ret = error(_("anchored file %s has incorrect checksum"),
				    anchored_name);
			free(data->anchor_ref);
			data->anchor_ref = NULL;
			goto cleanup;
		}
	}

cleanup:
	if (buf)
		munmap(buf, file_size);
	if (fd >= 0)
		close(fd);
	free(anchored_name);
	return ret;
}

int write_pack_anchored(struct packed_git *p,
			const struct object_id *anchor_commit,
			const char *anchor_ref,
			uint32_t pinned_timestamp)
{
	char *anchored_name = NULL;
	const struct git_hash_algo *algo = p->repo->hash_algo;
	struct hashfile *f;
	int fd;
	size_t ref_len;

	anchored_name = pack_anchored_filename(p);

	fd = xopen(anchored_name, O_WRONLY | O_CREAT | O_TRUNC, 0444);
	f = hashfd(algo, fd, anchored_name);

	/* header */
	hashwrite_be32(f, ANCHORED_SIGNATURE);
	hashwrite_be32(f, ANCHORED_VERSION);
	hashwrite_be32(f, oid_version(algo));

	/* anchor commit OID */
	hashwrite(f, anchor_commit->hash, algo->rawsz);

	/* pinned timestamp */
	hashwrite_be32(f, pinned_timestamp);

	/* anchor ref (NUL-terminated) */
	ref_len = strlen(anchor_ref);
	hashwrite(f, anchor_ref, ref_len + 1);

	finalize_hashfile(f, NULL, FSYNC_COMPONENT_PACK_METADATA,
			  CSUM_HASH_IN_STREAM | CSUM_CLOSE | CSUM_FSYNC);

	free(anchored_name);
	return 0;
}

int remove_pack_anchored(struct packed_git *p)
{
	char *anchored_name = pack_anchored_filename(p);
	int ret = unlink(anchored_name);
	if (ret) {
		error_errno(_("failed to remove %s"), anchored_name);
		free(anchored_name);
		return -1;
	}
	free(anchored_name);
	p->is_anchored = 0;
	return 0;
}

void clear_anchored_data(struct anchored_data *data)
{
	free(data->anchor_ref);
	memset(data, 0, sizeof(*data));
}

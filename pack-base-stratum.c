#include "git-compat-util.h"
#include "gettext.h"
#include "hash.h"
#include "chunk-format.h"
#include "csum-file.h"
#include "pack-base-stratum.h"
#include "packfile.h"
#include "strbuf.h"
#include "wrapper.h"

static size_t pack_base_len(struct packed_git *p)
{
	size_t len;
	if (!strip_suffix(p->pack_name, ".pack", &len))
		BUG("pack_name does not end in .pack");
	return len;
}

static char *pack_base_stratum_filename(struct packed_git *p)
{
	return xstrfmt("%.*s.base-stratum", (int)pack_base_len(p), p->pack_name);
}

static char *pack_keep_filename(struct packed_git *p)
{
	return xstrfmt("%.*s.keep", (int)pack_base_len(p), p->pack_name);
}

/*
 * .base-stratum file layout (version 1):
 *
 *   - 4 bytes: signature (0x53545241 = "STRA", network byte order)
 *   - 4 bytes: version (1, network byte order)
 *   - 4 bytes: hash_id (1=SHA1, 2=SHA256, network byte order)
 *   - N bytes: anchor_commit OID (raw, 20 or 32 bytes)
 *   - 4 bytes: stratified_timestamp (network byte order)
 *   - NUL-terminated string: anchor_ref name
 *   - N bytes: trailing checksum of all preceding data
 */
#define BASE_STRATUM_HEADER_SIZE (12)

int load_pack_base_stratum(struct packed_git *p, struct base_stratum_data *data)
{
	char *base_stratum_name = NULL;
	int fd, ret = 0;
	struct stat st;
	unsigned char *buf = NULL;
	size_t file_size, pos;
	uint32_t signature, version, hash_id;
	const struct git_hash_algo *algo;
	size_t hash_len;

	if (!p->in_base_stratum)
		return -1;

	base_stratum_name = pack_base_stratum_filename(p);
	fd = git_open(base_stratum_name);
	if (fd < 0) {
		ret = -1;
		goto cleanup;
	}

	if (fstat(fd, &st)) {
		ret = error_errno(_("failed to read %s"), base_stratum_name);
		goto cleanup;
	}

	file_size = xsize_t(st.st_size);
	if (file_size < BASE_STRATUM_HEADER_SIZE) {
		ret = error(_("base-stratum file %s is too small"), base_stratum_name);
		goto cleanup;
	}

	buf = xmmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);

	signature = get_be32(buf);
	version = get_be32(buf + 4);
	hash_id = get_be32(buf + 8);

	if (signature != BASE_STRATUM_SIGNATURE) {
		ret = error(_("base-stratum file %s has unknown signature"),
			    base_stratum_name);
		goto cleanup;
	}

	if (version != BASE_STRATUM_VERSION) {
		ret = error(_("base-stratum file %s has unsupported version %"PRIu32),
			    base_stratum_name, version);
		goto cleanup;
	}

	if (hash_id == 1)
		algo = &hash_algos[GIT_HASH_SHA1];
	else if (hash_id == 2)
		algo = &hash_algos[GIT_HASH_SHA256];
	else {
		ret = error(_("base-stratum file %s has unsupported hash id %"PRIu32),
			    base_stratum_name, hash_id);
		goto cleanup;
	}

	/* Verify hash algorithm matches the repository */
	if (algo != p->repo->hash_algo) {
		ret = error(_("base-stratum file %s uses %s, but repository uses %s"),
			    base_stratum_name, algo->name, p->repo->hash_algo->name);
		goto cleanup;
	}
	hash_len = algo->rawsz;

	pos = BASE_STRATUM_HEADER_SIZE;

	/* anchor commit OID */
	if (file_size < pos + hash_len + 4 + 1 + hash_len) {
		ret = error(_("base-stratum file %s is corrupt"), base_stratum_name);
		goto cleanup;
	}

	oidread(&data->anchor_commit, buf + pos, algo);
	pos += hash_len;

	/* stratified timestamp */
	data->stratified_timestamp = get_be32(buf + pos);
	pos += 4;

	/* anchor ref name (NUL-terminated) */
	{
		const char *ref_start = (const char *)buf + pos;
		size_t max_ref_len = file_size - pos - hash_len;
		const char *nul = memchr(ref_start, '\0', max_ref_len);
		if (!nul) {
			ret = error(_("base-stratum file %s has unterminated ref name"),
				    base_stratum_name);
			goto cleanup;
		}
		data->anchor_ref = xstrdup(ref_start);
		pos += (nul - ref_start) + 1;
	}

	/* trailing checksum — verify size and contents */
	if (pos + hash_len != file_size) {
		ret = error(_("base-stratum file %s is corrupt"), base_stratum_name);
		FREE_AND_NULL(data->anchor_ref);
		goto cleanup;
	}
	{
		struct git_hash_ctx ctx;
		unsigned char computed[GIT_MAX_RAWSZ];

		algo->init_fn(&ctx);
		algo->update_fn(&ctx, buf, pos);
		algo->final_fn(computed, &ctx);

		if (hashcmp(computed, buf + pos, algo)) {
			ret = error(_("base-stratum file %s has incorrect checksum"),
				    base_stratum_name);
			FREE_AND_NULL(data->anchor_ref);
			goto cleanup;
		}
	}

cleanup:
	if (buf)
		munmap(buf, file_size);
	if (fd >= 0)
		close(fd);
	free(base_stratum_name);
	return ret;
}

/*
 * Atomically (re)create "path" by writing to a unique temp sibling
 * and renaming over. Required for both .base-stratum and .keep because
 * we set their permissions to 0444 to discourage casual modification:
 * once such a file exists, a plain xopen(O_WRONLY|O_TRUNC) on the
 * same path would fail with EACCES, which collides with concurrent
 * writers from auto-maintenance and any sequential rerun against the
 * same deterministic pack hash. The rename is atomic and overwrites,
 * so the last writer wins without an error.
 *
 * Returns -1 on failure; the caller is responsible for diagnosing.
 */
static int begin_atomic_write(const char *path, char **tmp_name_out)
{
	char *tmp_name = xstrfmt("%s.tmp-%d", path, (int)getpid());
	int fd;

	/* Best-effort: clear any leftover temp from a prior crash. */
	unlink(tmp_name);

	fd = xopen(tmp_name, O_WRONLY | O_CREAT | O_EXCL, 0444);
	*tmp_name_out = tmp_name;
	return fd;
}

static int finish_atomic_write(char *tmp_name, const char *final_name)
{
	if (rename(tmp_name, final_name)) {
		error_errno(_("failed to rename %s to %s"),
			    tmp_name, final_name);
		unlink(tmp_name);
		free(tmp_name);
		return -1;
	}
	free(tmp_name);
	return 0;
}

int write_pack_base_stratum(struct packed_git *p,
			const struct object_id *anchor_commit,
			const char *anchor_ref,
			uint32_t stratified_timestamp)
{
	char *base_stratum_name = NULL;
	char *tmp_name = NULL;
	const struct git_hash_algo *algo = p->repo->hash_algo;
	struct hashfile *f;
	int fd;
	size_t ref_len;

	base_stratum_name = pack_base_stratum_filename(p);

	fd = begin_atomic_write(base_stratum_name, &tmp_name);
	f = hashfd(algo, fd, tmp_name);

	/* header */
	hashwrite_be32(f, BASE_STRATUM_SIGNATURE);
	hashwrite_be32(f, BASE_STRATUM_VERSION);
	hashwrite_be32(f, oid_version(algo));

	/* anchor commit OID */
	hashwrite(f, anchor_commit->hash, algo->rawsz);

	/* stratified timestamp */
	hashwrite_be32(f, stratified_timestamp);

	/* anchor ref (NUL-terminated) */
	ref_len = strlen(anchor_ref);
	hashwrite(f, anchor_ref, ref_len + 1);

	finalize_hashfile(f, NULL, FSYNC_COMPONENT_PACK_METADATA,
			  CSUM_HASH_IN_STREAM | CSUM_CLOSE | CSUM_FSYNC);

	if (finish_atomic_write(tmp_name, base_stratum_name)) {
		free(base_stratum_name);
		return -1;
	}

	/*
	 * Write a .keep file so that older git versions (unaware of
	 * .base-stratum) will not delete this pack during gc or repack.
	 * Same atomic-rename dance, same race reasons.
	 */
	{
		char *keep_name = pack_keep_filename(p);
		char *keep_tmp = NULL;
		int keep_fd = begin_atomic_write(keep_name, &keep_tmp);
		close(keep_fd);
		if (finish_atomic_write(keep_tmp, keep_name)) {
			free(keep_name);
			free(base_stratum_name);
			return -1;
		}
		free(keep_name);
	}

	free(base_stratum_name);
	return 0;
}

int remove_pack_base_stratum(struct packed_git *p)
{
	char *base_stratum_name = pack_base_stratum_filename(p);
	char *keep_name;
	int ret = unlink(base_stratum_name);
	if (ret) {
		error_errno(_("failed to remove %s"), base_stratum_name);
		free(base_stratum_name);
		return -1;
	}

	/* Remove the companion .keep file created for older git compat */
	keep_name = pack_keep_filename(p);
	unlink(keep_name); /* best-effort, may not exist */
	free(keep_name);

	free(base_stratum_name);
	p->in_base_stratum = 0;
	return 0;
}

void clear_base_stratum_data(struct base_stratum_data *data)
{
	free(data->anchor_ref);
	memset(data, 0, sizeof(*data));
}

void format_base_stratum_pack_basename(struct strbuf *out,
				       struct repository *r,
				       const char *anchor_ref)
{
	const struct git_hash_algo *algo = r->hash_algo;
	struct git_hash_ctx ctx;
	unsigned char digest[GIT_MAX_RAWSZ];

	algo->init_fn(&ctx);
	algo->update_fn(&ctx, anchor_ref, strlen(anchor_ref));
	algo->final_fn(digest, &ctx);

	strbuf_addf(out, "%s/pack/base-stratum-%02x%02x%02x%02x%02x%02x%02x%02x",
		    r->objects->sources->path,
		    digest[0], digest[1], digest[2], digest[3],
		    digest[4], digest[5], digest[6], digest[7]);
}

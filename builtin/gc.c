/*
 * git gc builtin command
 *
 * Cleanup unreachable files and optimize the repository.
 *
 * Copyright (c) 2007 James Bowes
 *
 * Based on git-gc.sh, which is
 *
 * Copyright (c) 2006 Shawn O. Pearce
 */

#define USE_THE_REPOSITORY_VARIABLE
#define DISABLE_SIGN_COMPARE_WARNINGS

#include "builtin.h"
#include "abspath.h"
#include "date.h"
#include "dir.h"
#include "environment.h"
#include "hex.h"
#include "config.h"
#include "tempfile.h"
#include "lockfile.h"
#include "parse-options.h"
#include "run-command.h"
#include "sigchain.h"
#include "strvec.h"
#include "commit.h"
#include "commit-graph.h"
#include "commit-reach.h"
#include "odb/source-files.h"
#include "oid-array.h"
#include "pack-base-stratum.h"
#include "packfile.h"
#include "object-file.h"
#include "oid-array.h"
#include "pack.h"
#include "pack-objects.h"
#include "path.h"
#include "reflog.h"
#include "repack.h"
#include "rerere.h"
#include "revision.h"
#include "blob.h"
#include "tree.h"
#include "promisor-remote.h"
#include "refs.h"
#include "remote.h"
#include "exec-cmd.h"
#include "gettext.h"
#include "hook.h"
#include "setup.h"
#include "trace2.h"
#include "worktree.h"

#define FAILED_RUN "failed to run %s"

static const char * const builtin_gc_usage[] = {
	N_("git gc [<options>]"),
	NULL
};

static timestamp_t gc_log_expire_time;
static struct tempfile *pidfile;
static struct lock_file log_lock;
static struct string_list pack_garbage = STRING_LIST_INIT_DUP;

static void clean_pack_garbage(void)
{
	int i;
	for (i = 0; i < pack_garbage.nr; i++)
		unlink_or_warn(pack_garbage.items[i].string);
	string_list_clear(&pack_garbage, 0);
}

static void report_pack_garbage(unsigned seen_bits, const char *path)
{
	if (seen_bits == PACKDIR_FILE_IDX)
		string_list_append(&pack_garbage, path);
}

static void process_log_file(void)
{
	struct stat st;
	if (fstat(get_lock_file_fd(&log_lock), &st)) {
		/*
		 * Perhaps there was an i/o error or another
		 * unlikely situation.  Try to make a note of
		 * this in gc.log along with any existing
		 * messages.
		 */
		int saved_errno = errno;
		fprintf(stderr, _("Failed to fstat %s: %s"),
			get_lock_file_path(&log_lock),
			strerror(saved_errno));
		fflush(stderr);
		commit_lock_file(&log_lock);
		errno = saved_errno;
	} else if (st.st_size) {
		/* There was some error recorded in the lock file */
		commit_lock_file(&log_lock);
	} else {
		char *path = repo_git_path(the_repository, "gc.log");
		/* No error, clean up any old gc.log */
		unlink(path);
		rollback_lock_file(&log_lock);
		free(path);
	}
}

static void process_log_file_at_exit(void)
{
	fflush(stderr);
	process_log_file();
}

static int gc_config_is_timestamp_never(const char *var)
{
	const char *value;
	timestamp_t expire;

	if (!repo_config_get_value(the_repository, var, &value) && value) {
		if (parse_expiry_date(value, &expire))
			die(_("failed to parse '%s' value '%s'"), var, value);
		return expire == 0;
	}
	return 0;
}

struct gc_config {
	int pack_refs;
	int prune_reflogs;
	int cruft_packs;
	unsigned long max_cruft_size;
	int aggressive_depth;
	int aggressive_window;
	int gc_auto_threshold;
	int gc_auto_pack_limit;
	int detach_auto;
	char *gc_log_expire;
	char *prune_expire;
	char *prune_worktrees_expire;
	char *repack_filter;
	char *repack_filter_to;
	char *repack_expire_to;
	unsigned long big_pack_threshold;
	unsigned long max_delta_cache_size;
	/*
	 * Remove this member from gc_config once repo_settings is passed
	 * through the callchain.
	 */
	size_t delta_base_cache_limit;
};

#define GC_CONFIG_INIT { \
	.pack_refs = 1, \
	.prune_reflogs = 1, \
	.cruft_packs = 1, \
	.aggressive_depth = 50, \
	.aggressive_window = 250, \
	.gc_auto_threshold = 6700, \
	.gc_auto_pack_limit = 50, \
	.detach_auto = 1, \
	.gc_log_expire = xstrdup("1.day.ago"), \
	.prune_expire = xstrdup("2.weeks.ago"), \
	.prune_worktrees_expire = xstrdup("3.months.ago"), \
	.max_delta_cache_size = DEFAULT_DELTA_CACHE_SIZE, \
	.delta_base_cache_limit = DEFAULT_DELTA_BASE_CACHE_LIMIT, \
}

static void gc_config_release(struct gc_config *cfg)
{
	free(cfg->gc_log_expire);
	free(cfg->prune_expire);
	free(cfg->prune_worktrees_expire);
	free(cfg->repack_filter);
	free(cfg->repack_filter_to);
}

static void gc_config(struct gc_config *cfg)
{
	const char *value;
	char *owned = NULL;
	unsigned long ulongval;

	if (!repo_config_get_value(the_repository, "gc.packrefs", &value)) {
		if (value && !strcmp(value, "notbare"))
			cfg->pack_refs = -1;
		else
			cfg->pack_refs = git_config_bool("gc.packrefs", value);
	}

	if (gc_config_is_timestamp_never("gc.reflogexpire") &&
	    gc_config_is_timestamp_never("gc.reflogexpireunreachable"))
		cfg->prune_reflogs = 0;

	repo_config_get_int(the_repository, "gc.aggressivewindow", &cfg->aggressive_window);
	repo_config_get_int(the_repository, "gc.aggressivedepth", &cfg->aggressive_depth);
	repo_config_get_int(the_repository, "gc.auto", &cfg->gc_auto_threshold);
	repo_config_get_int(the_repository, "gc.autopacklimit", &cfg->gc_auto_pack_limit);
	repo_config_get_bool(the_repository, "gc.autodetach", &cfg->detach_auto);
	repo_config_get_bool(the_repository, "gc.cruftpacks", &cfg->cruft_packs);
	repo_config_get_ulong(the_repository, "gc.maxcruftsize", &cfg->max_cruft_size);

	if (!repo_config_get_expiry(the_repository, "gc.pruneexpire", &owned)) {
		free(cfg->prune_expire);
		cfg->prune_expire = owned;
	}

	if (!repo_config_get_expiry(the_repository, "gc.worktreepruneexpire", &owned)) {
		free(cfg->prune_worktrees_expire);
		cfg->prune_worktrees_expire = owned;
	}

	if (!repo_config_get_expiry(the_repository, "gc.logexpiry", &owned)) {
		free(cfg->gc_log_expire);
		cfg->gc_log_expire = owned;
	}

	repo_config_get_ulong(the_repository, "gc.bigpackthreshold", &cfg->big_pack_threshold);
	repo_config_get_ulong(the_repository, "pack.deltacachesize", &cfg->max_delta_cache_size);

	if (!repo_config_get_ulong(the_repository, "core.deltabasecachelimit", &ulongval))
		cfg->delta_base_cache_limit = ulongval;

	if (!repo_config_get_string(the_repository, "gc.repackfilter", &owned)) {
		free(cfg->repack_filter);
		cfg->repack_filter = owned;
	}

	if (!repo_config_get_string(the_repository, "gc.repackfilterto", &owned)) {
		free(cfg->repack_filter_to);
		cfg->repack_filter_to = owned;
	}

	repo_config(the_repository, git_default_config, NULL);
}

enum schedule_priority {
	SCHEDULE_NONE = 0,
	SCHEDULE_WEEKLY = 1,
	SCHEDULE_DAILY = 2,
	SCHEDULE_HOURLY = 3,
};

static enum schedule_priority parse_schedule(const char *value)
{
	if (!value)
		return SCHEDULE_NONE;
	if (!strcasecmp(value, "hourly"))
		return SCHEDULE_HOURLY;
	if (!strcasecmp(value, "daily"))
		return SCHEDULE_DAILY;
	if (!strcasecmp(value, "weekly"))
		return SCHEDULE_WEEKLY;
	return SCHEDULE_NONE;
}

enum maintenance_task_label {
	TASK_PREFETCH,
	TASK_LOOSE_OBJECTS,
	TASK_INCREMENTAL_REPACK,
	TASK_GEOMETRIC_REPACK,
	TASK_GC,
	TASK_COMMIT_GRAPH,
	TASK_PACK_REFS,
	TASK_REFLOG_EXPIRE,
	TASK_WORKTREE_PRUNE,
	TASK_RERERE_GC,
	TASK_STRATIFY,
	TASK_STRATIFY_PRUNE,
	TASK_CONSOLIDATE_STRATUM,
	TASK_SURFACE_GC,

	/* Leave as final value */
	TASK__COUNT
};

struct maintenance_run_opts {
	enum maintenance_task_label *tasks;
	size_t tasks_nr, tasks_alloc;
	int auto_flag;
	int detach;
	int quiet;
	int dry_run;
	enum schedule_priority schedule;
};
#define MAINTENANCE_RUN_OPTS_INIT { \
	.detach = -1, \
}

static void maintenance_run_opts_release(struct maintenance_run_opts *opts)
{
	free(opts->tasks);
}

static int pack_refs_condition(UNUSED struct gc_config *cfg)
{
	struct string_list included_refs = STRING_LIST_INIT_NODUP;
	struct ref_exclusions excludes = REF_EXCLUSIONS_INIT;
	struct refs_optimize_opts optimize_opts = {
		.exclusions = &excludes,
		.includes = &included_refs,
		.flags = REFS_OPTIMIZE_PRUNE | REFS_OPTIMIZE_AUTO,
	};
	bool required;

	/* Check for all refs, similar to 'git refs optimize --all'. */
	string_list_append(optimize_opts.includes, "*");

	if (refs_optimize_required(get_main_ref_store(the_repository),
				   &optimize_opts, &required))
		return 0;

	clear_ref_exclusions(&excludes);
	string_list_clear(&included_refs, 0);

	return required;
}

static int maintenance_task_pack_refs(struct maintenance_run_opts *opts,
				      UNUSED struct gc_config *cfg)
{
	struct child_process cmd = CHILD_PROCESS_INIT;

	cmd.git_cmd = 1;
	strvec_pushl(&cmd.args, "pack-refs", "--all", "--prune", NULL);
	if (opts->auto_flag)
		strvec_push(&cmd.args, "--auto");

	return run_command(&cmd);
}

struct count_reflog_entries_data {
	struct expire_reflog_policy_cb policy;
	size_t count;
	size_t limit;
};

static int count_reflog_entries(const char *refname UNUSED,
				struct object_id *old_oid, struct object_id *new_oid,
				const char *committer, timestamp_t timestamp,
				int tz, const char *msg, void *cb_data)
{
	struct count_reflog_entries_data *data = cb_data;
	if (should_expire_reflog_ent(old_oid, new_oid, committer, timestamp, tz, msg, &data->policy))
		data->count++;
	return data->count >= data->limit;
}

static int reflog_expire_condition(struct gc_config *cfg UNUSED)
{
	timestamp_t now = time(NULL);
	struct count_reflog_entries_data data = {
		.policy = {
			.opts = REFLOG_EXPIRE_OPTIONS_INIT(now),
		},
	};
	int limit = 100;

	repo_config_get_int(the_repository, "maintenance.reflog-expire.auto", &limit);
	if (!limit)
		return 0;
	if (limit < 0)
		return 1;
	data.limit = limit;

	repo_config(the_repository, reflog_expire_config, &data.policy.opts);

	reflog_expire_options_set_refname(&data.policy.opts, "HEAD");
	refs_for_each_reflog_ent(get_main_ref_store(the_repository), "HEAD",
				 count_reflog_entries, &data);

	reflog_expiry_cleanup(&data.policy);
	reflog_clear_expire_config(&data.policy.opts);
	return data.count >= data.limit;
}

static int maintenance_task_reflog_expire(struct maintenance_run_opts *opts UNUSED,
					  struct gc_config *cfg UNUSED)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	cmd.git_cmd = 1;
	strvec_pushl(&cmd.args, "reflog", "expire", "--all", NULL);
	return run_command(&cmd);
}

static int maintenance_task_worktree_prune(struct maintenance_run_opts *opts UNUSED,
					   struct gc_config *cfg)
{
	struct child_process prune_worktrees_cmd = CHILD_PROCESS_INIT;

	prune_worktrees_cmd.git_cmd = 1;
	strvec_pushl(&prune_worktrees_cmd.args, "worktree", "prune", "--expire", NULL);
	strvec_push(&prune_worktrees_cmd.args, cfg->prune_worktrees_expire);

	return run_command(&prune_worktrees_cmd);
}

static int worktree_prune_condition(struct gc_config *cfg)
{
	struct strbuf buf = STRBUF_INIT;
	int should_prune = 0, limit = 1;
	timestamp_t expiry_date;
	struct dirent *d;
	DIR *dir = NULL;

	repo_config_get_int(the_repository, "maintenance.worktree-prune.auto", &limit);
	if (limit <= 0) {
		should_prune = limit < 0;
		goto out;
	}

	if (parse_expiry_date(cfg->prune_worktrees_expire, &expiry_date))
		goto out;

	dir = opendir(repo_git_path_replace(the_repository, &buf, "worktrees"));
	if (!dir)
		goto out;

	while (limit && (d = readdir_skip_dot_and_dotdot(dir))) {
		char *wtpath;
		strbuf_reset(&buf);
		if (should_prune_worktree(d->d_name, &buf, &wtpath, expiry_date))
			limit--;
		free(wtpath);
	}

	should_prune = !limit;

out:
	if (dir)
		closedir(dir);
	strbuf_release(&buf);
	return should_prune;
}

static int maintenance_task_rerere_gc(struct maintenance_run_opts *opts UNUSED,
				      struct gc_config *cfg UNUSED)
{
	struct child_process rerere_cmd = CHILD_PROCESS_INIT;
	rerere_cmd.git_cmd = 1;
	strvec_pushl(&rerere_cmd.args, "rerere", "gc", NULL);
	return run_command(&rerere_cmd);
}

static int rerere_gc_condition(struct gc_config *cfg UNUSED)
{
	struct strbuf path = STRBUF_INIT;
	int should_gc = 0, limit = 1;
	DIR *dir = NULL;

	repo_config_get_int(the_repository, "maintenance.rerere-gc.auto", &limit);
	if (limit <= 0) {
		should_gc = limit < 0;
		goto out;
	}

	/*
	 * We skip garbage collection in case we either have no "rr-cache"
	 * directory or when it doesn't contain at least one entry.
	 */
	repo_git_path_replace(the_repository, &path, "rr-cache");
	dir = opendir(path.buf);
	if (!dir)
		goto out;
	should_gc = !!readdir_skip_dot_and_dotdot(dir);

out:
	strbuf_release(&path);
	if (dir)
		closedir(dir);
	return should_gc;
}

static int too_many_loose_objects(int limit)
{
	/*
	 * This is weird, but stems from legacy behaviour: the GC auto
	 * threshold was always essentially interpreted as if it was rounded up
	 * to the next multiple 256 of, so we retain this behaviour for now.
	 */
	int auto_threshold = DIV_ROUND_UP(limit, 256) * 256;
	unsigned long loose_count;

	if (odb_source_loose_count_objects(the_repository->objects->sources,
					   ODB_COUNT_OBJECTS_APPROXIMATE,
					   &loose_count) < 0)
		return 0;

	return loose_count > auto_threshold;
}

static struct packed_git *find_base_packs(struct string_list *packs,
					  unsigned long limit)
{
	struct packed_git *p, *base = NULL;

	repo_for_each_pack(the_repository, p) {
		if (!p->pack_local || p->is_cruft)
			continue;
		if (limit) {
			if (p->pack_size >= limit)
				string_list_append(packs, p->pack_name);
		} else if (!base || base->pack_size < p->pack_size) {
			base = p;
		}
	}

	if (base)
		string_list_append(packs, base->pack_name);

	return base;
}

static int too_many_packs(struct gc_config *cfg)
{
	struct packed_git *p;
	int cnt = 0;

	if (cfg->gc_auto_pack_limit <= 0)
		return 0;

	repo_for_each_pack(the_repository, p) {
		if (!p->pack_local)
			continue;
		if (p->pack_keep)
			continue;
		/*
		 * Perhaps check the size of the pack and count only
		 * very small ones here?
		 */
		cnt++;
	}
	return cfg->gc_auto_pack_limit < cnt;
}

static uint64_t total_ram(void)
{
#if defined(HAVE_SYSINFO)
	struct sysinfo si;

	if (!sysinfo(&si)) {
		uint64_t total = si.totalram;

		if (si.mem_unit > 1)
			total *= (uint64_t)si.mem_unit;
		return total;
	}
#elif defined(HAVE_BSD_SYSCTL) && (defined(HW_MEMSIZE) || defined(HW_PHYSMEM) || defined(HW_PHYSMEM64))
	uint64_t physical_memory;
	int mib[2];
	size_t length;

	mib[0] = CTL_HW;
# if defined(HW_MEMSIZE)
	mib[1] = HW_MEMSIZE;
# elif defined(HW_PHYSMEM64)
	mib[1] = HW_PHYSMEM64;
# else
	mib[1] = HW_PHYSMEM;
# endif
	length = sizeof(physical_memory);
	if (!sysctl(mib, 2, &physical_memory, &length, NULL, 0)) {
		if (length == 4) {
			uint32_t mem;

			if (!sysctl(mib, 2, &mem, &length, NULL, 0))
				physical_memory = mem;
		}
		return physical_memory;
	}
#elif defined(GIT_WINDOWS_NATIVE)
	MEMORYSTATUSEX memInfo;

	memInfo.dwLength = sizeof(MEMORYSTATUSEX);
	if (GlobalMemoryStatusEx(&memInfo))
		return memInfo.ullTotalPhys;
#endif
	return 0;
}

static uint64_t estimate_repack_memory(struct gc_config *cfg,
				       struct packed_git *pack)
{
	unsigned long nr_objects;
	size_t os_cache, heap;

	if (odb_count_objects(the_repository->objects,
			      ODB_COUNT_OBJECTS_APPROXIMATE, &nr_objects) < 0)
		return 0;

	if (!pack || !nr_objects)
		return 0;

	/*
	 * First we have to scan through at least one pack.
	 * Assume enough room in OS file cache to keep the entire pack
	 * or we may accidentally evict data of other processes from
	 * the cache.
	 */
	os_cache = pack->pack_size + pack->index_size;
	/* then pack-objects needs lots more for book keeping */
	heap = sizeof(struct object_entry) * nr_objects;
	/*
	 * internal rev-list --all --objects takes up some memory too,
	 * let's say half of it is for blobs
	 */
	heap += sizeof(struct blob) * nr_objects / 2;
	/*
	 * and the other half is for trees (commits and tags are
	 * usually insignificant)
	 */
	heap += sizeof(struct tree) * nr_objects / 2;
	/* and then obj_hash[], underestimated in fact */
	heap += sizeof(struct object *) * nr_objects;
	/* revindex is used also */
	heap += (sizeof(off_t) + sizeof(uint32_t)) * nr_objects;
	/*
	 * read_sha1_file() (either at delta calculation phase, or
	 * writing phase) also fills up the delta base cache
	 */
	heap += cfg->delta_base_cache_limit;
	/* and of course pack-objects has its own delta cache */
	heap += cfg->max_delta_cache_size;

	return os_cache + heap;
}

static int keep_one_pack(struct string_list_item *item, void *data)
{
	struct strvec *args = data;
	strvec_pushf(args, "--keep-pack=%s", basename(item->string));
	return 0;
}

static void add_repack_all_option(struct gc_config *cfg,
				  struct string_list *keep_pack,
				  struct strvec *args)
{
	if (cfg->prune_expire && !strcmp(cfg->prune_expire, "now")
		&& !(cfg->cruft_packs && cfg->repack_expire_to))
		strvec_push(args, "-a");
	else if (cfg->cruft_packs) {
		strvec_push(args, "--cruft");
		if (cfg->prune_expire)
			strvec_pushf(args, "--cruft-expiration=%s", cfg->prune_expire);
		if (cfg->max_cruft_size)
			strvec_pushf(args, "--max-cruft-size=%lu",
				     cfg->max_cruft_size);
		if (cfg->repack_expire_to)
			strvec_pushf(args, "--expire-to=%s", cfg->repack_expire_to);
	} else {
		strvec_push(args, "-A");
		if (cfg->prune_expire)
			strvec_pushf(args, "--unpack-unreachable=%s", cfg->prune_expire);
	}

	if (keep_pack)
		for_each_string_list(keep_pack, keep_one_pack, args);

	if (cfg->repack_filter && *cfg->repack_filter)
		strvec_pushf(args, "--filter=%s", cfg->repack_filter);
	if (cfg->repack_filter_to && *cfg->repack_filter_to)
		strvec_pushf(args, "--filter-to=%s", cfg->repack_filter_to);
}

static void add_repack_incremental_option(struct strvec *args)
{
	strvec_push(args, "--no-write-bitmap-index");
}

static int need_to_gc(struct gc_config *cfg, struct strvec *repack_args)
{
	/*
	 * Setting gc.auto to 0 or negative can disable the
	 * automatic gc.
	 */
	if (cfg->gc_auto_threshold <= 0)
		return 0;

	/*
	 * If there are too many loose objects, but not too many
	 * packs, we run "repack -d -l".  If there are too many packs,
	 * we run "repack -A -d -l".  Otherwise we tell the caller
	 * there is no need.
	 */
	if (too_many_packs(cfg)) {
		struct string_list keep_pack = STRING_LIST_INIT_NODUP;

		if (cfg->big_pack_threshold) {
			find_base_packs(&keep_pack, cfg->big_pack_threshold);
			if (keep_pack.nr >= cfg->gc_auto_pack_limit) {
				cfg->big_pack_threshold = 0;
				string_list_clear(&keep_pack, 0);
				find_base_packs(&keep_pack, 0);
			}
		} else {
			struct packed_git *p = find_base_packs(&keep_pack, 0);
			uint64_t mem_have, mem_want;

			mem_have = total_ram();
			mem_want = estimate_repack_memory(cfg, p);

			/*
			 * Only allow 1/2 of memory for pack-objects, leave
			 * the rest for the OS and other processes in the
			 * system.
			 */
			if (!mem_have || mem_want < mem_have / 2)
				string_list_clear(&keep_pack, 0);
		}

		add_repack_all_option(cfg, &keep_pack, repack_args);
		string_list_clear(&keep_pack, 0);
	} else if (too_many_loose_objects(cfg->gc_auto_threshold))
		add_repack_incremental_option(repack_args);
	else
		return 0;

	if (run_hooks(the_repository, "pre-auto-gc"))
		return 0;
	return 1;
}

/* return NULL on success, else hostname running the gc */
static const char *lock_repo_for_gc(int force, pid_t* ret_pid)
{
	struct lock_file lock = LOCK_INIT;
	char my_host[HOST_NAME_MAX + 1];
	struct strbuf sb = STRBUF_INIT;
	struct stat st;
	uintmax_t pid;
	FILE *fp;
	int fd;
	char *pidfile_path;

	if (is_tempfile_active(pidfile))
		/* already locked */
		return NULL;

	if (xgethostname(my_host, sizeof(my_host)))
		xsnprintf(my_host, sizeof(my_host), "unknown");

	pidfile_path = repo_git_path(the_repository, "gc.pid");
	fd = hold_lock_file_for_update(&lock, pidfile_path,
				       LOCK_DIE_ON_ERROR);
	if (!force) {
		static char locking_host[HOST_NAME_MAX + 1];
		static char *scan_fmt;
		int should_exit;

		if (!scan_fmt)
			scan_fmt = xstrfmt("%s %%%ds", "%"SCNuMAX, HOST_NAME_MAX);
		fp = fopen(pidfile_path, "r");
		memset(locking_host, 0, sizeof(locking_host));
		should_exit =
			fp != NULL &&
			!fstat(fileno(fp), &st) &&
			/*
			 * 12 hour limit is very generous as gc should
			 * never take that long. On the other hand we
			 * don't really need a strict limit here,
			 * running gc --auto one day late is not a big
			 * problem. --force can be used in manual gc
			 * after the user verifies that no gc is
			 * running.
			 */
			time(NULL) - st.st_mtime <= 12 * 3600 &&
			fscanf(fp, scan_fmt, &pid, locking_host) == 2 &&
			/* be gentle to concurrent "gc" on remote hosts */
			(strcmp(locking_host, my_host) || !kill(pid, 0) || errno == EPERM);
		if (fp)
			fclose(fp);
		if (should_exit) {
			if (fd >= 0)
				rollback_lock_file(&lock);
			*ret_pid = pid;
			free(pidfile_path);
			return locking_host;
		}
	}

	strbuf_addf(&sb, "%"PRIuMAX" %s",
		    (uintmax_t) getpid(), my_host);
	write_in_full(fd, sb.buf, sb.len);
	strbuf_release(&sb);
	commit_lock_file(&lock);
	pidfile = register_tempfile(pidfile_path);
	free(pidfile_path);
	return NULL;
}

/*
 * Returns 0 if there was no previous error and gc can proceed, 1 if
 * gc should not proceed due to an error in the last run. Prints a
 * message and returns with a non-[01] status code if an error occurred
 * while reading gc.log
 */
static int report_last_gc_error(void)
{
	struct strbuf sb = STRBUF_INIT;
	int ret = 0;
	ssize_t len;
	struct stat st;
	char *gc_log_path = repo_git_path(the_repository, "gc.log");

	if (stat(gc_log_path, &st)) {
		if (errno == ENOENT)
			goto done;

		ret = die_message_errno(_("cannot stat '%s'"), gc_log_path);
		goto done;
	}

	if (st.st_mtime < gc_log_expire_time)
		goto done;

	len = strbuf_read_file(&sb, gc_log_path, 0);
	if (len < 0)
		ret = die_message_errno(_("cannot read '%s'"), gc_log_path);
	else if (len > 0) {
		/*
		 * A previous gc failed.  Report the error, and don't
		 * bother with an automatic gc run since it is likely
		 * to fail in the same way.
		 */
		warning(_("The last gc run reported the following. "
			       "Please correct the root cause\n"
			       "and remove %s\n"
			       "Automatic cleanup will not be performed "
			       "until the file is removed.\n\n"
			       "%s"),
			    gc_log_path, sb.buf);
		ret = 1;
	}
	strbuf_release(&sb);
done:
	free(gc_log_path);
	return ret;
}

static int gc_foreground_tasks(struct maintenance_run_opts *opts,
			       struct gc_config *cfg)
{
	if (cfg->pack_refs && maintenance_task_pack_refs(opts, cfg))
		return error(FAILED_RUN, "pack-refs");
	if (cfg->prune_reflogs && maintenance_task_reflog_expire(opts, cfg))
		return error(FAILED_RUN, "reflog");
	return 0;
}

int cmd_gc(int argc,
	   const char **argv,
	   const char *prefix,
	   struct repository *repo UNUSED)
{
	int aggressive = 0;
	int force = 0;
	const char *name;
	pid_t pid;
	int daemonized = 0;
	int keep_largest_pack = -1;
	int skip_foreground_tasks = 0;
	timestamp_t dummy;
	struct strvec repack_args = STRVEC_INIT;
	struct maintenance_run_opts opts = MAINTENANCE_RUN_OPTS_INIT;
	struct gc_config cfg = GC_CONFIG_INIT;
	const char *prune_expire_sentinel = "sentinel";
	const char *prune_expire_arg = prune_expire_sentinel;
	int ret;
	struct option builtin_gc_options[] = {
		OPT__QUIET(&opts.quiet, N_("suppress progress reporting")),
		{
			.type = OPTION_STRING,
			.long_name = "prune",
			.value = &prune_expire_arg,
			.argh = N_("date"),
			.help = N_("prune unreferenced objects"),
			.flags = PARSE_OPT_OPTARG,
			.defval = (intptr_t)prune_expire_arg,
		},
		OPT_BOOL(0, "cruft", &cfg.cruft_packs, N_("pack unreferenced objects separately")),
		OPT_UNSIGNED(0, "max-cruft-size", &cfg.max_cruft_size,
			     N_("with --cruft, limit the size of new cruft packs")),
		OPT_BOOL(0, "aggressive", &aggressive, N_("be more thorough (increased runtime)")),
		OPT_BOOL_F(0, "auto", &opts.auto_flag, N_("enable auto-gc mode"),
			   PARSE_OPT_NOCOMPLETE),
		OPT_BOOL(0, "detach", &opts.detach,
			 N_("perform garbage collection in the background")),
		OPT_BOOL_F(0, "force", &force,
			   N_("force running gc even if there may be another gc running"),
			   PARSE_OPT_NOCOMPLETE),
		OPT_BOOL(0, "keep-largest-pack", &keep_largest_pack,
			 N_("repack all other packs except the largest pack")),
		OPT_STRING(0, "expire-to", &cfg.repack_expire_to, N_("dir"),
			   N_("pack prefix to store a pack containing pruned objects")),
		OPT_HIDDEN_BOOL(0, "skip-foreground-tasks", &skip_foreground_tasks,
			   N_("skip maintenance tasks typically done in the foreground")),
		OPT_END()
	};

	show_usage_with_options_if_asked(argc, argv,
					 builtin_gc_usage, builtin_gc_options);

	strvec_pushl(&repack_args, "repack", "-d", "-l", NULL);

	gc_config(&cfg);

	if (parse_expiry_date(cfg.gc_log_expire, &gc_log_expire_time))
		die(_("failed to parse gc.logExpiry value %s"), cfg.gc_log_expire);

	if (cfg.pack_refs < 0)
		cfg.pack_refs = !is_bare_repository();

	argc = parse_options(argc, argv, prefix, builtin_gc_options,
			     builtin_gc_usage, 0);
	if (argc > 0)
		usage_with_options(builtin_gc_usage, builtin_gc_options);

	if (prune_expire_arg != prune_expire_sentinel) {
		free(cfg.prune_expire);
		cfg.prune_expire = xstrdup_or_null(prune_expire_arg);
	}
	if (cfg.prune_expire && parse_expiry_date(cfg.prune_expire, &dummy))
		die(_("failed to parse prune expiry value %s"), cfg.prune_expire);

	if (aggressive) {
		strvec_push(&repack_args, "-f");
		if (cfg.aggressive_depth > 0)
			strvec_pushf(&repack_args, "--depth=%d", cfg.aggressive_depth);
		if (cfg.aggressive_window > 0)
			strvec_pushf(&repack_args, "--window=%d", cfg.aggressive_window);
	}
	if (opts.quiet)
		strvec_push(&repack_args, "-q");

	if (opts.auto_flag) {
		if (cfg.detach_auto && opts.detach < 0)
			opts.detach = 1;

		/*
		 * Auto-gc should be least intrusive as possible.
		 */
		if (!need_to_gc(&cfg, &repack_args)) {
			ret = 0;
			goto out;
		}

		if (!opts.quiet) {
			if (opts.detach > 0)
				fprintf(stderr, _("Auto packing the repository in background for optimum performance.\n"));
			else
				fprintf(stderr, _("Auto packing the repository for optimum performance.\n"));
			fprintf(stderr, _("See \"git help gc\" for manual housekeeping.\n"));
		}
	} else {
		struct string_list keep_pack = STRING_LIST_INIT_NODUP;

		if (keep_largest_pack != -1) {
			if (keep_largest_pack)
				find_base_packs(&keep_pack, 0);
		} else if (cfg.big_pack_threshold) {
			find_base_packs(&keep_pack, cfg.big_pack_threshold);
		}

		add_repack_all_option(&cfg, &keep_pack, &repack_args);
		string_list_clear(&keep_pack, 0);
	}

	if (opts.detach > 0) {
		ret = report_last_gc_error();
		if (ret == 1) {
			/* Last gc --auto failed. Skip this one. */
			ret = 0;
			goto out;

		} else if (ret) {
			/* an I/O error occurred, already reported */
			goto out;
		}

		if (!skip_foreground_tasks) {
			if (lock_repo_for_gc(force, &pid)) {
				ret = 0;
				goto out;
			}

			if (gc_foreground_tasks(&opts, &cfg) < 0)
				die(NULL);
			delete_tempfile(&pidfile);
		}

		/*
		 * failure to daemonize is ok, we'll continue
		 * in foreground
		 */
		daemonized = !daemonize();
	}

	name = lock_repo_for_gc(force, &pid);
	if (name) {
		if (opts.auto_flag) {
			ret = 0;
			goto out; /* be quiet on --auto */
		}

		die(_("gc is already running on machine '%s' pid %"PRIuMAX" (use --force if not)"),
		    name, (uintmax_t)pid);
	}

	if (daemonized) {
		char *path = repo_git_path(the_repository, "gc.log");
		hold_lock_file_for_update(&log_lock, path,
					  LOCK_DIE_ON_ERROR);
		dup2(get_lock_file_fd(&log_lock), 2);
		atexit(process_log_file_at_exit);
		free(path);
	}

	if (opts.detach <= 0 && !skip_foreground_tasks)
		gc_foreground_tasks(&opts, &cfg);

	if (!the_repository->repository_format_precious_objects) {
		struct child_process repack_cmd = CHILD_PROCESS_INIT;

		repack_cmd.git_cmd = 1;
		repack_cmd.odb_to_close = the_repository->objects;
		strvec_pushv(&repack_cmd.args, repack_args.v);
		if (run_command(&repack_cmd))
			die(FAILED_RUN, repack_args.v[0]);

		if (cfg.prune_expire) {
			struct child_process prune_cmd = CHILD_PROCESS_INIT;

			strvec_pushl(&prune_cmd.args, "prune", "--expire", NULL);
			/* run `git prune` even if using cruft packs */
			strvec_push(&prune_cmd.args, cfg.prune_expire);
			if (opts.quiet)
				strvec_push(&prune_cmd.args, "--no-progress");
			if (repo_has_promisor_remote(the_repository))
				strvec_push(&prune_cmd.args,
					    "--exclude-promisor-objects");
			prune_cmd.git_cmd = 1;

			if (run_command(&prune_cmd))
				die(FAILED_RUN, prune_cmd.args.v[0]);
		}
	}

	if (cfg.prune_worktrees_expire &&
	    maintenance_task_worktree_prune(&opts, &cfg))
		die(FAILED_RUN, "worktree");

	if (maintenance_task_rerere_gc(&opts, &cfg))
		die(FAILED_RUN, "rerere");

	report_garbage = report_pack_garbage;
	odb_reprepare(the_repository->objects);
	if (pack_garbage.nr > 0) {
		odb_close(the_repository->objects);
		clean_pack_garbage();
	}

	if (the_repository->settings.gc_write_commit_graph == 1)
		write_commit_graph_reachable(the_repository->objects->sources,
					     !opts.quiet && !daemonized ? COMMIT_GRAPH_WRITE_PROGRESS : 0,
					     NULL);

	if (opts.auto_flag && too_many_loose_objects(cfg.gc_auto_threshold))
		warning(_("There are too many unreachable loose objects; "
			"run 'git prune' to remove them."));

	if (!daemonized) {
		char *path = repo_git_path(the_repository, "gc.log");
		unlink(path);
		free(path);
	}

out:
	maintenance_run_opts_release(&opts);
	strvec_clear(&repack_args);
	gc_config_release(&cfg);
	return 0;
}

static const char *const builtin_maintenance_run_usage[] = {
	N_("git maintenance run [--auto] [--[no-]quiet] [--task=<task>] [--schedule] [--dry-run]"),
	NULL
};

static int maintenance_opt_schedule(const struct option *opt, const char *arg,
				    int unset)
{
	enum schedule_priority *priority = opt->value;

	if (unset)
		die(_("--no-schedule is not allowed"));

	*priority = parse_schedule(arg);

	if (!*priority)
		die(_("unrecognized --schedule argument '%s'"), arg);

	return 0;
}

struct cg_auto_data {
	int num_not_in_graph;
	int limit;
};

static int dfs_on_ref(const struct reference *ref, void *cb_data)
{
	struct cg_auto_data *data = (struct cg_auto_data *)cb_data;
	int result = 0;
	const struct object_id *maybe_peeled = ref->oid;
	struct object_id peeled;
	struct commit_list *stack = NULL;
	struct commit *commit;

	if (!reference_get_peeled_oid(the_repository, ref, &peeled))
		maybe_peeled = &peeled;
	if (odb_read_object_info(the_repository->objects, maybe_peeled, NULL) != OBJ_COMMIT)
		return 0;

	commit = lookup_commit(the_repository, maybe_peeled);
	if (!commit || commit->object.flags & SEEN)
		return 0;
	commit->object.flags |= SEEN;

	if (repo_parse_commit(the_repository, commit) ||
	    commit_graph_position(commit) != COMMIT_NOT_FROM_GRAPH)
		return 0;

	data->num_not_in_graph++;

	if (data->num_not_in_graph >= data->limit)
		return 1;

	commit_list_insert(commit, &stack);

	while (!result && stack) {
		struct commit_list *parent;

		commit = pop_commit(&stack);

		for (parent = commit->parents; parent; parent = parent->next) {
			if (repo_parse_commit(the_repository, parent->item) ||
			    commit_graph_position(parent->item) != COMMIT_NOT_FROM_GRAPH ||
			    parent->item->object.flags & SEEN)
				continue;

			parent->item->object.flags |= SEEN;
			data->num_not_in_graph++;

			if (data->num_not_in_graph >= data->limit) {
				result = 1;
				break;
			}

			commit_list_insert(parent->item, &stack);
		}
	}

	commit_list_free(stack);
	return result;
}

static int should_write_commit_graph(struct gc_config *cfg UNUSED)
{
	int result;
	struct cg_auto_data data;

	data.num_not_in_graph = 0;
	data.limit = 100;
	repo_config_get_int(the_repository, "maintenance.commit-graph.auto",
			    &data.limit);

	if (!data.limit)
		return 0;
	if (data.limit < 0)
		return 1;

	result = refs_for_each_ref(get_main_ref_store(the_repository),
				   dfs_on_ref, &data);

	repo_clear_commit_marks(the_repository, SEEN);

	return result;
}

static int run_write_commit_graph(struct maintenance_run_opts *opts)
{
	struct child_process child = CHILD_PROCESS_INIT;

	child.git_cmd = 1;
	child.odb_to_close = the_repository->objects;
	strvec_pushl(&child.args, "commit-graph", "write",
		     "--split", "--reachable", NULL);

	if (opts->quiet)
		strvec_push(&child.args, "--no-progress");
	else
		strvec_push(&child.args, "--progress");

	return !!run_command(&child);
}

static int maintenance_task_commit_graph(struct maintenance_run_opts *opts,
					 struct gc_config *cfg UNUSED)
{
	prepare_repo_settings(the_repository);
	if (!the_repository->settings.core_commit_graph)
		return 0;

	if (run_write_commit_graph(opts)) {
		error(_("failed to write commit-graph"));
		return 1;
	}

	return 0;
}

static int fetch_remote(struct remote *remote, void *cbdata)
{
	struct maintenance_run_opts *opts = cbdata;
	struct child_process child = CHILD_PROCESS_INIT;

	if (remote->skip_default_update)
		return 0;

	child.git_cmd = 1;
	strvec_pushl(&child.args, "fetch", remote->name,
		     "--prefetch", "--prune", "--no-tags",
		     "--no-write-fetch-head", "--recurse-submodules=no",
		     NULL);

	if (opts->quiet)
		strvec_push(&child.args, "--quiet");

	return !!run_command(&child);
}

static int maintenance_task_prefetch(struct maintenance_run_opts *opts,
				     struct gc_config *cfg UNUSED)
{
	if (for_each_remote(fetch_remote, opts)) {
		error(_("failed to prefetch remotes"));
		return 1;
	}

	return 0;
}

static int maintenance_task_gc_foreground(struct maintenance_run_opts *opts,
					  struct gc_config *cfg)
{
	return gc_foreground_tasks(opts, cfg);
}

static int maintenance_task_gc_background(struct maintenance_run_opts *opts,
					  struct gc_config *cfg UNUSED)
{
	struct child_process child = CHILD_PROCESS_INIT;

	child.git_cmd = 1;
	child.odb_to_close = the_repository->objects;
	strvec_push(&child.args, "gc");

	if (opts->auto_flag)
		strvec_push(&child.args, "--auto");
	if (opts->quiet)
		strvec_push(&child.args, "--quiet");
	else
		strvec_push(&child.args, "--no-quiet");
	strvec_push(&child.args, "--no-detach");
	strvec_push(&child.args, "--skip-foreground-tasks");

	return run_command(&child);
}

static int gc_condition(struct gc_config *cfg)
{
	/*
	 * Note that it's fine to drop the repack arguments here, as we execute
	 * git-gc(1) as a separate child process anyway. So it knows to compute
	 * these arguments again.
	 */
	struct strvec repack_args = STRVEC_INIT;
	int ret = need_to_gc(cfg, &repack_args);
	strvec_clear(&repack_args);
	return ret;
}

static int prune_packed(struct maintenance_run_opts *opts)
{
	struct child_process child = CHILD_PROCESS_INIT;

	child.git_cmd = 1;
	strvec_push(&child.args, "prune-packed");

	if (opts->quiet)
		strvec_push(&child.args, "--quiet");

	return !!run_command(&child);
}

struct write_loose_object_data {
	FILE *in;
	int count;
	int batch_size;
};

static int loose_object_auto_limit = 100;

static int loose_object_count(const struct object_id *oid UNUSED,
			      const char *path UNUSED,
			      void *data)
{
	int *count = (int*)data;
	if (++(*count) >= loose_object_auto_limit)
		return 1;
	return 0;
}

static int loose_object_auto_condition(struct gc_config *cfg UNUSED)
{
	int count = 0;

	repo_config_get_int(the_repository, "maintenance.loose-objects.auto",
			    &loose_object_auto_limit);

	if (!loose_object_auto_limit)
		return 0;
	if (loose_object_auto_limit < 0)
		return 1;

	return for_each_loose_file_in_source(the_repository->objects->sources,
					     loose_object_count,
					     NULL, NULL, &count);
}

static int bail_on_loose(const struct object_id *oid UNUSED,
			 const char *path UNUSED,
			 void *data UNUSED)
{
	return 1;
}

static int write_loose_object_to_stdin(const struct object_id *oid,
				       const char *path UNUSED,
				       void *data)
{
	struct write_loose_object_data *d = (struct write_loose_object_data *)data;

	fprintf(d->in, "%s\n", oid_to_hex(oid));

	/* If batch_size is INT_MAX, then this will return 0 always. */
	return ++(d->count) > d->batch_size;
}

static int pack_loose(struct maintenance_run_opts *opts)
{
	struct repository *r = the_repository;
	int result = 0;
	struct write_loose_object_data data;
	struct child_process pack_proc = CHILD_PROCESS_INIT;

	/*
	 * Do not start pack-objects process
	 * if there are no loose objects.
	 */
	if (!for_each_loose_file_in_source(r->objects->sources,
					   bail_on_loose,
					   NULL, NULL, NULL))
		return 0;

	pack_proc.git_cmd = 1;

	strvec_push(&pack_proc.args, "pack-objects");
	if (opts->quiet)
		strvec_push(&pack_proc.args, "--quiet");
	else
		strvec_push(&pack_proc.args, "--no-quiet");
	strvec_pushf(&pack_proc.args, "%s/pack/loose", r->objects->sources->path);

	pack_proc.in = -1;

	/*
	 * git-pack-objects(1) ends up writing the pack hash to stdout, which
	 * we do not care for.
	 */
	pack_proc.out = -1;

	if (start_command(&pack_proc)) {
		error(_("failed to start 'git pack-objects' process"));
		return 1;
	}

	data.in = xfdopen(pack_proc.in, "w");
	data.count = 0;
	data.batch_size = 50000;

	repo_config_get_int(r, "maintenance.loose-objects.batchSize",
			    &data.batch_size);

	/* If configured as 0, then remove limit. */
	if (!data.batch_size)
		data.batch_size = INT_MAX;
	else if (data.batch_size > 0)
		data.batch_size--; /* Decrease for equality on limit. */

	for_each_loose_file_in_source(r->objects->sources,
				      write_loose_object_to_stdin,
				      NULL, NULL, &data);

	fclose(data.in);

	if (finish_command(&pack_proc)) {
		error(_("failed to finish 'git pack-objects' process"));
		result = 1;
	}

	return result;
}

static int maintenance_task_loose_objects(struct maintenance_run_opts *opts,
					  struct gc_config *cfg UNUSED)
{
	return prune_packed(opts) || pack_loose(opts);
}

static int incremental_repack_auto_condition(struct gc_config *cfg UNUSED)
{
	struct packed_git *p;
	int incremental_repack_auto_limit = 10;
	int count = 0;

	prepare_repo_settings(the_repository);
	if (!the_repository->settings.core_multi_pack_index)
		return 0;

	repo_config_get_int(the_repository, "maintenance.incremental-repack.auto",
			    &incremental_repack_auto_limit);

	if (!incremental_repack_auto_limit)
		return 0;
	if (incremental_repack_auto_limit < 0)
		return 1;

	repo_for_each_pack(the_repository, p) {
		if (count >= incremental_repack_auto_limit)
			break;
		if (!p->multi_pack_index)
			count++;
	}

	return count >= incremental_repack_auto_limit;
}

static int multi_pack_index_write(struct maintenance_run_opts *opts)
{
	struct child_process child = CHILD_PROCESS_INIT;

	child.git_cmd = 1;
	strvec_pushl(&child.args, "multi-pack-index", "write", NULL);

	if (opts->quiet)
		strvec_push(&child.args, "--no-progress");
	else
		strvec_push(&child.args, "--progress");

	if (run_command(&child))
		return error(_("failed to write multi-pack-index"));

	return 0;
}

static int multi_pack_index_expire(struct maintenance_run_opts *opts)
{
	struct child_process child = CHILD_PROCESS_INIT;

	child.git_cmd = 1;
	child.odb_to_close = the_repository->objects;
	strvec_pushl(&child.args, "multi-pack-index", "expire", NULL);

	if (opts->quiet)
		strvec_push(&child.args, "--no-progress");
	else
		strvec_push(&child.args, "--progress");

	if (run_command(&child))
		return error(_("'git multi-pack-index expire' failed"));

	return 0;
}

#define TWO_GIGABYTES (INT32_MAX)

static off_t get_auto_pack_size(void)
{
	/*
	 * The "auto" value is special: we optimize for
	 * one large pack-file (i.e. from a clone) and
	 * expect the rest to be small and they can be
	 * repacked quickly.
	 *
	 * The strategy we select here is to select a
	 * size that is one more than the second largest
	 * pack-file. This ensures that we will repack
	 * at least two packs if there are three or more
	 * packs.
	 */
	off_t max_size = 0;
	off_t second_largest_size = 0;
	off_t result_size;
	struct packed_git *p;
	struct repository *r = the_repository;

	odb_reprepare(r->objects);
	repo_for_each_pack(r, p) {
		if (p->pack_size > max_size) {
			second_largest_size = max_size;
			max_size = p->pack_size;
		} else if (p->pack_size > second_largest_size)
			second_largest_size = p->pack_size;
	}

	result_size = second_largest_size + 1;

	/* But limit ourselves to a batch size of 2g */
	if (result_size > TWO_GIGABYTES)
		result_size = TWO_GIGABYTES;

	return result_size;
}

static int multi_pack_index_repack(struct maintenance_run_opts *opts)
{
	struct child_process child = CHILD_PROCESS_INIT;

	child.git_cmd = 1;
	child.odb_to_close = the_repository->objects;
	strvec_pushl(&child.args, "multi-pack-index", "repack", NULL);

	if (opts->quiet)
		strvec_push(&child.args, "--no-progress");
	else
		strvec_push(&child.args, "--progress");

	strvec_pushf(&child.args, "--batch-size=%"PRIuMAX,
				  (uintmax_t)get_auto_pack_size());

	if (run_command(&child))
		return error(_("'git multi-pack-index repack' failed"));

	return 0;
}

static int maintenance_task_incremental_repack(struct maintenance_run_opts *opts,
					       struct gc_config *cfg UNUSED)
{
	prepare_repo_settings(the_repository);
	if (!the_repository->settings.core_multi_pack_index) {
		warning(_("skipping incremental-repack task because core.multiPackIndex is disabled"));
		return 0;
	}

	if (multi_pack_index_write(opts))
		return 1;
	if (multi_pack_index_expire(opts))
		return 1;
	if (multi_pack_index_repack(opts))
		return 1;
	return 0;
}

static int maintenance_task_geometric_repack(struct maintenance_run_opts *opts,
					     struct gc_config *cfg)
{
	struct pack_geometry geometry = {
		.split_factor = 2,
	};
	struct pack_objects_args po_args = {
		.local = 1,
	};
	struct existing_packs existing_packs = EXISTING_PACKS_INIT;
	struct string_list kept_packs = STRING_LIST_INIT_DUP;
	struct child_process child = CHILD_PROCESS_INIT;
	int ret;

	repo_config_get_int(the_repository, "maintenance.geometric-repack.splitFactor",
			    &geometry.split_factor);

	existing_packs.repo = the_repository;
	existing_packs_collect(&existing_packs, &kept_packs);
	pack_geometry_init(&geometry, &existing_packs, &po_args);
	pack_geometry_split(&geometry);

	child.git_cmd = 1;

	strvec_pushl(&child.args, "repack", "-d", "-l", NULL);
	if (geometry.split < geometry.pack_nr)
		strvec_pushf(&child.args, "--geometric=%d",
			     geometry.split_factor);
	else
		add_repack_all_option(cfg, NULL, &child.args);
	if (opts->quiet)
		strvec_push(&child.args, "--quiet");
	if (the_repository->settings.core_multi_pack_index)
		strvec_push(&child.args, "--write-midx");

	if (run_command(&child)) {
		ret = error(_("failed to perform geometric repack"));
		goto out;
	}

	ret = 0;

out:
	existing_packs_release(&existing_packs);
	pack_geometry_release(&geometry);
	return ret;
}

static int geometric_repack_auto_condition(struct gc_config *cfg UNUSED)
{
	struct pack_geometry geometry = {
		.split_factor = 2,
	};
	struct pack_objects_args po_args = {
		.local = 1,
	};
	struct existing_packs existing_packs = EXISTING_PACKS_INIT;
	struct string_list kept_packs = STRING_LIST_INIT_DUP;
	int auto_value = 100;
	int ret;

	repo_config_get_int(the_repository, "maintenance.geometric-repack.auto",
			    &auto_value);
	if (!auto_value)
		return 0;
	if (auto_value < 0)
		return 1;

	repo_config_get_int(the_repository, "maintenance.geometric-repack.splitFactor",
			    &geometry.split_factor);

	existing_packs.repo = the_repository;
	existing_packs_collect(&existing_packs, &kept_packs);
	pack_geometry_init(&geometry, &existing_packs, &po_args);
	pack_geometry_split(&geometry);

	/*
	 * When we'd merge at least two packs with one another we always
	 * perform the repack.
	 */
	if (geometry.split) {
		ret = 1;
		goto out;
	}

	/*
	 * Otherwise, we estimate the number of loose objects to determine
	 * whether we want to create a new packfile or not.
	 */
	if (too_many_loose_objects(auto_value)) {
		ret = 1;
		goto out;
	}

	ret = 0;

out:
	existing_packs_release(&existing_packs);
	pack_geometry_release(&geometry);
	return ret;
}

/*
 * Base-stratum maintenance task: incrementally stratify objects reachable from
 * configured anchor refs into base-stratum packs (the "base stratum"
 * in Stratified GC).
 */

/*
 * Validate all base-stratum packs: check that each pack's anchor ref
 * still exists and still descends from the recorded anchor commit.
 * Demote invalid packs by removing the .base-stratum file.
 */
struct base_stratum_pack_entry {
	struct packed_git *pack;
	struct oid_array anchors;
	char *anchor_ref;
	uint32_t stratified_timestamp;
};

struct base_stratum_pack_group {
	struct base_stratum_pack_entry *entries;
	size_t nr;
	size_t alloc;
};

/*
 * Validate a single base-stratum pack against a pre-resolved tip commit
 * for its anchor ref: check that the recorded anchor commit can be
 * loaded and is an ancestor of the tip. Returns 0 on success, nonzero
 * on failure. The caller resolves the anchor ref and parses the tip
 * commit once per group, so this function is O(1) child processes
 * (zero) instead of one fork per pack.
 */
static int validate_single_base_stratum_pack(struct repository *r,
					  struct base_stratum_pack_entry *entry,
					  struct commit *tip_commit)
{
	size_t i;

	for (i = 0; i < entry->anchors.nr; i++) {
		struct commit *anchor_commit;

		anchor_commit = lookup_commit(r, &entry->anchors.oid[i]);
		if (!anchor_commit || repo_parse_commit(r, anchor_commit)) {
			warning(_("stratify: anchor commit %s cannot be parsed, "
				  "demoting pack %s"),
				oid_to_hex(&entry->anchors.oid[i]),
				entry->pack->pack_name);
			return -1;
		}

		/*
		 * repo_in_merge_bases returns 1 if anchor_commit is an ancestor
		 * of tip_commit, 0 if not, -1 on generation-graph walk failure.
		 * Treat anything but a definitive "ancestor" as not-ancestor and
		 * demote; using a non-ancestor as the validated frontier would
		 * break the closed-set invariant.
		 */
		if (repo_in_merge_bases(r, anchor_commit, tip_commit) <= 0) {
			warning(_("stratify: anchor commit %s is not ancestor "
				  "of %s tip, demoting pack %s"),
				oid_to_hex(&entry->anchors.oid[i]),
				entry->anchor_ref, entry->pack->pack_name);
			return -1;
		}
	}

	return 0;
}

/*
 * Collect all base-stratum packs, grouped by anchor_ref.
 * Each string_list entry's util points to an base_stratum_pack_group.
 *
 * A pack whose .base-stratum file cannot be loaded is KEPT, not demoted,
 * and skipped from the grouping. in_base_stratum is set from the mere
 * existence of the sidecar (see add_packed_git()), so such a pack is
 * still a surface-gc kept-pack boundary. Demoting it (unlinking its
 * .keep) would expose its objects while a later, still-loadable pack of
 * the same anchor that depends on them stays kept -- breaking the
 * closed-set invariant exactly like a per-pack validation cascade would
 * (see validate_stratify_packs()). We cannot recover the pack's
 * anchor_ref to cascade its whole group (the sidecar is unreadable and
 * the pack basename is a one-way hash of the ref), so the only
 * closure-safe action is to leave it kept. It stays invisible to the
 * frontier/dedup computations, which is conservative-safe; reclaiming a
 * genuinely corrupt sidecar is a repair concern, not something this
 * collector may do silently.
 *
 * When `kept_corrupt` is non-NULL it is incremented once per corrupt
 * sidecar kept, so a caller can denounce them (warn loudly, emit trace2,
 * exit non-zero) without the collector deciding policy.
 */
static void collect_base_stratum_pack_groups(struct repository *r,
					 struct string_list *ref_groups,
					 size_t *kept_corrupt)
{
	struct packed_git *p;

	repo_for_each_pack(r, p) {
		struct base_stratum_data adata = { 0 };
		struct string_list_item *item;
		struct base_stratum_pack_group *group;
		struct base_stratum_pack_entry *entry;

		if (!p->in_base_stratum)
			continue;
		if (load_pack_base_stratum(p, &adata)) {
			warning(_("stratify: cannot load .base-stratum for %s, "
				  "keeping it as a base-stratum boundary"),
				p->pack_name);
			if (kept_corrupt)
				(*kept_corrupt)++;
			continue;
		}

		item = string_list_lookup(ref_groups, adata.anchor_ref);
		if (!item) {
			item = string_list_insert(ref_groups, adata.anchor_ref);
			item->util = xcalloc(1, sizeof(struct base_stratum_pack_group));
		}

		group = item->util;
		ALLOC_GROW(group->entries, group->nr + 1, group->alloc);
		entry = &group->entries[group->nr++];
		/*
		 * ALLOC_GROW reallocs without zeroing, so this slot holds
		 * garbage. memset before touching entry->anchors: calling
		 * oid_array_clear() here would FREE_AND_NULL() an
		 * uninitialized ->oid pointer (double free / abort).
		 */
		memset(entry, 0, sizeof(*entry));
		entry->pack = p;
		for (size_t k = 0; k < adata.anchors.nr; k++)
			oid_array_append(&entry->anchors, &adata.anchors.oid[k]);
		entry->anchor_ref = xstrdup(adata.anchor_ref);
		entry->stratified_timestamp = adata.stratified_timestamp;

		clear_base_stratum_data(&adata);
	}
}

static void free_base_stratum_pack_groups(struct string_list *ref_groups)
{
	size_t i;

	for (i = 0; i < ref_groups->nr; i++) {
		struct base_stratum_pack_group *group = ref_groups->items[i].util;
		size_t j;

		for (j = 0; j < group->nr; j++) {
			free(group->entries[j].anchor_ref);
			oid_array_clear(&group->entries[j].anchors);
		}
		free(group->entries);
		free(group);
	}
	string_list_clear(ref_groups, 0);
}

/*
 * Validate all base-stratum packs.
 *
 * Each pack in an anchor's group is checked independently against the
 * current ref tip: if the pack's recorded anchor_commit is no longer an
 * ancestor of the ref, the pack is invalid. When any pack in a group is
 * invalid the WHOLE group is demoted, because surface-gc treats the
 * union of an anchor's base-stratum packs as a single closed boundary
 * and a surviving pack may depend on the invalid one (see the cascade
 * rationale in the loop body). The cascade is keyed on validation and
 * group membership, never on stratified_timestamp ordering, so it is
 * unaffected by a timestamp source that isn't strictly monotone with
 * commit-graph order (clock rollbacks, manual sidecar edits, or any
 * future code path that reuses an older timestamp on a newer pack).
 *
 * Returns the number of packs that should have been demoted but whose
 * sidecar removal failed. A pack that keeps its .base-stratum is still
 * treated as base-stratum by newer git and one that keeps its .keep
 * stays pinned against older-git repacks, so a caller must propagate a
 * non-zero count into its own exit status rather than report a clean
 * validation over a still-invalid pack.
 *
 * Corrupt-sidecar packs are NOT counted in the return value: they are
 * kept as safe boundaries (see collect_base_stratum_pack_groups()) and a
 * single one must not abort all stratification. Their count is reported
 * separately via the optional `kept_corrupt` out-param so the caller can
 * denounce them after finishing its work.
 */
static size_t validate_stratify_packs(struct string_list *configured,
				    int quiet, size_t *kept_corrupt)
{
	struct repository *r = the_repository;
	struct string_list ref_groups = STRING_LIST_INIT_DUP;
	size_t i, orphan_groups = 0, demote_failed = 0;

	collect_base_stratum_pack_groups(r, &ref_groups, kept_corrupt);

	/*
	 * Phase 2: for each configured anchor ref, validate every pack
	 * in the group independently against the current ref tip.
	 *
	 * Resolve the anchor ref and parse the tip commit once per group
	 * so the per-pack ancestry check can run in-process via
	 * repo_in_merge_bases() instead of forking "git merge-base
	 * --is-ancestor" once per pack.
	 *
	 * Orphan groups (anchors with packs on disk but no longer in
	 * maintenance.stratified.anchor) are surfaced via warning +
	 * trace2 but NOT demoted here; automatic demotion would
	 * amplify a config typo into a many-GB re-stratification. The
	 * stratify-prune task is the explicit way to reclaim them.
	 */
	for (i = 0; i < ref_groups.nr; i++) {
		struct string_list_item *item = &ref_groups.items[i];
		struct base_stratum_pack_group *group = item->util;
		const char *anchor_ref = item->string;
		struct object_id ref_oid;
		struct commit *tip_commit = NULL;
		int ref_resolved;
		int group_bad;
		size_t j;

		if (!unsorted_string_list_has_string(configured, anchor_ref)) {
			orphan_groups++;
			trace2_data_string("stratify", r, "orphan-anchor",
					   anchor_ref);
			if (!quiet)
				warning(_("stratify: anchor '%s' has %"PRIuMAX
					  " base-stratum pack(s) but is no "
					  "longer configured; run "
					  "'git maintenance run --task=stratify-prune' "
					  "to reclaim"),
					anchor_ref, (uintmax_t)group->nr);
			/*
			 * Skip per-pack validation for orphans: their
			 * anchor_ref may have been deleted along with the
			 * config entry, in which case validate_single_*
			 * would demote anyway. Pruning is the right tool.
			 */
			continue;
		}

		ref_resolved = refs_resolve_ref_unsafe(
				get_main_ref_store(r), anchor_ref,
				RESOLVE_REF_READING, &ref_oid, NULL) != NULL;
		if (ref_resolved) {
			/* Peel through annotated tags before lookup. */
			tip_commit = lookup_commit_reference_gently(r,
								    &ref_oid, 1);
			if (!tip_commit || repo_parse_commit(r, tip_commit))
				tip_commit = NULL;
		}

		/*
		 * Closure is a whole-group property, not a per-pack one.
		 * surface-gc's --kept-pack-boundary stops the reachability
		 * walk at any object in a kept pack, trusting that the union
		 * of an anchor's base-stratum packs is closed under
		 * reachability. A later pack (e.g. one holding a merge commit)
		 * depends on earlier sibling packs of the same anchor, so
		 * demoting only the individual pack that fails validation can
		 * leave a dependent pack kept while the objects it relies on
		 * are no longer in any kept pack. surface-gc would then cut
		 * the walk at the dependent commit and expire the orphaned
		 * side as unreachable -- a repository-integrity hole, not a
		 * missed optimisation.
		 *
		 * So validate every pack independently, but if ANY pack in
		 * the group is invalid, demote the WHOLE group. Each anchor's
		 * packs together carry its full closure (coverage is never
		 * shared across anchors), so dropping the group falls back to
		 * the active stratum and the same stratify run rebuilds it
		 * from the current tip; no dependent pack is left behind an
		 * unsafe kept-pack boundary. The cascade is keyed on
		 * validation plus group membership, never on
		 * stratified_timestamp ordering, so a non-monotone timestamp
		 * source cannot trigger it.
		 */
		group_bad = 0;
		if (!ref_resolved) {
			if (!quiet)
				warning(_("stratify: anchor ref '%s' no longer "
					  "exists"), anchor_ref);
			group_bad = 1;
		} else if (!tip_commit) {
			if (!quiet)
				warning(_("stratify: anchor ref '%s' tip cannot "
					  "be parsed as a commit"), anchor_ref);
			group_bad = 1;
		} else {
			for (j = 0; j < group->nr; j++) {
				if (validate_single_base_stratum_pack(r,
						&group->entries[j],
						tip_commit)) {
					group_bad = 1;
					break;
				}
			}
		}

		if (!group_bad)
			continue;

		if (!quiet)
			warning(_("stratify: demoting all %"PRIuMAX
				  " base-stratum pack(s) for anchor '%s' to "
				  "preserve the closed-set invariant"),
				(uintmax_t)group->nr, anchor_ref);
		trace2_data_intmax("stratify", r, "group-cascade-demoted",
				   (intmax_t)group->nr);

		for (j = 0; j < group->nr; j++) {
			if (remove_pack_base_stratum(group->entries[j].pack))
				demote_failed++;
		}
	}

	trace2_data_intmax("stratify", r, "orphan-groups", orphan_groups);
	if (demote_failed)
		trace2_data_intmax("stratify", r, "demote-failed",
				   demote_failed);
	free_base_stratum_pack_groups(&ref_groups);
	return demote_failed;
}

/*
 * Add `cand_commit` (OID `cand_oid`) to `frontier`, preserving the
 * invariant that no element of `frontier` is an ancestor of another
 * — i.e., `frontier` is a maximal antichain of stratified commits.
 *
 * Selection is purely topological: committer date is never used to
 * order candidates. A child can commit earlier than its parent
 * (clock skew, amended dates, cherry-picks), so date-based ordering
 * would silently misclassify the antichain.
 *
 * Multiple maximal frontiers are normal in DAGs with merges:
 * `maintenance.stratified.batch-size` can stratify the two sides of
 * a pending merge in separate runs, leaving sidecars whose
 * anchor_commits are siblings on different branches of the merge.
 * Both are legitimate `^bound`s for the next rev-list walk; the
 * antichain collapses to one element once the merge commit itself
 * is stratified.
 *
 * Algorithm:
 *   - If `cand` is an ancestor of any existing element, `cand` is
 *     dominated; drop it.
 *   - Otherwise, remove every existing element that is an ancestor
 *     of `cand` (each is dominated by `cand`), then append `cand`.
 *
 * Equal-OID candidates are handled by the dominance check: a commit
 * is its own ancestor (reflexively in repo_in_merge_bases), so a
 * duplicate falls into the "dominated" branch and is dropped without
 * appending. Order-independence: any reordering of candidate
 * insertions yields the same final antichain.
 *
 * repo_in_merge_bases() returns -1 on walk failure; treat anything
 * other than a definitive "is ancestor" (return value > 0) as
 * not-an-ancestor, matching validate_single_base_stratum_pack().
 */
static void antichain_add(struct repository *r,
			  struct oid_array *frontier,
			  const struct object_id *cand_oid,
			  struct commit *cand_commit)
{
	size_t i, j;

	for (i = 0; i < frontier->nr; i++) {
		struct commit *existing = lookup_commit(r, &frontier->oid[i]);
		if (!existing)
			continue;
		if (repo_in_merge_bases(r, cand_commit, existing) > 0)
			return;
	}

	for (i = 0, j = 0; i < frontier->nr; i++) {
		struct commit *existing = lookup_commit(r, &frontier->oid[i]);
		if (existing &&
		    repo_in_merge_bases(r, existing, cand_commit) > 0)
			continue;
		if (i != j)
			oidcpy(&frontier->oid[j], &frontier->oid[i]);
		j++;
	}
	frontier->nr = j;

	oid_array_append(frontier, cand_oid);
}

/*
 * Populate `out` with the maximal antichain of base-stratum frontier
 * commits for `anchor_ref` along `tip_oid`'s history. Each element
 * is an anchor_commit that
 *
 *   - is an ancestor of tip_oid, AND
 *   - is not an ancestor of any other element in `out`.
 *
 * Suitable for use as `^bound` arguments to rev-list when stratifying
 * tip_oid: rev-list excludes objects reachable from any element in
 * `out`, so the walk skips history already covered by this anchor's
 * pack set. The antichain commonly has one element (linear history)
 * but legitimately has multiple elements between stratifying the
 * branches of a merge and stratifying the merge commit itself.
 *
 * The lookup is per-anchor: base-stratum coverage is anchor-scoped,
 * so an anchor's incrementality is determined by its own pack set
 * only. Another anchor's pack — even one that happens to share
 * history — does not establish coverage for this anchor.
 *
 * Caller initializes `out` with `OID_ARRAY_INIT` and frees with
 * `oid_array_clear()`.
 */
static void find_stratified_frontier(struct repository *r,
				     const char *anchor_ref,
				     const struct object_id *tip_oid,
				     struct oid_array *out)
{
	struct packed_git *p;
	struct commit *tip_commit;

	/*
	 * tip_oid comes from refs_resolve_ref_unsafe(), which for an
	 * annotated tag is the tag object's OID, not the commit it
	 * points to. Peel through tags before looking up the commit so
	 * tag-backed anchors are handled the same as branch tips.
	 */
	tip_commit = lookup_commit_reference_gently(r, tip_oid, 1);
	if (!tip_commit || repo_parse_commit(r, tip_commit))
		return;

	/*
	 * Collect candidate anchor_commits in one pass over the
	 * packed_git list, then process them in a second pass. Calling
	 * commit-graph walks (lookup_commit / repo_parse_commit /
	 * repo_in_merge_bases) inside repo_for_each_pack triggers lazy
	 * loads on the packfile store that corrupt linked-list
	 * iteration mid-walk and silently drop later packs from view.
	 */
	{
		struct oid_array candidates = OID_ARRAY_INIT;
		size_t k;

		repo_for_each_pack(r, p) {
			struct base_stratum_data adata = { 0 };

			if (!p->in_base_stratum)
				continue;
			if (load_pack_base_stratum(p, &adata))
				continue;
			if (!adata.anchor_ref ||
			    strcmp(adata.anchor_ref, anchor_ref)) {
				clear_base_stratum_data(&adata);
				continue;
			}
			for (size_t a = 0; a < adata.anchors.nr; a++)
				oid_array_append(&candidates, &adata.anchors.oid[a]);
			clear_base_stratum_data(&adata);
		}

		for (k = 0; k < candidates.nr; k++) {
			struct commit *anchor_commit;

			anchor_commit = lookup_commit(r, &candidates.oid[k]);
			if (!anchor_commit ||
			    repo_parse_commit(r, anchor_commit))
				continue;

			/*
			 * Validation already enforces that each pack's
			 * anchor_commit is an ancestor of its anchor_ref's
			 * tip, but check again defensively: stale in-memory
			 * state or a manual sidecar edit could violate the
			 * invariant, and using a non-ancestor as ^bound
			 * would over-exclude or have no effect.
			 * repo_in_merge_bases can also return -1 on walk
			 * failure; treat that like a non-ancestor and skip.
			 */
			if (repo_in_merge_bases(r, anchor_commit,
						tip_commit) <= 0)
				continue;

			antichain_add(r, out, &candidates.oid[k], anchor_commit);
		}

		oid_array_clear(&candidates);
	}
}

/*
 * Populate `out` with anchor refs from "maintenance.stratified.anchor",
 * skipping duplicates while preserving config order. Returns 0 on
 * success, -1 if no anchors are configured.
 *
 * Duplicates can arise from overlapping `includeIf` rules that pull the
 * same anchor definition into the merged config more than once. Each
 * extra entry would re-stratify the same ref, doing redundant work and
 * doubling trace2 output. `out` must be initialized by the caller in
 * STRING_LIST_INIT_DUP mode and cleared afterwards.
 */
static int load_unique_stratify_anchors(struct repository *r,
					struct string_list *out)
{
	const struct string_list *raw = NULL;
	size_t i;

	if (repo_config_get_string_multi(r, "maintenance.stratified.anchor",
					 &raw))
		return -1;

	for (i = 0; i < raw->nr; i++) {
		const char *anchor_ref = raw->items[i].string;
		if (!unsorted_string_list_has_string(out, anchor_ref))
			string_list_append(out, anchor_ref);
	}

	return 0;
}

/*
 * Feed data to a child pack-objects' stdin. pack-objects can exit early
 * (e.g. disk full or an internal error), leaving us writing to a broken
 * pipe; the main "git maintenance" process does not otherwise ignore
 * SIGPIPE, so that write would terminate the whole run instead of letting
 * us report a per-anchor failure. Ignore SIGPIPE across the write and
 * return write_in_full()'s result; on failure the caller stops feeding and
 * falls through to the finish_command() error handling, which reaps the
 * child and emits the per-anchor warning.
 */
static ssize_t feed_pack_objects(int fd, const void *buf, size_t len)
{
	ssize_t ret;

	sigchain_push(SIGPIPE, SIG_IGN);
	ret = write_in_full(fd, buf, len);
	sigchain_pop(SIGPIPE);
	return ret;
}

/*
 * Result of the per-anchor readiness query.
 */
enum stratify_caught_up {
	STRATIFY_CAUGHT_UP = 0,        /* no eligible commits left outside the frontier */
	STRATIFY_LAGGING,              /* at least one commit older than cutoff is outside the frontier */
	STRATIFY_NO_FRONTIER,          /* anchor has no base-stratum pack yet */
	STRATIFY_CAUGHT_UP_ERROR,      /* rev-list failed; treat as lagging (fail closed) */
};

/*
 * Ask the graph: is there any commit older than `cutoff_ts` that is
 * reachable from `tip_oid` but not from any element of `anchor_ref`'s
 * maximal stratified frontier antichain?
 *
 * This is the correct readiness signal for surface-gc. The previous
 * implementation returned max(anchor_commit->date) across the
 * frontier and let the caller compare against the cutoff; that's a
 * false positive when one frontier branch is recent but another path
 * from tip still has old eligible commits outside any frontier — e.g.
 * a merge whose old-dated sibling has been stratified, whose
 * recent-dated sibling has been stratified, but whose old-dated
 * merge commit itself has not.
 *
 *	git rev-list --max-count=1 --before=<cutoff> <tip> [^<frontier>...]
 *
 * If that yields a commit, stratify is still lagging for this anchor.
 * If it yields nothing, there are no old eligible commits left
 * outside the base-stratum frontier, so surface-gc is caught up.
 *
 * The lookup is per-anchor: base-stratum coverage is anchor-scoped,
 * so an anchor's readiness depends only on its own pack set.
 */
static enum stratify_caught_up stratify_anchor_caught_up(struct repository *r,
							 const char *anchor_ref,
							 const struct object_id *tip_oid,
							 timestamp_t cutoff_ts)
{
	struct oid_array frontier = OID_ARRAY_INIT;
	struct child_process rev_list = CHILD_PROCESS_INIT;
	struct strbuf out = STRBUF_INIT;
	enum stratify_caught_up result;
	size_t i;

	find_stratified_frontier(r, anchor_ref, tip_oid, &frontier);

	if (!frontier.nr) {
		oid_array_clear(&frontier);
		return STRATIFY_NO_FRONTIER;
	}

	rev_list.git_cmd = 1;
	strvec_pushl(&rev_list.args, "rev-list", "--max-count=1", NULL);
	strvec_pushf(&rev_list.args, "--before=%"PRItime, cutoff_ts);
	strvec_push(&rev_list.args, oid_to_hex(tip_oid));
	for (i = 0; i < frontier.nr; i++)
		strvec_pushf(&rev_list.args, "^%s",
			     oid_to_hex(&frontier.oid[i]));

	rev_list.out = -1;
	if (start_command(&rev_list)) {
		oid_array_clear(&frontier);
		return STRATIFY_CAUGHT_UP_ERROR;
	}
	if (strbuf_read(&out, rev_list.out, 0) < 0) {
		close(rev_list.out);
		finish_command(&rev_list);
		strbuf_release(&out);
		oid_array_clear(&frontier);
		return STRATIFY_CAUGHT_UP_ERROR;
	}
	close(rev_list.out);
	if (finish_command(&rev_list)) {
		strbuf_release(&out);
		oid_array_clear(&frontier);
		return STRATIFY_CAUGHT_UP_ERROR;
	}

	strbuf_trim(&out);
	result = out.len ? STRATIFY_LAGGING : STRATIFY_CAUGHT_UP;

	strbuf_release(&out);
	oid_array_clear(&frontier);
	return result;
}

/*
 * Like stratify_anchor_caught_up(), but reports the full lag distance for a
 * human-readable status report rather than a cheap caught-up/lagging boolean.
 *
 * Counts commits older than `cutoff_ts` still reachable from `tip_oid` after
 * excluding the anchor's stratified frontier antichain, and stores the
 * frontier width (antichain size) in *width. A zero width means the anchor
 * has no base-stratum pack yet (the caller reports that distinctly). Returns
 * the lag count (0 == caught up), or -1 on rev-list failure.
 *
 * This uses `--count` (a full walk) rather than the gate's `--max-count=1`
 * early-stop because the report wants the actual distance, not just presence.
 */
static long stratify_anchor_lag(struct repository *r, const char *anchor_ref,
				const struct object_id *tip_oid,
				timestamp_t cutoff_ts, size_t *width)
{
	struct oid_array frontier = OID_ARRAY_INIT;
	struct child_process rev_list = CHILD_PROCESS_INIT;
	struct strbuf out = STRBUF_INIT;
	long lag = -1;
	int count;
	size_t i;

	find_stratified_frontier(r, anchor_ref, tip_oid, &frontier);
	*width = frontier.nr;
	if (!frontier.nr) {
		oid_array_clear(&frontier);
		return 0;
	}

	rev_list.git_cmd = 1;
	strvec_pushl(&rev_list.args, "rev-list", "--count", NULL);
	strvec_pushf(&rev_list.args, "--before=%"PRItime, cutoff_ts);
	strvec_push(&rev_list.args, oid_to_hex(tip_oid));
	for (i = 0; i < frontier.nr; i++)
		strvec_pushf(&rev_list.args, "^%s", oid_to_hex(&frontier.oid[i]));

	rev_list.out = -1;
	if (start_command(&rev_list))
		goto done;
	if (strbuf_read(&out, rev_list.out, 0) < 0) {
		close(rev_list.out);
		finish_command(&rev_list);
		goto done;
	}
	close(rev_list.out);
	if (finish_command(&rev_list))
		goto done;

	strbuf_trim(&out);
	if (strtol_i(out.buf, 10, &count) || count < 0)
		lag = -1;
	else
		lag = count;

done:
	strbuf_release(&out);
	oid_array_clear(&frontier);
	return lag;
}

/*
 * Build the set of base-stratum packs recorded for `anchor_ref`. The
 * `^<frontier>...` bounds on rev-list usually keep already-packed objects
 * out of the input, but those bounds are at commit granularity; the
 * tree/blob OIDs that survive a tight rev-list window get filtered out
 * inline by oid_in_any_pack() during the streaming pump. Base-stratum
 * coverage is anchor-scoped, so only packs of this anchor count toward
 * this anchor's incrementality.
 *
 * Caller frees *out_packs.
 */
static void load_anchor_packs(const char *anchor_ref,
			      struct packed_git ***out_packs,
			      size_t *out_nr)
{
	struct packed_git **existing = NULL;
	size_t existing_nr = 0, existing_alloc = 0;
	struct packed_git *p;

	repo_for_each_pack(the_repository, p) {
		struct base_stratum_data adata = { 0 };

		if (!p->in_base_stratum)
			continue;
		if (load_pack_base_stratum(p, &adata))
			continue;
		if (!adata.anchor_ref || strcmp(adata.anchor_ref, anchor_ref)) {
			clear_base_stratum_data(&adata);
			continue;
		}
		clear_base_stratum_data(&adata);
		if (open_pack_index(p))
			continue;

		ALLOC_GROW(existing, existing_nr + 1, existing_alloc);
		existing[existing_nr++] = p;
	}

	*out_packs = existing;
	*out_nr = existing_nr;
}

static int oid_in_any_pack(struct packed_git **packs, size_t nr,
			   const struct object_id *oid)
{
	size_t k;
	for (k = 0; k < nr; k++) {
		if (find_pack_entry_one(oid, packs[k]))
			return 1;
	}
	return 0;
}

/*
 * Lazily start pack-objects on the first flush. Returns 0 on success
 * (pack-objects is running and *out_prefix holds the pack basename to
 * be combined with the trailing hash). Returns -1 on failure (with a
 * warning already emitted); the caller should skip this anchor.
 */
static int start_stratify_pack_objects(struct child_process *pack_proc,
				       char **out_prefix,
				       struct repository *r,
				       const char *anchor_ref,
				       int quiet)
{
	struct strbuf basename = STRBUF_INIT;

	format_base_stratum_pack_basename(&basename, r, anchor_ref);
	pack_proc->git_cmd = 1;
	/*
	 * A base-stratum pack must be self-contained: exactly one pack
	 * whose hash we record in the .base-stratum sidecar. pack.packSizeLimit
	 * would split the output into several packs (and print several hashes),
	 * leaving base-stratum-prefixed packs with no sidecar. Disable it for
	 * this child via -c, which sets pack_size_limit_cfg to 0 (a command-line
	 * --max-pack-size=0 would fall back to the configured value instead).
	 */
	strvec_pushl(&pack_proc->args, "-c", "pack.packSizeLimit=0", NULL);
	strvec_push(&pack_proc->args, "pack-objects");
	if (quiet)
		strvec_push(&pack_proc->args, "--quiet");
	else
		strvec_push(&pack_proc->args, "--no-quiet");
	strvec_push(&pack_proc->args, basename.buf);
	*out_prefix = strbuf_detach(&basename, NULL);

	pack_proc->in = -1;
	pack_proc->out = -1;
	if (start_command(pack_proc)) {
		warning(_("stratify: failed to start pack-objects for '%s'"),
			anchor_ref);
		FREE_AND_NULL(*out_prefix);
		return -1;
	}
	return 0;
}

static int maintenance_task_stratify(struct maintenance_run_opts *opts,
				       struct gc_config *cfg UNUSED)
{
	struct repository *r = the_repository;
	struct string_list anchors = STRING_LIST_INIT_DUP;
	const char *min_age_str = "2.weeks.ago";
	timestamp_t min_age_ts;
	unsigned long batch_size = 0;
	int result = 0;
	size_t i, kept_corrupt = 0;

	if (load_unique_stratify_anchors(r, &anchors)) {
		if (!opts->quiet)
			fprintf(stderr, _("stratify: skipped, no anchor refs configured\n"));
		string_list_clear(&anchors, 0);
		return 0;
	}

	/*
	 * Validate existing base-stratum packs before stratifying new ones.
	 * A demotion that could not unlink its sidecar leaves an invalid
	 * pack masquerading as base-stratum, so abort the task rather than
	 * silently proceeding over it: the stale pack still satisfies
	 * oid_in_any_pack() for its anchor, so a new stratify run would omit
	 * objects that only exist in it and produce a pack that is no longer
	 * closed once the invalid pack is eventually demoted.
	 */
	if (validate_stratify_packs(&anchors, opts->quiet, &kept_corrupt)) {
		string_list_clear(&anchors, 0);
		return 1;
	}

	repo_config_get_string_tmp(r, "maintenance.stratified.min-age",
				   &min_age_str);
	min_age_ts = approxidate(min_age_str);

	repo_config_get_ulong(r, "maintenance.stratified.batch-size",
			      &batch_size);

	trace2_data_intmax("stratify", r, "anchors", anchors.nr);

	for (i = 0; i < anchors.nr; i++) {
		const char *anchor_ref = anchors.items[i].string;
		struct object_id tip_oid;
		struct oid_array frontier = OID_ARRAY_INIT;
		struct oid_array recorded_anchors = OID_ARRAY_INIT;
		size_t fi;
		struct child_process rev_list = CHILD_PROCESS_INIT;
		struct child_process pack_proc = CHILD_PROCESS_INIT;
		struct strbuf pack_hash = STRBUF_INIT;
		struct packed_git *new_pack;
		char *pack_prefix = NULL;
		char *pack_path = NULL;

		trace2_region_enter("stratify", anchor_ref, r);

		/* Resolve the anchor ref */
		if (!refs_resolve_ref_unsafe(get_main_ref_store(r),
					     anchor_ref, RESOLVE_REF_READING,
					     &tip_oid, NULL)) {
			warning(_("stratify: cannot resolve anchor ref '%s'"),
				anchor_ref);
			trace2_data_string("stratify", r, "skipped/reason",
					   "unresolvable-ref");
			trace2_region_leave("stratify", anchor_ref, r);
			continue;
		}

		/*
		 * Find this anchor's stratified frontier: the maximal
		 * antichain of anchor_commits among packs recorded for
		 * `anchor_ref` that are ancestors of the current tip.
		 * Each element is passed as a `^bound` to rev-list so
		 * we don't re-walk objects this anchor's earlier packs
		 * already cover. The antichain commonly has one element
		 * (linear history) but legitimately has multiple between
		 * stratifying the branches of a merge and stratifying
		 * the merge commit itself.
		 */
		find_stratified_frontier(r, anchor_ref, &tip_oid, &frontier);

		/*
		 * Use rev-list to find objects reachable from the anchor
		 * ref (up to min-age) that are not reachable from any
		 * already-stratified frontier commit.
		 *
		 * --in-commit-order is required for the batch-size
		 * truncation below to work: without it, rev-list emits
		 * all commits first and then all reachable trees and
		 * blobs, so there are no inline commit boundaries to
		 * truncate at and the trees/blobs of an included commit
		 * would be cut off. With it, each commit is followed by
		 * the trees and blobs reached through that commit, so
		 * we can truncate at the next commit line and the
		 * preserved prefix has full object closure for the
		 * commits it includes.
		 *
		 * git rev-list --objects --in-commit-order --reverse \
		 *	--before=<min-age> <tip> [^<frontier>...]
		 */
		rev_list.git_cmd = 1;
		strvec_pushl(&rev_list.args, "rev-list", "--objects",
			     "--in-commit-order", "--reverse", NULL);
		strvec_pushf(&rev_list.args, "--before=%"PRItime, min_age_ts);
		strvec_push(&rev_list.args, oid_to_hex(&tip_oid));
		for (fi = 0; fi < frontier.nr; fi++)
			strvec_pushf(&rev_list.args, "^%s",
				     oid_to_hex(&frontier.oid[fi]));
		/*
		 * Do NOT pass --quiet to rev-list here: we need its
		 * stdout output to feed pack-objects. --quiet suppresses
		 * all output and would make the task a silent no-op.
		 */

		rev_list.out = -1;
		if (start_command(&rev_list)) {
			warning(_("stratify: failed to start rev-list for '%s'"),
				anchor_ref);
			oid_array_clear(&frontier);
			trace2_region_leave("stratify", anchor_ref, r);
			continue;
		}

		/*
		 * Stream rev-list output to pack-objects one commit at a
		 * time rather than buffering it all in memory. Both
		 * children stay alive concurrently: rev-list produces,
		 * we forward each completed commit's lines to
		 * pack-objects' stdin, and pack-objects drains as we
		 * feed it. When the batch trips (or a fork opens), close
		 * rev-list's stdout so it stops on SIGPIPE — batch-size is
		 * a soft limit: rev-list may run on to the next commit
		 * boundary before we close the pipe, and the parent's
		 * per-anchor memory is bounded by the largest single
		 * commit group (plus pipe and stdio buffering), not by the
		 * full eligible remainder.
		 *
		 * --in-commit-order on rev-list groups each commit with
		 * its reachable trees and blobs, so flushing at commit
		 * boundaries preserves full object closure for every
		 * included commit. Truncating mid-commit would leave
		 * some objects unpacked but advance the frontier past
		 * the commit, so we drop the partial commit instead.
		 *
		 * We only flush (and advance the recorded anchor to) a
		 * commit that dominates everything walked so far this run.
		 * --in-commit-order interleaves the branches of an
		 * un-merged fork, so once a fork opens the in-progress
		 * commit no longer has a single dominating anchor; we stop
		 * the run there and leave the sibling commits for a later
		 * run (where, with one tip already stratified, the other
		 * branch walks linearly and each commit dominates again).
		 *
		 * rev-list --objects format:
		 *   commits: "<hex>\n"
		 *   trees/blobs: "<hex> <path>\n"
		 */
		{
			FILE *rev_in = xfdopen(rev_list.out, "r");
			struct strbuf line = STRBUF_INIT;
			struct strbuf commit_buf = STRBUF_INIT;
			unsigned long fed_objs = 0;
			unsigned long cur_commit_objs = 0;
			size_t filtered_count = 0;
			int truncated = 0;
			int pack_feed_failed = 0;
			int anchor_parse_failed = 0;
			int pack_started = 0;
			int pack_start_failed = 0;
			struct packed_git **existing = NULL;
			size_t existing_nr = 0;
			/*
			 * Maximal antichain of the commits fully included in
			 * the pack so far this run. Every commit written to the
			 * pack is dominated by some element of this set, so the
			 * antichain is recorded verbatim in the sidecar and
			 * forms the pack's coverage frontier for the next run.
			 * Maintained by antichain_add() as each commit is
			 * flushed; see find_stratified_frontier().
			 */
			struct oid_array included = OID_ARRAY_INIT;
			/*
			 * pending_anchor tracks the commit currently being
			 * scanned, only known to be fully included once we
			 * cross into the next commit (or rev-list output ends).
			 */
			struct object_id pending_anchor;
			int have_pending_anchor = 0;

			/*
			 * Peek the first line so we can skip starting
			 * pack-objects when rev-list has nothing to emit.
			 */
			if (strbuf_getline_lf(&line, rev_in) == EOF) {
				fclose(rev_in);
				if (finish_command(&rev_list)) {
					warning(_("stratify: rev-list failed for '%s'"),
						anchor_ref);
					result = 1;
				} else if (!opts->quiet) {
					if (frontier.nr == 1)
						fprintf(stderr,
							_("stratify: '%s' already fully stratified at %s\n"),
							anchor_ref,
							oid_to_hex(&frontier.oid[0]));
					else if (frontier.nr > 1)
						fprintf(stderr,
							_("stratify: '%s' already fully stratified (frontier antichain of %"PRIuMAX" commits)\n"),
							anchor_ref,
							(uintmax_t)frontier.nr);
					else
						fprintf(stderr,
							_("stratify: no objects to stratify for '%s' (all newer than min-age)\n"),
							anchor_ref);
				}
				trace2_data_intmax("stratify", r,
						   "objects/stratified", 0);
				strbuf_release(&line);
				strbuf_release(&commit_buf);
				oid_array_clear(&included);
				oid_array_clear(&frontier);
				trace2_region_leave("stratify", anchor_ref, r);
				continue;
			}

			/*
			 * Build the set of base-stratum packs for this
			 * anchor once; the inner loop calls
			 * oid_in_any_pack() per OID to filter out lines
			 * for objects already packed in prior runs.
			 */
			load_anchor_packs(anchor_ref, &existing, &existing_nr);

			/*
			 * Streaming loop. Process the peeked first line,
			 * then continue reading via do/while so each line
			 * goes through the same code path. Buffer one
			 * commit's lines at a time and flush to
			 * pack-objects' stdin at each commit boundary
			 * (see below).
			 *
			 * pack-objects is started lazily on the first
			 * flush: if the per-OID filter drops every line,
			 * we never fork pack-objects and avoid writing an
			 * empty pack.
			 */
			do {
				int is_commit_line = !memchr(line.buf, ' ', line.len);

				/*
				 * A commit line means the previous pending
				 * commit's closure has now been fully emitted.
				 * Flush it to pack-objects and add it to the
				 * coverage antichain.
				 */
				if (is_commit_line && have_pending_anchor) {
					struct commit *pc;

					if (commit_buf.len) {
						if (!pack_started) {
							if (start_stratify_pack_objects(
								    &pack_proc,
								    &pack_prefix,
								    r, anchor_ref,
								    opts->quiet)) {
								pack_start_failed = 1;
								break;
							}
							pack_started = 1;
						}
						if (feed_pack_objects(pack_proc.in,
								      commit_buf.buf,
								      commit_buf.len) < 0) {
							pack_feed_failed = 1;
							break;
						}
					}
					strbuf_reset(&commit_buf);
					fed_objs += cur_commit_objs;
					cur_commit_objs = 0;

					/*
					 * Add the just-flushed commit to the
					 * coverage antichain. A parse failure
					 * here means rev-list emitted a commit
					 * we cannot load — we cannot prove the
					 * pack is closed, so abort this anchor
					 * without writing a sidecar.
					 */
					pc = lookup_commit_reference_gently(
						r, &pending_anchor, 1);
					if (!pc || repo_parse_commit(r, pc)) {
						anchor_parse_failed = 1;
						break;
					}
					antichain_add(r, &included,
						      &pending_anchor, pc);

					/*
					 * Soft batch limit: stop once this many
					 * objects have been flushed. We stop on
					 * a commit boundary, so the recorded
					 * antichain dominates the pack and a
					 * follow-up run resumes from it.
					 */
					if (batch_size > 0 && fed_objs >= batch_size) {
						truncated = 1;
						break;
					}
				}

				if (is_commit_line)
					have_pending_anchor =
						!get_oid_hex(line.buf, &pending_anchor);

				/*
				 * Apply the per-OID already-packed filter
				 * inline; objects already in a base-stratum
				 * pack for this anchor are skipped from the
				 * output. The ^frontier bounds (built from the
				 * recorded anchor antichain) normally exclude
				 * them already, so this is a safety net for
				 * commit-granularity leakage and should rarely
				 * fire.
				 */
				{
					struct object_id oid;
					int skip = !get_oid_hex(line.buf, &oid)
						&& oid_in_any_pack(existing,
								   existing_nr,
								   &oid);
					if (skip) {
						filtered_count++;
					} else {
						strbuf_add(&commit_buf,
							   line.buf, line.len);
						strbuf_addch(&commit_buf, '\n');
						cur_commit_objs++;
					}
				}
			} while (strbuf_getline_lf(&line, rev_in) != EOF);

			if (pack_start_failed) {
				fclose(rev_in);
				finish_command(&rev_list);
				strbuf_release(&line);
				strbuf_release(&commit_buf);
				free(existing);
				oid_array_clear(&included);
				oid_array_clear(&frontier);
				result = 1;
				trace2_region_leave("stratify", anchor_ref, r);
				continue;
			}

			/*
			 * Loop ended at rev-list EOF without stopping early:
			 * the last pending commit's objects were fully
			 * consumed, so complete it like any other commit
			 * boundary and add it to the coverage antichain.
			 *
			 * The recorded antichain (not the ref tip) is what the
			 * sidecar stores. The rev-list is bounded by
			 * --before=<min-age>, so it represents the actual
			 * stratified frontier. Using the ref tip would skip
			 * objects currently newer than min-age but eligible in
			 * future runs as time passes.
			 */
			if (!truncated && !pack_feed_failed && !anchor_parse_failed &&
			    have_pending_anchor) {
				if (commit_buf.len) {
					if (!pack_started) {
						if (start_stratify_pack_objects(
							    &pack_proc,
							    &pack_prefix,
							    r, anchor_ref,
							    opts->quiet)) {
							fclose(rev_in);
							finish_command(&rev_list);
							strbuf_release(&line);
							strbuf_release(&commit_buf);
							free(existing);
							oid_array_clear(&included);
							oid_array_clear(&frontier);
							oid_array_clear(&recorded_anchors);
							result = 1;
							trace2_region_leave("stratify", anchor_ref, r);
							continue;
						}
						pack_started = 1;
					}
					if (feed_pack_objects(pack_proc.in,
							      commit_buf.buf,
							      commit_buf.len) < 0)
						pack_feed_failed = 1;
				}
				if (!pack_feed_failed) {
					struct commit *pc;

					fed_objs += cur_commit_objs;
					cur_commit_objs = 0;
					pc = lookup_commit_reference_gently(
						r, &pending_anchor, 1);
					if (!pc || repo_parse_commit(r, pc))
						anchor_parse_failed = 1;
					else
						antichain_add(r, &included,
							      &pending_anchor, pc);
				}
			}

			fclose(rev_in);          /* closes rev_list.out */
			if (pack_started) {
				close(pack_proc.in); /* signal EOF */
				strbuf_read(&pack_hash, pack_proc.out,
					    the_hash_algo->hexsz);
				close(pack_proc.out);
				strbuf_trim_trailing_newline(&pack_hash);
			}

			{
				int rev_list_rc = finish_command(&rev_list);

				/*
				 * SIGPIPE (rc 128+13=141) is expected
				 * when we close rev-list's stdout early
				 * because the batch tripped or a fork
				 * opened. Any other non-zero status means
				 * the walk did not complete; any partial
				 * output is not a closed set, so we must
				 * not advance the frontier by writing a
				 * sidecar from it.
				 */
				if (rev_list_rc &&
				    !((truncated || pack_feed_failed ||
				       anchor_parse_failed) && rev_list_rc == 141)) {
					warning(_("stratify: rev-list failed for '%s'"),
						anchor_ref);
					if (pack_started)
						finish_command(&pack_proc);
					strbuf_release(&pack_hash);
					strbuf_release(&line);
					strbuf_release(&commit_buf);
					free(existing);
					free(pack_prefix);
					oid_array_clear(&included);
					oid_array_clear(&frontier);
					oid_array_clear(&recorded_anchors);
					result = 1;
					trace2_region_leave("stratify", anchor_ref, r);
					continue;
				}
			}

			if (pack_started && finish_command(&pack_proc)) {
				warning(_("stratify: pack-objects failed for '%s'"),
					anchor_ref);
				strbuf_release(&pack_hash);
				strbuf_release(&line);
				strbuf_release(&commit_buf);
				free(existing);
				free(pack_prefix);
				oid_array_clear(&included);
				oid_array_clear(&frontier);
				oid_array_clear(&recorded_anchors);
				result = 1;
				trace2_region_leave("stratify", anchor_ref, r);
				continue;
			}

			/*
			 * A commit emitted by rev-list could not be parsed, so
			 * we cannot prove the pack is a closed set: discard it
			 * (leave the orphan pack for a later gc to reclaim) and
			 * do not record a sidecar.
			 */
			if (anchor_parse_failed) {
				warning(_("stratify: could not parse a stratified "
					  "commit for '%s'; not recording pack"),
					anchor_ref);
				strbuf_release(&pack_hash);
				strbuf_release(&line);
				strbuf_release(&commit_buf);
				free(pack_prefix);
				oid_array_clear(&included);
				oid_array_clear(&frontier);
				oid_array_clear(&recorded_anchors);
				result = 1;
				continue;
			}

			/*
			 * Copy the coverage antichain out of this block's scope
			 * so the sidecar write below can record it.
			 */
			for (size_t k = 0; k < included.nr; k++)
				oid_array_append(&recorded_anchors,
						 &included.oid[k]);

			if (!opts->quiet) {
				if (!pack_started) {
					fprintf(stderr,
						_("stratify: all objects already packed for '%s'\n"),
						anchor_ref);
				} else if (truncated)
					fprintf(stderr,
						_("stratify: stratified %lu objects (batch limit) for '%s' (%"PRIuMAX" anchor(s))\n"),
						fed_objs, anchor_ref,
						(uintmax_t)recorded_anchors.nr);
				else
					fprintf(stderr,
						_("stratify: stratified %lu objects for '%s' (%"PRIuMAX" anchor(s))\n"),
						fed_objs, anchor_ref,
						(uintmax_t)recorded_anchors.nr);
				if (filtered_count)
					fprintf(stderr,
						_("stratify: skipped %"PRIuMAX" objects already in base-stratum packs for '%s'\n"),
						(uintmax_t)filtered_count, anchor_ref);
			}

			trace2_data_intmax("stratify", r, "objects/stratified",
					   fed_objs);
			trace2_data_intmax("stratify", r, "batch/truncated",
					   truncated);
			trace2_data_intmax("stratify", r, "anchors/recorded",
					   recorded_anchors.nr);
			if (filtered_count)
				trace2_data_intmax("stratify", r,
						   "objects/already-packed",
						   filtered_count);

			strbuf_release(&line);
			strbuf_release(&commit_buf);
			oid_array_clear(&included);
			free(existing);
		}

		if (!pack_hash.len) {
			strbuf_release(&pack_hash);
			free(pack_prefix);
			oid_array_clear(&frontier);
			oid_array_clear(&recorded_anchors);
			trace2_region_leave("stratify", anchor_ref, r);
			continue;
		}

		/*
		 * A pack was produced but no anchor was recorded — this should
		 * not happen (every flushed commit adds to the antichain), but
		 * a sidecar with no anchors is unusable, so refuse to write one.
		 */
		if (!recorded_anchors.nr) {
			warning(_("stratify: produced a pack with no recorded "
				  "anchor for '%s'"), anchor_ref);
			strbuf_release(&pack_hash);
			free(pack_prefix);
			oid_array_clear(&frontier);
			oid_array_clear(&recorded_anchors);
			result = 1;
			trace2_region_leave("stratify", anchor_ref, r);
			continue;
		}

		/*
		 * Write the .base-stratum sidecar for the new pack.
		 * pack-objects writes to <base>-<hash>.pack and outputs
		 * <hash> on stdout. We need to find the pack to write
		 * the sidecar.
		 */
		{
			char *full_prefix = pack_prefix;
			pack_prefix = xstrfmt("%s-%s", full_prefix,
					      pack_hash.buf);
			free(full_prefix);
		}
		pack_path = xstrfmt("%s.idx", pack_prefix);

		/*
		 * Register the new pack with the in-memory packfile store
		 * so that the helpers we call below see it via
		 * repo_for_each_pack — write_pack_base_stratum() flips
		 * new_pack->in_base_stratum and any subsequent lookup of
		 * base-stratum coverage in this same process needs to find
		 * the pack on the store.
		 */
		{
			struct odb_source_files *files =
				odb_source_files_downcast(r->objects->sources);
			new_pack = packfile_store_load_pack(files->packed,
							    pack_path, 1);
		}
		if (new_pack) {
			if (write_pack_base_stratum(new_pack, &recorded_anchors,
						    anchor_ref,
						    (uint32_t)time(NULL))) {
				warning(_("stratify: failed to write base-stratum "
					  "metadata for '%s'"), anchor_ref);
				result = 1;
			} else {
				new_pack->in_base_stratum = 1;
			}
		} else {
			/*
			 * The single expected pack could not be loaded from the
			 * hash pack-objects reported (e.g. an unexpected
			 * multi-pack split). Without a sidecar the pack is not a
			 * recognized base stratum, so surface the failure rather
			 * than reporting success.
			 */
			warning(_("stratify: could not load packed objects for '%s'"),
				anchor_ref);
			result = 1;
		}

		free(pack_path);
		free(pack_prefix);
		strbuf_release(&pack_hash);
		oid_array_clear(&frontier);
		oid_array_clear(&recorded_anchors);
		trace2_region_leave("stratify", anchor_ref, r);
	}

	string_list_clear(&anchors, 0);

	/*
	 * Denounce corrupt-sidecar packs kept as safe boundaries by
	 * validation: every healthy anchor was still stratified above, but a
	 * kept corrupt pack is an unresolved condition (its objects stay
	 * pinned and invisible to incrementality). Emit a trace2 metric and
	 * exit non-zero so scheduled-maintenance monitoring notices; the
	 * non-zero status persists every run until 'stratify-prune' reclaims
	 * the pack (when redundant) or an operator repairs it.
	 */
	if (kept_corrupt) {
		trace2_data_intmax("stratify", r, "corrupt-sidecar-kept",
				   (intmax_t)kept_corrupt);
		result = 1;
	}
	return result;
}

static int stratify_auto_condition(struct gc_config *cfg UNUSED)
{
	const struct string_list *anchors = NULL;

	/* Only run if anchor refs are configured */
	if (repo_config_get_string_multi(the_repository,
					 "maintenance.stratified.anchor",
					 &anchors))
		return 0;

	return anchors->nr > 0;
}

static int repo_has_base_stratum_packs(struct repository *r)
{
	struct packed_git *p;
	repo_for_each_pack(r, p)
		if (p->in_base_stratum)
			return 1;
	return 0;
}

/*
 * Consolidate base-stratum packs: apply geometric repacking within
 * each anchor ref's pack set to bound the number of base-stratum packs.
 */
static int maintenance_task_consolidate_stratum(
		struct maintenance_run_opts *opts,
		struct gc_config *cfg UNUSED)
{
	struct repository *r = the_repository;
	struct string_list ref_groups = STRING_LIST_INIT_DUP;
	struct string_list configured = STRING_LIST_INIT_DUP;
	int split_factor = 2;
	int result = 0;
	size_t i;

	repo_config_get_int(r, "maintenance.consolidate-stratum.splitfactor",
			    &split_factor);
	if (split_factor < 2)
		split_factor = 2;

	/*
	 * Standalone invocation safety: when run on its own (rather than
	 * after stratify in the same `git maintenance run` invocation),
	 * consolidate-stratum must validate that each pack's anchor_commit
	 * is still an ancestor of the configured anchor ref before merging.
	 * A pack invalidated by a force-push or branch rewind would otherwise
	 * get folded into the merged result and resurrected as a valid
	 * base-stratum pack at the new path.
	 *
	 * If no anchors are configured every pack is an orphan; merging
	 * them would re-anchor the cluster under whichever orphan ref
	 * happened to win the group sort. Skip and point the user at
	 * stratify-prune, which is the explicit tool for retiring packs.
	 */
	if (load_unique_stratify_anchors(r, &configured)) {
		if (repo_has_base_stratum_packs(r))
			warning(_("consolidate-stratum: skipped, no configured "
				  "anchors; run 'git maintenance run "
				  "--task=stratify-prune' to retire existing "
				  "base-stratum packs"));
		string_list_clear(&configured, 0);
		return 0;
	}

	/*
	 * Abort before collecting/merging if an invalid pack could not be
	 * demoted: merging a group that still contains a stale pack would
	 * fold its objects into the consolidated result and resurrect them
	 * under the merged pack's path. (ref_groups is still empty here, so
	 * only the configured list needs freeing.)
	 */
	if (validate_stratify_packs(&configured, opts->quiet, NULL)) {
		string_list_clear(&configured, 0);
		return 1;
	}

	collect_base_stratum_pack_groups(r, &ref_groups, NULL);
	trace2_data_intmax("consolidate-stratum", r, "groups",
			   ref_groups.nr);

	for (i = 0; i < ref_groups.nr; i++) {
		struct base_stratum_pack_group *group = ref_groups.items[i].util;
		struct packed_git **packs;
		uint32_t split, j;
		struct oid_array below_antichain = OID_ARRAY_INIT;
		const char *anchor_ref = ref_groups.items[i].string;
		struct child_process pack_proc = CHILD_PROCESS_INIT;
		struct strbuf pack_hash = STRBUF_INIT;
		char *pack_prefix, *pack_path;
		struct packed_git *new_pack;

		if (group->nr < 2)
			continue;

		/*
		 * Skip orphan groups whose anchor is no longer configured;
		 * stratify-prune is the task that handles demotion. Merging
		 * across an orphan group would re-stamp the result with the
		 * unconfigured anchor_ref and re-anchor the cluster — i.e.,
		 * resurrect packs the user already chose to abandon.
		 */
		if (!unsorted_string_list_has_string(&configured, anchor_ref))
			continue;

		trace2_region_enter("consolidate-stratum", anchor_ref, r);
		trace2_data_intmax("consolidate-stratum", r, "packs/total",
				   group->nr);

		/* Build array sorted by object count (ascending) */
		ALLOC_ARRAY(packs, group->nr);
		for (j = 0; j < group->nr; j++)
			packs[j] = group->entries[j].pack;
		QSORT(packs, group->nr, pack_geometry_cmp);

		split = compute_pack_geometry_split(packs, group->nr,
						    split_factor);
		if (split < 2) {
			free(packs);
			trace2_data_intmax("consolidate-stratum", r,
					   "packs/merged", 0);
			trace2_region_leave("consolidate-stratum",
					    anchor_ref, r);
			continue;
		}
		trace2_data_intmax("consolidate-stratum", r,
				   "packs/merging", split);

		/*
		 * Build the maximal antichain of anchor_commits by unioning
		 * the recorded anchor sets of all the below-split packs.
		 * Validation has already established that every recorded
		 * anchor is an ancestor of this anchor's tip, so the antichain
		 * elements are all valid ancestors — but they may not be
		 * pairwise comparable (legitimate merge histories produce
		 * sibling anchors; see find_stratified_frontier()).
		 *
		 * The merged sidecar records the whole antichain, so a
		 * multi-frontier merge is fine: the union of the inputs'
		 * coverage frontiers is itself a valid frontier for the merged
		 * pack (every object in the merged pack is reachable from some
		 * element of the union, by closure of each input). No need to
		 * wait for stratify to advance past a merge commit.
		 *
		 * Selection is topological; we do NOT use
		 * e->stratified_timestamp: that records wall-clock time at
		 * sidecar write and is not a trustworthy ordering signal.
		 */
		for (j = 0; j < group->nr; j++) {
			struct base_stratum_pack_entry *e = &group->entries[j];
			size_t k;
			int below_split = 0;

			for (k = 0; k < split; k++) {
				if (packs[k] == e->pack) {
					below_split = 1;
					break;
				}
			}
			if (!below_split)
				continue;

			for (k = 0; k < e->anchors.nr; k++) {
				struct commit *anchor_commit;

				anchor_commit = lookup_commit(r, &e->anchors.oid[k]);
				if (!anchor_commit ||
				    repo_parse_commit(r, anchor_commit))
					continue;

				antichain_add(r, &below_antichain,
					      &e->anchors.oid[k], anchor_commit);
			}
		}

		if (below_antichain.nr == 0) {
			warning(_("consolidate-stratum: no valid anchor "
				  "among below-split packs for '%s'"),
				anchor_ref);
			oid_array_clear(&below_antichain);
			free(packs);
			result = 1;
			trace2_region_leave("consolidate-stratum",
					    anchor_ref, r);
			continue;
		}

		trace2_data_intmax("consolidate-stratum", r,
				   "merged/antichain-size", below_antichain.nr);

		/*
		 * Merge packs below the split using pack-objects --stdin-packs.
		 * Positive basenames are included, ^-prefixed ones excluded.
		 *
		 * Use the same anchor-scoped basename as the stratify task so
		 * two anchors whose merged pack contents collide do not
		 * overwrite each other's pack and sidecar.
		 */
		pack_proc.git_cmd = 1;
		/* See start_stratify_pack_objects(): the merged pack must be a
		 * single self-contained pack, so disable pack.packSizeLimit. */
		strvec_pushl(&pack_proc.args, "-c", "pack.packSizeLimit=0", NULL);
		strvec_push(&pack_proc.args, "pack-objects");
		if (opts->quiet)
			strvec_push(&pack_proc.args, "--quiet");
		else
			strvec_push(&pack_proc.args, "--no-quiet");
		strvec_push(&pack_proc.args, "--stdin-packs");
		{
			struct strbuf basename = STRBUF_INIT;
			format_base_stratum_pack_basename(&basename, r,
							  anchor_ref);
			strvec_push(&pack_proc.args, basename.buf);
			pack_prefix = strbuf_detach(&basename, NULL);
		}

		pack_proc.in = -1;
		pack_proc.out = -1;

		if (start_command(&pack_proc)) {
			warning(_("consolidate-stratum: failed to start "
				  "pack-objects for '%s'"), anchor_ref);
			oid_array_clear(&below_antichain);
			free(pack_prefix);
			free(packs);
			result = 1;
			trace2_region_leave("consolidate-stratum",
					    anchor_ref, r);
			continue;
		}

		{
			struct strbuf stdin_buf = STRBUF_INIT;

			/*
			 * Only feed below-split packs as positive entries.
			 * Do NOT ^-exclude above-split packs: base-stratum packs
			 * must maintain the closed-set property (all objects
			 * reachable from the anchor are present). Excluding
			 * objects that also appear in above-split packs would
			 * break this if pack demotion later removes those
			 * packs.
			 */
			for (j = 0; j < split; j++)
				strbuf_addf(&stdin_buf, "%s\n",
					    pack_basename(packs[j]));
			/*
			 * Ignore SIGPIPE while feeding: if pack-objects has
			 * already exited, the broken-pipe write must not kill
			 * the whole maintenance run. The finish_command()
			 * check below reaps the child and reports the failure.
			 */
			feed_pack_objects(pack_proc.in, stdin_buf.buf,
					  stdin_buf.len);
			strbuf_release(&stdin_buf);
			close(pack_proc.in);
		}

		strbuf_read(&pack_hash, pack_proc.out, the_hash_algo->hexsz);
		close(pack_proc.out);
		strbuf_trim_trailing_newline(&pack_hash);

		if (finish_command(&pack_proc) || !pack_hash.len) {
			warning(_("consolidate-stratum: pack-objects "
				  "failed for '%s'"), anchor_ref);
			oid_array_clear(&below_antichain);
			strbuf_release(&pack_hash);
			free(pack_prefix);
			free(packs);
			result = 1;
			trace2_region_leave("consolidate-stratum",
					    anchor_ref, r);
			continue;
		}

		/* Write .base-stratum sidecar for the new merged pack */
		{
			char *full_prefix = pack_prefix;
			pack_prefix = xstrfmt("%s-%s", full_prefix,
					      pack_hash.buf);
			free(full_prefix);
		}
		pack_path = xstrfmt("%s.idx", pack_prefix);
		{
			struct odb_source_files *files =
				odb_source_files_downcast(r->objects->sources);
			new_pack = packfile_store_load_pack(files->packed,
							    pack_path, 1);
		}
		free(pack_path);
		free(pack_prefix);
		strbuf_release(&pack_hash);
		if (!new_pack) {
			/*
			 * The merged pack was written but we cannot stat
			 * its .pack file. Leave the source packs in place
			 * so the anchor's objects remain reachable through
			 * a recognized base-stratum pack; the orphaned
			 * pack-objects output (if any) is harmless and can
			 * be cleaned up by a later run.
			 */
			warning(_("consolidate-stratum: failed to register "
				  "merged pack for '%s'; keeping source packs"),
				anchor_ref);
			oid_array_clear(&below_antichain);
			free(packs);
			result = 1;
			trace2_region_leave("consolidate-stratum",
					    anchor_ref, r);
			continue;
		}
		if (write_pack_base_stratum(new_pack, &below_antichain,
					    anchor_ref,
					    (uint32_t)time(NULL))) {
			/*
			 * The merged pack file exists but its
			 * .base-stratum sidecar (or .keep) was not
			 * durably installed. Leave the source packs in
			 * place so the anchor's objects remain reachable
			 * through recognized base-stratum packs; the
			 * orphaned merged pack is harmless and can be
			 * cleaned up by a later run.
			 */
			warning(_("consolidate-stratum: failed to write "
				  "base-stratum metadata for '%s'; keeping "
				  "source packs"), anchor_ref);
			oid_array_clear(&below_antichain);
			free(packs);
			result = 1;
			trace2_region_leave("consolidate-stratum",
					    anchor_ref, r);
			continue;
		}
		oid_array_clear(&below_antichain);
		new_pack->in_base_stratum = 1;

		/* Remove the old packs that were merged */
		{
			char *packdir = mkpathdup("%s/pack",
						  r->objects->sources->path);

			for (j = 0; j < split; j++) {
				struct strbuf base = STRBUF_INIT;

				strbuf_addstr(&base, pack_basename(packs[j]));
				strbuf_strip_suffix(&base, ".pack");
				repack_remove_redundant_pack(r, packdir,
							     base.buf);
				strbuf_release(&base);
			}
			free(packdir);
		}

		free(packs);
		trace2_data_intmax("consolidate-stratum", r,
				   "packs/merged", split);
		trace2_region_leave("consolidate-stratum", anchor_ref, r);
	}

	free_base_stratum_pack_groups(&ref_groups);
	string_list_clear(&configured, 0);
	return result;
}

/*
 * Reclaim corrupt-sidecar packs that are provably redundant, and report
 * (denounce) the rest. A pack whose .base-stratum will not load is kept
 * by the collector because its anchor is unrecoverable, so it cannot be
 * cascade-demoted with its group (see collect_base_stratum_pack_groups()).
 * But if every object in such a pack is also present in another
 * base-stratum pack that will REMAIN kept, demoting the corrupt copy
 * cannot break closure: each object stays reachable through a kept pack.
 *
 * Holders (the packs that remain kept and may cover a corrupt pack) are
 * the in_base_stratum packs whose sidecar loads and -- when anchors are
 * configured -- whose anchor is configured. Orphan groups have already
 * been demoted by the caller (their in_base_stratum flag is cleared), so
 * a fresh scan here naturally excludes them. Redundancy is deliberately
 * NOT checked against other corrupt packs or about-to-be-demoted orphans:
 * coverage may only be credited to a pack we are sure stays.
 *
 * The redundancy test only matters while a configured anchor might still
 * depend on the pack. When no anchors are configured the operator is
 * winding stratification down: there is no frontier to strand, and since
 * demotion merely unlinks sidecars (the .pack/.idx remain as a regular
 * pack), no objects are lost. In that case every corrupt pack is demoted
 * unconditionally so the documented retirement path completes even when
 * the last remaining sidecar is unreadable.
 *
 * Returns the number of corrupt packs that could not be reclaimed and
 * remain kept -- the caller's denounce count. *demoted_out is increased
 * by the number reclaimed.
 */
static size_t reclaim_redundant_corrupt_packs(struct repository *r,
					      struct maintenance_run_opts *opts,
					      struct string_list *configured,
					      int have_configured,
					      size_t *demoted_out)
{
	struct packed_git **holders = NULL;
	size_t holders_nr = 0, holders_alloc = 0;
	struct packed_git **corrupt = NULL;
	size_t corrupt_nr = 0, corrupt_alloc = 0;
	struct string_list covered = STRING_LIST_INIT_DUP;
	struct packed_git *p;
	size_t i, reclaimed = 0, orphan_demoted = 0, kept = 0;
	int guard_block = 0;

	/* Partition base-stratum packs into holders and corrupt. */
	repo_for_each_pack(r, p) {
		struct base_stratum_data adata = { 0 };

		if (!p->in_base_stratum)
			continue;
		if (load_pack_base_stratum(p, &adata)) {
			ALLOC_GROW(corrupt, corrupt_nr + 1, corrupt_alloc);
			corrupt[corrupt_nr++] = p;
			continue;
		}
		if (have_configured &&
		    !unsorted_string_list_has_string(configured, adata.anchor_ref)) {
			clear_base_stratum_data(&adata);
			continue;
		}
		/*
		 * This loadable pack gives its anchor a usable frontier:
		 * find_stratified_frontier() reads the antichain straight from
		 * the sidecar, so it counts even if the .idx later fails to
		 * open. Record which configured anchors still have such a pack
		 * so the reclaim below can refuse to strand one.
		 */
		if (have_configured &&
		    !unsorted_string_list_has_string(&covered, adata.anchor_ref))
			string_list_append(&covered, adata.anchor_ref);
		clear_base_stratum_data(&adata);
		if (open_pack_index(p))
			continue;
		ALLOC_GROW(holders, holders_nr + 1, holders_alloc);
		holders[holders_nr++] = p;
	}

	/*
	 * A corrupt pack's anchor_ref is unrecoverable, so we cannot tell
	 * which configured anchor it belongs to -- and in the duplicate-anchor
	 * case its objects are byte-for-byte identical to another anchor's
	 * pack, making attribution fundamentally impossible. Crediting its
	 * coverage to the global holder union would let us reclaim the only
	 * sidecar standing in for a configured anchor's frontier: closure
	 * survives (another anchor holds the objects), but that anchor is left
	 * with no base-stratum frontier of its own, so surface-gc skips it
	 * with "no stratified commits yet" until a fresh stratify pass rebuilds
	 * the pack. base-stratum coverage is anchor-scoped, and reclaim must
	 * honor that.
	 *
	 * So when any configured anchor has lost every loadable pack, refuse
	 * to reclaim corrupt packs: keep and denounce them (non-zero exit plus
	 * the corrupt-sidecar-kept metric) so the broken anchor stays visible
	 * and a re-stratify is prompted, rather than silently clearing the
	 * alarm. This is deliberately conservative -- it may keep a corrupt
	 * pack that genuinely belongs to a still-covered anchor -- but the
	 * surplus is reclaimed on the next prune once every anchor is whole.
	 */
	if (have_configured) {
		for (i = 0; i < configured->nr; i++) {
			if (!unsorted_string_list_has_string(&covered,
					configured->items[i].string)) {
				guard_block = 1;
				break;
			}
		}
	}

	for (i = 0; i < corrupt_nr; i++) {
		struct packed_git *cp = corrupt[i];
		uint32_t n;
		int redundant = 1;

		if (guard_block) {
			/*
			 * A configured anchor has no loadable frontier pack;
			 * this corrupt pack might be its only stand-in, and we
			 * cannot prove otherwise. Keep and denounce.
			 */
			kept++;
			continue;
		}

		/*
		 * Wind-down (no configured anchors): there is no anchor whose
		 * frontier this pack could be stranding, and demotion only
		 * unlinks the .base-stratum and .keep sidecars -- the .pack and
		 * .idx stay on disk as a regular pack -- so no objects are lost.
		 * Reclaim unconditionally. Without this the documented
		 * retirement flow (unset anchors, then stratify-prune) could
		 * never fully disable stratification when the last sidecar is
		 * unreadable: the orphan sweep has already demoted every
		 * loadable pack, so holders_nr == 0 and the reachability test
		 * below would keep every non-empty corrupt pack forever.
		 */
		if (!have_configured) {
			if (!opts->quiet)
				fprintf(stderr,
					_("stratify-prune: demoting "
					  "corrupt-sidecar pack %s "
					  "(no anchors configured)\n"),
					pack_basename(cp));
			if (remove_pack_base_stratum(cp)) {
				/* Demotion failed: it stays kept-corrupt. */
				kept++;
				continue;
			}
			orphan_demoted++;
			continue;
		}

		if (open_pack_index(cp)) {
			/* Cannot enumerate objects: keep and denounce. */
			kept++;
			continue;
		}
		for (n = 0; n < cp->num_objects; n++) {
			struct object_id oid;

			if (nth_packed_object_id(&oid, cp, n) < 0 ||
			    !oid_in_any_pack(holders, holders_nr, &oid)) {
				redundant = 0;
				break;
			}
		}
		if (!redundant) {
			kept++;
			continue;
		}
		if (!opts->quiet)
			fprintf(stderr,
				_("stratify-prune: reclaiming redundant "
				  "corrupt-sidecar pack %s\n"),
				pack_basename(cp));
		if (remove_pack_base_stratum(cp)) {
			/* Demotion failed: it stays kept-corrupt. */
			kept++;
			continue;
		}
		reclaimed++;
	}

	if (reclaimed) {
		*demoted_out += reclaimed;
		trace2_data_intmax("stratify-prune", r,
				   "corrupt-redundant-demoted",
				   (intmax_t)reclaimed);
	}
	if (orphan_demoted) {
		*demoted_out += orphan_demoted;
		trace2_data_intmax("stratify-prune", r,
				   "corrupt-orphan-demoted",
				   (intmax_t)orphan_demoted);
	}
	if (kept)
		trace2_data_intmax("stratify-prune", r, "corrupt-sidecar-kept",
				   (intmax_t)kept);
	if (guard_block && corrupt_nr) {
		if (!opts->quiet)
			fprintf(stderr,
				_("stratify-prune: keeping corrupt-sidecar "
				  "pack(s); a configured anchor has no readable "
				  "base-stratum frontier (run stratify to "
				  "rebuild it)\n"));
		trace2_data_intmax("stratify-prune", r,
				   "corrupt-frontier-guarded",
				   (intmax_t)corrupt_nr);
	}

	free(holders);
	free(corrupt);
	string_list_clear(&covered, 0);
	return kept;
}

/*
 * Demote base-stratum packs whose anchor_ref is not in the configured
 * anchor set. Only the .base-stratum and .keep sidecars are removed;
 * the .pack and .idx files remain on disk and become regular packs
 * that geometric-repack will absorb on its normal cadence. The change
 * is therefore reversible until the next geometric-repack run.
 *
 * When no anchors are configured the configured set is empty, so every
 * base-stratum pack is an orphan and gets demoted. This makes
 * stratify-prune the explicit, idempotent way to wind stratification
 * down: unset maintenance.stratified.anchor, run the task, and the
 * next geometric repack folds the freed packs back into the active
 * stratum.
 *
 * After the orphan sweep, reclaim_redundant_corrupt_packs() reclaims the
 * corrupt-sidecar packs that are provably redundant and denounces the
 * rest. The task exits non-zero while any corrupt sidecar remains kept.
 */
static int maintenance_task_stratify_prune(struct maintenance_run_opts *opts,
					   struct gc_config *cfg UNUSED)
{
	struct repository *r = the_repository;
	struct string_list ref_groups = STRING_LIST_INIT_DUP;
	struct string_list configured = STRING_LIST_INIT_DUP;
	int have_configured = !load_unique_stratify_anchors(r, &configured);
	size_t i, j, demoted = 0, failed = 0, kept_corrupt = 0;

	/*
	 * Packs with a corrupt/unreadable .base-stratum are kept (not
	 * demoted) by the collector and skipped from the grouping: their
	 * anchor_ref is unrecoverable, so demoting one could break the
	 * closed-set invariant for a still-loadable sibling that depends on
	 * it (see collect_base_stratum_pack_groups()). The orphan-group loop
	 * below cannot touch them; reclaim_redundant_corrupt_packs() then
	 * reclaims the subset that is provably redundant and denounces the
	 * rest (see below).
	 */
	collect_base_stratum_pack_groups(r, &ref_groups, NULL);

	/*
	 * Base-stratum coverage is anchor-scoped: an anchor's pack set
	 * together carries every object reachable from its anchor down to
	 * the min-age window. Demoting an orphan unlinks its .base-stratum
	 * and .keep sidecars; the .pack itself stays on disk and the next
	 * geometric repack folds it into the active stratum. Surviving
	 * packs cannot lose coverage because coverage is never shared
	 * across anchors.
	 */
	for (i = 0; i < ref_groups.nr; i++) {
		struct base_stratum_pack_group *group = ref_groups.items[i].util;
		const char *anchor = ref_groups.items[i].string;

		if (have_configured &&
		    unsorted_string_list_has_string(&configured, anchor))
			continue;

		trace2_region_enter("stratify-prune", anchor, r);
		for (j = 0; j < group->nr; j++) {
			struct base_stratum_pack_entry *entry = &group->entries[j];

			if (!opts->quiet)
				fprintf(stderr,
					_("stratify-prune: demoting %s "
					  "(anchor '%s' no longer configured)\n"),
					pack_basename(entry->pack), anchor);
			/*
			 * remove_pack_base_stratum() reports failure when it
			 * cannot unlink the .base-stratum or .keep sidecar. A
			 * pack that keeps its .base-stratum is still treated as
			 * base-stratum by newer git; one that keeps its .keep is
			 * pinned against older-git repacks. Either way the pack
			 * was not demoted, so do not count it and surface the
			 * failure rather than reporting a phantom cleanup.
			 */
			if (remove_pack_base_stratum(entry->pack)) {
				failed++;
				continue;
			}
			demoted++;
		}
		trace2_data_intmax("stratify-prune", r, "packs/processed-group",
				   group->nr);
		trace2_region_leave("stratify-prune", anchor, r);
	}
	/*
	 * The orphan loop above only touched loadable packs. Now reclaim
	 * corrupt-sidecar packs that are provably redundant against the
	 * still-kept set, and denounce (count) any that remain. This runs
	 * after the orphan loop so demoted orphans -- now in_base_stratum ==
	 * 0 -- are excluded from the holder set.
	 */
	kept_corrupt = reclaim_redundant_corrupt_packs(r, opts, &configured,
						       have_configured, &demoted);

	trace2_data_intmax("stratify-prune", r, "packs/demoted", demoted);
	if (failed)
		trace2_data_intmax("stratify-prune", r, "packs/demote-failed",
				   failed);

	free_base_stratum_pack_groups(&ref_groups);
	string_list_clear(&configured, 0);
	return (failed || kept_corrupt) ? 1 : 0;
}

static int consolidate_stratum_auto_condition(struct gc_config *cfg UNUSED)
{
	struct repository *r = the_repository;
	struct string_list counts = STRING_LIST_INIT_DUP;
	struct string_list configured = STRING_LIST_INIT_DUP;
	struct packed_git *p;
	int threshold = 0;
	int should_run = 0;

	repo_config_get_int(r, "maintenance.consolidate-stratum.auto",
			    &threshold);
	if (!threshold)
		return 0;
	if (threshold < 0)
		return 1;

	/* No configured anchors: nothing this task would act on. */
	if (load_unique_stratify_anchors(r, &configured))
		return 0;

	/*
	 * A should-run predicate must not mutate the repository, so count
	 * base-stratum packs per anchor by reading their sidecars directly
	 * and skipping any that fail to load. Calling
	 * collect_base_stratum_pack_groups() here would instead demote an
	 * unreadable pack (unlink its .base-stratum and .keep) as a side
	 * effect of merely evaluating whether the task should run.
	 *
	 * Only configured anchors count toward the threshold: the task body
	 * skips groups for unconfigured (orphan) anchors, so counting them
	 * here would schedule a run that then does nothing.
	 */
	repo_for_each_pack(r, p) {
		struct base_stratum_data adata = { 0 };
		struct string_list_item *item;
		intptr_t n;

		if (!p->in_base_stratum)
			continue;
		if (load_pack_base_stratum(p, &adata))
			continue;
		if (!unsorted_string_list_has_string(&configured, adata.anchor_ref)) {
			clear_base_stratum_data(&adata);
			continue;
		}

		item = string_list_lookup(&counts, adata.anchor_ref);
		if (!item)
			item = string_list_insert(&counts, adata.anchor_ref);
		n = (intptr_t)item->util + 1;
		item->util = (void *)n;
		clear_base_stratum_data(&adata);

		if (n >= threshold) {
			should_run = 1;
			break;
		}
	}
	string_list_clear(&counts, 0);
	string_list_clear(&configured, 0);
	return should_run;
}

/*
 * Surface GC: lightweight garbage collection that only processes
 * unstratified (active stratum) objects. Base-stratum packs are kept
 * intact via --keep-pack, so the reachability walk and repack only
 * cover the active stratum.
 */
/*
 * Compute the surface-gc readiness cutoff timestamp from
 * maintenance.stratified.min-age and .grace-period.
 *
 * min-age is the timestamp boundary stratify packs before (matching the
 * --before=<min-age> rev-list elsewhere); grace-period is an additional
 * relative duration behind now. The cutoff is min-age pushed back by the
 * grace duration:
 *   cutoff = min_age - (now - grace)
 * Both values must be relative durations (e.g. "2.weeks.ago"); parse them
 * carefully so a malformed value is rejected rather than silently treated as
 * "now" (which would let surface-gc run prematurely).
 *
 * On success returns 0, stores the cutoff in *cutoff_ts, and points
 * *min_age_str / *grace_str at the configured (or default) strings for use in
 * diagnostics. Returns -1 if either value is malformed.
 */
static int compute_stratify_cutoff(struct repository *r, timestamp_t *cutoff_ts,
				   const char **min_age_str, const char **grace_str)
{
	timestamp_t now_ts, min_age_ts, grace_ts, grace_offset;
	int min_err = 0, grace_err = 0;

	*min_age_str = "2.weeks.ago";
	*grace_str = "1.week.ago";
	repo_config_get_string_tmp(r, "maintenance.stratified.min-age",
				   min_age_str);
	repo_config_get_string_tmp(r, "maintenance.stratified.grace-period",
				   grace_str);

	now_ts = approxidate("now");
	min_age_ts = approxidate_careful(*min_age_str, &min_err);
	grace_ts = approxidate_careful(*grace_str, &grace_err);
	if (min_err || grace_err)
		return -1;

	/*
	 * Clamp so a value resolving to the future cannot underflow the
	 * unsigned subtraction into a far-past cutoff that would wrongly
	 * report "caught up".
	 */
	grace_offset = now_ts > grace_ts ? now_ts - grace_ts : 0;
	*cutoff_ts = min_age_ts > grace_offset ? min_age_ts - grace_offset : 0;
	return 0;
}

/*
 * Check whether stratify stratifying has caught up sufficiently for
 * surface-gc to be worthwhile. For each anchor ref, ask whether any
 * commit older than (now - min-age - grace) is still reachable from
 * the tip after excluding the anchor's stratified frontier antichain
 * (see stratify_anchor_caught_up()). If such a commit exists,
 * stratify is still lagging and surface-gc is skipped — the active
 * stratum is too large for the cruft repack to save work.
 */
static int stratify_stratifying_caught_up(struct repository *r, int quiet)
{
	struct string_list anchors = STRING_LIST_INIT_DUP;
	const char *min_age_str, *grace_str;
	timestamp_t cutoff_ts;
	int ret = 1;
	size_t i;

	if (load_unique_stratify_anchors(r, &anchors)) {
		ret = 0;
		goto out;
	}

	if (compute_stratify_cutoff(r, &cutoff_ts, &min_age_str, &grace_str)) {
		if (!quiet)
			fprintf(stderr,
				_("surface-gc: invalid maintenance.stratified.min-age "
				  "or grace-period; skipping\n"));
		ret = 0;
		goto out;
	}

	for (i = 0; i < anchors.nr; i++) {
		const char *anchor_ref = anchors.items[i].string;
		struct object_id tip_oid;
		enum stratify_caught_up status;

		if (!refs_resolve_ref_unsafe(get_main_ref_store(r),
					     anchor_ref,
					     RESOLVE_REF_READING,
					     &tip_oid, NULL)) {
			if (!quiet)
				fprintf(stderr,
					_("surface-gc: cannot resolve anchor '%s'\n"),
					anchor_ref);
			ret = 0;
			goto out;
		}

		/*
		 * Per-anchor readiness: ask the graph whether any
		 * commit older than the cutoff is still reachable
		 * from the tip without going through the stratified
		 * frontier antichain. Other anchors' packs do not
		 * contribute coverage — base-stratum coverage is
		 * anchor-scoped.
		 */
		status = stratify_anchor_caught_up(r, anchor_ref, &tip_oid,
						   cutoff_ts);

		switch (status) {
		case STRATIFY_CAUGHT_UP:
			break;
		case STRATIFY_NO_FRONTIER:
			if (!quiet)
				fprintf(stderr,
					_("surface-gc: anchor '%s' has no stratified commits yet\n"),
					anchor_ref);
			ret = 0;
			goto out;
		case STRATIFY_LAGGING:
			if (!quiet)
				fprintf(stderr,
					_("surface-gc: skipped, stratifying for '%s' is lagging "
					  "(old commits remain outside the frontier; "
					  "need stratify to advance past maintenance.stratified.min-age(%s) "
					  "+ maintenance.stratified.grace-period(%s))\n"),
					anchor_ref, min_age_str, grace_str);
			ret = 0;
			goto out;
		case STRATIFY_CAUGHT_UP_ERROR:
			if (!quiet)
				fprintf(stderr,
					_("surface-gc: readiness check failed for anchor '%s'; skipping\n"),
					anchor_ref);
			ret = 0;
			goto out;
		}
	}

out:
	string_list_clear(&anchors, 0);
	return ret;
}

/*
 * Read-only counterpart to stratify_stratifying_caught_up(): for each
 * configured anchor, print whether it is caught up or how far it is lagging,
 * plus the resulting surface-gc verdict, without touching the repository.
 * Reached via `git maintenance run --task=surface-gc --dry-run`. Always
 * returns 0 — it is a report, not a gate.
 */
static int report_stratify_status(struct repository *r)
{
	struct string_list anchors = STRING_LIST_INIT_DUP;
	const char *min_age_str, *grace_str;
	timestamp_t cutoff_ts;
	const char *blocker = NULL;
	size_t i;

	if (load_unique_stratify_anchors(r, &anchors)) {
		printf(_("stratify status: no anchors configured\n"));
		goto out;
	}

	if (compute_stratify_cutoff(r, &cutoff_ts, &min_age_str, &grace_str)) {
		printf(_("stratify status: invalid maintenance.stratified.min-age "
			 "or grace-period\n"));
		goto out;
	}

	printf(_("stratify status (cutoff: min-age %s, grace-period %s)\n"),
	       min_age_str, grace_str);

	for (i = 0; i < anchors.nr; i++) {
		const char *anchor_ref = anchors.items[i].string;
		struct object_id tip_oid;
		size_t width = 0;
		long lag;
		const char *state;

		if (!refs_resolve_ref_unsafe(get_main_ref_store(r), anchor_ref,
					     RESOLVE_REF_READING, &tip_oid,
					     NULL)) {
			state = "unresolvable";
			printf(_("  %s: unresolvable, skipped\n"), anchor_ref);
			if (!blocker)
				blocker = anchor_ref;
			goto record;
		}

		lag = stratify_anchor_lag(r, anchor_ref, &tip_oid, cutoff_ts,
					  &width);
		if (!width) {
			state = "no-frontier";
			printf(_("  %s: no stratified commits yet\n"), anchor_ref);
			if (!blocker)
				blocker = anchor_ref;
		} else if (lag < 0) {
			state = "error";
			printf(_("  %s: readiness check failed\n"), anchor_ref);
			if (!blocker)
				blocker = anchor_ref;
		} else if (lag == 0) {
			state = "caught-up";
			printf(_("  %s: caught up (frontier width %"PRIuMAX")\n"),
			       anchor_ref, (uintmax_t)width);
		} else {
			state = "lagging";
			printf(Q_("  %s: lagging by %ld commit (frontier width %"PRIuMAX")\n",
				  "  %s: lagging by %ld commits (frontier width %"PRIuMAX")\n",
				  lag),
			       anchor_ref, lag, (uintmax_t)width);
			if (!blocker)
				blocker = anchor_ref;
		}

record:
		trace2_data_string("surface-gc", r, "dry-run/anchor", anchor_ref);
		trace2_data_string("surface-gc", r, "dry-run/state", state);
	}

	if (anchors.nr) {
		if (blocker)
			printf(_("surface-gc would be skipped (%s is not caught up)\n"),
			       blocker);
		else
			printf(_("surface-gc would run\n"));
	}

out:
	string_list_clear(&anchors, 0);
	return 0;
}

/*
 * Mirror git-repack(1)'s default for whether bitmaps should be written.
 * `repack.writeBitmaps` (or its older `pack.writeBitmaps` spelling) wins
 * when set; otherwise the cruft repack run by surface-gc is an
 * all-into-one repack, so bitmaps default on for bare repositories.
 */
static int surface_gc_wants_bitmaps(struct repository *r)
{
	int value;

	if (!repo_config_get_bool(r, "repack.writebitmaps", &value))
		return value;
	if (!repo_config_get_bool(r, "pack.writebitmaps", &value))
		return value;
	return is_bare_repository();
}

static int maintenance_task_surface_gc(struct maintenance_run_opts *opts,
				      struct gc_config *cfg UNUSED)
{
	struct repository *r = the_repository;
	struct child_process child = CHILD_PROCESS_INIT;
	struct packed_git *p;
	const char *expiration = "2.weeks.ago";
	int have_base_stratum = 0;
	int kept_packs = 0;

	if (opts->dry_run)
		return report_stratify_status(r);

	if (repo_config_get_string_tmp(r, "maintenance.stratified.cruft-expiration",
				       &expiration))
		repo_config_get_string_tmp(r, "gc.pruneexpire",
					   &expiration);

	/*
	 * Check if stratify stratifying has caught up enough for
	 * surface-gc to be effective. If stratifying is lagging behind,
	 * the active stratum is still too large and surface-gc
	 * would be as expensive as a full repack.
	 */
	if (!stratify_stratifying_caught_up(r, opts->quiet)) {
		trace2_data_string("surface-gc", r, "skipped/reason",
				   "stratifying-not-caught-up");
		return 0;
	}

	trace2_data_string("surface-gc", r, "expiration", expiration);

	child.git_cmd = 1;
	strvec_pushl(&child.args, "repack", "-d", "-l", "--cruft", NULL);
	strvec_pushf(&child.args, "--cruft-expiration=%s", expiration);

	if (opts->quiet)
		strvec_push(&child.args, "--quiet");

	/*
	 * Keep all base-stratum packs intact and treat them as traversal
	 * boundaries. The closed-set property guarantees that every
	 * object transitively reachable from a stratified object is also
	 * in a base-stratum pack, so the reachability walk can stop when
	 * it hits an object in a kept pack.
	 */
	repo_for_each_pack(r, p) {
		if (!p->in_base_stratum)
			continue;
		have_base_stratum = 1;
		kept_packs++;
		strvec_pushf(&child.args, "--keep-pack=%s",
			     pack_basename(p));
	}

	trace2_data_intmax("surface-gc", r, "kept-packs", kept_packs);

	if (have_base_stratum)
		strvec_push(&child.args, "--kept-pack-boundary");

	/*
	 * With --kept-pack-boundary the surface pack deliberately omits
	 * every base-stratum object, so it cannot satisfy a single-pack
	 * bitmap's closure requirement on its own. Always write a
	 * multi-pack index, which spans the kept base-stratum packs and
	 * the new surface pack; when bitmaps are wanted they are written
	 * as a MIDX bitmap that closes across the stratum boundary.
	 */
	strvec_push(&child.args, "--write-midx");
	if (surface_gc_wants_bitmaps(r))
		strvec_push(&child.args, "--write-bitmap-index");

	/*
	 * If no base-stratum packs exist, this degrades to a normal
	 * cruft repack (which is fine but expensive). In practice,
	 * surface-gc is only useful after stratify has stratified objects.
	 */
	if (!have_base_stratum)
		warning(_("surface-gc: no base-stratum packs found; "
			  "this will be equivalent to a full cruft repack"));

	if (run_command(&child))
		return error(_("failed to perform surface garbage collection"));

	return 0;
}

static int surface_gc_auto_condition(struct gc_config *cfg UNUSED)
{
	struct packed_git *p;
	int have_base_stratum = 0;
	int have_active_packs = 0;

	/*
	 * Surface GC is useful when there are both base-stratum packs
	 * and regular (active-stratum) packs to process.
	 */
	repo_for_each_pack(the_repository, p) {
		if (p->in_base_stratum)
			have_base_stratum = 1;
		else if (!p->is_cruft && !p->pack_keep)
			have_active_packs = 1;
		if (have_base_stratum && have_active_packs)
			return 1;
	}

	return 0;
}

typedef int (*maintenance_task_fn)(struct maintenance_run_opts *opts,
				   struct gc_config *cfg);
typedef int (*maintenance_auto_fn)(struct gc_config *cfg);

struct maintenance_task {
	const char *name;

	/*
	 * Work that will be executed before detaching. This should not include
	 * tasks that may run for an extended amount of time as it does cause
	 * auto-maintenance to block until foreground tasks have been run.
	 */
	maintenance_task_fn foreground;

	/*
	 * Work that will be executed after detaching. When not detaching the
	 * work will be run in the foreground, as well.
	 */
	maintenance_task_fn background;

	/*
	 * An auto condition function returns 1 if the task should run and 0 if
	 * the task should NOT run. See needs_to_gc() for an example.
	 */
	maintenance_auto_fn auto_condition;
};

static const struct maintenance_task tasks[] = {
	[TASK_PREFETCH] = {
		.name = "prefetch",
		.background = maintenance_task_prefetch,
	},
	[TASK_LOOSE_OBJECTS] = {
		.name = "loose-objects",
		.background = maintenance_task_loose_objects,
		.auto_condition = loose_object_auto_condition,
	},
	[TASK_INCREMENTAL_REPACK] = {
		.name = "incremental-repack",
		.background = maintenance_task_incremental_repack,
		.auto_condition = incremental_repack_auto_condition,
	},
	[TASK_GEOMETRIC_REPACK] = {
		.name = "geometric-repack",
		.background = maintenance_task_geometric_repack,
		.auto_condition = geometric_repack_auto_condition,
	},
	[TASK_GC] = {
		.name = "gc",
		.foreground = maintenance_task_gc_foreground,
		.background = maintenance_task_gc_background,
		.auto_condition = gc_condition,
	},
	[TASK_COMMIT_GRAPH] = {
		.name = "commit-graph",
		.background = maintenance_task_commit_graph,
		.auto_condition = should_write_commit_graph,
	},
	[TASK_PACK_REFS] = {
		.name = "pack-refs",
		.foreground = maintenance_task_pack_refs,
		.auto_condition = pack_refs_condition,
	},
	[TASK_REFLOG_EXPIRE] = {
		.name = "reflog-expire",
		.foreground = maintenance_task_reflog_expire,
		.auto_condition = reflog_expire_condition,
	},
	[TASK_WORKTREE_PRUNE] = {
		.name = "worktree-prune",
		.background = maintenance_task_worktree_prune,
		.auto_condition = worktree_prune_condition,
	},
	[TASK_RERERE_GC] = {
		.name = "rerere-gc",
		.background = maintenance_task_rerere_gc,
		.auto_condition = rerere_gc_condition,
	},
	[TASK_STRATIFY] = {
		.name = "stratify",
		.background = maintenance_task_stratify,
		.auto_condition = stratify_auto_condition,
	},
	[TASK_STRATIFY_PRUNE] = {
		.name = "stratify-prune",
		.background = maintenance_task_stratify_prune,
		/*
		 * No auto_condition: pruning demotes packs and is only
		 * safe when the user has explicitly opted in by selecting
		 * the task with --task=stratify-prune.
		 */
	},
	[TASK_CONSOLIDATE_STRATUM] = {
		.name = "consolidate-stratum",
		.background = maintenance_task_consolidate_stratum,
		.auto_condition = consolidate_stratum_auto_condition,
	},
	[TASK_SURFACE_GC] = {
		.name = "surface-gc",
		.background = maintenance_task_surface_gc,
		.auto_condition = surface_gc_auto_condition,
	},
};

enum task_phase {
	TASK_PHASE_FOREGROUND,
	TASK_PHASE_BACKGROUND,
};

static int maybe_run_task(const struct maintenance_task *task,
			  struct repository *repo,
			  struct maintenance_run_opts *opts,
			  struct gc_config *cfg,
			  enum task_phase phase)
{
	int foreground = (phase == TASK_PHASE_FOREGROUND);
	maintenance_task_fn fn = foreground ? task->foreground : task->background;
	const char *region = foreground ? "maintenance foreground" : "maintenance";
	int ret = 0;

	if (!fn)
		return 0;
	if (opts->auto_flag &&
	    (!task->auto_condition || !task->auto_condition(cfg)))
		return 0;

	trace2_region_enter(region, task->name, repo);
	if (fn(opts, cfg)) {
		error(_("task '%s' failed"), task->name);
		ret = 1;
	}
	trace2_region_leave(region, task->name, repo);

	return ret;
}

static int maintenance_run_tasks(struct maintenance_run_opts *opts,
				 struct gc_config *cfg)
{
	int result = 0;
	struct lock_file lk;
	struct repository *r = the_repository;
	char *lock_path = xstrfmt("%s/maintenance", r->objects->sources->path);

	if (hold_lock_file_for_update(&lk, lock_path, LOCK_NO_DEREF) < 0) {
		/*
		 * Another maintenance command is running.
		 *
		 * If --auto was provided, then it is likely due to a
		 * recursive process stack. Do not report an error in
		 * that case.
		 */
		if (!opts->auto_flag && !opts->quiet)
			warning(_("lock file '%s' exists, skipping maintenance"),
				lock_path);
		free(lock_path);
		return 0;
	}
	free(lock_path);

	for (size_t i = 0; i < opts->tasks_nr; i++)
		if (maybe_run_task(&tasks[opts->tasks[i]], r, opts, cfg,
				   TASK_PHASE_FOREGROUND))
			result = 1;

	/* Failure to daemonize is ok, we'll continue in foreground. */
	if (opts->detach > 0) {
		trace2_region_enter("maintenance", "detach", the_repository);
		daemonize();
		trace2_region_leave("maintenance", "detach", the_repository);
	}

	for (size_t i = 0; i < opts->tasks_nr; i++)
		if (maybe_run_task(&tasks[opts->tasks[i]], r, opts, cfg,
				   TASK_PHASE_BACKGROUND))
			result = 1;

	rollback_lock_file(&lk);
	return result;
}

enum maintenance_type {
	/* As invoked via `git maintenance run --schedule=`. */
	MAINTENANCE_TYPE_SCHEDULED = (1 << 0),
	/* As invoked via `git maintenance run` and with `--auto`. */
	MAINTENANCE_TYPE_MANUAL    = (1 << 1),
};

struct maintenance_strategy {
	struct {
		unsigned type;
		enum schedule_priority schedule;
	} tasks[TASK__COUNT];
};

static const struct maintenance_strategy none_strategy = { 0 };

static const struct maintenance_strategy gc_strategy = {
	.tasks = {
		[TASK_GC] = {
			.type = MAINTENANCE_TYPE_MANUAL | MAINTENANCE_TYPE_SCHEDULED,
			.schedule = SCHEDULE_DAILY,
		},
	},
};

static const struct maintenance_strategy incremental_strategy = {
	.tasks = {
		[TASK_COMMIT_GRAPH] = {
			.type = MAINTENANCE_TYPE_SCHEDULED,
			.schedule = SCHEDULE_HOURLY,
		},
		[TASK_PREFETCH] = {
			.type = MAINTENANCE_TYPE_SCHEDULED,
			.schedule = SCHEDULE_HOURLY,
		},
		[TASK_INCREMENTAL_REPACK] = {
			.type = MAINTENANCE_TYPE_SCHEDULED,
			.schedule = SCHEDULE_DAILY,
		},
		[TASK_LOOSE_OBJECTS] = {
			.type = MAINTENANCE_TYPE_SCHEDULED,
			.schedule = SCHEDULE_DAILY,
		},
		[TASK_PACK_REFS] = {
			.type = MAINTENANCE_TYPE_SCHEDULED,
			.schedule = SCHEDULE_WEEKLY,
		},
		/*
		 * Historically, the "incremental" strategy was only available
		 * in the context of scheduled maintenance when set up via
		 * "maintenance.strategy". We have later expanded that config
		 * to also cover manual maintenance.
		 *
		 * To retain backwards compatibility with the previous status
		 * quo we thus run git-gc(1) in case manual maintenance was
		 * requested. This is the same as the default strategy, which
		 * would have been in use beforehand.
		 */
		[TASK_GC] = {
			.type = MAINTENANCE_TYPE_MANUAL,
		},
	},
};

static const struct maintenance_strategy geometric_strategy = {
	.tasks = {
		[TASK_COMMIT_GRAPH] = {
			.type = MAINTENANCE_TYPE_SCHEDULED | MAINTENANCE_TYPE_MANUAL,
			.schedule = SCHEDULE_HOURLY,
		},
		[TASK_GEOMETRIC_REPACK] = {
			.type = MAINTENANCE_TYPE_SCHEDULED | MAINTENANCE_TYPE_MANUAL,
			.schedule = SCHEDULE_DAILY,
		},
		[TASK_PACK_REFS] = {
			.type = MAINTENANCE_TYPE_SCHEDULED | MAINTENANCE_TYPE_MANUAL,
			.schedule = SCHEDULE_DAILY,
		},
		[TASK_RERERE_GC] = {
			.type = MAINTENANCE_TYPE_SCHEDULED | MAINTENANCE_TYPE_MANUAL,
			.schedule = SCHEDULE_WEEKLY,
		},
		[TASK_REFLOG_EXPIRE] = {
			.type = MAINTENANCE_TYPE_SCHEDULED | MAINTENANCE_TYPE_MANUAL,
			.schedule = SCHEDULE_WEEKLY,
		},
		[TASK_WORKTREE_PRUNE] = {
			.type = MAINTENANCE_TYPE_SCHEDULED | MAINTENANCE_TYPE_MANUAL,
			.schedule = SCHEDULE_WEEKLY,
		},
		[TASK_STRATIFY] = {
			.type = MAINTENANCE_TYPE_SCHEDULED | MAINTENANCE_TYPE_MANUAL,
			.schedule = SCHEDULE_DAILY,
		},
		/*
		 * stratify-prune is deliberately absent from every strategy: it
		 * demotes base-stratum packs, and an empty or misconfigured
		 * maintenance.stratified.anchor turns every pack into an orphan.
		 * Were it scheduled or part of the manual strategy, a transient
		 * config slip would silently retire the whole base stratum and
		 * force an expensive re-stratification. It runs only when the
		 * user selects it explicitly with --task=stratify-prune (or opts
		 * a repo in via maintenance.stratify-prune.enabled).
		 */
		[TASK_CONSOLIDATE_STRATUM] = {
			.type = MAINTENANCE_TYPE_SCHEDULED | MAINTENANCE_TYPE_MANUAL,
			.schedule = SCHEDULE_DAILY,
		},
		[TASK_SURFACE_GC] = {
			.type = MAINTENANCE_TYPE_SCHEDULED | MAINTENANCE_TYPE_MANUAL,
			.schedule = SCHEDULE_WEEKLY,
		},
	},
};

static struct maintenance_strategy parse_maintenance_strategy(const char *name)
{
	if (!strcasecmp(name, "incremental"))
		return incremental_strategy;
	if (!strcasecmp(name, "gc"))
		return gc_strategy;
	if (!strcasecmp(name, "geometric"))
		return geometric_strategy;
	die(_("unknown maintenance strategy: '%s'"), name);
}

static void initialize_task_config(struct maintenance_run_opts *opts,
				   const struct string_list *selected_tasks)
{
	struct strbuf config_name = STRBUF_INIT;
	struct maintenance_strategy strategy;
	enum maintenance_type type;
	const char *config_str;

	/*
	 * In case the user has asked us to run tasks explicitly we only use
	 * those specified tasks. Specifically, we do _not_ want to consult the
	 * config or maintenance strategy.
	 */
	if (selected_tasks->nr) {
		for (size_t i = 0; i < selected_tasks->nr; i++) {
			enum maintenance_task_label label = (intptr_t)selected_tasks->items[i].util;;
			ALLOC_GROW(opts->tasks, opts->tasks_nr + 1, opts->tasks_alloc);
			opts->tasks[opts->tasks_nr++] = label;
		}

		return;
	}

	/*
	 * Otherwise, the strategy depends on whether we run as part of a
	 * scheduled job or not:
	 *
	 *   - Scheduled maintenance does not perform any housekeeping by
	 *     default, but requires the user to pick a maintenance strategy.
	 *
	 *   - Unscheduled maintenance uses our default strategy.
	 *
	 * Both of these are affected by the gitconfig though, which may
	 * override specific aspects of our strategy. Furthermore, both
	 * strategies can be overridden by setting "maintenance.strategy".
	 */
	if (opts->schedule) {
		strategy = none_strategy;
		type = MAINTENANCE_TYPE_SCHEDULED;
	} else {
		strategy = geometric_strategy;
		type = MAINTENANCE_TYPE_MANUAL;
	}

	if (!repo_config_get_string_tmp(the_repository, "maintenance.strategy", &config_str))
		strategy = parse_maintenance_strategy(config_str);

	for (size_t i = 0; i < TASK__COUNT; i++) {
		int config_value;

		strbuf_reset(&config_name);
		strbuf_addf(&config_name, "maintenance.%s.enabled",
			    tasks[i].name);
		if (!repo_config_get_bool(the_repository, config_name.buf, &config_value))
			strategy.tasks[i].type = config_value ? type : 0;
		if (!(strategy.tasks[i].type & type))
			continue;

		if (opts->schedule) {
			strbuf_reset(&config_name);
			strbuf_addf(&config_name, "maintenance.%s.schedule",
				    tasks[i].name);
			if (!repo_config_get_string_tmp(the_repository, config_name.buf, &config_str))
				strategy.tasks[i].schedule = parse_schedule(config_str);
			if (strategy.tasks[i].schedule < opts->schedule)
				continue;
		}

		ALLOC_GROW(opts->tasks, opts->tasks_nr + 1, opts->tasks_alloc);
		opts->tasks[opts->tasks_nr++] = i;
	}

	strbuf_release(&config_name);
}

static int task_option_parse(const struct option *opt,
			     const char *arg, int unset)
{
	struct string_list *selected_tasks = opt->value;
	size_t i;

	BUG_ON_OPT_NEG(unset);

	for (i = 0; i < TASK__COUNT; i++)
		if (!strcasecmp(tasks[i].name, arg))
			break;
	if (i >= TASK__COUNT) {
		error(_("'%s' is not a valid task"), arg);
		return 1;
	}

	if (unsorted_string_list_has_string(selected_tasks, arg)) {
		error(_("task '%s' cannot be selected multiple times"), arg);
		return 1;
	}

	string_list_append(selected_tasks, arg)->util = (void *)(intptr_t)i;

	return 0;
}

static int maintenance_run(int argc, const char **argv, const char *prefix,
			   struct repository *repo UNUSED)
{
	struct maintenance_run_opts opts = MAINTENANCE_RUN_OPTS_INIT;
	struct string_list selected_tasks = STRING_LIST_INIT_DUP;
	struct gc_config cfg = GC_CONFIG_INIT;
	struct option builtin_maintenance_run_options[] = {
		OPT_BOOL(0, "auto", &opts.auto_flag,
			 N_("run tasks based on the state of the repository")),
		OPT_BOOL(0, "detach", &opts.detach,
			 N_("perform maintenance in the background")),
		OPT_CALLBACK(0, "schedule", &opts.schedule, N_("frequency"),
			     N_("run tasks based on frequency"),
			     maintenance_opt_schedule),
		OPT_BOOL(0, "quiet", &opts.quiet,
			 N_("do not report progress or other information over stderr")),
		OPT_CALLBACK_F(0, "task", &selected_tasks, N_("task"),
			N_("run a specific task"),
			PARSE_OPT_NONEG, task_option_parse),
		OPT_BOOL(0, "dry-run", &opts.dry_run,
			 N_("report stratification status without modifying the repository")),
		OPT_END()
	};
	int ret;

	opts.quiet = !isatty(2);

	argc = parse_options(argc, argv, prefix,
			     builtin_maintenance_run_options,
			     builtin_maintenance_run_usage,
			     PARSE_OPT_STOP_AT_NON_OPTION);

	die_for_incompatible_opt2(opts.auto_flag, "--auto",
				  opts.schedule, "--schedule=");
	die_for_incompatible_opt2(selected_tasks.nr, "--task=",
				  opts.schedule, "--schedule=");

	/*
	 * --dry-run must never mutate the repository, so it is restricted to
	 * exactly "--task=surface-gc", whose dry-run path only reports
	 * stratification status. Reject every other combination rather than
	 * silently letting a mutating task run.
	 */
	if (opts.dry_run) {
		die_for_incompatible_opt2(opts.dry_run, "--dry-run",
					  opts.auto_flag, "--auto");
		die_for_incompatible_opt2(opts.dry_run, "--dry-run",
					  opts.schedule, "--schedule=");
		if (selected_tasks.nr != 1 ||
		    strcmp(selected_tasks.items[0].string, "surface-gc"))
			die(_("--dry-run is only supported with --task=surface-gc"));
	}

	gc_config(&cfg);
	initialize_task_config(&opts, &selected_tasks);

	if (argc != 0)
		usage_with_options(builtin_maintenance_run_usage,
				   builtin_maintenance_run_options);

	ret = maintenance_run_tasks(&opts, &cfg);

	string_list_clear(&selected_tasks, 0);
	maintenance_run_opts_release(&opts);
	gc_config_release(&cfg);
	return ret;
}

static char *get_maintpath(void)
{
	struct strbuf sb = STRBUF_INIT;
	const char *p = the_repository->worktree ?
		the_repository->worktree : the_repository->gitdir;

	strbuf_realpath(&sb, p, 1);
	return strbuf_detach(&sb, NULL);
}

static char const * const builtin_maintenance_register_usage[] = {
	"git maintenance register [--config-file <path>]",
	NULL
};

static int maintenance_register(int argc, const char **argv, const char *prefix,
				struct repository *repo UNUSED)
{
	char *config_file = NULL;
	struct option options[] = {
		OPT_STRING(0, "config-file", &config_file, N_("file"), N_("use given config file")),
		OPT_END(),
	};
	int found = 0;
	const char *key = "maintenance.repo";
	char *maintpath = get_maintpath();
	struct string_list_item *item;
	const struct string_list *list;

	argc = parse_options(argc, argv, prefix, options,
			     builtin_maintenance_register_usage, 0);
	if (argc)
		usage_with_options(builtin_maintenance_register_usage,
				   options);

	/* Disable foreground maintenance */
	repo_config_set(the_repository, "maintenance.auto", "false");

	/* Set maintenance strategy, if unset */
	if (repo_config_get(the_repository, "maintenance.strategy"))
		repo_config_set(the_repository, "maintenance.strategy", "incremental");

	if (!repo_config_get_string_multi(the_repository, key, &list)) {
		for_each_string_list_item(item, list) {
			if (!strcmp(maintpath, item->string)) {
				found = 1;
				break;
			}
		}
	}

	if (!found) {
		int rc;
		char *global_config_file = NULL;

		if (!config_file) {
			global_config_file = git_global_config();
			config_file = global_config_file;
		}
		if (!config_file)
			die(_("$HOME not set"));
		rc = repo_config_set_multivar_in_file_gently(the_repository,
			config_file, "maintenance.repo", maintpath,
			CONFIG_REGEX_NONE, NULL, 0);
		free(global_config_file);

		if (rc)
			die(_("unable to add '%s' value of '%s'"),
			    key, maintpath);
	}

	free(maintpath);
	return 0;
}

static char const * const builtin_maintenance_unregister_usage[] = {
	"git maintenance unregister [--config-file <path>] [--force]",
	NULL
};

static int maintenance_unregister(int argc, const char **argv, const char *prefix,
				  struct repository *repo UNUSED)
{
	int force = 0;
	char *config_file = NULL;
	struct option options[] = {
		OPT_STRING(0, "config-file", &config_file, N_("file"), N_("use given config file")),
		OPT__FORCE(&force,
			   N_("return success even if repository was not registered"),
			   PARSE_OPT_NOCOMPLETE),
		OPT_END(),
	};
	const char *key = "maintenance.repo";
	char *maintpath = get_maintpath();
	int found = 0;
	struct string_list_item *item;
	const struct string_list *list;
	struct config_set cs = { { 0 } };

	argc = parse_options(argc, argv, prefix, options,
			     builtin_maintenance_unregister_usage, 0);
	if (argc)
		usage_with_options(builtin_maintenance_unregister_usage,
				   options);

	if (config_file) {
		git_configset_init(&cs);
		git_configset_add_file(&cs, config_file);
	}
	if (!(config_file
	      ? git_configset_get_string_multi(&cs, key, &list)
	      : repo_config_get_string_multi(the_repository, key, &list))) {
		for_each_string_list_item(item, list) {
			if (!strcmp(maintpath, item->string)) {
				found = 1;
				break;
			}
		}
	}

	if (found) {
		int rc;
		char *global_config_file = NULL;

		if (!config_file) {
			global_config_file = git_global_config();
			config_file = global_config_file;
		}
		if (!config_file)
			die(_("$HOME not set"));
		rc = repo_config_set_multivar_in_file_gently(the_repository,
			config_file, key, NULL, maintpath, NULL,
			CONFIG_FLAGS_MULTI_REPLACE | CONFIG_FLAGS_FIXED_VALUE);
		free(global_config_file);

		if (rc &&
		    (!force || rc == CONFIG_NOTHING_SET))
			die(_("unable to unset '%s' value of '%s'"),
			    key, maintpath);
	} else if (!force) {
		die(_("repository '%s' is not registered"), maintpath);
	}

	git_configset_clear(&cs);
	free(maintpath);
	return 0;
}

static const char *get_frequency(enum schedule_priority schedule)
{
	switch (schedule) {
	case SCHEDULE_HOURLY:
		return "hourly";
	case SCHEDULE_DAILY:
		return "daily";
	case SCHEDULE_WEEKLY:
		return "weekly";
	default:
		BUG("invalid schedule %d", schedule);
	}
}

static const char *extraconfig[] = {
	"credential.interactive=false",
	"core.askPass=true", /* 'true' returns success, but no output. */
	NULL
};

static const char *get_extra_config_parameters(void) {
	static const char *result = NULL;
	struct strbuf builder = STRBUF_INIT;

	if (result)
		return result;

	for (const char **s = extraconfig; s && *s; s++)
		strbuf_addf(&builder, "-c %s ", *s);

	result = strbuf_detach(&builder, NULL);
	return result;
}

static const char *get_extra_launchctl_strings(void) {
	static const char *result = NULL;
	struct strbuf builder = STRBUF_INIT;

	if (result)
		return result;

	for (const char **s = extraconfig; s && *s; s++) {
		strbuf_addstr(&builder, "<string>-c</string>\n");
		strbuf_addf(&builder, "<string>%s</string>\n", *s);
	}

	result = strbuf_detach(&builder, NULL);
	return result;
}

/*
 * get_schedule_cmd` reads the GIT_TEST_MAINT_SCHEDULER environment variable
 * to mock the schedulers that `git maintenance start` rely on.
 *
 * For test purpose, GIT_TEST_MAINT_SCHEDULER can be set to a comma-separated
 * list of colon-separated key/value pairs where each pair contains a scheduler
 * and its corresponding mock.
 *
 * * If $GIT_TEST_MAINT_SCHEDULER is not set, return false and leave the
 *   arguments unmodified.
 *
 * * If $GIT_TEST_MAINT_SCHEDULER is set, return true.
 *   In this case, the *cmd value is read as input.
 *
 *   * if the input value cmd is the key of one of the comma-separated list
 *     item, then *is_available is set to true and *out is set to
 *     the mock command.
 *
 *   * if the input value *cmd isn’t the key of any of the comma-separated list
 *     item, then *is_available is set to false and *out is set to the original
 *     command.
 *
 * Ex.:
 *   GIT_TEST_MAINT_SCHEDULER not set
 *     +-------+-------------------------------------------------+
 *     | Input |                     Output                      |
 *     | *cmd  | return code |       *out        | *is_available |
 *     +-------+-------------+-------------------+---------------+
 *     | "foo" |    false    | "foo" (allocated) |  (unchanged)  |
 *     +-------+-------------+-------------------+---------------+
 *
 *   GIT_TEST_MAINT_SCHEDULER set to “foo:./mock_foo.sh,bar:./mock_bar.sh”
 *     +-------+-------------------------------------------------+
 *     | Input |                     Output                      |
 *     | *cmd  | return code |       *out        | *is_available |
 *     +-------+-------------+-------------------+---------------+
 *     | "foo" |    true     |  "./mock.foo.sh"  |     true      |
 *     | "qux" |    true     | "qux" (allocated) |     false     |
 *     +-------+-------------+-------------------+---------------+
 */
static int get_schedule_cmd(const char *cmd, int *is_available, char **out)
{
	char *testing = xstrdup_or_null(getenv("GIT_TEST_MAINT_SCHEDULER"));
	struct string_list_item *item;
	struct string_list list = STRING_LIST_INIT_NODUP;

	if (!testing) {
		if (out)
			*out = xstrdup(cmd);
		return 0;
	}

	if (is_available)
		*is_available = 0;

	string_list_split_in_place(&list, testing, ",", -1);
	for_each_string_list_item(item, &list) {
		struct string_list pair = STRING_LIST_INIT_NODUP;

		if (string_list_split_in_place(&pair, item->string, ":", 2) != 2)
			continue;

		if (!strcmp(cmd, pair.items[0].string)) {
			if (out)
				*out = xstrdup(pair.items[1].string);
			if (is_available)
				*is_available = 1;
			string_list_clear(&pair, 0);
			goto out;
		}

		string_list_clear(&pair, 0);
	}

	if (out)
		*out = xstrdup(cmd);

out:
	string_list_clear(&list, 0);
	free(testing);
	return 1;
}

static int get_random_minute(void)
{
	/* Use a static value when under tests. */
	if (getenv("GIT_TEST_MAINT_SCHEDULER"))
		return 13;

	return git_rand(0) % 60;
}

static int is_launchctl_available(void)
{
	int is_available;
	if (get_schedule_cmd("launchctl", &is_available, NULL))
		return is_available;

#ifdef __APPLE__
	return 1;
#else
	return 0;
#endif
}

static char *launchctl_service_name(const char *frequency)
{
	struct strbuf label = STRBUF_INIT;
	strbuf_addf(&label, "org.git-scm.git.%s", frequency);
	return strbuf_detach(&label, NULL);
}

static char *launchctl_service_filename(const char *name)
{
	char *expanded;
	struct strbuf filename = STRBUF_INIT;
	strbuf_addf(&filename, "~/Library/LaunchAgents/%s.plist", name);

	expanded = interpolate_path(filename.buf, 1);
	if (!expanded)
		die(_("failed to expand path '%s'"), filename.buf);

	strbuf_release(&filename);
	return expanded;
}

static char *launchctl_get_uid(void)
{
	return xstrfmt("gui/%d", getuid());
}

static int launchctl_boot_plist(int enable, const char *filename)
{
	char *cmd;
	int result;
	struct child_process child = CHILD_PROCESS_INIT;
	char *uid = launchctl_get_uid();

	get_schedule_cmd("launchctl", NULL, &cmd);
	strvec_split(&child.args, cmd);
	strvec_pushl(&child.args, enable ? "bootstrap" : "bootout", uid,
		     filename, NULL);

	child.no_stderr = 1;
	child.no_stdout = 1;

	if (start_command(&child))
		die(_("failed to start launchctl"));

	result = finish_command(&child);

	free(cmd);
	free(uid);
	return result;
}

static int launchctl_remove_plist(enum schedule_priority schedule)
{
	const char *frequency = get_frequency(schedule);
	char *name = launchctl_service_name(frequency);
	char *filename = launchctl_service_filename(name);
	int result = launchctl_boot_plist(0, filename);
	unlink(filename);
	free(filename);
	free(name);
	return result;
}

static int launchctl_remove_plists(void)
{
	return launchctl_remove_plist(SCHEDULE_HOURLY) ||
	       launchctl_remove_plist(SCHEDULE_DAILY) ||
	       launchctl_remove_plist(SCHEDULE_WEEKLY);
}

static int launchctl_list_contains_plist(const char *name, const char *cmd)
{
	struct child_process child = CHILD_PROCESS_INIT;

	strvec_split(&child.args, cmd);
	strvec_pushl(&child.args, "list", name, NULL);

	child.no_stderr = 1;
	child.no_stdout = 1;

	if (start_command(&child))
		die(_("failed to start launchctl"));

	/* Returns failure if 'name' doesn't exist. */
	return !finish_command(&child);
}

static int launchctl_schedule_plist(const char *exec_path, enum schedule_priority schedule)
{
	int i, fd;
	const char *preamble, *repeat;
	const char *frequency = get_frequency(schedule);
	char *name = launchctl_service_name(frequency);
	char *filename = launchctl_service_filename(name);
	struct lock_file lk = LOCK_INIT;
	static unsigned long lock_file_timeout_ms = ULONG_MAX;
	struct strbuf plist = STRBUF_INIT, plist2 = STRBUF_INIT;
	struct stat st;
	char *cmd;
	int minute = get_random_minute();

	get_schedule_cmd("launchctl", NULL, &cmd);
	preamble = "<?xml version=\"1.0\"?>\n"
		   "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
		   "<plist version=\"1.0\">"
		   "<dict>\n"
		   "<key>Label</key><string>%s</string>\n"
		   "<key>ProgramArguments</key>\n"
		   "<array>\n"
		   "<string>%s/git</string>\n"
		   "<string>--exec-path=%s</string>\n"
		   "%s" /* For extra config parameters. */
		   "<string>for-each-repo</string>\n"
		   "<string>--keep-going</string>\n"
		   "<string>--config=maintenance.repo</string>\n"
		   "<string>maintenance</string>\n"
		   "<string>run</string>\n"
		   "<string>--schedule=%s</string>\n"
		   "</array>\n"
		   "<key>StartCalendarInterval</key>\n"
		   "<array>\n";
	strbuf_addf(&plist, preamble, name, exec_path, exec_path,
		    get_extra_launchctl_strings(), frequency);

	switch (schedule) {
	case SCHEDULE_HOURLY:
		repeat = "<dict>\n"
			 "<key>Hour</key><integer>%d</integer>\n"
			 "<key>Minute</key><integer>%d</integer>\n"
			 "</dict>\n";
		for (i = 1; i <= 23; i++)
			strbuf_addf(&plist, repeat, i, minute);
		break;

	case SCHEDULE_DAILY:
		repeat = "<dict>\n"
			 "<key>Weekday</key><integer>%d</integer>\n"
			 "<key>Hour</key><integer>0</integer>\n"
			 "<key>Minute</key><integer>%d</integer>\n"
			 "</dict>\n";
		for (i = 1; i <= 6; i++)
			strbuf_addf(&plist, repeat, i, minute);
		break;

	case SCHEDULE_WEEKLY:
		strbuf_addf(&plist,
			    "<dict>\n"
			    "<key>Weekday</key><integer>0</integer>\n"
			    "<key>Hour</key><integer>0</integer>\n"
			    "<key>Minute</key><integer>%d</integer>\n"
			    "</dict>\n",
			    minute);
		break;

	default:
		/* unreachable */
		break;
	}
	strbuf_addstr(&plist, "</array>\n</dict>\n</plist>\n");

	if (safe_create_leading_directories(the_repository, filename))
		die(_("failed to create directories for '%s'"), filename);

	if ((long)lock_file_timeout_ms < 0 &&
	    repo_config_get_ulong(the_repository, "gc.launchctlplistlocktimeoutms",
				 &lock_file_timeout_ms))
		lock_file_timeout_ms = 150;

	fd = hold_lock_file_for_update_timeout(&lk, filename, LOCK_DIE_ON_ERROR,
					       lock_file_timeout_ms);

	/*
	 * Does this file already exist? With the intended contents? Is it
	 * registered already? Then it does not need to be re-registered.
	 */
	if (!stat(filename, &st) && st.st_size == plist.len &&
	    strbuf_read_file(&plist2, filename, plist.len) == plist.len &&
	    !strbuf_cmp(&plist, &plist2) &&
	    launchctl_list_contains_plist(name, cmd))
		rollback_lock_file(&lk);
	else {
		if (write_in_full(fd, plist.buf, plist.len) < 0 ||
		    commit_lock_file(&lk))
			die_errno(_("could not write '%s'"), filename);

		/* bootout might fail if not already running, so ignore */
		launchctl_boot_plist(0, filename);
		if (launchctl_boot_plist(1, filename))
			die(_("failed to bootstrap service %s"), filename);
	}

	free(filename);
	free(name);
	free(cmd);
	strbuf_release(&plist);
	strbuf_release(&plist2);
	return 0;
}

static int launchctl_add_plists(void)
{
	const char *exec_path = git_exec_path();

	return launchctl_schedule_plist(exec_path, SCHEDULE_HOURLY) ||
	       launchctl_schedule_plist(exec_path, SCHEDULE_DAILY) ||
	       launchctl_schedule_plist(exec_path, SCHEDULE_WEEKLY);
}

static int launchctl_update_schedule(int run_maintenance, int fd UNUSED)
{
	if (run_maintenance)
		return launchctl_add_plists();
	else
		return launchctl_remove_plists();
}

static int is_schtasks_available(void)
{
	int is_available;
	if (get_schedule_cmd("schtasks", &is_available, NULL))
		return is_available;

#ifdef GIT_WINDOWS_NATIVE
	return 1;
#else
	return 0;
#endif
}

static char *schtasks_task_name(const char *frequency)
{
	struct strbuf label = STRBUF_INIT;
	strbuf_addf(&label, "Git Maintenance (%s)", frequency);
	return strbuf_detach(&label, NULL);
}

static int schtasks_remove_task(enum schedule_priority schedule)
{
	char *cmd;
	struct child_process child = CHILD_PROCESS_INIT;
	const char *frequency = get_frequency(schedule);
	char *name = schtasks_task_name(frequency);

	get_schedule_cmd("schtasks", NULL, &cmd);
	strvec_split(&child.args, cmd);
	strvec_pushl(&child.args, "/delete", "/tn", name, "/f", NULL);
	free(name);
	free(cmd);

	return run_command(&child);
}

static int schtasks_remove_tasks(void)
{
	return schtasks_remove_task(SCHEDULE_HOURLY) ||
	       schtasks_remove_task(SCHEDULE_DAILY) ||
	       schtasks_remove_task(SCHEDULE_WEEKLY);
}

static int schtasks_schedule_task(const char *exec_path, enum schedule_priority schedule)
{
	char *cmd;
	int result;
	struct child_process child = CHILD_PROCESS_INIT;
	const char *xml;
	struct tempfile *tfile;
	const char *frequency = get_frequency(schedule);
	char *name = schtasks_task_name(frequency);
	struct strbuf tfilename = STRBUF_INIT;
	int minute = get_random_minute();

	get_schedule_cmd("schtasks", NULL, &cmd);

	strbuf_addf(&tfilename, "%s/schedule_%s_XXXXXX",
		    repo_get_common_dir(the_repository), frequency);
	tfile = xmks_tempfile(tfilename.buf);
	strbuf_release(&tfilename);

	if (!fdopen_tempfile(tfile, "w"))
		die(_("failed to create temp xml file"));

	xml = "<?xml version=\"1.0\" ?>\n"
	      "<Task version=\"1.4\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
	      "<Triggers>\n"
	      "<CalendarTrigger>\n";
	fputs(xml, tfile->fp);

	switch (schedule) {
	case SCHEDULE_HOURLY:
		fprintf(tfile->fp,
			"<StartBoundary>2020-01-01T01:%02d:00</StartBoundary>\n"
			"<Enabled>true</Enabled>\n"
			"<ScheduleByDay>\n"
			"<DaysInterval>1</DaysInterval>\n"
			"</ScheduleByDay>\n"
			"<Repetition>\n"
			"<Interval>PT1H</Interval>\n"
			"<Duration>PT23H</Duration>\n"
			"<StopAtDurationEnd>false</StopAtDurationEnd>\n"
			"</Repetition>\n",
			minute);
		break;

	case SCHEDULE_DAILY:
		fprintf(tfile->fp,
			"<StartBoundary>2020-01-01T00:%02d:00</StartBoundary>\n"
			"<Enabled>true</Enabled>\n"
			"<ScheduleByWeek>\n"
			"<DaysOfWeek>\n"
			"<Monday />\n"
			"<Tuesday />\n"
			"<Wednesday />\n"
			"<Thursday />\n"
			"<Friday />\n"
			"<Saturday />\n"
			"</DaysOfWeek>\n"
			"<WeeksInterval>1</WeeksInterval>\n"
			"</ScheduleByWeek>\n",
			minute);
		break;

	case SCHEDULE_WEEKLY:
		fprintf(tfile->fp,
			"<StartBoundary>2020-01-01T00:%02d:00</StartBoundary>\n"
			"<Enabled>true</Enabled>\n"
			"<ScheduleByWeek>\n"
			"<DaysOfWeek>\n"
			"<Sunday />\n"
			"</DaysOfWeek>\n"
			"<WeeksInterval>1</WeeksInterval>\n"
			"</ScheduleByWeek>\n",
			minute);
		break;

	default:
		break;
	}

	xml = "</CalendarTrigger>\n"
	      "</Triggers>\n"
	      "<Principals>\n"
	      "<Principal id=\"Author\">\n"
	      "<LogonType>InteractiveToken</LogonType>\n"
	      "<RunLevel>LeastPrivilege</RunLevel>\n"
	      "</Principal>\n"
	      "</Principals>\n"
	      "<Settings>\n"
	      "<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
	      "<Enabled>true</Enabled>\n"
	      "<Hidden>true</Hidden>\n"
	      "<UseUnifiedSchedulingEngine>true</UseUnifiedSchedulingEngine>\n"
	      "<WakeToRun>false</WakeToRun>\n"
	      "<ExecutionTimeLimit>PT72H</ExecutionTimeLimit>\n"
	      "<Priority>7</Priority>\n"
	      "</Settings>\n"
	      "<Actions Context=\"Author\">\n"
	      "<Exec>\n"
	      "<Command>\"%s\\headless-git.exe\"</Command>\n"
	      "<Arguments>--exec-path=\"%s\" %s for-each-repo --keep-going --config=maintenance.repo maintenance run --schedule=%s</Arguments>\n"
	      "</Exec>\n"
	      "</Actions>\n"
	      "</Task>\n";
	fprintf(tfile->fp, xml, exec_path, exec_path,
		get_extra_config_parameters(), frequency);
	strvec_split(&child.args, cmd);
	strvec_pushl(&child.args, "/create", "/tn", name, "/f", "/xml",
				  get_tempfile_path(tfile), NULL);
	close_tempfile_gently(tfile);

	child.no_stdout = 1;
	child.no_stderr = 1;

	if (start_command(&child))
		die(_("failed to start schtasks"));
	result = finish_command(&child);

	delete_tempfile(&tfile);
	free(name);
	free(cmd);
	return result;
}

static int schtasks_schedule_tasks(void)
{
	const char *exec_path = git_exec_path();

	return schtasks_schedule_task(exec_path, SCHEDULE_HOURLY) ||
	       schtasks_schedule_task(exec_path, SCHEDULE_DAILY) ||
	       schtasks_schedule_task(exec_path, SCHEDULE_WEEKLY);
}

static int schtasks_update_schedule(int run_maintenance, int fd UNUSED)
{
	if (run_maintenance)
		return schtasks_schedule_tasks();
	else
		return schtasks_remove_tasks();
}

MAYBE_UNUSED
static int check_crontab_process(const char *cmd)
{
	struct child_process child = CHILD_PROCESS_INIT;

	strvec_split(&child.args, cmd);
	strvec_push(&child.args, "-l");
	child.no_stdin = 1;
	child.no_stdout = 1;
	child.no_stderr = 1;
	child.silent_exec_failure = 1;

	if (start_command(&child))
		return 0;
	/* Ignore exit code, as an empty crontab will return error. */
	finish_command(&child);
	return 1;
}

static int is_crontab_available(void)
{
	char *cmd;
	int is_available;
	int ret;

	if (get_schedule_cmd("crontab", &is_available, &cmd)) {
		ret = is_available;
		goto out;
	}

#ifdef __APPLE__
	/*
	 * macOS has cron, but it requires special permissions and will
	 * create a UI alert when attempting to run this command.
	 */
	ret = 0;
#else
	ret = check_crontab_process(cmd);
#endif

out:
	free(cmd);
	return ret;
}

#define BEGIN_LINE "# BEGIN GIT MAINTENANCE SCHEDULE"
#define END_LINE "# END GIT MAINTENANCE SCHEDULE"

static int crontab_update_schedule(int run_maintenance, int fd)
{
	char *cmd;
	int result = 0;
	int in_old_region = 0;
	struct child_process crontab_list = CHILD_PROCESS_INIT;
	struct child_process crontab_edit = CHILD_PROCESS_INIT;
	FILE *cron_list, *cron_in;
	struct strbuf line = STRBUF_INIT;
	struct tempfile *tmpedit = NULL;
	int minute = get_random_minute();

	get_schedule_cmd("crontab", NULL, &cmd);
	strvec_split(&crontab_list.args, cmd);
	strvec_push(&crontab_list.args, "-l");
	crontab_list.in = -1;
	crontab_list.out = dup(fd);
	crontab_list.git_cmd = 0;

	if (start_command(&crontab_list)) {
		result = error(_("failed to run 'crontab -l'; your system might not support 'cron'"));
		goto out;
	}

	/* Ignore exit code, as an empty crontab will return error. */
	finish_command(&crontab_list);

	tmpedit = mks_tempfile_t(".git_cron_edit_tmpXXXXXX");
	if (!tmpedit) {
		result = error(_("failed to create crontab temporary file"));
		goto out;
	}
	cron_in = fdopen_tempfile(tmpedit, "w");
	if (!cron_in) {
		result = error(_("failed to open temporary file"));
		goto out;
	}

	/*
	 * Read from the .lock file, filtering out the old
	 * schedule while appending the new schedule.
	 */
	cron_list = fdopen(fd, "r");
	rewind(cron_list);

	while (!strbuf_getline_lf(&line, cron_list)) {
		if (!in_old_region && !strcmp(line.buf, BEGIN_LINE))
			in_old_region = 1;
		else if (in_old_region && !strcmp(line.buf, END_LINE))
			in_old_region = 0;
		else if (!in_old_region)
			fprintf(cron_in, "%s\n", line.buf);
	}
	strbuf_release(&line);

	if (run_maintenance) {
		struct strbuf line_format = STRBUF_INIT;
		const char *exec_path = git_exec_path();

		fprintf(cron_in, "%s\n", BEGIN_LINE);
		fprintf(cron_in,
			"# The following schedule was created by Git\n");
		fprintf(cron_in, "# Any edits made in this region might be\n");
		fprintf(cron_in,
			"# replaced in the future by a Git command.\n\n");

		strbuf_addf(&line_format,
			    "%%d %%s * * %%s \"%s/git\" --exec-path=\"%s\" %s for-each-repo --keep-going --config=maintenance.repo maintenance run --schedule=%%s\n",
			    exec_path, exec_path, get_extra_config_parameters());
		fprintf(cron_in, line_format.buf, minute, "1-23", "*", "hourly");
		fprintf(cron_in, line_format.buf, minute, "0", "1-6", "daily");
		fprintf(cron_in, line_format.buf, minute, "0", "0", "weekly");
		strbuf_release(&line_format);

		fprintf(cron_in, "\n%s\n", END_LINE);
	}

	fflush(cron_in);

	strvec_split(&crontab_edit.args, cmd);
	strvec_push(&crontab_edit.args, get_tempfile_path(tmpedit));
	crontab_edit.git_cmd = 0;

	if (start_command(&crontab_edit)) {
		result = error(_("failed to run 'crontab'; your system might not support 'cron'"));
		goto out;
	}

	if (finish_command(&crontab_edit))
		result = error(_("'crontab' died"));
	else
		fclose(cron_list);

out:
	delete_tempfile(&tmpedit);
	free(cmd);
	return result;
}

static int real_is_systemd_timer_available(void)
{
	struct child_process child = CHILD_PROCESS_INIT;

	strvec_pushl(&child.args, "systemctl", "--user", "list-timers", NULL);
	child.no_stdin = 1;
	child.no_stdout = 1;
	child.no_stderr = 1;
	child.silent_exec_failure = 1;

	if (start_command(&child))
		return 0;
	if (finish_command(&child))
		return 0;
	return 1;
}

static int is_systemd_timer_available(void)
{
	int is_available;

	if (get_schedule_cmd("systemctl", &is_available, NULL))
		return is_available;

	return real_is_systemd_timer_available();
}

static char *xdg_config_home_systemd(const char *filename)
{
	return xdg_config_home_for("systemd/user", filename);
}

#define SYSTEMD_UNIT_FORMAT "git-maintenance@%s.%s"

static int systemd_timer_delete_timer_file(enum schedule_priority priority)
{
	int ret = 0;
	const char *frequency = get_frequency(priority);
	char *local_timer_name = xstrfmt(SYSTEMD_UNIT_FORMAT, frequency, "timer");
	char *filename = xdg_config_home_systemd(local_timer_name);

	if (unlink(filename) && !is_missing_file_error(errno))
		ret = error_errno(_("failed to delete '%s'"), filename);

	free(filename);
	free(local_timer_name);
	return ret;
}

static int systemd_timer_delete_service_template(void)
{
	int ret = 0;
	char *local_service_name = xstrfmt(SYSTEMD_UNIT_FORMAT, "", "service");
	char *filename = xdg_config_home_systemd(local_service_name);
	if (unlink(filename) && !is_missing_file_error(errno))
		ret = error_errno(_("failed to delete '%s'"), filename);

	free(filename);
	free(local_service_name);
	return ret;
}

/*
 * Write the schedule information into a git-maintenance@<schedule>.timer
 * file using a custom minute. This timer file cannot use the templating
 * system, so we generate a specific file for each.
 */
static int systemd_timer_write_timer_file(enum schedule_priority schedule,
					  int minute)
{
	int res = -1;
	char *filename;
	FILE *file;
	const char *unit;
	char *schedule_pattern = NULL;
	const char *frequency = get_frequency(schedule);
	char *local_timer_name = xstrfmt(SYSTEMD_UNIT_FORMAT, frequency, "timer");

	filename = xdg_config_home_systemd(local_timer_name);

	if (safe_create_leading_directories(the_repository, filename)) {
		error(_("failed to create directories for '%s'"), filename);
		goto error;
	}
	file = fopen_or_warn(filename, "w");
	if (!file)
		goto error;

	switch (schedule) {
	case SCHEDULE_HOURLY:
		schedule_pattern = xstrfmt("*-*-* 1..23:%02d:00", minute);
		break;

	case SCHEDULE_DAILY:
		schedule_pattern = xstrfmt("Tue..Sun *-*-* 0:%02d:00", minute);
		break;

	case SCHEDULE_WEEKLY:
		schedule_pattern = xstrfmt("Mon 0:%02d:00", minute);
		break;

	default:
		BUG("Unhandled schedule_priority");
	}

	unit = "# This file was created and is maintained by Git.\n"
	       "# Any edits made in this file might be replaced in the future\n"
	       "# by a Git command.\n"
	       "\n"
	       "[Unit]\n"
	       "Description=Optimize Git repositories data\n"
	       "\n"
	       "[Timer]\n"
	       "OnCalendar=%s\n"
	       "Persistent=true\n"
	       "\n"
	       "[Install]\n"
	       "WantedBy=timers.target\n";
	if (fprintf(file, unit, schedule_pattern) < 0) {
		error(_("failed to write to '%s'"), filename);
		fclose(file);
		goto error;
	}
	if (fclose(file) == EOF) {
		error_errno(_("failed to flush '%s'"), filename);
		goto error;
	}

	res = 0;

error:
	free(schedule_pattern);
	free(local_timer_name);
	free(filename);
	return res;
}

/*
 * No matter the schedule, we use the same service and can make use of the
 * templating system. When installing git-maintenance@<schedule>.timer,
 * systemd will notice that git-maintenance@.service exists as a template
 * and will use this file and insert the <schedule> into the template at
 * the position of "%i".
 */
static int systemd_timer_write_service_template(const char *exec_path)
{
	int res = -1;
	char *filename;
	FILE *file;
	const char *unit;
	char *local_service_name = xstrfmt(SYSTEMD_UNIT_FORMAT, "", "service");

	filename = xdg_config_home_systemd(local_service_name);
	if (safe_create_leading_directories(the_repository, filename)) {
		error(_("failed to create directories for '%s'"), filename);
		goto error;
	}
	file = fopen_or_warn(filename, "w");
	if (!file)
		goto error;

	unit = "# This file was created and is maintained by Git.\n"
	       "# Any edits made in this file might be replaced in the future\n"
	       "# by a Git command.\n"
	       "\n"
	       "[Unit]\n"
	       "Description=Optimize Git repositories data\n"
	       "\n"
	       "[Service]\n"
	       "Type=oneshot\n"
	       "ExecStart=\"%s/git\" --exec-path=\"%s\" %s for-each-repo --keep-going --config=maintenance.repo maintenance run --schedule=%%i\n"
	       "LockPersonality=yes\n"
	       "MemoryDenyWriteExecute=yes\n"
	       "NoNewPrivileges=yes\n"
	       "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_VSOCK\n"
	       "RestrictNamespaces=yes\n"
	       "RestrictRealtime=yes\n"
	       "RestrictSUIDSGID=yes\n"
	       "SystemCallArchitectures=native\n"
	       "SystemCallFilter=@system-service\n";
	if (fprintf(file, unit, exec_path, exec_path, get_extra_config_parameters()) < 0) {
		error(_("failed to write to '%s'"), filename);
		fclose(file);
		goto error;
	}
	if (fclose(file) == EOF) {
		error_errno(_("failed to flush '%s'"), filename);
		goto error;
	}

	res = 0;

error:
	free(local_service_name);
	free(filename);
	return res;
}

static int systemd_timer_enable_unit(int enable,
				     enum schedule_priority schedule,
				     int minute)
{
	char *cmd = NULL;
	struct child_process child = CHILD_PROCESS_INIT;
	const char *frequency = get_frequency(schedule);
	int ret;

	/*
	 * Disabling the systemd unit while it is already disabled makes
	 * systemctl print an error.
	 * Let's ignore it since it means we already are in the expected state:
	 * the unit is disabled.
	 *
	 * On the other hand, enabling a systemd unit which is already enabled
	 * produces no error.
	 */
	if (!enable) {
		child.no_stderr = 1;
	} else if (systemd_timer_write_timer_file(schedule, minute)) {
		ret = -1;
		goto out;
	}

	get_schedule_cmd("systemctl", NULL, &cmd);
	strvec_split(&child.args, cmd);
	strvec_pushl(&child.args, "--user", enable ? "enable" : "disable",
		     "--now", NULL);
	strvec_pushf(&child.args, SYSTEMD_UNIT_FORMAT, frequency, "timer");

	if (start_command(&child)) {
		ret = error(_("failed to start systemctl"));
		goto out;
	}

	if (finish_command(&child)) {
		/*
		 * Disabling an already disabled systemd unit makes
		 * systemctl fail.
		 * Let's ignore this failure.
		 *
		 * Enabling an enabled systemd unit doesn't fail.
		 */
		if (enable) {
			ret = error(_("failed to run systemctl"));
			goto out;
		}
	}

	ret = 0;

out:
	free(cmd);
	return ret;
}

/*
 * A previous version of Git wrote the timer units as template files.
 * Clean these up, if they exist.
 */
static void systemd_timer_delete_stale_timer_templates(void)
{
	char *timer_template_name = xstrfmt(SYSTEMD_UNIT_FORMAT, "", "timer");
	char *filename = xdg_config_home_systemd(timer_template_name);

	if (unlink(filename) && !is_missing_file_error(errno))
		warning(_("failed to delete '%s'"), filename);

	free(filename);
	free(timer_template_name);
}

static int systemd_timer_delete_unit_files(void)
{
	systemd_timer_delete_stale_timer_templates();

	/* Purposefully not short-circuited to make sure all are called. */
	return systemd_timer_delete_timer_file(SCHEDULE_HOURLY) |
	       systemd_timer_delete_timer_file(SCHEDULE_DAILY) |
	       systemd_timer_delete_timer_file(SCHEDULE_WEEKLY) |
	       systemd_timer_delete_service_template();
}

static int systemd_timer_delete_units(void)
{
	int minute = get_random_minute();
	/* Purposefully not short-circuited to make sure all are called. */
	return systemd_timer_enable_unit(0, SCHEDULE_HOURLY, minute) |
	       systemd_timer_enable_unit(0, SCHEDULE_DAILY, minute) |
	       systemd_timer_enable_unit(0, SCHEDULE_WEEKLY, minute) |
	       systemd_timer_delete_unit_files();
}

static int systemd_timer_setup_units(void)
{
	int minute = get_random_minute();
	const char *exec_path = git_exec_path();

	int ret = systemd_timer_write_service_template(exec_path) ||
		  systemd_timer_enable_unit(1, SCHEDULE_HOURLY, minute) ||
		  systemd_timer_enable_unit(1, SCHEDULE_DAILY, minute) ||
		  systemd_timer_enable_unit(1, SCHEDULE_WEEKLY, minute);

	if (ret)
		systemd_timer_delete_units();
	else
		systemd_timer_delete_stale_timer_templates();

	return ret;
}

static int systemd_timer_update_schedule(int run_maintenance, int fd UNUSED)
{
	if (run_maintenance)
		return systemd_timer_setup_units();
	else
		return systemd_timer_delete_units();
}

enum scheduler {
	SCHEDULER_INVALID = -1,
	SCHEDULER_AUTO,
	SCHEDULER_CRON,
	SCHEDULER_SYSTEMD,
	SCHEDULER_LAUNCHCTL,
	SCHEDULER_SCHTASKS,
};

static const struct {
	const char *name;
	int (*is_available)(void);
	int (*update_schedule)(int run_maintenance, int fd);
} scheduler_fn[] = {
	[SCHEDULER_CRON] = {
		.name = "crontab",
		.is_available = is_crontab_available,
		.update_schedule = crontab_update_schedule,
	},
	[SCHEDULER_SYSTEMD] = {
		.name = "systemctl",
		.is_available = is_systemd_timer_available,
		.update_schedule = systemd_timer_update_schedule,
	},
	[SCHEDULER_LAUNCHCTL] = {
		.name = "launchctl",
		.is_available = is_launchctl_available,
		.update_schedule = launchctl_update_schedule,
	},
	[SCHEDULER_SCHTASKS] = {
		.name = "schtasks",
		.is_available = is_schtasks_available,
		.update_schedule = schtasks_update_schedule,
	},
};

static enum scheduler parse_scheduler(const char *value)
{
	if (!value)
		return SCHEDULER_INVALID;
	else if (!strcasecmp(value, "auto"))
		return SCHEDULER_AUTO;
	else if (!strcasecmp(value, "cron") || !strcasecmp(value, "crontab"))
		return SCHEDULER_CRON;
	else if (!strcasecmp(value, "systemd") ||
		 !strcasecmp(value, "systemd-timer"))
		return SCHEDULER_SYSTEMD;
	else if (!strcasecmp(value, "launchctl"))
		return SCHEDULER_LAUNCHCTL;
	else if (!strcasecmp(value, "schtasks"))
		return SCHEDULER_SCHTASKS;
	else
		return SCHEDULER_INVALID;
}

static int maintenance_opt_scheduler(const struct option *opt, const char *arg,
				     int unset)
{
	enum scheduler *scheduler = opt->value;

	BUG_ON_OPT_NEG(unset);

	*scheduler = parse_scheduler(arg);
	if (*scheduler == SCHEDULER_INVALID)
		return error(_("unrecognized --scheduler argument '%s'"), arg);
	return 0;
}

struct maintenance_start_opts {
	enum scheduler scheduler;
};

static enum scheduler resolve_scheduler(enum scheduler scheduler)
{
	if (scheduler != SCHEDULER_AUTO)
		return scheduler;

#if defined(__APPLE__)
	return SCHEDULER_LAUNCHCTL;

#elif defined(GIT_WINDOWS_NATIVE)
	return SCHEDULER_SCHTASKS;

#elif defined(__linux__)
	if (is_systemd_timer_available())
		return SCHEDULER_SYSTEMD;
	else if (is_crontab_available())
		return SCHEDULER_CRON;
	else
		die(_("neither systemd timers nor crontab are available"));

#else
	return SCHEDULER_CRON;
#endif
}

static void validate_scheduler(enum scheduler scheduler)
{
	if (scheduler == SCHEDULER_INVALID)
		BUG("invalid scheduler");
	if (scheduler == SCHEDULER_AUTO)
		BUG("resolve_scheduler should have been called before");

	if (!scheduler_fn[scheduler].is_available())
		die(_("%s scheduler is not available"),
		    scheduler_fn[scheduler].name);
}

static int update_background_schedule(const struct maintenance_start_opts *opts,
				      int enable)
{
	unsigned int i;
	int result = 0;
	struct lock_file lk;
	char *lock_path = xstrfmt("%s/schedule", the_repository->objects->sources->path);

	if (hold_lock_file_for_update(&lk, lock_path, LOCK_NO_DEREF) < 0) {
		if (errno == EEXIST)
			error(_("unable to create '%s.lock': %s.\n\n"
			    "Another scheduled git-maintenance(1) process seems to be running in this\n"
			    "repository. Please make sure no other maintenance processes are running and\n"
			    "then try again. If it still fails, a git-maintenance(1) process may have\n"
			    "crashed in this repository earlier: remove the file manually to continue."),
			    absolute_path(lock_path), strerror(errno));
		else
			error_errno(_("cannot acquire lock for scheduled background maintenance"));
		free(lock_path);
		return -1;
	}

	for (i = 1; i < ARRAY_SIZE(scheduler_fn); i++) {
		if (enable && opts->scheduler == i)
			continue;
		if (!scheduler_fn[i].is_available())
			continue;
		scheduler_fn[i].update_schedule(0, get_lock_file_fd(&lk));
	}

	if (enable)
		result = scheduler_fn[opts->scheduler].update_schedule(
			1, get_lock_file_fd(&lk));

	rollback_lock_file(&lk);

	free(lock_path);
	return result;
}

static const char *const builtin_maintenance_start_usage[] = {
	N_("git maintenance start [--scheduler=<scheduler>]"),
	NULL
};

static int maintenance_start(int argc, const char **argv, const char *prefix,
			     struct repository *repo)
{
	struct maintenance_start_opts opts = { 0 };
	struct option options[] = {
		OPT_CALLBACK_F(
			0, "scheduler", &opts.scheduler, N_("scheduler"),
			N_("scheduler to trigger git maintenance run"),
			PARSE_OPT_NONEG, maintenance_opt_scheduler),
		OPT_END()
	};
	const char *register_args[] = { "register", NULL };

	argc = parse_options(argc, argv, prefix, options,
			     builtin_maintenance_start_usage, 0);
	if (argc)
		usage_with_options(builtin_maintenance_start_usage, options);

	opts.scheduler = resolve_scheduler(opts.scheduler);
	validate_scheduler(opts.scheduler);

	if (update_background_schedule(&opts, 1))
		die(_("failed to set up maintenance schedule"));

	if (maintenance_register(ARRAY_SIZE(register_args)-1, register_args, NULL, repo))
		warning(_("failed to add repo to global config"));
	return 0;
}

static const char *const builtin_maintenance_stop_usage[] = {
	"git maintenance stop",
	NULL
};

static int maintenance_stop(int argc, const char **argv, const char *prefix,
			    struct repository *repo UNUSED)
{
	struct option options[] = {
		OPT_END()
	};
	argc = parse_options(argc, argv, prefix, options,
			     builtin_maintenance_stop_usage, 0);
	if (argc)
		usage_with_options(builtin_maintenance_stop_usage, options);
	return update_background_schedule(NULL, 0);
}

static const char *const builtin_maintenance_is_needed_usage[] = {
	"git maintenance is-needed [--task=<task>] [--schedule]",
	NULL
};

static int maintenance_is_needed(int argc, const char **argv, const char *prefix,
				 struct repository *repo UNUSED)
{
	struct maintenance_run_opts opts = MAINTENANCE_RUN_OPTS_INIT;
	struct string_list selected_tasks = STRING_LIST_INIT_DUP;
	struct gc_config cfg = GC_CONFIG_INIT;
	struct option options[] = {
		OPT_BOOL(0, "auto", &opts.auto_flag,
			 N_("run tasks based on the state of the repository")),
		OPT_CALLBACK_F(0, "task", &selected_tasks, N_("task"),
			       N_("check a specific task"),
			       PARSE_OPT_NONEG, task_option_parse),
		OPT_END()
	};
	bool is_needed = false;

	argc = parse_options(argc, argv, prefix, options,
			     builtin_maintenance_is_needed_usage,
			     PARSE_OPT_STOP_AT_NON_OPTION);
	if (argc)
		usage_with_options(builtin_maintenance_is_needed_usage, options);

	gc_config(&cfg);
	initialize_task_config(&opts, &selected_tasks);

	if (opts.auto_flag) {
		for (size_t i = 0; i < opts.tasks_nr; i++) {
			if (tasks[opts.tasks[i]].auto_condition &&
			    tasks[opts.tasks[i]].auto_condition(&cfg)) {
				is_needed = true;
				break;
			}
		}
	} else {
		/*
		 * When not using --auto we always require maintenance right now.
		 *
		 * TODO: this certainly is too eager, as some maintenance tasks may
		 * decide to not do anything because the data structures are already
		 * fully optimized. We may eventually want to extend the auto
		 * condition to also cover non-auto runs so that we can detect such
		 * cases.
		 */
		is_needed = true;
	}

	string_list_clear(&selected_tasks, 0);
	maintenance_run_opts_release(&opts);
	gc_config_release(&cfg);

	if (is_needed)
		return 0;
	return 1;
}

static const char *const builtin_maintenance_usage[] = {
	N_("git maintenance <subcommand> [<options>]"),
	NULL,
};

int cmd_maintenance(int argc,
		    const char **argv,
		    const char *prefix,
		    struct repository *repo)
{
	parse_opt_subcommand_fn *fn = NULL;
	struct option builtin_maintenance_options[] = {
		OPT_SUBCOMMAND("run", &fn, maintenance_run),
		OPT_SUBCOMMAND("start", &fn, maintenance_start),
		OPT_SUBCOMMAND("stop", &fn, maintenance_stop),
		OPT_SUBCOMMAND("register", &fn, maintenance_register),
		OPT_SUBCOMMAND("unregister", &fn, maintenance_unregister),
		OPT_SUBCOMMAND("is-needed", &fn, maintenance_is_needed),
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, builtin_maintenance_options,
			     builtin_maintenance_usage, 0);
	return fn(argc, argv, prefix, repo);
}

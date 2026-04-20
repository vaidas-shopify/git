#!/bin/sh

test_description='maintenance stratify task'

. ./test-lib.sh

GIT_TEST_COMMIT_GRAPH=0
GIT_TEST_MULTI_PACK_INDEX=0

# Stratify only packs commits older than maintenance.stratified.min-age
# (default "2.weeks.ago"). Force commits to land in the distant past so
# every commit is eligible without having to tune the config.
GIT_AUTHOR_DATE="@1 +0000"
GIT_COMMITTER_DATE="@1 +0000"
export GIT_AUTHOR_DATE GIT_COMMITTER_DATE

count_packs () {
	ls .git/objects/pack/*.pack 2>/dev/null | wc -l | tr -d ' '
}

count_sidecars () {
	ls .git/objects/pack/*.base-stratum 2>/dev/null | wc -l | tr -d ' '
}

# Print the anchor_ref recorded in each .base-stratum sidecar in the
# current repo, one per line, sorted.
extract_sidecar_refs () {
	perl - .git/objects/pack/*.base-stratum <<-\EOF | sort
		for my $path (@ARGV) {
			open my $fh, "<", $path or die "open $path: $!";
			binmode $fh;
			my $buf;
			read $fh, $buf, -s $path;
			my $hash_id = unpack("N", substr($buf, 8, 4));
			my $rawsz = ($hash_id == 1) ? 20 : 32;
			my $count = unpack("N", substr($buf, 12, 4));
			my $ref_start = 16 + $count * $rawsz + 4;
			my $ref_end = index($buf, "\0", $ref_start);
			print substr($buf, $ref_start, $ref_end - $ref_start), "\n";
		}
	EOF
}

test_expect_success 'two anchors at same commit get distinct packs' '
	test_create_repo two-anchor-same-commit &&
	(
		cd two-anchor-same-commit &&
		test_commit --no-tag c1 &&
		test_commit --no-tag c2 &&

		# Two refs pointing at the same commit
		git update-ref refs/heads/release HEAD &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config --add maintenance.stratified.anchor refs/heads/release &&

		git maintenance run --task=stratify --quiet &&

		test 2 -eq $(count_sidecars) &&

		# Each sidecar must record a different anchor_ref. If the
		# packs collided on the same path, the second write would
		# have overwritten the first sidecar, leaving only one.
		extract_sidecar_refs >actual &&
		printf "refs/heads/master\nrefs/heads/release\n" >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'second stratify run preserves both anchors sidecars' '
	test_create_repo two-anchor-incremental &&
	(
		cd two-anchor-incremental &&
		test_commit --no-tag c1 &&
		test_commit --no-tag c2 &&
		git update-ref refs/heads/release HEAD &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config --add maintenance.stratified.anchor refs/heads/release &&

		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		# A re-run with no new commits should be a no-op for both
		# anchors. If one sidecar had been clobbered by the other,
		# the orphaned anchor would re-stratify from scratch and
		# create another pack.
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&
		test 2 -eq $(count_packs)
	)
'

test_expect_success 'stratify rewrites read-only sidecar in place' '
	test_create_repo rewrite-sidecar &&
	(
		cd rewrite-sidecar &&
		test_commit --no-tag c1 &&
		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&

		# write_pack_base_stratum creates .base-stratum and .keep
		# at mode 0444. A later run that produces the same pack
		# hash (deterministic from rev-list output) must be able to
		# rewrite the sidecar even though it cannot be opened for
		# writing in place.
		#
		# This race is real: "git commit" with the geometric
		# maintenance strategy spawns a detached "git maintenance
		# run --auto --detach" that runs stratify in the
		# background, then a subsequent explicit stratify run
		# produces the same deterministic pack hash and rewrites
		# the same sidecar path. A non-atomic implementation fails
		# the loser with EACCES on the existing 0444 file.
		#
		# Approximate the race by rerunning stratify with no new
		# commits; the rev-list bound by ^last_stratified is empty
		# so this is a no-op, but if last_stratified had been
		# stale we would re-pack. Force the rewrite path by
		# unlinking only the .base-stratum sidecar (leaving the
		# .pack and .keep) and rerunning: stratify will re-stratify
		# the anchor, produce the same pack hash, and need to
		# overwrite the existing read-only .keep alongside the
		# missing .base-stratum.
		newest=$(ls -t .git/objects/pack/*.base-stratum | head -1) &&
		rm -f "$newest" &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars)
	)
'

test_expect_success 'frontier picks ancestrally-latest, not max committer date' '
	test_create_repo frontier-by-ancestry &&
	(
		cd frontier-by-ancestry &&

		# Construct a non-monotonic linear chain: c2 (child) is
		# committed earlier than c1 (parent). Committer date is
		# therefore NOT a faithful topological order along master.
		test_commit --no-tag --date="@2000 +0000" c1 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&

		test_commit --no-tag --date="@1000 +0000" c2 &&
		c2_oid=$(git rev-parse HEAD) &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		# Run stratify a third time with no new commits. The
		# frontier must be c2 (the descendant), not c1 (the parent
		# with the larger committer date). When the descendant is
		# correctly chosen, rev-list ^c2..master walks nothing and
		# stratify reports "already fully stratified at <c2>".
		# A buggy date-only frontier would pick c1, walk c2 again,
		# and the OID-filter "skipped ... already in base-stratum"
		# path would fire.
		git maintenance run --task=stratify --no-quiet 2>err &&
		test_grep "already fully stratified at $c2_oid" err &&
		! grep "skipped.*objects already in base-stratum" err
	)
'

test_done

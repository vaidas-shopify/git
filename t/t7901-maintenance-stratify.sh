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

# Print the anchor_commit OIDs recorded in the sidecar at $1, one per
# line, sorted. A sidecar records the maximal antichain of fully-included
# commits, so this is one OID per live branch covered by the pack.
extract_sidecar_anchor () {
	perl - "$1" <<-\EOF | sort
		my $path = $ARGV[0];
		open my $fh, "<", $path or die "open $path: $!";
		binmode $fh;
		my $buf;
		read $fh, $buf, -s $path;
		my $hash_id = unpack("N", substr($buf, 8, 4));
		my $rawsz = ($hash_id == 1) ? 20 : 32;
		my $count = unpack("N", substr($buf, 12, 4));
		for my $i (0 .. $count - 1) {
			print unpack("H*", substr($buf, 16 + $i * $rawsz, $rawsz)), "\n";
		}
	EOF
}

extract_sidecar_anchor_count () {
	perl - "$1" <<-\EOF
		my $path = $ARGV[0];
		open my $fh, "<", $path or die "open $path: $!";
		binmode $fh;
		my $buf;
		read $fh, $buf, -s $path;
		print unpack("N", substr($buf, 12, 4)), "\n";
	EOF
}

# Print the .base-stratum sidecar whose recorded anchor set CONTAINS $1.
sidecar_for_anchor () {
	for sc in .git/objects/pack/*.base-stratum
	do
		if extract_sidecar_anchor "$sc" | grep -qx "$1"
		then
			echo "$sc"
			return
		fi
	done
}

# Overwrite the stratified_timestamp in a .base-stratum sidecar and
# recompute its trailing checksum, so we can construct timestamps that
# disagree with the natural commit-graph order.
set_sidecar_timestamp () {
	perl - "$1" "$2" <<-\EOF
	use Digest::SHA;
	my ($path, $new_ts) = @ARGV;
	chmod 0644, $path or die "chmod $path: $!";
	open my $fh, "+<:raw", $path or die "open $path: $!";
	binmode $fh;
	my $buf;
	{ local $/; $buf = <$fh>; }
	my $hash_id = unpack("N", substr($buf, 8, 4));
	my $rawsz = ($hash_id == 1) ? 20 : 32;
	my $algo = ($hash_id == 1) ? "sha1" : "sha256";
	my $count = unpack("N", substr($buf, 12, 4));
	substr($buf, 16 + $count * $rawsz, 4) = pack("N", $new_ts);
	my $body_len = length($buf) - $rawsz;
	my $sha = Digest::SHA->new($algo);
	$sha->add(substr($buf, 0, $body_len));
	substr($buf, $body_len, $rawsz) = $sha->digest;
	seek $fh, 0, 0 or die;
	print $fh $buf;
	close $fh or die;
	chmod 0444, $path;
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

test_expect_success 'batch-size truncation records the last fully-included commit' '
	test_create_repo batch-truncate-anchor &&
	(
		cd batch-truncate-anchor &&

		# Three commits, each adding one new file: 1 commit + 1
		# root tree + 1 blob = 3 objects per commit, so 9 objects
		# total. With --in-commit-order, rev-list emits each
		# commit followed by its trees and blobs.
		test_commit --no-tag c1 &&
		test_commit --no-tag c2 &&
		c2_oid=$(git rev-parse HEAD) &&
		test_commit --no-tag c3 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&

		# The batch limit is a soft limit checked at commit
		# boundaries on the count of objects already
		# flushed. batch-size=4: c1 flushes (3 < 4, keep going),
		# then c2 flushes (6 >= 4, stop). c1 and c2 land in the
		# pack and the recorded anchor is c2 -- the last
		# fully-included commit, which dominates the pack -- so a
		# follow-up run resumes cleanly from it.
		git config maintenance.stratified.batch-size 4 &&
		git maintenance run --task=stratify --quiet &&

		test 1 -eq $(count_sidecars) &&
		sc=$(ls .git/objects/pack/*.base-stratum) &&
		extract_sidecar_anchor "$sc" >actual &&
		echo "$c2_oid" >expect &&
		test_cmp expect actual &&

		# A follow-up run with no batch limit must pick up where
		# the previous one left off and stratify the rest. With
		# the buggy frontier, ^c2 would have permanently hidden
		# c3 from the next walk.
		git config --unset maintenance.stratified.batch-size &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars)
	)
'

test_expect_success 'batch-size smaller than a single commit still makes progress' '
	test_create_repo batch-single-commit &&
	(
		cd batch-single-commit &&

		# A single commit alone produces 3 objects (commit, root
		# tree, blob); batch-size=2 cannot hold even one commit.
		# The task must still advance the frontier — silently
		# emitting an empty batch would mean stratification can
		# never complete on this repo.
		test_commit --no-tag c1 &&
		c1_oid=$(git rev-parse HEAD) &&
		test_commit --no-tag c2 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config maintenance.stratified.batch-size 2 &&

		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&
		sc=$(ls .git/objects/pack/*.base-stratum) &&
		extract_sidecar_anchor "$sc" >actual &&
		echo "$c1_oid" >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'stratify ignores pack.packSizeLimit and writes one self-contained pack' '
	test_create_repo packsizelimit-anchor &&
	(
		cd packsizelimit-anchor &&

		# pack-objects enforces a 1MB floor on pack.packSizeLimit, so
		# the stratified content has to exceed it to risk a split: three
		# incompressible ~700k blobs add up to ~2MB. If packSizeLimit
		# leaked into the child pack-objects, the output would split into
		# several base-stratum-prefixed packs (with several emitted
		# hashes) and stratify would report success while writing no
		# sidecar at all.
		for i in 1 2 3
		do
			test-tool genrandom "blob$i" 700000 >big$i &&
			git add big$i &&
			git commit -q -m big$i || return 1
		done &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config pack.packSizeLimit 1m &&

		git maintenance run --task=stratify --no-quiet &&

		# Exactly one base-stratum pack, with its sidecar.
		test 1 -eq $(count_packs) &&
		test 1 -eq $(count_sidecars)
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

test_expect_success 'stratify advances through a merge using multi-bound frontier' '
	test_create_repo merge-multi-bound &&
	(
		cd merge-multi-bound &&

		# Build a merge history: c1, then sibling branches left
		# and right1, then a merge commit on master, then c3.
		# Total stratifiable commits: c1, left, right1, merge, c3.
		# With batch-size=1, each stratify run packs exactly one
		# commit, so after stratifying both siblings the frontier
		# becomes ancestrally incomparable (neither sibling is an
		# ancestor of the other). The merge commit is reachable
		# from master but NOT from either sibling alone, so a
		# single ^bound would cause the rev-list to re-walk the
		# other sibling. Multi-bound advancement requires passing
		# all maximal frontier OIDs as ^bounds.
		test_commit --no-tag c1 &&
		git branch right HEAD &&
		test_commit --no-tag left &&
		git checkout -q right &&
		test_commit --no-tag right1 &&
		git checkout -q master &&
		git merge --no-ff right -m merge &&
		test_commit --no-tag c3 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config maintenance.stratified.batch-size 1 &&

		# Five stratify runs should each pack one commit; if
		# multi-bound advancement does not work, runs 4 and 5
		# would either be no-ops or repack already-packed objects.
		for i in 1 2 3 4 5
		do
			git maintenance run --task=stratify --quiet || return 1
		done &&
		test 5 -eq $(count_sidecars) &&

		# Sixth run must be a clean no-op: full frontier covers
		# master, no rev-list work, no "skipped" or "incomparable"
		# noise.
		git maintenance run --task=stratify --no-quiet 2>err &&
		test_grep "already fully stratified" err &&
		! grep -i "incomparable" err &&
		! grep "skipped.*objects already in base-stratum" err
	)
'

test_expect_success PERL 'validate is robust to non-monotonic stratified_timestamp' '
	test_create_repo cascade-bug &&
	(
		cd cascade-bug &&

		# Two stratify runs with new commits in between produce
		# two packs in the same anchor group:
		#   P1 covers c1..c2 with anchor_commit=c2
		#   P2 covers c3      with anchor_commit=c3
		#
		# Their stratified_timestamps naturally come out
		# monotone (P2 strictly later than P1).
		test_commit --no-tag c1 &&
		test_commit --no-tag c2 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&
		p1_sc=$(ls .git/objects/pack/*.base-stratum) &&
		c2_oid=$(git rev-parse HEAD) &&

		test_commit --no-tag c3 &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&
		p2_sc=$(ls .git/objects/pack/*.base-stratum |
			grep -v -F -- "$p1_sc") &&

		# Force the timestamp ordering to disagree with the
		# commit-graph ordering: set P2 (anchor=c3) to a tiny
		# timestamp so it sorts BEFORE P1 (anchor=c2).
		# Cascade-by-timestamp would then validate P2 first,
		# find it invalid against the rewound ref, and demote
		# every later entry in sort order — which would include
		# P1, the still-valid pack covering c1..c2.
		set_sidecar_timestamp "$p2_sc" 1 &&

		# Rewind master so c3 (P2 anchor) is unreachable but c2
		# (P1 anchor) remains reachable.
		git update-ref refs/heads/master "$c2_oid" &&

		# Stratify runs validate first.
		git maintenance run --task=stratify --no-quiet 2>err &&

		# A buggy cascade would demote every pack in the group
		# after the first invalid entry, regardless of whether
		# those later entries are actually invalid; the per-anchor
		# loop then re-stratifies from scratch and silently
		# resurrects the same pack at the same path. The path /
		# count assertions cannot tell the two regimes apart, so
		# pin the diagnostic instead: independent validation
		# never emits the "cascade-demoting" message.
		test ! -f "$p2_sc" &&
		test -f "$p1_sc" &&
		test 1 -eq $(count_sidecars) &&
		! grep "cascade-demoting" err
	)
'

test_expect_success PERL 'consolidate-stratum picks merged anchor by commit date' '
	test_create_repo consolidate-by-date &&
	(
		cd consolidate-by-date &&

		# Three sequential commits with strictly increasing
		# committer dates, each followed by an incremental
		# stratify run, produce three sidecars in one anchor
		# group whose anchor_commits lie on one linear chain
		# (c1 < c2 < c3 by committer date).
		test_commit --no-tag --date="@1000 +0000" c1 &&
		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		p1_sc=$(ls .git/objects/pack/*.base-stratum) &&

		test_commit --no-tag --date="@2000 +0000" c2 &&
		git maintenance run --task=stratify --quiet &&
		p2_sc=$(ls .git/objects/pack/*.base-stratum |
			grep -v -F -- "$p1_sc") &&

		test_commit --no-tag --date="@3000 +0000" c3 &&
		git maintenance run --task=stratify --quiet &&
		p3_sc=$(ls .git/objects/pack/*.base-stratum |
			grep -v -F -- "$p1_sc" |
			grep -v -F -- "$p2_sc") &&
		test 3 -eq $(count_sidecars) &&

		c3_oid=$(git rev-parse HEAD) &&

		# Invert the stratified_timestamp ordering so it
		# disagrees with the commit-graph order: P3
		# (anchor=c3, latest commit) carries the smallest
		# timestamp, P1 the largest. A buggy
		# consolidate-stratum would prefer P1 as the merged
		# anchor (largest sidecar timestamp).
		set_sidecar_timestamp "$p3_sc" 1 &&
		set_sidecar_timestamp "$p2_sc" 100 &&
		set_sidecar_timestamp "$p1_sc" 200 &&

		git maintenance run --task=consolidate-stratum --quiet &&

		# Exactly one merged sidecar remains, and its
		# anchor_commit is c3 — latest by committer date,
		# not P1 (latest by stratified_timestamp).
		test 1 -eq $(count_sidecars) &&
		merged_sc=$(ls .git/objects/pack/*.base-stratum) &&
		extract_sidecar_anchor "$merged_sc" >actual &&
		echo "$c3_oid" >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'consolidate-stratum merges packs into a multi-anchor union' '
	test_create_repo consolidate-multi-anchor &&
	(
		cd consolidate-multi-anchor &&

		# Old fork joined by a recent (un-stratifiable) merge, exactly
		# as in the stranding test. With batch-size=1 the eligible fork
		# is stratified one commit per run, producing several
		# single-anchor packs whose anchors together form the antichain
		# {left3, right3}.
		test_commit --no-tag c1 &&
		git branch right &&
		test_commit --no-tag left1 &&
		test_commit --no-tag left2 &&
		test_commit --no-tag left3 &&
		left3=$(git rev-parse HEAD) &&
		git checkout -q right &&
		test_commit --no-tag right1 &&
		test_commit --no-tag right2 &&
		test_commit --no-tag right3 &&
		right3=$(git rev-parse HEAD) &&
		git checkout -q master &&
		now=$(date +%s) &&
		recent="@$now +0000" &&
		GIT_AUTHOR_DATE="$recent" GIT_COMMITTER_DATE="$recent" \
			git merge --no-ff -m merge right &&

		git config --add maintenance.stratified.anchor refs/heads/master &&

		git config maintenance.stratified.batch-size 1 &&
		for i in $(test_seq 1 7)
		do
			git maintenance run --task=stratify --quiet || return 1
		done &&
		git config --unset maintenance.stratified.batch-size &&
		nsc=$(count_sidecars) &&
		test "$nsc" -ge 2 &&

		# Consolidate merges the below-split packs and records the
		# UNION of their anchor antichains in one sidecar. The previous
		# implementation refused this (a single recorded anchor could
		# not represent both fork branches) and left the packs split.
		git maintenance run --task=consolidate-stratum --quiet &&
		test $(count_sidecars) -lt "$nsc" &&

		# Coverage is preserved: both branch tips remain recorded
		# anchors across the surviving sidecars, and at least one
		# sidecar now records more than one anchor.
		test -n "$(sidecar_for_anchor "$left3")" &&
		test -n "$(sidecar_for_anchor "$right3")" &&
		for sc in .git/objects/pack/*.base-stratum
		do
			extract_sidecar_anchor_count "$sc" || return 1
		done >counts &&
		sort -rn counts >counts_desc &&
		test "$(head -n 1 counts_desc)" -ge 2 &&

		# Stratify remains a clean no-op afterwards.
		git maintenance run --task=stratify --no-quiet 2>err &&
		test_grep "already fully stratified" err &&
		! grep "skipped.*already in base-stratum" err
	)
'

test_expect_success PERL 'standalone consolidate-stratum validates packs before merging' '
	test_create_repo consolidate-validates &&
	(
		cd consolidate-validates &&

		# Two stratify runs produce two base-stratum packs in the
		# same anchor group: P1 covers c1, P2 covers c2. A rewind
		# of master back to c1 invalidates P2 (its anchor_commit
		# c2 is no longer an ancestor of the tip) without touching
		# P1.
		test_commit --no-tag c1 &&
		c1_oid=$(git rev-parse HEAD) &&
		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		p1_sc=$(ls .git/objects/pack/*.base-stratum) &&

		test_commit --no-tag c2 &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		git update-ref refs/heads/master "$c1_oid" &&

		# Without validation, consolidate-stratum would happily
		# merge P1 and P2 into a new pack stamped with the orphan
		# c2 anchor_commit, silently resurrecting the rewound
		# state under a fresh pack hash. Standalone runs must
		# validate first and demote P2 themselves.
		git maintenance run --task=consolidate-stratum --no-quiet 2>err &&
		test_grep "demoting" err &&
		test 1 -eq $(count_sidecars) &&
		surviving_sc=$(ls .git/objects/pack/*.base-stratum) &&
		extract_sidecar_anchor "$surviving_sc" >actual &&
		echo "$c1_oid" >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'standalone consolidate-stratum refuses with no configured anchors' '
	test_create_repo consolidate-empty &&
	(
		cd consolidate-empty &&
		test_commit --no-tag c1 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&

		git config --unset-all maintenance.stratified.anchor &&

		# With every pack an orphan, merging them all into one
		# would re-anchor the cluster under whatever orphan ref
		# happened to win the group sort. Refuse, same as
		# stratify-prune.
		git maintenance run --task=consolidate-stratum --no-quiet 2>err &&
		test_grep "refusing to run" err &&
		test 1 -eq $(count_sidecars)
	)
'

test_done

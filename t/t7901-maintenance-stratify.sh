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

# Print the anchor_ref recorded in the single sidecar at $1.
extract_sidecar_ref () {
	perl - "$1" <<-\EOF
		my $path = $ARGV[0];
		open my $fh, "<", $path or die "open $path: $!";
		binmode $fh;
		my $buf;
		read $fh, $buf, -s $path;
		my $hash_id = unpack("N", substr($buf, 8, 4));
		my $rawsz = ($hash_id == 1) ? 20 : 32;
		my $count = unpack("N", substr($buf, 12, 4));
		my $ref_start = 16 + $count * $rawsz + 4;
		my $ref_end = index($buf, "\0", $ref_start);
		print substr($buf, $ref_start, $ref_end - $ref_start);
	EOF
}

# Print the .base-stratum sidecar whose recorded anchor_ref is $1.
sidecar_for_ref () {
	for sc in .git/objects/pack/*.base-stratum
	do
		if test "$(extract_sidecar_ref "$sc")" = "$1"
		then
			echo "$sc"
			return
		fi
	done
}

# Configure two anchors that point at sibling commits — they share a
# common base (c1) but each has its own unique commit beyond it. Each
# pack is self-contained and includes the shared c1 base independently.
setup_two_distinct_anchors () {
	test_commit --no-tag c1 &&
	git branch release HEAD &&
	test_commit --no-tag c2 &&
	git checkout -q release &&
	test_commit --no-tag r1 &&
	git checkout -q master &&

	git config --add maintenance.stratified.anchor refs/heads/master &&
	git config --add maintenance.stratified.anchor refs/heads/release
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

		git maintenance run --task=stratify --no-quiet &&

		# Two sidecars and two packs, one per anchor. Each
		# sidecar records a different anchor_ref; if the packs
		# collided on the same path, the second write would have
		# overwritten the first sidecar, leaving only one.
		test 2 -eq $(count_sidecars) &&
		test 2 -eq $(count_packs) &&
		extract_sidecar_refs >actual &&
		printf "refs/heads/master\nrefs/heads/release\n" >expect &&
		test_cmp expect actual &&

		# Surface-gc readiness recognizes each anchor from its
		# own pack — no cross-anchor frontier lookup is needed.
		git config maintenance.stratified.min-age "now" &&
		git config maintenance.stratified.grace-period "now" &&
		git maintenance run --task=surface-gc --no-quiet 2>err &&
		! grep "no stratified commits yet" err
	)
'

test_expect_success 'each anchor gets a self-contained pack even with shared history' '
	test_create_repo shared-history-anchors &&
	(
		cd shared-history-anchors &&

		# release is a strict ancestor of master: release at c1,
		# master at c2 (child of c1). Each anchor produces its
		# own pack and the shared c1 objects appear in both
		# packs — no cross-anchor filtering.
		test_commit --no-tag c1 &&
		git branch release HEAD &&
		test_commit --no-tag c2 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config --add maintenance.stratified.anchor refs/heads/release &&

		git maintenance run --task=stratify --no-quiet &&

		# Two sidecars and two packs, one per anchor.
		test 2 -eq $(count_sidecars) &&
		test 2 -eq $(count_packs) &&
		extract_sidecar_refs >actual &&
		printf "refs/heads/master\nrefs/heads/release\n" >expect &&
		test_cmp expect actual &&

		# Surface-gc readiness recognizes each anchor from its
		# own pack — no cross-anchor frontier lookup is needed.
		git config maintenance.stratified.min-age "now" &&
		git config maintenance.stratified.grace-period "now" &&
		git maintenance run --task=surface-gc --no-quiet 2>err &&
		! grep "no stratified commits yet" err
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

test_expect_success 'annotated-tag anchor: incremental detection and surface-gc gating' '
	test_create_repo annotated-tag-anchor &&
	(
		cd annotated-tag-anchor &&
		test_commit --no-tag c1 &&
		git tag -a -m "release v1" v1 &&

		# Sanity-check: the tag must be annotated, otherwise
		# refs/tags/v1 resolves directly to the commit and the
		# bug under test cannot manifest.
		tag_oid=$(git rev-parse refs/tags/v1) &&
		commit_oid=$(git rev-parse refs/tags/v1^{commit}) &&
		test "$tag_oid" != "$commit_oid" &&

		git config --add maintenance.stratified.anchor refs/tags/v1 &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&

		# The sidecar records the peeled commit OID, not the tag
		# object OID; rev-list emits commits, and the stratify
		# task picks the last fully-included commit as the anchor.
		sc=$(ls .git/objects/pack/*.base-stratum) &&
		extract_sidecar_anchor "$sc" >actual &&
		echo "$commit_oid" >expect &&
		test_cmp expect actual &&

		# A second stratify run with no new commits must be a
		# no-op. find_stratified_ancestor() peels tip_oid through
		# the tag to find last_stratified=commit_oid; without
		# peeling, lookup_commit() rejects the tag OID, the
		# helper returns NULL, and rev-list re-walks all history
		# and writes a second pack.
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&
		test 1 -eq $(count_packs) &&

		# surface-gc readiness: stratified_frontier_date() must
		# also peel the tag, otherwise it reports "no stratified
		# commits yet" and surface-gc skips indefinitely on
		# tag-anchored repos.
		git config maintenance.stratified.min-age "now" &&
		git config maintenance.stratified.grace-period "now" &&
		git maintenance run --task=surface-gc --no-quiet 2>err &&
		! grep "no stratified commits yet" err
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

		# P2 (anchor c3) is invalid after the rewind. Closure is a
		# whole-group property -- a surviving sibling can depend on a
		# demoted one -- so validation demotes the ENTIRE anchor
		# group, including the still-valid P1, and the same run
		# rebuilds the stratum for master from the current tip c2.
		# The rebuilt pack has identical contents to P1, so it reappears
		# at the same path and the count returns to 1; the path/count
		# assertions cannot tell that regime from "P2 demoted, P1
		# untouched", so pin the cascade diagnostic. The cascade is
		# driven by validation + group membership, never by the
		# (here inverted) stratified_timestamp ordering.
		test_grep "closed-set invariant" err &&
		test ! -f "$p2_sc" &&
		test -f "$p1_sc" &&
		test 1 -eq $(count_sidecars)
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

test_expect_success 'surface-gc readiness uses graph query, not max(date) on antichain' '
	test_create_repo readiness-graph &&
	(
		cd readiness-graph &&

		# Build a merge DAG:
		#   c1 -> old-left ----> merge -> tip
		#      \              /
		#       recent-right -
		#
		# c1, old-left, merge, tip have very old committer dates;
		# recent-right is dated 1 hour ago. With
		# min-age=30.minutes.ago and grace-period=1.day.ago the
		# surface-gc cutoff is roughly 24.5 hours ago — recent-right
		# is far newer than the cutoff but still older than min-age,
		# so stratify will pack it and a max(date)-based readiness
		# check would treat the anchor as caught up even on a slow
		# CI machine.
		#
		# After three batch-size=1 stratify runs the antichain
		# frontier is {old-left, recent-right}. The merge commit
		# is still in the active stratum and is older than the
		# cutoff. A max(date)-based readiness check would have
		# seen recent-right (newer than cutoff) and wrongly
		# declared "caught up"; the graph query finds the old
		# merge commit reachable from tip without going through
		# any antichain element and reports "lagging".
		now=$(date +%s) &&
		old="@1 +0000" &&
		recent="@$((now - 3600)) +0000" &&

		test_commit --no-tag --date "$old" c1 &&
		test_commit --no-tag --date "$old" old-left &&
		git branch right HEAD~1 &&
		git checkout -q right &&
		test_commit --no-tag --date "$recent" recent-right &&
		git checkout -q master &&
		GIT_AUTHOR_DATE="$old" GIT_COMMITTER_DATE="$old" \
			git merge --no-ff -m merge right &&
		test_commit --no-tag --date "$old" tip &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config maintenance.stratified.batch-size 1 &&
		git config maintenance.stratified.min-age "30.minutes.ago" &&
		git config maintenance.stratified.grace-period "1.day.ago" &&

		for i in 1 2 3
		do
			git maintenance run --task=stratify --quiet || return 1
		done &&
		test 3 -eq $(count_sidecars) &&

		git maintenance run --task=surface-gc --no-quiet 2>err &&
		test_grep "is lagging" err &&
		test_grep "old commits remain outside the frontier" err
	)
'

test_expect_success 'stratify does not strand fork siblings behind a recent merge' '
	test_create_repo fork-no-strand &&
	(
		cd fork-no-strand &&

		# Old fork joined by a merge that is too recent to stratify:
		#
		#   c1 -> left1 -> left2 -> left3 --\
		#     \                              merge   (recent, master tip)
		#      -> right1 -> right2 -> right3 /
		#
		# c1 and the branch commits use the incrementing test_tick dates
		# (years old, so older than min-age and eligible); the merge is
		# dated "now" (newer than min-age, so never stratified). The
		# merge is the only common descendant of the two branches, so
		# while it stays outside the walk the eligible frontier can only
		# ever be the antichain {left3, right3}: it never collapses to a
		# single commit.
		#
		# A run that recorded a single anchor covering objects from both
		# branches would strand one branch: ^frontier could not exclude
		# it, so its objects would be re-walked and re-filtered on every
		# later run, and surface-gc would lag forever. Instead the pack
		# records the whole maximal antichain of fully-included commits,
		# {left3, right3}, so a single base-stratum pack covers the entire
		# old fork and the next run excludes both branches with nothing
		# re-walked.
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

		# The merge must keep a recent committer date; do not route it
		# through test_commit, whose test_tick would reset the date back
		# into the eligible (old) range.
		now=$(date +%s) &&
		recent="@$now +0000" &&
		GIT_AUTHOR_DATE="$recent" GIT_COMMITTER_DATE="$recent" \
			git merge --no-ff -m merge right &&

		git config --add maintenance.stratified.anchor refs/heads/master &&

		# A single run packs the entire old fork into ONE pack and
		# records both eligible branch tips as anchors -- no batch-size
		# is set, so the whole eligible frontier is covered at once. An
		# implementation that stopped at the first fork boundary would
		# have needed many runs (one commit per fork commit), and
		# recording a single anchor would have stranded a branch
		# entirely.
		git maintenance run --task=stratify --no-quiet 2>err &&
		test 1 -eq $(count_sidecars) &&
		sc=$(ls .git/objects/pack/*.base-stratum) &&
		test 2 -eq $(extract_sidecar_anchor_count "$sc") &&
		extract_sidecar_anchor "$sc" >actual &&
		printf "%s\n%s\n" "$left3" "$right3" | sort >expect &&
		test_cmp expect actual &&
		! grep "skipped.*already in base-stratum" err &&

		# A further run is a clean no-op: fully stratified, nothing
		# re-walked, nothing skipped.
		git maintenance run --task=stratify --no-quiet 2>err2 &&
		test_grep "already fully stratified" err2 &&
		! grep "skipped.*already in base-stratum" err2 &&

		# With the old fork fully stratified and only the recent merge
		# outside the frontier, surface-gc is caught up rather than
		# permanently lagging.
		git maintenance run --task=surface-gc --no-quiet 2>sgc_err &&
		! grep "is lagging" sgc_err
	)
'

test_expect_success 'orphan anchor is reported but not demoted by stratify' '
	test_create_repo orphan-warns &&
	(
		cd orphan-warns &&
		test_commit --no-tag c1 &&
		test_commit --no-tag c2 &&
		git update-ref refs/heads/release HEAD &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config --add maintenance.stratified.anchor refs/heads/release &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		# Drop refs/heads/release from config (but not from refs).
		git config --unset-all maintenance.stratified.anchor &&
		git config --add maintenance.stratified.anchor refs/heads/master &&

		# Stratify should warn about the orphan but leave the
		# sidecar in place — automatic demotion would amplify a
		# config typo. --no-quiet because maintenance defaults to
		# quiet when stderr is not a tty.
		git maintenance run --task=stratify --no-quiet 2>err &&
		test_grep "no longer configured" err &&
		test_grep "stratify-prune" err &&
		test 2 -eq $(count_sidecars)
	)
'

test_expect_success 'stratify-prune demotes orphan anchor packs' '
	test_create_repo orphan-prune &&
	(
		cd orphan-prune &&
		test_commit --no-tag c1 &&
		test_commit --no-tag c2 &&
		git update-ref refs/heads/release HEAD &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config --add maintenance.stratified.anchor refs/heads/release &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&
		before=$(count_packs) &&

		git config --unset-all maintenance.stratified.anchor &&
		git config --add maintenance.stratified.anchor refs/heads/master &&

		# stratify-prune demotes the orphan pack by unlinking
		# .base-stratum and .keep. The .pack stays on disk and is
		# absorbed by the next geometric repack. Self-contained
		# packs make this safe: no surviving anchor depends on
		# the orphan pack'\''s contents.
		git maintenance run --task=stratify-prune --no-quiet 2>err &&
		test_grep "demoting" err &&
		after=$(count_packs) &&
		test "$before" = "$after" &&
		test 1 -eq $(count_sidecars) &&
		extract_sidecar_refs >actual &&
		echo refs/heads/master >expect &&
		test_cmp expect actual &&
		test 1 -eq $(ls .git/objects/pack/*.keep 2>/dev/null | wc -l | tr -d " ")
	)
'

test_expect_success SANITY 'stratify-prune surfaces sidecar removal failure' '
	test_create_repo prune-fails &&
	(
		cd prune-fails &&
		test_commit --no-tag c1 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&

		# Orphan the only anchor so prune attempts to demote it.
		git config --unset-all maintenance.stratified.anchor &&

		# Make the pack directory non-writable so unlink() of the
		# .base-stratum sidecar fails. The task must report failure
		# instead of counting a phantom demotion, and the sidecar
		# must remain so the pack is still recognised as base-stratum.
		# Restore write permission unconditionally so trash cleanup
		# can remove the directory even if an assertion below fails.
		chmod a-w .git/objects/pack &&
		{
			test_must_fail git maintenance run \
				--task=stratify-prune --quiet 2>err
			status=$?
		} &&
		chmod u+w .git/objects/pack &&
		test "$status" -eq 0 &&

		test_grep "failed to remove" err &&
		test_grep "task .stratify-prune. failed" err &&
		test 1 -eq $(count_sidecars) &&
		test 1 -eq $(ls .git/objects/pack/*.keep 2>/dev/null |
			wc -l | tr -d " ")
	)
'

test_expect_success 'collector keeps corrupt-sidecar packs as boundaries' '
	test_create_repo prune-corrupt-keeps &&
	(
		cd prune-corrupt-keeps &&
		test_commit --no-tag c1 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&
		test 1 -eq $(ls .git/objects/pack/*.keep 2>/dev/null |
			wc -l | tr -d " ") &&

		# Corrupt the .base-stratum so load_pack_base_stratum() fails.
		# in_base_stratum is set from the sidecar merely existing, so
		# the pack is still a kept-pack boundary, but its anchor_ref is
		# no longer recoverable. Demoting it could strand a dependent
		# sibling, so the collector keeps it kept and warns instead of
		# unlinking its .keep. The sidecar is 0444, so make it writable
		# before truncating. The anchor stays configured, proving this
		# is the collector path, not the grouped-orphan path.
		sidecar=$(ls .git/objects/pack/*.base-stratum) &&
		chmod u+w "$sidecar" &&
		printf "xx" >"$sidecar" &&

		# It is the only base-stratum pack, so there is no other kept
		# pack covering its objects: it cannot be reclaimed as
		# redundant. stratify-prune keeps it and denounces it -- warns,
		# emits the corrupt-sidecar-kept metric, and exits non-zero.
		test_env GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			test_must_fail git maintenance run \
				--task=stratify-prune --no-quiet 2>err &&

		# The pack is kept: its .keep and (corrupt) .base-stratum both
		# survive, the boundary-retention warning is emitted, and the
		# denounce metric is logged.
		test_grep "keeping it as a base-stratum boundary" err &&
		grep "\"key\":\"corrupt-sidecar-kept\"" trace.txt &&
		test 1 -eq $(count_sidecars) &&
		test 1 -eq $(ls .git/objects/pack/*.keep 2>/dev/null |
			wc -l | tr -d " ")
	)
'

test_expect_success SANITY 'stratify surfaces validation demotion failure' '
	test_create_repo stratify-validate-fails &&
	(
		cd stratify-validate-fails &&
		test_commit --no-tag c1 &&
		c1_oid=$(git rev-parse HEAD) &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&

		# A second commit + run stratifies an incremental pack whose
		# anchor_commit is c2 (frontier ^c1).
		test_commit --no-tag c2 &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		# Rewind master to c1 so the c2-anchored pack is no longer an
		# ancestor of the tip: validation must demote it. Make the pack
		# directory non-writable so the sidecar unlink fails. The task
		# must report failure rather than exit 0 while the invalid
		# sidecar remains. Restore write permission unconditionally for
		# trash cleanup.
		git reset --hard "$c1_oid" &&
		chmod a-w .git/objects/pack &&
		{
			test_must_fail git maintenance run \
				--task=stratify --quiet 2>err
			status=$?
		} &&
		chmod u+w .git/objects/pack &&
		test "$status" -eq 0 &&

		test_grep "is not ancestor" err &&
		test_grep "failed to remove" err &&
		test_grep "task .stratify. failed" err &&
		test 2 -eq $(count_sidecars)
	)
'

test_expect_success 'demoted orphan retains its objects after surface-gc' '
	test_create_repo orphan-survives-surface-gc &&
	(
		cd orphan-survives-surface-gc &&
		setup_two_distinct_anchors &&
		git maintenance run --task=stratify --quiet &&

		# Record the release tip — release is about to be retired
		# but its commit must still be reachable from the demoted
		# pack after surface-gc.
		release_tip=$(git rev-parse refs/heads/release) &&

		git config --unset-all maintenance.stratified.anchor &&
		git config --add maintenance.stratified.anchor refs/heads/master &&

		git maintenance run --task=stratify-prune --quiet &&

		# Force expiration past now so cruft would be pruned
		# immediately if surface-gc misclassified anything.
		git config maintenance.stratified.min-age "now" &&
		git config maintenance.stratified.grace-period "now" &&
		git config maintenance.stratified.cruft-expiration "now" &&
		git maintenance run --task=surface-gc --quiet &&

		# release ref is still live (only config changed); its tip
		# commit must remain readable.
		git cat-file -e "$release_tip" &&
		git fsck --strict
	)
'

# Build "old history fully stratified, one recent surface commit left
# active" -- the caught-up shape surface-gc requires. Old commits use the
# incrementing test_tick dates (years old, so eligible and stratified);
# the surface commit is dated now, so it stays outside the frontier
# without making stratifying lag. test_commit accumulates files, so the
# surface commit's tree and parent reference c1/c2 objects that live only
# in the base-stratum pack -- exactly the objects the surface pack omits,
# forcing the bitmap to close across the boundary.
setup_surface_commit () {
	test_commit --no-tag c1 &&
	test_commit --no-tag c2 &&

	git config --add maintenance.stratified.anchor refs/heads/master &&
	git maintenance run --task=stratify --quiet &&
	test 1 -le $(count_sidecars) &&

	now=$(date +%s) &&
	recent="@$now +0000" &&
	echo surface >surface.t &&
	git add surface.t &&
	GIT_AUTHOR_DATE="$recent" GIT_COMMITTER_DATE="$recent" \
		git commit -q -m surface
}

test_expect_success 'surface-gc writes a closing MIDX bitmap when bitmaps are enabled' '
	test_create_repo surface-gc-bitmap &&
	(
		cd surface-gc-bitmap &&

		# Bitmaps on. This is what bare repositories get by default
		# for the all-into-one cruft repack surface-gc runs, and it
		# is the configuration that exposed the closure failure: the
		# surface pack omits every base-stratum object, so a
		# single-pack bitmap could not reach across the boundary.
		git config repack.writeBitmaps true &&

		setup_surface_commit &&

		git maintenance run --task=surface-gc --no-quiet 2>err &&

		# The bug manifested as a fatal bitmap-closure failure.
		test_grep ! "full closure" err &&
		test_grep ! "failed to write bitmap index" err &&

		# The closing artifact is a MIDX bitmap spanning the kept
		# base-stratum packs and the surface pack, not a per-pack
		# .bitmap (which could not close on its own).
		test 1 -eq $(ls .git/objects/pack/multi-pack-index-*.bitmap 2>/dev/null | wc -l) &&
		test 0 -eq $(ls .git/objects/pack/pack-*.bitmap 2>/dev/null | wc -l) &&

		# The bitmap must actually be usable and the repo intact.
		git rev-list --use-bitmap-index --count --all >/dev/null &&
		git fsck --strict
	)
'

test_expect_success 'surface-gc skips bitmaps when the environment does not want them' '
	test_create_repo surface-gc-nobitmap &&
	(
		cd surface-gc-nobitmap &&

		# Non-bare repo, no repack.writeBitmaps: bitmaps default
		# off, so surface-gc must not attempt (and fail) to write
		# one. A MIDX without a bitmap is still written and is fine.
		setup_surface_commit &&

		git maintenance run --task=surface-gc --no-quiet 2>err &&

		test_grep ! "failed to write bitmap index" err &&
		test 1 -eq $(ls .git/objects/pack/multi-pack-index 2>/dev/null | wc -l) &&
		test 0 -eq $(ls .git/objects/pack/multi-pack-index-*.bitmap 2>/dev/null | wc -l) &&
		git fsck --strict
	)
'

test_expect_success 'corrupt-sidecar ancestor pack stays a boundary; merge side survives surface-gc' '
	test_create_repo merge-corrupt-closure &&
	(
		cd merge-corrupt-closure &&

		# Merge DAG: c1, sibling branches left and right1, a merge
		# commit on master, then c3. With batch-size=1 each stratify
		# run packs exactly one commit, so the anchor group ends up
		# with five packs whose anchor_commits are c1, left, right1,
		# merge and c3. The merge pack depends on the left and right1
		# packs: left.t / right1.t are packed once, in their own
		# packs, and excluded from the merge pack.
		test_commit --no-tag c1 &&
		git branch right HEAD &&
		test_commit --no-tag left &&
		left_oid=$(git rev-parse HEAD) &&
		git checkout -q right &&
		test_commit --no-tag right1 &&
		git checkout -q master &&
		git merge --no-ff right -m merge &&
		test_commit --no-tag c3 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config maintenance.stratified.batch-size 1 &&
		for i in 1 2 3 4 5
		do
			git maintenance run --task=stratify --quiet || return 1
		done &&
		test 5 -eq $(count_sidecars) &&

		# Corrupt the sidecar whose anchor_commit is the left commit
		# so load_pack_base_stratum() fails. This is an ancestor pack
		# the merge pack depends on. in_base_stratum is set from the
		# sidecar merely existing, so the pack is still a kept-pack
		# boundary; its anchor_ref is no longer recoverable.
		left_sc=$(sidecar_for_anchor "$left_oid") &&
		test -n "$left_sc" &&
		left_keep="${left_sc%.base-stratum}.keep" &&
		test -f "$left_keep" &&
		chmod u+w "$left_sc" &&
		printf "xx" >"$left_sc" &&

		# A stratify run drives the collector (validate) over the
		# group -- where demotion would happen -- and is the realistic
		# predecessor of surface-gc in a geometric maintenance run.
		# Demoting the corrupt pack would unlink its .keep, dropping it
		# from the surface-gc kept-pack boundary while the dependent
		# merge pack stayed kept and leaving the merge left side outside
		# any kept pack. The collector must instead keep it: its .keep
		# (and corrupt sidecar) must survive and all five packs stay
		# kept boundaries. The kept corrupt pack is denounced, so the
		# task exits non-zero and logs the corrupt-sidecar-kept metric,
		# but every healthy anchor was still validated.
		test_env GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			test_must_fail git maintenance run \
				--task=stratify --quiet &&
		grep "\"key\":\"corrupt-sidecar-kept\"" trace.txt &&
		test -f "$left_keep" &&
		test 5 -eq $(count_sidecars) &&
		test 5 -eq $(ls .git/objects/pack/*.keep | wc -l | tr -d " ") &&

		# With the boundary intact, surface-gc keeps the merge left
		# side and the object store stays consistent.
		git config maintenance.stratified.min-age "now" &&
		git config maintenance.stratified.grace-period "now" &&
		git config maintenance.stratified.cruft-expiration "now" &&
		git maintenance run --task=surface-gc --quiet &&
		git cat-file -e "$left_oid:left.t" &&
		git fsck --strict
	)
'

test_expect_success 'stratify-prune keeps a corrupt pack that is an anchor'\''s only frontier' '
	test_create_repo guard-corrupt-frontier &&
	(
		cd guard-corrupt-frontier &&
		test_commit --no-tag c1 &&

		# Two anchors at the same commit each get a self-contained,
		# content-identical pack (cross-anchor objects are duplicated,
		# never shared). release is listed first so surface-gc evaluates
		# it before master, making the assertion below independent of
		# how master'\''s own readiness is judged.
		git update-ref refs/heads/release HEAD &&
		git config --add maintenance.stratified.anchor refs/heads/release &&
		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		# Corrupt the release anchor'\''s sidecar so load_pack_base_stratum()
		# fails. find_stratified_frontier() skips an unreadable sidecar,
		# so release immediately loses its frontier.
		sidecar=$(sidecar_for_ref refs/heads/release) &&
		test -n "$sidecar" &&
		chmod u+w "$sidecar" &&
		printf "xx" >"$sidecar" &&

		# release'\''s objects are still covered by master'\''s pack, so a
		# reachability-only reclaim would demote the corrupt copy --
		# but that silently strands release with no frontier of its
		# own. The guard refuses: it keeps and denounces the corrupt
		# pack (non-zero exit), logs corrupt-frontier-guarded, and does
		# NOT log corrupt-redundant-demoted.
		test_env GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			test_must_fail git maintenance run \
				--task=stratify-prune --no-quiet 2>err &&
		test_grep "no readable base-stratum frontier" err &&
		grep "\"key\":\"corrupt-frontier-guarded\"" trace.txt &&
		! grep "\"key\":\"corrupt-redundant-demoted\"" trace.txt &&
		test 2 -eq $(count_sidecars) &&
		test 2 -eq $(ls .git/objects/pack/*.keep | wc -l | tr -d " ") &&

		# Readiness is anchor-scoped: surface-gc surfaces release as
		# having no frontier rather than crediting master'\''s coverage.
		# release comes first in config order, so the skip names it.
		git config maintenance.stratified.min-age "now" &&
		git config maintenance.stratified.grace-period "now" &&
		git maintenance run --task=surface-gc --no-quiet 2>sgc_err &&
		test_grep "anchor .refs/heads/release. has no stratified commits yet" sgc_err &&
		git fsck --strict
	)
'

test_expect_success 'stratify-prune reclaims a corrupt pack while its anchor keeps a loadable one' '
	test_create_repo reclaim-surplus &&
	(
		cd reclaim-surplus &&
		test_commit --no-tag c1 &&
		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&

		# Manufacture a surplus base-stratum pack: duplicate the anchor
		# pack (a content-identical copy) and corrupt the copy'\''s
		# sidecar. master still has its original loadable pack, so the
		# copy is a genuine surplus -- redundant against a pack that
		# stays -- not the anchor'\''s only frontier. The guard stands
		# down and the corrupt copy is reclaimable.
		base=$(ls .git/objects/pack/*.base-stratum) &&
		b=${base%.base-stratum} &&
		cp "$b.pack" "$b-dup.pack" &&
		cp "$b.idx" "$b-dup.idx" &&
		cp "$b.base-stratum" "$b-dup.base-stratum" &&
		cp "$b.keep" "$b-dup.keep" &&
		chmod u+w "$b-dup.base-stratum" &&
		printf "xx" >"$b-dup.base-stratum" &&
		test 2 -eq $(count_sidecars) &&

		# Every configured anchor keeps a loadable frontier, so prune
		# reclaims the redundant corrupt copy (sidecar + .keep gone),
		# logs corrupt-redundant-demoted (not corrupt-frontier-guarded),
		# and exits zero.
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git maintenance run --task=stratify-prune --no-quiet 2>err &&
		test_grep "reclaiming redundant" err &&
		grep "\"key\":\"corrupt-redundant-demoted\"" trace.txt &&
		! grep "\"key\":\"corrupt-frontier-guarded\"" trace.txt &&
		test 1 -eq $(count_sidecars) &&
		test 1 -eq $(ls .git/objects/pack/*.keep | wc -l | tr -d " ") &&

		# master kept its frontier, so surface-gc stays caught up and
		# the object store is consistent.
		git config maintenance.stratified.min-age "now" &&
		git config maintenance.stratified.grace-period "now" &&
		git maintenance run --task=surface-gc --no-quiet 2>sgc_err &&
		! grep "no stratified commits yet" sgc_err &&
		git fsck --strict
	)
'

test_expect_success 'stratify-prune retires stratification when the last pack has a corrupt sidecar' '
	test_create_repo wind-down-corrupt &&
	(
		cd wind-down-corrupt &&
		test_commit --no-tag c1 &&
		c1_oid=$(git rev-parse HEAD) &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&
		test 1 -eq $(ls .git/objects/pack/*.keep | wc -l | tr -d " ") &&

		# Corrupt the only base-stratum sidecar so
		# load_pack_base_stratum() fails. in_base_stratum stays set
		# (the sidecar still exists), so the collector keeps the pack
		# and the orphan sweep cannot touch it.
		sidecar=$(ls .git/objects/pack/*.base-stratum) &&
		chmod u+w "$sidecar" &&
		printf "xx" >"$sidecar" &&

		# Wind stratification down via the documented path: unset every
		# anchor, then run stratify-prune. With no anchor to strand and
		# demotion losing no objects (only the sidecars are unlinked),
		# the corrupt pack must be demoted unconditionally -- not kept
		# forever for lack of a holder. The task logs corrupt-orphan-
		# demoted (not corrupt-sidecar-kept) and exits zero.
		git config --unset-all maintenance.stratified.anchor &&
		GIT_TRACE2_EVENT="$(pwd)/trace.txt" \
			git maintenance run --task=stratify-prune --no-quiet 2>err &&
		test_grep "no anchors configured" err &&
		grep "\"key\":\"corrupt-orphan-demoted\"" trace.txt &&
		! grep "\"key\":\"corrupt-sidecar-kept\"" trace.txt &&

		# Both sidecars are gone: stratification is fully retired and a
		# future run no longer treats the pack as pinned base-stratum.
		test 0 -eq $(count_sidecars) &&
		test 0 -eq $(ls .git/objects/pack/*.keep 2>/dev/null |
			wc -l | tr -d " ") &&

		# Demotion never deletes objects -- the .pack stays on disk.
		git cat-file -e "$c1_oid" &&
		git fsck --strict
	)
'

test_expect_success 'stratify-prune leaves configured anchors alone' '
	test_create_repo prune-noop &&
	(
		cd prune-noop &&
		test_commit --no-tag c1 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&

		git maintenance run --task=stratify-prune --quiet &&
		test 1 -eq $(count_sidecars)
	)
'

test_expect_success 'stratify-prune is never scheduled and runs only when selected' '
	test_create_repo prune-schedule &&
	(
		cd prune-schedule &&
		test_commit --no-tag c1 &&

		git config maintenance.strategy geometric &&
		git config --add maintenance.stratified.anchor refs/heads/master &&

		# stratify-prune demotes packs, so it is deliberately absent
		# from every strategy: a misconfigured anchor list must not let
		# a scheduled or whole-strategy run retire the base stratum.
		GIT_TRACE2_EVENT="$(pwd)/daily.txt" \
			git maintenance run --schedule=daily --no-quiet &&
		! grep "\"label\":\"stratify-prune\"" daily.txt &&

		GIT_TRACE2_EVENT="$(pwd)/weekly.txt" \
			git maintenance run --schedule=weekly --no-quiet &&
		! grep "\"label\":\"stratify-prune\"" weekly.txt &&

		# A bare "git maintenance run" (manual strategy) skips it too.
		GIT_TRACE2_EVENT="$(pwd)/manual.txt" \
			git maintenance run --no-quiet &&
		! grep "\"label\":\"stratify-prune\"" manual.txt &&

		# It still runs when the user selects it explicitly.
		GIT_TRACE2_EVENT="$(pwd)/explicit.txt" \
			git maintenance run --task=stratify-prune --no-quiet &&
		grep "\"label\":\"stratify-prune\"" explicit.txt
	)
'

test_expect_success 'maintenance.stratify-prune.enabled opts the task back into manual runs' '
	test_create_repo prune-opt-in &&
	(
		cd prune-opt-in &&
		test_commit --no-tag c1 &&

		git config maintenance.strategy geometric &&
		git config --add maintenance.stratified.anchor refs/heads/master &&

		# Default: a bare run does not include stratify-prune.
		GIT_TRACE2_EVENT="$(pwd)/off.txt" \
			git maintenance run --no-quiet &&
		! grep "\"label\":\"stratify-prune\"" off.txt &&

		# Explicit per-repo opt-in re-enables it for manual runs.
		git config maintenance.stratify-prune.enabled true &&
		GIT_TRACE2_EVENT="$(pwd)/on.txt" \
			git maintenance run --no-quiet &&
		grep "\"label\":\"stratify-prune\"" on.txt
	)
'

test_expect_success 'stratify-prune demotes all packs with no configured anchors' '
	test_create_repo prune-empty &&
	(
		cd prune-empty &&
		test_commit --no-tag c1 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&
		before=$(count_packs) &&

		git config --unset-all maintenance.stratified.anchor &&

		# Unsetting maintenance.stratified.anchor empties the
		# configured set, so every existing base-stratum pack is
		# an orphan. stratify-prune demotes them all; the .pack
		# files stay on disk and the next geometric repack folds
		# them into the active stratum.
		git maintenance run --task=stratify-prune --no-quiet 2>err &&
		test_grep "demoting" err &&
		test 0 -eq $(count_sidecars) &&
		after=$(count_packs) &&
		test "$before" = "$after"
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
		# validate first. Because closure is a whole-group property,
		# finding P2 invalid demotes the ENTIRE anchor group;
		# consolidate-stratum does not rebuild, so no base-stratum
		# pack remains (the objects stay on disk in the demoted packs
		# and the next stratify run rebuilds the stratum). Either way
		# the rewound c2 state is never resurrected under a merged pack.
		git maintenance run --task=consolidate-stratum --no-quiet 2>err &&
		test_grep "closed-set invariant" err &&
		test 0 -eq $(count_sidecars)
	)
'

test_expect_success 'standalone consolidate-stratum skips with no configured anchors and points at stratify-prune' '
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
		# happened to win the group sort. consolidate-stratum
		# skips and points at stratify-prune, which is the
		# explicit tool for retiring orphan packs.
		git maintenance run --task=consolidate-stratum --no-quiet 2>err &&
		test_grep "stratify-prune" err &&
		test 1 -eq $(count_sidecars)
	)
'

test_expect_success 'surface-gc --dry-run reports lagging without modifying the repo' '
	test_create_repo dry-run-lagging &&
	(
		cd dry-run-lagging &&

		test_commit --no-tag c1 &&
		test_commit --no-tag c2 &&
		test_commit --no-tag c3 &&
		test_commit --no-tag c4 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config maintenance.stratified.batch-size 1 &&

		# One batch-size=1 run stratifies only the oldest commit, so
		# old commits remain outside the frontier: lagging.
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&

		ls .git/objects/pack >before &&
		git maintenance run --task=surface-gc --dry-run >out &&
		ls .git/objects/pack >after &&

		# The report names the lagging anchor with a lag distance and
		# frontier width, and concludes surface-gc would be skipped.
		test_grep "refs/heads/master: lagging by .* commit" out &&
		test_grep "frontier width" out &&
		test_grep "surface-gc would be skipped" out &&

		# A dry run must never touch the object store.
		test_cmp before after
	)
'

test_expect_success 'surface-gc --dry-run reports caught up once fully stratified' '
	test_create_repo dry-run-caught-up &&
	(
		cd dry-run-caught-up &&

		test_commit --no-tag c1 &&
		test_commit --no-tag c2 &&
		test_commit --no-tag c3 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&

		# Unbounded batch: the whole eligible history is stratified in
		# one run, so nothing old remains outside the frontier.
		git maintenance run --task=stratify --quiet &&

		ls .git/objects/pack >before &&
		git maintenance run --task=surface-gc --dry-run >out &&
		ls .git/objects/pack >after &&

		test_grep "refs/heads/master: caught up (frontier width 1)" out &&
		test_grep "surface-gc would run" out &&
		test_cmp before after
	)
'

test_expect_success 'surface-gc --dry-run reports anchors with no stratified commits yet' '
	test_create_repo dry-run-no-frontier &&
	(
		cd dry-run-no-frontier &&
		test_commit --no-tag c1 &&
		git config --add maintenance.stratified.anchor refs/heads/master &&

		# No stratify run yet: the anchor has no base-stratum pack.
		git maintenance run --task=surface-gc --dry-run >out &&
		test_grep "refs/heads/master: no stratified commits yet" out &&
		test_grep "surface-gc would be skipped" out
	)
'

test_expect_success '--dry-run is rejected for tasks other than surface-gc' '
	test_create_repo dry-run-guardrail &&
	(
		cd dry-run-guardrail &&
		test_commit --no-tag c1 &&
		git config --add maintenance.stratified.anchor refs/heads/master &&

		test_must_fail git maintenance run --task=gc --dry-run 2>err &&
		test_grep "only supported with --task=surface-gc" err &&

		test_must_fail git maintenance run --dry-run 2>err2 &&
		test_grep "only supported with --task=surface-gc" err2 &&

		test_must_fail git maintenance run --task=surface-gc --dry-run --auto 2>err3 &&
		test_grep "dry-run" err3
	)
'

test_done

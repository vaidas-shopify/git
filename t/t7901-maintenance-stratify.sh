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
			my $ref_start = 12 + $rawsz + 4;
			my $ref_end = index($buf, "\0", $ref_start);
			print substr($buf, $ref_start, $ref_end - $ref_start), "\n";
		}
	EOF
}

# Print the anchor_commit OID recorded in the sidecar at $1.
extract_sidecar_anchor () {
	perl - "$1" <<-\EOF
		my $path = $ARGV[0];
		open my $fh, "<", $path or die "open $path: $!";
		binmode $fh;
		my $buf;
		read $fh, $buf, -s $path;
		my $hash_id = unpack("N", substr($buf, 8, 4));
		my $rawsz = ($hash_id == 1) ? 20 : 32;
		print unpack("H*", substr($buf, 12, $rawsz)), "\n";
	EOF
}

# Configure two anchors that point at sibling commits — they share a
# common base (c1) but each has its own unique commit beyond it. This
# avoids both OID-level dedup (which collapses same-OID anchors) and
# the cross-anchor filter eating one anchor's pack entirely (which
# happens when one anchor is a strict ancestor of the other). Each
# anchor still gets its own pack with anchor-unique objects only;
# the shared base lands in whichever pack is written first.
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
	substr($buf, 12 + $rawsz, 4) = pack("N", $new_ts);
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

test_expect_success 'two anchors at same commit are deduped to one pack' '
	test_create_repo two-anchor-same-commit &&
	(
		cd two-anchor-same-commit &&
		test_commit --no-tag c1 &&
		test_commit --no-tag c2 &&

		# Two refs pointing at the same commit.
		git update-ref refs/heads/release HEAD &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config --add maintenance.stratified.anchor refs/heads/release &&

		git maintenance run --task=stratify --no-quiet 2>err &&

		# Only one pack is written; the second anchor is skipped
		# because its tip OID was already stratified by the first.
		test 1 -eq $(count_sidecars) &&
		test 1 -eq $(count_packs) &&
		test_grep "already stratified by another anchor" err &&

		# The surviving sidecar records the first-listed anchor.
		extract_sidecar_refs >actual &&
		echo refs/heads/master >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'cross-anchor filter dedupes shared history' '
	test_create_repo cross-anchor-shared &&
	(
		cd cross-anchor-shared &&

		# Create two anchors with overlapping history. release
		# is a strict ancestor of master: release at c1, master
		# at c2 (child of c1). Without the cross-anchor filter,
		# both packs would carry c1'\''s objects. With it, the
		# anchor processed second skips them entirely.
		test_commit --no-tag c1 &&
		git branch release HEAD &&
		test_commit --no-tag c2 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git config --add maintenance.stratified.anchor refs/heads/release &&

		git maintenance run --task=stratify --no-quiet 2>err &&

		# Master is processed first (config order) and writes a
		# full pack covering c1+c2. Release sees its rev-list
		# output (c1 only) entirely filtered against the just-
		# written master pack and produces no pack of its own.
		# The trace2 stream records the full skip on the
		# already-packed objects line.
		test 1 -eq $(count_sidecars) &&
		extract_sidecar_refs >actual &&
		echo refs/heads/master >expect &&
		test_cmp expect actual &&
		test_grep "objects already in base-stratum packs" err &&

		# Surface-gc readiness for release must still report
		# release as caught up — master'\''s pack covers all of
		# release'\''s reachables (master is a descendant of
		# release'\''s tip). Without the descendant fallback in
		# stratified_frontier_date(), readiness would say
		# "no stratified commits yet" and surface-gc would skip
		# indefinitely.
		git config maintenance.stratified.min-age "now" &&
		git config maintenance.stratified.grace-period "now" &&
		git maintenance run --task=surface-gc --no-quiet 2>err2 &&
		! grep "no stratified commits yet" err2
	)
'

test_expect_success 'distinct anchors get distinct anchor-scoped pack files' '
	test_create_repo two-anchor-distinct &&
	(
		cd two-anchor-distinct &&
		setup_two_distinct_anchors &&

		git maintenance run --task=stratify --quiet &&

		# Each anchor at a different OID produces its own pack and
		# sidecar; pack basenames are anchor-scoped so even if the
		# packs ever produced identical content the filenames
		# would not collide.
		test 2 -eq $(count_sidecars) &&
		test 2 -eq $(count_packs) &&

		extract_sidecar_refs >actual &&
		printf "refs/heads/master\nrefs/heads/release\n" >expect &&
		test_cmp expect actual
	)
'

test_expect_success 'second stratify run with distinct anchors is a no-op' '
	test_create_repo two-anchor-incremental &&
	(
		cd two-anchor-incremental &&
		setup_two_distinct_anchors &&

		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		# Re-run with no new commits should not add packs or
		# rewrite sidecars.
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
		c1_oid=$(git rev-parse HEAD) &&
		test_commit --no-tag c2 &&
		test_commit --no-tag c3 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&

		# batch-size=4 trips while processing c2 (commit line is
		# the 4th object) and truncation happens when we reach
		# c3. Only c1 and its tree/blob make it into the pack;
		# the anchor must record c1 (the last fully-included
		# commit), not c2.
		git config maintenance.stratified.batch-size 4 &&
		git maintenance run --task=stratify --quiet &&

		test 1 -eq $(count_sidecars) &&
		sc=$(ls .git/objects/pack/*.base-stratum) &&
		extract_sidecar_anchor "$sc" >actual &&
		echo "$c1_oid" >expect &&
		test_cmp expect actual &&

		# A follow-up run with no batch limit must pick up where
		# the previous one left off and stratify the rest. With
		# the buggy frontier, ^c2 would have permanently hidden
		# c2 from the next walk.
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

test_expect_success 'orphan anchor is reported but not demoted by stratify' '
	test_create_repo orphan-warns &&
	(
		cd orphan-warns &&
		setup_two_distinct_anchors &&
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
		setup_two_distinct_anchors &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		git config --unset-all maintenance.stratified.anchor &&
		git config --add maintenance.stratified.anchor refs/heads/master &&

		# Pack count is unchanged; only the .base-stratum and .keep
		# sidecars for the orphaned anchor go away.
		before=$(count_packs) &&
		git maintenance run --task=stratify-prune --quiet &&
		after=$(count_packs) &&
		test "$before" = "$after" &&
		test 1 -eq $(count_sidecars) &&

		# The remaining sidecar belongs to the still-configured anchor.
		extract_sidecar_refs >actual &&
		echo refs/heads/master >expect &&
		test_cmp expect actual
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

test_expect_success 'stratify-prune is wired into --schedule=weekly' '
	test_create_repo prune-schedule &&
	(
		cd prune-schedule &&
		test_commit --no-tag c1 &&

		git config maintenance.strategy geometric &&
		git config --add maintenance.stratified.anchor refs/heads/master &&

		# Without --schedule= the task only runs when explicitly
		# selected; with --schedule=daily it should be skipped (it
		# is a weekly task); with --schedule=weekly it should run.
		GIT_TRACE2_EVENT="$(pwd)/daily.txt" \
			git maintenance run --schedule=daily --no-quiet &&
		! grep "\"label\":\"stratify-prune\"" daily.txt &&

		GIT_TRACE2_EVENT="$(pwd)/weekly.txt" \
			git maintenance run --schedule=weekly --no-quiet &&
		grep "\"label\":\"stratify-prune\"" weekly.txt
	)
'

test_expect_success 'stratify-prune refuses to run with no configured anchors' '
	test_create_repo prune-empty &&
	(
		cd prune-empty &&
		test_commit --no-tag c1 &&

		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 1 -eq $(count_sidecars) &&

		git config --unset-all maintenance.stratified.anchor &&

		# Without anchors, every existing pack would be an orphan.
		# The task must refuse rather than wipe state silently.
		git maintenance run --task=stratify-prune --no-quiet 2>err &&
		test_grep "refusing to run" err &&
		test 1 -eq $(count_sidecars)
	)
'

test_done

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

test_expect_success 'stratify-prune relabels orphan anchor packs to a surviving anchor' '
	test_create_repo orphan-prune &&
	(
		cd orphan-prune &&
		setup_two_distinct_anchors &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		git config --unset-all maintenance.stratified.anchor &&
		git config --add maintenance.stratified.anchor refs/heads/master &&

		# Pack and sidecar counts are both unchanged: stratify-prune
		# rewrites the orphan pack'\''s .base-stratum sidecar so it
		# claims the surviving anchor (master). Dropping the sidecar
		# outright would break the closed-set property under
		# cross-anchor dedup — the orphan pack may exclusively hold
		# objects that the surviving anchor'\''s pack references as
		# ancestors.
		before=$(count_packs) &&
		git maintenance run --task=stratify-prune --no-quiet 2>err &&
		test_grep "relabeled" err &&
		after=$(count_packs) &&
		test "$before" = "$after" &&
		test 2 -eq $(count_sidecars) &&

		# Both sidecars now belong to the surviving anchor.
		extract_sidecar_refs >actual &&
		printf "refs/heads/master\nrefs/heads/master\n" >expect &&
		test_cmp expect actual &&

		# The orphan pack'\''s original anchor_commit (release tip r1)
		# is not on master'\''s history — they are siblings of c1. To
		# satisfy the next stratify validation pass, the relabel
		# substitutes the merge-base (c1) as the new anchor_commit.
		# The two sidecars therefore record c1 (relabeled) and c2
		# (master'\''s own pack).
		>anchors &&
		for sidecar in .git/objects/pack/*.base-stratum
		do
			extract_sidecar_anchor "$sidecar" >>anchors || return 1
		done &&
		sort -u anchors >anchors.uniq &&
		git rev-parse refs/heads/master^ refs/heads/master | sort -u >expect &&
		test_cmp expect anchors.uniq &&

		# A follow-up stratify validation pass must accept both
		# sidecars: anchor_commit c1 is on master'\''s history, and
		# anchor_commit c2 is master itself.
		git maintenance run --task=stratify --no-quiet 2>err2 &&
		! grep "not ancestor" err2 &&
		test 2 -eq $(count_sidecars)
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

test_expect_success 'shared-history anchors survive donor demotion and surface-gc' '
	test_create_repo cross-anchor-survive &&
	(
		cd cross-anchor-survive &&
		setup_two_distinct_anchors &&

		# Cross-anchor dedup: master is processed first (config
		# order) and packs c1+c2; release packs r1 only (c1 is
		# already covered by master'\''s pack and gets filtered).
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		# c1 is the shared base. It is reachable from release via
		# r1'\''s parent pointer but lives only in master'\''s pack.
		c1=$(git rev-parse refs/heads/release^) &&

		# Retire master entirely: drop config AND delete the ref.
		# This is the configuration where the corruption window
		# opens — with master'\''s ref still alive, surface-gc'\''s
		# walk would reach c1 through master and mark it reachable
		# even with --kept-pack-boundary.
		git config --unset-all maintenance.stratified.anchor refs/heads/master &&
		git update-ref -d refs/heads/master &&

		# Real-world parity: in production the corruption window
		# opens once reflog entries that pinned the deleted branch
		# expire. release was branched from master at c1, so
		# release@{1} still pins c1 right after the ref deletion;
		# expiring all reflog entries now collapses what would
		# otherwise be a multi-week timeline into one test step.
		git reflog expire --expire=now --expire-unreachable=now --all &&

		# stratify-prune must relabel master'\''s pack rather than
		# drop its sidecars; otherwise the closed-set property
		# breaks for release'\''s pack.
		git maintenance run --task=stratify-prune --no-quiet 2>err &&
		test_grep "relabeled" err &&
		extract_sidecar_refs >actual &&
		printf "refs/heads/release\nrefs/heads/release\n" >expect &&
		test_cmp expect actual &&

		# master'\''s original anchor_commit (c2) is not on release'\''s
		# history. The relabel must substitute the merge-base (c1)
		# so the sidecar passes validation: every recorded
		# anchor_commit must be an ancestor of the recorded
		# anchor_ref'\''s tip.
		>anchors &&
		for sidecar in .git/objects/pack/*.base-stratum
		do
			extract_sidecar_anchor "$sidecar" >>anchors || return 1
		done &&
		sort -u anchors >anchors.uniq &&
		git rev-parse refs/heads/release refs/heads/release^ |
			sort -u >expect &&
		test_cmp expect anchors.uniq &&

		# surface-gc'\''s readiness check compares the stratify
		# frontier against (now - min-age - grace-period); against
		# the suite'\''s 1970-dated commits the default cutoff is
		# always lagging. min-age=@1 with grace=now puts the
		# cutoff exactly at the frontier so the cruft repack
		# actually runs.
		git config maintenance.stratified.min-age "@1 +0000" &&
		git config maintenance.stratified.grace-period now &&

		# Force any newly-cruft objects to expire in the same
		# invocation: cruft-expiration cuts off mtimes "older than
		# X", and "tomorrow" is past now, so any object that
		# surface-gc moves into a cruft pack in this run is
		# immediately pruned. Without the relabel fix, c1 ends up
		# in that cruft pack and disappears here.
		git -c maintenance.stratified.cruft-expiration=tomorrow \
			maintenance run --task=surface-gc --quiet &&

		# c1 must still resolve, and the repository must remain
		# self-consistent.
		git cat-file -e $c1 &&
		git fsck --strict
	)
'

# Pick a relabel target per pack, not once per run keyed by config order.
# With multiple surviving anchors of which only one is reachable from the
# orphan'\''s anchor_commit, the per-pack target must be the compatible
# one — regardless of which surviving anchor is listed first.
test_expect_success 'stratify-prune target selection is independent of anchor config order' '
	for order in main_first other_first
	do
		test_when_finished "rm -rf prune-order-$order" &&
		test_create_repo prune-order-$order &&
		(
			cd prune-order-$order &&

			# main: c1 → c2, shares c1 with feature.
			# feature: c1 → f1 (sibling of c2 from c1).
			# other: independent root, u1 → u2.
			test_commit --no-tag c1 &&
			git branch feature HEAD &&
			git checkout -q --orphan other-root &&
			git rm -rf . &&
			test_commit --no-tag u1 &&
			test_commit --no-tag u2 &&
			git branch -M other &&
			git checkout -q master &&
			test_commit --no-tag c2 &&
			git checkout -q feature &&
			test_commit --no-tag f1 &&
			git checkout -q master &&

			git config --add maintenance.stratified.anchor refs/heads/master &&
			git config --add maintenance.stratified.anchor refs/heads/feature &&
			git config --add maintenance.stratified.anchor refs/heads/other &&
			git maintenance run --task=stratify --quiet &&
			test 3 -eq $(count_sidecars) &&

			# Retire feature; keep master and other configured. Try
			# both orders: only master shares any history with f1
			# (via merge-base c1); other has an independent root and
			# no common ancestor at all. Both orders must pick
			# master.
			git config --unset-all maintenance.stratified.anchor &&
			case "$order" in
			main_first)
				git config --add maintenance.stratified.anchor refs/heads/master &&
				git config --add maintenance.stratified.anchor refs/heads/other
				;;
			other_first)
				git config --add maintenance.stratified.anchor refs/heads/other &&
				git config --add maintenance.stratified.anchor refs/heads/master
				;;
			esac &&

			git maintenance run --task=stratify-prune --no-quiet 2>err &&
			test_grep "relabeled" err &&
			test_grep "to surviving anchor .refs/heads/master." err &&
			! test_grep "to surviving anchor .refs/heads/other." err &&

			# A follow-up stratify validation pass must not demote
			# the relabeled pack: its substituted anchor_commit must
			# be on master'\''s history.
			git maintenance run --task=stratify --no-quiet 2>err2 &&
			! grep "not ancestor" err2 &&
			test 3 -eq $(count_sidecars)
		) || return 1
	done
'

# Cross-anchor dedup operates at the OID level (find_pack_entry_one
# lookups in filter_already_packed_oids), not by commit ancestry. Two
# anchors on independent roots can therefore share tree/blob OIDs — the
# empty tree, an identical LICENSE blob, etc. — and the later anchor'\''s
# pack legitimately omits objects covered by the earlier one. Demoting
# the earlier pack when its anchor is retired would break the closed
# set the same way unlinking the .keep does in the shared-history case,
# even though no merge-base exists. stratify-prune must instead borrow
# a surviving anchor'\''s own recorded anchor_commit so the orphan stays
# kept.
test_expect_success 'no-merge-base orphan is relabeled, not demoted' '
	test_create_repo no-merge-base-survive &&
	(
		cd no-merge-base-survive &&

		# unrelated: orphan root with shared.txt only.
		# master: own root with shared.txt + master.txt, then m1.
		# The shared.txt blob has identical content in both
		# histories, so its OID is identical too. master and
		# unrelated have no common commit ancestor.
		test_commit --no-tag master-root master.txt master &&
		echo "shared content" >shared.txt &&
		git add shared.txt &&
		git -c user.email=t@t -c user.name=t commit -m "add shared" &&
		test_commit --no-tag m1 m1.txt m1 &&

		git checkout -q --orphan unrelated-root &&
		git rm -rf . &&
		echo "shared content" >shared.txt &&
		git add shared.txt &&
		git -c user.email=t@t -c user.name=t commit -m "unrelated: add shared" &&
		git branch -M unrelated &&
		git checkout -q master &&

		# Sanity: master and unrelated really have no merge-base,
		# and the shared.txt blob has the same OID in both.
		test_must_fail git merge-base refs/heads/master \
				refs/heads/unrelated &&
		shared_blob=$(git rev-parse refs/heads/master:shared.txt) &&
		test "$shared_blob" = \
			$(git rev-parse refs/heads/unrelated:shared.txt) &&

		# Stratify with unrelated FIRST so its pack absorbs the
		# shared.txt blob; master is processed next and its pack
		# filters that blob out via cross-anchor OID dedup.
		git config --add maintenance.stratified.anchor refs/heads/unrelated &&
		git config --add maintenance.stratified.anchor refs/heads/master &&
		git maintenance run --task=stratify --quiet &&
		test 2 -eq $(count_sidecars) &&

		# Confirm master'\''s pack actually omits shared_blob (it
		# now depends on unrelated'\''s pack via OID dedup).
		master_pack=$(grep -l "refs/heads/master" \
			.git/objects/pack/*.base-stratum |
			sed "s/\\.base-stratum$/.idx/") &&
		! git verify-pack -v "$master_pack" |
			grep -q "^$shared_blob " &&

		# Retire unrelated entirely: drop config AND delete the
		# ref. The orphan pack'\''s anchor_commit has no merge-base
		# with master'\''s tip, so the merge-base strategies fail
		# and only the survivor-anchor borrow can preserve the
		# closed set.
		git config --unset-all maintenance.stratified.anchor refs/heads/unrelated &&
		git update-ref -d refs/heads/unrelated &&
		git reflog expire --expire=now --expire-unreachable=now --all &&

		git maintenance run --task=stratify-prune --no-quiet 2>err &&
		test_grep "relabeled" err &&
		! test_grep "demoting" err &&

		# Both sidecars now name refs/heads/master.
		extract_sidecar_refs >actual &&
		printf "refs/heads/master\nrefs/heads/master\n" >expect &&
		test_cmp expect actual &&

		# The borrowed anchor_commit must equal master'\''s own
		# recorded anchor_commit so validation passes on the next
		# stratify run.
		>anchors &&
		for sidecar in .git/objects/pack/*.base-stratum
		do
			extract_sidecar_anchor "$sidecar" >>anchors || return 1
		done &&
		sort -u anchors >anchors.uniq &&
		test_line_count = 1 anchors.uniq &&

		git maintenance run --task=stratify --no-quiet 2>err2 &&
		! grep "not ancestor" err2 &&
		test 2 -eq $(count_sidecars) &&

		# surface-gc'\''s readiness check needs the frontier to
		# clear (now - min-age - grace-period); the 1970-dated
		# commits in the test suite always lag the default, so
		# move the cutoff to the frontier.
		git config maintenance.stratified.min-age "@1 +0000" &&
		git config maintenance.stratified.grace-period now &&

		# cruft-expiration=tomorrow forces anything classified as
		# cruft in this run to be pruned immediately, so a missing
		# relabel surfaces as a vanished shared_blob rather than a
		# delayed pruning weeks later.
		git -c maintenance.stratified.cruft-expiration=tomorrow \
			maintenance run --task=surface-gc --quiet &&

		git cat-file -e $shared_blob &&
		git fsck --strict
	)
'

test_done

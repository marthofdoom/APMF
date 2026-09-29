#!/usr/bin/env bash
# Cut an APMF (Harbinger) release. Builds are archived under releases/vX.Y.Z/
# PERMANENTLY.
#
# Usage (also ./release.sh --help):
#   ./release.sh 0.9.5              PHASE 1: stamp + commit + push, then wait for CI
#   ./release.sh --run <run-id>     PHASE 2: verify + package + tag the stamped
#                                   version, taking the DLL from exactly that green
#                                   'native' CI run
#   ./release.sh --run <run-id> --dry-run
#                                   phase 2's checks only (run, artifact); writes,
#                                   packages and tags nothing
#   ./release.sh                    lists the last green 'native' runs and stops
#
# Release folders and tags are IMMUTABLE. Bump the version for every build you
# want to keep. Nothing is ever overwritten or deleted. This is the ONLY way a
# build reaches the game: a hand-copied DLL is how the running game and the
# archive end up disagreeing about what is live, and that cost MRO a session.
#
# The DLL comes from a GREEN CI run the operator names with --run, never a
# local build. There is no local MSVC by design, so CI is the only compiler that
# ever sees this code. The run is never guessed: with parallel branch builds
# "the newest green run" is often another branch's DLL.
set -euo pipefail
cd "$(dirname "$0")"

GH="${GH:-$HOME/.local/bin/gh}"
ARTIFACT="APMF-dll"
DLL_NAME="APMF.dll"

usage() {
    cat <<EOF
Usage:
  ./release.sh <X.Y.Z>                   PHASE 1: stamp native/CMakeLists.txt (and name the
                                         MinReleaseForAbi placeholder), commit, push the
                                         current branch. Wait for CI.
  ./release.sh --run <run-id>            PHASE 2: verify, package and tag the stamped version,
                                         with the DLL from that 'native' CI run.
  ./release.sh --run <run-id> --dry-run  PHASE 2 checks only. Nothing is written or tagged.
  ./release.sh                           list the last green 'native' runs, then stop.
  ./release.sh --help                    this text.

--run <run-id> (or --run=<run-id>) is REQUIRED for phase 2. The run must be a
'native' workflow run that completed with 'success', whose commit has the SAME
native/ tree as HEAD, and that still holds the ${ARTIFACT} artifact. Anything
else stops the release. Pick the id from the list printed by a bare ./release.sh
(or: gh run list --workflow=native --status=success).
EOF
}

RUN_SEL=""
DRY_RUN=0
POS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run)     [[ $# -ge 2 && -n "$2" ]] || { echo "ERROR: --run needs a run id." >&2; exit 1; }
                   RUN_SEL="$2"; shift 2 ;;
        --run=*)   RUN_SEL="${1#--run=}"; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*)        echo "ERROR: unknown option '$1'." >&2; usage >&2; exit 1 ;;
        *)         POS+=("$1"); shift ;;
    esac
done
if [[ ${#POS[@]} -gt 1 ]]; then
    echo "ERROR: too many arguments: ${POS[*]}" >&2; usage >&2; exit 1
fi
if [[ ${#POS[@]} -eq 1 && ( -n "$RUN_SEL" || "$DRY_RUN" -eq 1 ) ]]; then
    echo "ERROR: --run / --dry-run belong to phase 2; phase 1 (./release.sh ${POS[0]}) takes no options." >&2
    exit 1
fi

# Print the last green 'native' runs so the operator can pick one for --run.
# The mark says which of them built the native/ tree HEAD has.
list_green_runs() {
    local head_tree
    head_tree="$(git rev-parse HEAD:native)"
    echo "Last green 'native' runs, newest first (HEAD $(git rev-parse --short HEAD), native tree ${head_tree:0:8}):" >&2
    printf '  %-12s %-9s %-20s  %-40s %s\n' RUN SHA CREATED BRANCH "" >&2
    $GH run list --workflow=native --status=success --limit 8 \
        --json databaseId,headBranch,headSha,createdAt \
        -q '.[] | [(.databaseId|tostring), .headSha, .createdAt, .headBranch] | join("|")' |
    while IFS='|' read -r id sha created branch; do
        local tree mark=""
        tree="$(git rev-parse -q --verify "${sha}:native" 2>/dev/null || true)"
        if [[ -z "$tree" ]]; then
            mark="(commit not fetched here)"
        elif [[ "$tree" == "$head_tree" ]]; then
            mark="<- native/ matches HEAD"
        fi
        printf '  %-12s %-9s %-20s  %-40s %s\n' "$id" "${sha:0:8}" "$created" "$branch" "$mark" >&2
    done
}

# Resolve --run into RUN_ID + RUN_SHA, refusing anything that is not a green
# 'native' run of exactly HEAD's native/ tree with the DLL artifact still there.
select_run() {
    if [[ -z "$RUN_SEL" ]]; then
        echo "ERROR: phase 2 needs the CI run to take the DLL from:  ./release.sh --run <run-id>" >&2
        echo "       It is never picked automatically: with parallel branch builds the newest" >&2
        echo "       green run is often another branch's DLL." >&2
        list_green_runs
        exit 1
    fi
    if [[ ! "$RUN_SEL" =~ ^[0-9]+$ ]]; then
        echo "ERROR: --run takes a numeric run id, got '${RUN_SEL}'." >&2
        exit 1
    fi
    local info wf status concl branch created
    if ! info="$($GH run view "$RUN_SEL" --json workflowName,status,conclusion,headBranch,headSha,createdAt \
            -q '[.workflowName, .status, .conclusion, .headBranch, .headSha, .createdAt] | join("|")')"; then
        echo "ERROR: gh could not read run ${RUN_SEL}." >&2
        exit 1
    fi
    IFS='|' read -r wf status concl branch RUN_SHA created <<<"$info"
    echo "CI run:   ${RUN_SEL}  branch ${branch}  sha ${RUN_SHA}  (${wf}, ${status}/${concl:-none}, ${created})"
    if [[ "$wf" != "native" ]]; then
        echo "ERROR: run ${RUN_SEL} is workflow '${wf}', not 'native'." >&2
        exit 1
    fi
    if [[ "$status" != "completed" || "$concl" != "success" ]]; then
        echo "ERROR: run ${RUN_SEL} is ${status}/${concl:-none}, not completed/success." >&2
        exit 1
    fi
    # Compare the native/ TREE, not the commit sha. The DLL is a function of
    # native/ alone, so a docs- or script-only commit since the run is harmless.
    # Any drift in native/ means the artifact is not this code. Comparing shas
    # instead would force a pointless rebuild every time this very file changed,
    # and APMF's CI only fires on native/** anyway, so a docs commit has no run.
    local head_tree run_tree
    head_tree="$(git rev-parse HEAD:native)"
    run_tree="$(git rev-parse -q --verify "${RUN_SHA}:native" 2>/dev/null || echo unknown)"
    if [[ "$run_tree" != "$head_tree" ]]; then
        echo "ERROR: run ${RUN_SEL} did not build HEAD's native/ tree." >&2
        echo "       run ${RUN_SEL} built ${RUN_SHA:0:8} on ${branch} (native tree ${run_tree:0:8})" >&2
        echo "       HEAD is $(git rev-parse --short HEAD) (native tree ${head_tree:0:8})" >&2
        [[ "$run_tree" == unknown ]] && echo "       (${RUN_SHA:0:8} is not in this clone: git fetch origin, then retry)" >&2
        echo "       Pick a run that built this code, or you will ship a DLL that is not it." >&2
        exit 1
    fi
    local names
    if ! names="$($GH api "repos/{owner}/{repo}/actions/runs/${RUN_SEL}/artifacts?per_page=100" \
            -q '.artifacts[] | select(.expired | not) | .name')"; then
        echo "ERROR: gh could not list the artifacts of run ${RUN_SEL}." >&2
        exit 1
    fi
    if ! grep -qxF "$ARTIFACT" <<<"$names"; then
        echo "ERROR: run ${RUN_SEL} has no unexpired '${ARTIFACT}' artifact (has: ${names//$'\n'/, })." >&2
        exit 1
    fi
    RUN_ID="$RUN_SEL"
    echo "CI run ${RUN_ID} verified: green, native/ tree matches HEAD, ${ARTIFACT} present."
}

# APMF has no VERSION file and no MCM config (MFO has both). The single source
# of truth for the version is native/CMakeLists.txt, which is also the thing CI
# builds, so phase 2 reads the version back out of the stamp itself.
CMAKELISTS="native/CMakeLists.txt"

# TWO-PHASE, deliberately. Stamping the version touches native/, and the DLL we
# ship must come from a CI run that built exactly that tree. So the bump has to
# be committed and BUILT before the release can be cut. Doing both in one pass
# means either shipping a DLL whose version does not match the zip, or skipping
# the check that catches exactly that.
#
#   ./release.sh 0.9.5          -> phase 1: stamp, commit, push. Wait for CI.
#   ./release.sh --run <id>     -> phase 2: verify + package + tag.
if [[ ${#POS[@]} -eq 1 ]]; then
    NEWVER="${POS[0]}"
    if [[ ! "$NEWVER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "ERROR: version must look like 0.9.5, got '${NEWVER}'." >&2
        exit 1
    fi
    if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then
        echo "ERROR: commit your work before bumping the version." >&2
        git status --short >&2
        exit 1
    fi

    sed -i "s/^project(APMF VERSION [0-9.]*/project(APMF VERSION ${NEWVER}/" "$CMAKELISTS"
    git add "$CMAKELISTS"

    # REVIEW-BACKLOG APMF-B10. core/ClientAPI.cpp's MinReleaseForAbi maps an ABI
    # revision to the first APMF release that implements it, and the newest
    # revisions carry a placeholder ("the first release after 0.9.4") because no
    # release carrying them existed when they were written. The backlog entry
    # says to name it in the SAME commit as the CMake bump, so this does it. A
    # user running an older APMF under a newer client reads that string as the
    # version they need, so a stale placeholder is a wrong instruction, not a
    # cosmetic one. Harmless once the placeholder is gone: the sed matches
    # nothing and the file is only staged when it actually changed.
    if grep -q 'the first release after [0-9.]*' native/core/ClientAPI.cpp; then
        sed -i "s/the first release after [0-9.]*/${NEWVER}/g" native/core/ClientAPI.cpp
        git add native/core/ClientAPI.cpp
        echo "Named MinReleaseForAbi's placeholder as ${NEWVER} (REVIEW-BACKLOG APMF-B10)."
    fi

    git commit -q -m "Bump version to ${NEWVER}"
    git push -q origin "$(git rev-parse --abbrev-ref HEAD)"
    echo "Stamped ${NEWVER} and pushed. CI is rebuilding native/."
    echo "When it is green:  ./release.sh --run <run-id>   (a bare ./release.sh lists the green runs)"
    exit 0
fi

# Phase 2 without --run: list the green runs to pick from and stop, before any
# other check, so the list is always one bare ./release.sh away.
[[ -n "$RUN_SEL" ]] || select_run

VER="$(grep -oP '^project\(APMF VERSION \K[0-9.]+' "$CMAKELISTS")"
if [[ -z "$VER" ]]; then
    echo "ERROR: no 'project(APMF VERSION ...)' line in ${CMAKELISTS}." >&2
    exit 1
fi
DEST="releases/v${VER}"

if [[ -e "$DEST" ]]; then
    echo "ERROR: $DEST already exists. Releases are immutable, bump the version." >&2
    exit 1
fi

# Every release ships with its changelog entry. Enforced mechanically because in
# MRO the convention was silently skipped once and backfilled after the fact.
if ! grep -q "^## v${VER}" CHANGELOG.md 2>/dev/null; then
    echo "ERROR: CHANGELOG.md has no '## v${VER}' entry. Write the changelog first." >&2
    exit 1
fi
# A leftover '## Unreleased' block above the new entry means the cut did not
# consolidate, and the zip would ship a changelog whose top entry is not this
# release. Warn rather than refuse: two historical '## Unreleased' headings sit
# below v0.9.1 and are residue, not pending work.
if grep -q "^## Unreleased" CHANGELOG.md 2>/dev/null; then
    echo "WARN: CHANGELOG.md still has an '## Unreleased' heading:" >&2
    grep -n "^## Unreleased" CHANGELOG.md >&2
    echo "      Fold it into '## v${VER}' unless it is residue below v0.9.1." >&2
fi

# Refuse to ship uncommitted work, otherwise the tag does not describe the zip.
if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then
    echo "ERROR: working tree is dirty. Commit first, or the tag lies about what shipped." >&2
    git status --short >&2
    exit 1
fi

echo "== APMF v${VER} =="

# 1. DLL from the CI run the operator named with --run, verified before anything
#    is written (a wrong or missing run stops the release with nothing touched),
#    provenance recorded below.
select_run
if [[ "$DRY_RUN" -eq 1 ]]; then
    echo
    echo "DRY RUN: every phase 2 check passed for v${VER} with run ${RUN_ID}."
    echo "         Nothing was generated, downloaded, packaged or tagged."
    exit 0
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
if ! $GH run download "$RUN_ID" -n "$ARTIFACT" -D "$STAGE/dll"; then
    echo "ERROR: could not download artifact ${ARTIFACT} of run ${RUN_ID}." >&2
    exit 1
fi
if [[ ! -f "$STAGE/dll/$DLL_NAME" ]]; then
    echo "ERROR: ${DLL_NAME} is not in artifact ${ARTIFACT} of run ${RUN_ID}." >&2
    exit 1
fi

# 2. ESL, always regenerated so a zip can never carry a stale plugin. It is a
#    pure function of APMF_GenerateESL.py, so regenerating and comparing catches
#    both a hand-edited ESL and a generator change nobody re-ran. The FormID
#    band is a frozen contract with channels/Travel.cpp, so a silent drift here
#    is a feature that refuses every claim in the field.
python3 APMF_GenerateESL.py "$STAGE/esl" >/dev/null
if ! cmp -s "$STAGE/esl/APMF.esl" Data/APMF.esl; then
    echo "ERROR: Data/APMF.esl does not match APMF_GenerateESL.py's output." >&2
    echo "       Never hand-edit the ESL. Run:  python3 APMF_GenerateESL.py Data" >&2
    echo "       then review and commit the result before cutting the release." >&2
    exit 1
fi
echo "ESL regenerated and identical to the committed Data/APMF.esl."

# 3. Stage in Data/ layout. The zip root IS the virtual Data folder, so a mod
#    manager installs it with zero manual placement, same as the v0.9.4 zip.
mkdir -p "$STAGE/pkg/SKSE/Plugins"
cp "$STAGE/dll/APMF.dll"        "$STAGE/pkg/SKSE/Plugins/"
cp Data/SKSE/Plugins/APMF.ini   "$STAGE/pkg/SKSE/Plugins/"
# APMF.esl sits at the ZIP ROOT, not under SKSE/Plugins. It is new in v0.9.5 and
# it is the one file that is easy to forget, because every release before this
# one was two files. Without it ch.19 (kIntent_Travel) refuses every claim, so a
# client's travel facet is dead on arrival and the only sign is one log line.
cp Data/APMF.esl                "$STAGE/pkg/"

# Verify the staged layout by name, loudly, before anything is zipped. A missing
# file stops the release instead of shipping.
MISSING=0
for f in APMF.esl SKSE/Plugins/APMF.dll SKSE/Plugins/APMF.ini; do
    if [[ -f "$STAGE/pkg/$f" ]]; then
        echo "  ok        $f"
    else
        echo "  MISSING   $f" >&2
        MISSING=1
    fi
done
if [[ "$MISSING" -ne 0 ]]; then
    echo "ERROR: the staged package is incomplete. Nothing was written." >&2
    exit 1
fi

mkdir -p "$DEST"
ZIP="$DEST/APMF-v${VER}.zip"
(cd "$STAGE/pkg" && zip -rq "$OLDPWD/$ZIP" .)

{
    echo "APMF v${VER}"
    echo "commit:   $(git rev-parse HEAD)"
    echo "ci run:   $RUN_ID (${RUN_SHA})"
    echo "dll:      $(sha256sum "$STAGE/pkg/SKSE/Plugins/APMF.dll" | cut -d' ' -f1)"
    echo "esl:      $(sha256sum "$STAGE/pkg/APMF.esl"              | cut -d' ' -f1)"
    echo "zip:      $(sha256sum "$ZIP"                             | cut -d' ' -f1)"
    echo "built:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$DEST/MANIFEST.txt"

git tag "v${VER}"    # fails if it exists, tags are immutable too

echo
cat "$DEST/MANIFEST.txt"
echo
echo "Released -> $ZIP"
echo "Push the tag when ready:  git push origin v${VER}"
echo "REMINDER: every deploy and every release carries THREE files now."
echo "          APMF.dll, APMF.ini and APMF.esl. The esl must be enabled in the"
echo "          load order or ch.19 refuses every travel claim."
echo "REMINDER: update Docs/STATUS.md. Move this release to shipped, refresh the"
echo "          field-test status and the open items. Keep the living handoff"
echo "          current or the next session inherits a stale map."

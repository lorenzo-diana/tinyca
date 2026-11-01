#!/bin/bash
# Documentation structure check. Four rules, each of which has been violated in
# this repository at least once:
#
#   1. No orphans. Every .md (and .html) file must be reachable by following
#      links from one of the two roots. A document nobody can reach is one
#      nobody updates, and a stale document is worse than a missing one because
#      it gets trusted.
#   2. No dangling links. A link pointing at a file that does not exist is the
#      other half of the same problem.
#   3. Two disjoint trees. Project documentation (rooted at README.md) describes
#      what TinyCA is; the development record (rooted at dev/README.md) describes
#      how it was built. No link may cross between them in either direction --
#      a reader looking for how to use TinyCA must never land in a journal
#      entry, and the journal must not drag the project docs into its tree.
#   4. Coverage. Every CLI long option and every configuration key that exists
#      in the code must appear in the usage reference, which is the single
#      definition of both.
#
# Reachability is transitive. Only relative links are followed; http(s) links
# and #anchors are ignored.
set -u
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROJECT_ROOT="README.md"
PROCESS_ROOT="dev/README.md"
MANUAL="docs/MANUAL.md"
FAIL=0

mapfile -t ALL_DOCS < <(find . \( -name '*.md' -o -name '*.html' \) \
    -not -path './.git/*' -not -path './build/*' -printf '%P\n' | sort)

# Each tree is walked INDEPENDENTLY, into its own set. Sharing one "seen" map
# across both walks hides exactly the violation rule 3 exists to catch: a
# document already found by the first walk would be skipped by the second, so a
# dev/ document linking into the project tree looked clean. Found by breaking
# the rule on purpose and watching the check pass.
#
# Writes the reachable set as "<tree>|<path>" lines on stdout.
walk_from() {
    local root="$1"
    local tree_name="$2"
    local -a queue=("$root")
    declare -A seen=()
    seen["$root"]=1
    printf '%s|%s\n' "$tree_name" "$root"

    while [ "${#queue[@]}" -gt 0 ]; do
        local current="${queue[0]}"
        queue=("${queue[@]:1}")
        [ -f "$current" ] || continue
        local base_dir
        base_dir="$(dirname "$current")"

        while read -r target; do
            case "$target" in
                http://*|https://*|mailto:*|"#"*) continue ;;
            esac
            target="${target%%#*}"
            [ -n "$target" ] || continue
            local resolved
            case "$target" in
                /*) resolved="${target#/}" ;;
                *)  if [ "$base_dir" = "." ]; then resolved="$target"; else resolved="$base_dir/$target"; fi ;;
            esac
            resolved="$(printf '%s' "$resolved" | sed -e 's|/\./|/|g' -e 's|^\./||')"
            while printf '%s' "$resolved" | grep -q '[^/][^/]*/\.\./'; do
                resolved="$(printf '%s' "$resolved" | sed -e 's|[^/][^/]*/\.\./||')"
            done
            case "$resolved" in
                *.md|*.html) ;;
                *) continue ;;
            esac
            if [ -z "${seen[$resolved]:-}" ]; then
                seen["$resolved"]=1
                printf '%s|%s\n' "$tree_name" "$resolved"
                queue+=("$resolved")
            fi
        done < <(grep -oE '\]\([^)]+\)|href="[^"]+"' "$current" \
            | sed -e 's|^](||' -e 's|)$||' -e 's|^href="||' -e 's|"$||')
    done
}

declare -A IN_PROJECT=()
declare -A IN_PROCESS=()
while IFS='|' read -r tree path; do
    [ -n "$path" ] || continue
    if [ "$tree" = "project" ]; then
        IN_PROJECT["$path"]=1
    else
        IN_PROCESS["$path"]=1
    fi
done < <(walk_from "$PROJECT_ROOT" "project"; walk_from "$PROCESS_ROOT" "process")

# Rule 1: orphans.
for doc in "${ALL_DOCS[@]}"; do
    if [ -z "${IN_PROJECT[$doc]:-}" ] && [ -z "${IN_PROCESS[$doc]:-}" ]; then
        echo "FAIL: integration_docs: $doc is reachable from neither $PROJECT_ROOT nor $PROCESS_ROOT"
        FAIL=1
    fi
done

# Rule 2: dangling links.
for doc in "${!IN_PROJECT[@]}" "${!IN_PROCESS[@]}"; do
    if [ ! -f "$doc" ]; then
        echo "FAIL: integration_docs: a link points at $doc, which does not exist"
        FAIL=1
    fi
done

# Rule 3: no link crosses between the trees, in either direction.
for doc in "${!IN_PROJECT[@]}"; do
    case "$doc" in
        dev/*)
            echo "FAIL: integration_docs: $doc (development record) is reachable from $PROJECT_ROOT"
            FAIL=1
            ;;
    esac
done
for doc in "${!IN_PROCESS[@]}"; do
    case "$doc" in
        dev/*) ;;
        *)
            echo "FAIL: integration_docs: $doc (project documentation) is reachable from $PROCESS_ROOT"
            FAIL=1
            ;;
    esac
done

# Rule 4: coverage of the code's own surface by the usage reference.
if [ ! -f "$MANUAL" ]; then
    echo "FAIL: integration_docs: $MANUAL is missing"
    FAIL=1
else
    while read -r opt; do
        grep -q -- "--$opt" "$MANUAL" || {
            echo "FAIL: integration_docs: --$opt exists in the code but not in $MANUAL"
            FAIL=1
        }
    done < <(grep -oE '\{"[a-z-]+", (no|required)_argument' src/config.cpp | sed 's/{"//; s/", .*//' | sort -u)

    while read -r key; do
        # The manual lists keys per [section], so match the bare key name.
        grep -qE "\`${key#*.}\`" "$MANUAL" || {
            echo "FAIL: integration_docs: config key $key exists in the code but not in $MANUAL"
            FAIL=1
        }
    done < <(grep -oE '\{"[a-z_]+\.[a-z_0-9]+", &Config' src/config.cpp | sed 's/{"//; s/", &Config//' | sort -u)
fi

if [ "$FAIL" -eq 0 ]; then
    project_count="${#IN_PROJECT[@]}"
    process_count="${#IN_PROCESS[@]}"
    echo "PASS: integration_docs (${#ALL_DOCS[@]} documents: $project_count project / $process_count process, trees disjoint, code surface covered)"
    exit 0
fi
exit 1

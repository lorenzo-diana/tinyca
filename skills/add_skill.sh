#!/bin/bash
# @name: add_skill
# @description: Create a new reusable skill from one bash command line, callable by name afterwards.
# @usage: add_skill.sh <name> <description> <usage> <command line...>
#
# Writes $TINYCA_SKILLS_DIR/<name>.sh with the @name/@description/@usage
# header TinyCA's discovery requires, then TinyCA re-scans the directory and
# tells the model the new skill is available (SPEC.md §7.1).
#
# Deliberate constraints, do not "simplify" them away:
#  - The body arrives as ARGUMENTS, not on stdin. With the default
#    allow_metacharacters = false the model cannot type a heredoc or a
#    redirection at all, so a stdin-fed design would be unusable exactly
#    where it matters.
#  - The new file is created by copying .skill_template, which already has
#    mode 755. chmod is absent from the default whitelist (and on the default
#    blacklist), so needing it here would make this skill fail its own
#    startup screen.
#  - Only single-line bodies. Multi-line scripts need a real editor, and a
#    1.5B model does not produce them correctly anyway (SPEC.md §16).
#  - Every program this script invokes must be in the operator's whitelist,
#    because TinyCA screens skill bodies at startup (SPEC.md §7.1). That is
#    why it uses echo/cp/[ and never chmod, printf or install.
set -euo pipefail

if [ "$#" -lt 4 ]; then
    echo "usage: $0 <name> <description> <usage> <command line...>" >&2
    exit 2
fi

skills_dir="${TINYCA_SKILLS_DIR:-}"
if [ -z "$skills_dir" ]; then
    echo "error: TINYCA_SKILLS_DIR is unset, so this skill must be run by TinyCA" >&2
    exit 2
fi

name="$1"
description="$2"
usage_line="$3"
shift 3
body="$*"

case "$name" in
    *[!a-zA-Z0-9_-]*|"")
        echo "error: skill name must contain only letters, digits, '_' and '-'" >&2
        exit 2
        ;;
esac

template="$skills_dir/.skill_template"
dest="$skills_dir/$name.sh"

if [ ! -f "$template" ]; then
    echo "error: missing skill template: $template" >&2
    exit 2
fi
if [ -e "$dest" ]; then
    echo "error: a skill named '$name' already exists: $dest" >&2
    exit 2
fi

cp "$template" "$dest"
echo "# @name: $name" >> "$dest"
echo "# @description: $description" >> "$dest"
echo "# @usage: $usage_line" >> "$dest"
echo "$body" >> "$dest"

echo "created skill '$name' at $dest"
echo "run it as: $name.sh"

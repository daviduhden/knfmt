#!/bin/sh
# Verify that a Git change set includes a substantive agent/skills update.
set -eu

usage() {
	echo "usage: $0 [--base REV]" >&2
	exit 2
}

base=
if [ "$#" -eq 2 ] && [ "$1" = "--base" ]; then
	base=$2
elif [ "$#" -ne 0 ]; then
	usage
fi

root=$(git rev-parse --show-toplevel) || exit 1
cd "$root"

if [ -n "$base" ]; then
	git rev-parse --verify "$base^{commit}" >/dev/null 2>&1 || {
		echo "agent-sync: base revision does not exist: $base" >&2
		exit 2
	}
	changed=$(git diff --name-only "$base...HEAD")
	working=$(git diff --name-only HEAD)
	staged=$(git diff --cached --name-only)
	untracked=$(git ls-files --others --exclude-standard)
	changed=$(printf '%s\n%s\n%s\n%s\n' "$changed" "$working" "$staged" "$untracked")
else
	changed=$(git diff --name-only HEAD)
	staged=$(git diff --cached --name-only)
	untracked=$(git ls-files --others --exclude-standard)
	changed=$(printf '%s\n%s\n%s\n' "$changed" "$staged" "$untracked")
fi

# Remove empty entries and duplicate paths. Repository filenames containing
# newlines are unsupported by this line-oriented human-facing check.
changed=$(printf '%s\n' "$changed" | awk 'NF && !seen[$0]++')
if [ -z "$changed" ]; then
	echo "agent-sync: no changed files found"
	exit 0
fi

maintenance=$(printf '%s\n' "$changed" | awk '
	/(^|\/)AGENTS\.md$/ || /\.agent\.md$/ || /(^|\/)SKILL\.md$/ { print }
')
maintenance=$(printf '%s\n' "$maintenance" | while IFS= read -r file; do
	[ -f "$file" ] && printf '%s\n' "$file"
done)
if [ -z "$maintenance" ]; then
	echo "agent-sync: repository changes require a substantive AGENTS.md, *.agent.md, or SKILL.md update." >&2
	echo "agent-sync: review the applicable instructions, update reusable knowledge, then rerun tools/check-agent-sync.sh." >&2
	exit 1
fi

errors=0
for file in $maintenance; do
	case "$file" in
	*/SKILL.md|SKILL.md)
		if ! sed -n '1p' "$file" | grep -q '^---$' ||
		   ! sed -n '2,20p' "$file" | grep -q '^name:' ||
		   ! sed -n '2,30p' "$file" | grep -q '^description:'; then
			echo "agent-sync: invalid Agent Skills frontmatter in $file" >&2
			errors=1
		fi
		if ! grep -q '^## Maintenance & Continuous Learning$' "$file"; then
			echo "agent-sync: missing Maintenance & Continuous Learning section in $file" >&2
			errors=1
		fi
		;;
	*.agent.md)
		if ! grep -q '^## Post-Change Knowledge Synchronization$' "$file"; then
			echo "agent-sync: missing Post-Change Knowledge Synchronization section in $file" >&2
			errors=1
		fi
		;;
	*)
		if ! grep -q '^## Continuous Agent & Skills Maintenance$' "$file"; then
			echo "agent-sync: missing Continuous Agent & Skills Maintenance section in $file" >&2
			errors=1
		fi
		;;
	esac

	# Require actual edited prose or instructions, not a blank/timestamp-only
	# touch. Count meaningful added/removed lines in this file's diff.
	if [ -n "$base" ]; then
		diff_args="$base...HEAD"
		patch=$(git diff --unified=0 "$diff_args" -- "$file")
	else
		patch=$(git diff --unified=0 HEAD -- "$file")
		patch="$patch$(git diff --cached --unified=0 -- "$file")"
		if [ -z "$patch" ] && git ls-files --others --exclude-standard -- "$file" | grep -q .; then
			# New, untracked instruction files have no Git diff yet. Their
			# non-empty contents are the substantive addition.
			patch=$(sed 's/^/+/' "$file")
		fi
	fi
	meaningful=$(printf '%s\n' "$patch" | awk '
		/^[+-]/ && !/^\+\+\+/ && !/^---/ {
			line = substr($0, 2)
			gsub(/[[:space:]`*_#>-]/, "", line)
			if (length(line) > 8) count++
		}
		END { print count + 0 }
	')
	if [ "$meaningful" -lt 2 ]; then
		echo "agent-sync: $file has no substantive instruction change (need at least two meaningful changed lines)." >&2
		errors=1
	fi

	# Check inline Markdown links to repository-local paths. Web links,
	# mailto links, anchors and images are excluded.
	links=$(sed -nE 's/.*\]\(([^)#]+)(#[^)]*)?\).*/\1/p' "$file")
	for link in $links; do
		case "$link" in
			https://*|http://*|mailto:*|\#*) continue ;;
		esac
		case "$link" in
			/*) target=".${link}" ;;
			*) target="$(dirname "$file")/$link" ;;
		esac
		if [ ! -e "$target" ]; then
			echo "agent-sync: broken local Markdown reference in $file: $link" >&2
			errors=1
		fi
	done
done

if [ "$errors" -ne 0 ]; then
	exit 1
fi
printf 'agent-sync: PASS (%s instruction file(s) updated)\n' "$(printf '%s\n' "$maintenance" | awk 'NF { n++ } END { print n + 0 }')"

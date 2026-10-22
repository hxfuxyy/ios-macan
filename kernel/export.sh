#!/bin/bash
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=${1:-${XNU_SRC:-$HERE/../xnu-src/xnu-8796.121.2}}
BASE=c54f35ca767986246321eb901baf8f5ff7923f6a
JUNK='^BUILD/|(^|/)__pycache__/|\.(orig|rej|pyc|o|swp)$|\.bak[0-9]*$|~$|(^|/)\.DS_Store$'

cd "$SRC"
git cat-file -e "$BASE^{commit}" 2>/dev/null || { echo "export.sh: $SRC is not a clone of xnu at $BASE" >&2; exit 1; }

git -c core.abbrev=12 diff --binary --no-renames --no-color --no-ext-diff --diff-filter=MDT "$BASE" -- . > "$HERE/kernel.patch.new"
mv "$HERE/kernel.patch.new" "$HERE/kernel.patch"

rm -rf "$HERE/files.new"
mkdir "$HERE/files.new"
{ git diff --no-renames --name-only --diff-filter=A "$BASE" -- .; git ls-files --others --exclude-standard; } |
	sort -u | grep -Ev "$JUNK" > "$HERE/files.list"
while IFS= read -r f; do
	[ -f "$f" ] || [ -L "$f" ] || continue
	mkdir -p "$HERE/files.new/$(dirname "$f")"
	cp -P --preserve=mode "$f" "$HERE/files.new/$f"
done < "$HERE/files.list"
rm -rf "$HERE/files" "$HERE/files.list"
mv "$HERE/files.new" "$HERE/files"

echo "kernel/kernel.patch: $(grep -c '^diff --git' "$HERE/kernel.patch") files changed, $(wc -c < "$HERE/kernel.patch") bytes"
echo "kernel/files: $(find "$HERE/files" -type f | wc -l) new files, $(du -sk "$HERE/files" | cut -f1) KB"

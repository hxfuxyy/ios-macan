#!/bin/bash
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DIR=${1:-${XNU_SRC:-$HERE/../xnu-src/xnu-8796.121.2}}
URL=${XNU_URL:-https://github.com/apple-oss-distributions/xnu.git}
TAG=xnu-8796.121.2
BASE=c54f35ca767986246321eb901baf8f5ff7923f6a

[ -e "$DIR" ] && { echo "apply.sh: $DIR already exists; not touching it" >&2; exit 1; }
[ -s "$HERE/kernel.patch" ] && [ -d "$HERE/files" ] || { echo "apply.sh: no kernel.patch and files/ in $HERE" >&2; exit 1; }
mkdir -p "$(dirname "$DIR")"
DIR=$(cd "$(dirname "$DIR")" && pwd)/$(basename "$DIR")
git init -q "$DIR"
trap 'echo "apply.sh: failed; removing the incomplete $DIR" >&2; rm -rf "$DIR"' ERR
cd "$DIR"
git remote add origin "$URL"
git fetch -q --depth 1 origin "refs/tags/$TAG:refs/tags/$TAG"
git checkout -q -b redwood "$TAG"
[ "$(git rev-parse HEAD)" = "$BASE" ] || { echo "apply.sh: $TAG is $(git rev-parse HEAD), expected $BASE" >&2; false; }
echo 'BUILD/' >> .git/info/exclude
git apply --whitespace=nowarn "$HERE/kernel.patch"
cp -a "$HERE/files/." .
echo "apply.sh: $DIR: $TAG + $(grep -c '^diff --git' "$HERE/kernel.patch") changed and $(find "$HERE/files" -type f | wc -l) new files"

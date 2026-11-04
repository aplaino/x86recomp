#!/bin/sh
# Lifts a game's eboot and the PRXs it starts with into OUT, each PRX at
# the base the loader maps it at (both from `orbis_app modules`,
# orbis/app.h). MANIFESTS/<module>.manifest goes with each module that
# has one ("eboot" for the eboot), and the platform's
# lifter/middleware/<SHA-256>.manifest with each module file of that hash
# (fixes to a middleware build, ARCHITECTURE.md).
#
# usage: lift.sh RECOMP_LIFT ORBIS_APP APP0 MANIFESTS OUT
set -e
if [ $# -ne 5 ]; then
  echo "usage: lift.sh RECOMP_LIFT ORBIS_APP APP0 MANIFESTS OUT" >&2
  exit 2
fi
lift=$1 app=$2 app0=$3 manifests=$4 out=$5
middleware=$(cd "$(dirname "$0")/../lifter/middleware" && pwd)
sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1
}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
modules=$("$app" modules "$app0")
tab=$(printf '\t')
set --
while IFS=$tab read -r base name path; do
  if [ "$base" = 0 ]; then set -- "$@" --exe "$path"; else set -- "$@" --module "$path"; fi
  set -- "$@" --symbols ""
  own=$manifests/$name.manifest
  fix=$middleware/$(sha256 "$path").manifest
  if [ -f "$own" ] && [ -f "$fix" ]; then
    cat "$own" "$fix" >"$tmp/$name.manifest"
    set -- "$@" --manifest "$tmp/$name.manifest"
  elif [ -f "$own" ]; then
    set -- "$@" --manifest "$own"
  elif [ -f "$fix" ]; then
    set -- "$@" --manifest "$fix"
  else
    set -- "$@" --manifest ""
  fi
  if [ "$base" != 0 ]; then set -- "$@" --base "$base"; fi
done <<EOF2
$modules
EOF2
"$lift" "$@" --out "$out"

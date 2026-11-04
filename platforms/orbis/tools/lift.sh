#!/bin/sh
# Lifts a game's eboot and the PRXs it starts with into OUT, each PRX at
# the base the loader maps it at (both from `orbis_app modules`,
# orbis/app.h). MANIFESTS/<module>.manifest goes with each module that
# has one ("eboot" for the eboot).
#
# usage: lift.sh RECOMP_LIFT ORBIS_APP APP0 MANIFESTS OUT
set -e
if [ $# -ne 5 ]; then
  echo "usage: lift.sh RECOMP_LIFT ORBIS_APP APP0 MANIFESTS OUT" >&2
  exit 2
fi
lift=$1 app=$2 app0=$3 manifests=$4 out=$5
modules=$("$app" modules "$app0")
tab=$(printf '\t')
set --
while IFS=$tab read -r base name path; do
  if [ "$base" = 0 ]; then set -- "$@" --exe "$path"; else set -- "$@" --module "$path"; fi
  set -- "$@" --symbols ""
  if [ -f "$manifests/$name.manifest" ]; then set -- "$@" --manifest "$manifests/$name.manifest"; fi
  if [ "$base" != 0 ]; then set -- "$@" --base "$base"; fi
done <<EOF
$modules
EOF
exec "$lift" "$@" --out "$out"

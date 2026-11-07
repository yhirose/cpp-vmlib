#!/usr/bin/env bash
# Every example front end's samples, run through the real language it
# imitates, against the golden files its samples test holds the front end
# to (examples/<front end>/samples/golden/<sample>.txt). How each language
# runs, and at which version, is test/languages.sh's to say.
#
# As it stands it checks: a golden that is no longer what the language
# prints fails here, where the front end's own test would go on passing
# against it. --update rewrites the goldens from the language, by hand and
# never from the build: a golden file the tests could refresh themselves
# is not evidence of anything.
#
#   test/goldens.sh [--update] [FRONT_END...]
#
# FRONT_END is a directory under examples/ (mini-python, pl0, ...); with
# none, every one that has samples/golden/. Its samples are every
# samples/**/<name>.<ext>. Each runs from its own directory by its bare
# file name, as its front end's test runs it (Lua's messages name the
# chunk as it was given), with <name>.stdin as its input when there is
# one, and with that directory's prelude.<ext> ahead of it when there is
# one (mini-js's). It must exit 0, and what it prints is its golden. A
# golden no sample makes fails the check, and --update removes it.
#
# A language not at the version its goldens were made with is refused
# rather than compared: its differences would be the version's.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/test/languages.sh"

update=0
if [ "${1:-}" = --update ]; then
  update=1
  shift
fi
if [ $# -eq 0 ]; then
  for d in "$ROOT"/examples/*/samples/golden; do
    d=${d%/samples/golden}
    set -- "$@" "${d##*/}"
  done
fi

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

# Checks (or with --update, writes) one front end's goldens; prints one
# line for it, and a diff for each golden that is not what the language
# prints. Fails if anything did.
front_end() {
  local fe=${1%/}
  fe=${fe##*/}
  local lang=${fe#mini-}
  local samples=$ROOT/examples/$fe/samples
  local ext=${LANG_EXT[$lang]:-}
  if [ -z "$ext" ]; then
    echo "$fe: test/languages.sh has no language '$lang'"
    return 1
  fi

  local want=${LANG_VERSION[$lang]} have
  have=$(lang_version "$lang" 2>"$SCRATCH/version.err") || have=
  local real="${LANG_NAME[$lang]} ${have:-(not found)}"
  if [ "$have" != "$want" ] && [[ $have != "$want".* ]]; then
    echo "$fe: its goldens are ${LANG_NAME[$lang]} $want's output; found $real"
    cat "$SCRATCH/version.err"
    return 1
  fi

  local golden=$samples/golden scratch=$SCRATCH/$fe
  mkdir -p "$scratch"
  if [ "$update" = 1 ]; then mkdir -p "$golden"; fi
  local ok=1 n=0 src dir name
  local -A made=()
  while IFS= read -r src; do
    dir=${src%/*}
    name=${src##*/}
    name=${name%."$ext"}
    made[$name]=1
    n=$((n + 1))

    local lead=() input=/dev/null
    if [ -f "$dir/prelude.$ext" ]; then lead=("prelude.$ext"); fi
    if [ -f "$dir/$name.stdin" ]; then input=$dir/$name.stdin; fi
    local out=$scratch/$name.out err=$scratch/$name.err
    if ! (cd "$dir" &&
          lang_command "$lang" "$scratch/$name" "${lead[@]}" "$name.$ext" &&
          "${CMD[@]}" <"$input" >"$out") 2>"$err"; then
      echo "$fe: $name.$ext failed under $real"
      cat "$err"
      ok=0
      continue
    fi

    if [ "$update" = 1 ]; then
      cp "$out" "$golden/$name.txt"
    elif ! diff -u --label "golden/$name.txt" --label "$real" \
                "$golden/$name.txt" "$out"; then
      ok=0
    fi
  done < <(find "$samples" -name "*.$ext" ! -name "prelude.$ext" \
                ! -path "$golden/*" | sort)

  local g
  for g in "$golden"/*.txt; do
    [ -e "$g" ] || continue
    name=${g##*/}
    name=${name%.txt}
    [ -n "${made[$name]:-}" ] && continue
    if [ "$update" = 1 ]; then
      rm "$g"
      echo "$fe: removed golden/$name.txt, which no sample makes"
    else
      echo "$fe: golden/$name.txt is made by no sample"
      ok=0
    fi
  done

  if [ "$n" = 0 ]; then
    echo "$fe: no samples/**/*.$ext"
    return 1
  fi
  if [ "$ok" = 0 ]; then
    echo "$fe: FAILED against $real"
    return 1
  fi
  if [ "$update" = 1 ]; then
    echo "$fe: $n goldens written by $real"
  else
    echo "$fe: $n goldens are what $real prints"
  fi
}

failed=0
for fe in "$@"; do
  front_end "$fe" || failed=1
done
exit "$failed"

# The real language each example front end imitates, and how to run a
# program under it -- the one place that knows. test/goldens.sh runs every
# front end's samples through it to make and check their golden files,
# bench/languages/run.sh times it next to the front end, and
# .github/actions/languages installs it in CI at the versions below.
# Sourced, not run.
#
# A language is named as its front end is, less the `mini-`:
# examples/mini-python's is `python`, and examples/pl0's is `pl0` --
# culebra running its own PL/0 interpreter, pl0.cul, which this repository
# does not contain.
#
#   LANG_EXT[lang]       its programs' file extension.
#   LANG_NAME[lang]      what its implementation is called.
#   LANG_VERSION[lang]   the version whose output the goldens are, and the
#                        only one test/goldens.sh accepts (CI installs
#                        exactly these). Below is why each is pinned as
#                        tightly as it is.
#   lang_version LANG    prints the version of LANG that would run.
#   lang_command LANG SCRATCH FILE...
#                        sets the array CMD to the command that runs the
#                        program FILE... (paths as the caller's directory
#                        sees them), compiling it first, into the directory
#                        SCRATCH, for a language that has a compile step.
#                        More than one FILE is one program, concatenated in
#                        order into SCRATCH under the last one's name --
#                        what mini-js's own CLI does with the prelude its
#                        samples share. Fails, with the reason on stderr,
#                        when the compile does. Running CMD starts nothing
#                        but the program itself, so bench/languages times
#                        the language rather than this file.
#
# Each implementation is the command in a variable of its own -- PYTHON,
# RUBY, LUA, GUILE, CULEBRA, NODE, DOTNET, GO; PL/0's is CULEBRA running
# the file PL0_CUL -- and the environment overrides any of them, the way
# CC names a C compiler: to run a version installed under another name
# (PYTHON=python3.14) or not on PATH at all.

if [ "${BASH_VERSINFO[0]}" -lt 4 ] ||
   { [ "${BASH_VERSINFO[0]}" -eq 4 ] && [ "${BASH_VERSINFO[1]}" -lt 4 ]; }; then
  echo "test/languages.sh: needs bash 4.4 or later, found $BASH_VERSION" >&2
  return 1
fi

declare -A LANG_EXT=(
  [python]=py [ruby]=rb [lua]=lua [scheme]=scm [culebra]=cul [js]=js
  [csharp]=cs [go]=go [pl0]=pas
)
declare -A LANG_NAME=(
  [python]=python3 [ruby]=ruby [lua]=lua [scheme]=guile [culebra]=culebra
  [js]=node [csharp]=dotnet [go]=go [pl0]="culebra pl0.cul"
)

# A version matches when it is this one or a release of it: 3.14 is any
# 3.14.x, and 0.8.0 is exactly 0.8.0 (not a 0.8.0-37-g6447b28d build from
# between releases).
#
# python 3.14: CPython rewords its messages between minor releases -- 3.14
#   made every division by zero "division by zero" and gave "too many
#   values to unpack" its count -- and mini-python's words are 3.14's.
# ruby 3.3: 3.4 changed Hash#inspect from {"a"=>1} to {"a" => 1}, and
#   mini-ruby prints the former.
# culebra 0.8.0: culebra is not at 1.0 and its output moves between
#   releases -- 0.5.0 began answering type_of with an object's class
#   ("Point", and "Class" for the class) where 0.4.0 said "Object", and
#   printing a class value with its name -- and even between the commits
#   in between, so mini-culebra follows one release, and moves with the
#   pin.
# pl0, culebra's: the same culebra running pl0.cul from that release's
#   tag. pl0.cul is written against its own release's PEG API, which keeps
#   changing (0.8.0 runs no earlier release's pl0.cul), so the two move
#   together.
# csharp 8.0 is .NET's: the program is built for net8.0, and the newest
#   8.0.x runtime is what runs it.
# The rest are pinned at the release the language is named by (Lua 5.4,
# Guile 3.0) or at a current one (Node's LTS, Go's newest), so that what
# checks the goldens does not move under them when a newer one comes out.
declare -A LANG_VERSION=(
  [python]=3.14 [ruby]=3.3 [lua]=5.4 [scheme]=3.0 [culebra]=0.8.0 [js]=24
  [csharp]=8.0 [go]=1.27
)
LANG_VERSION[pl0]=${LANG_VERSION[culebra]}

: "${PYTHON:=python3}" "${RUBY:=ruby}" "${GUILE:=guile}" "${CULEBRA:=culebra}"
: "${NODE:=node}" "${DOTNET:=dotnet}" "${GO:=go}"
# Debian and Ubuntu name Lua's binary by its version (lua5.4); elsewhere it
# is plain `lua`.
: "${LUA:=$(command -v "lua${LANG_VERSION[lua]}" || echo lua)}"

# PL/0's interpreter is a culebra program from culebra's own repository.
lang_pl0_cul() {
  if [ ! -f "${PL0_CUL:-}" ]; then
    echo "PL0_CUL must name pl0.cul (examples/languages/pl0/ in culebra's" \
         "repository) from its v${LANG_VERSION[pl0]} tag" >&2
    return 1
  fi
  echo "$PL0_CUL"
}

lang_version() {
  case $1 in
    python) "$PYTHON" -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])' ;;
    ruby) "$RUBY" -e 'puts RUBY_VERSION' ;;
    lua) "$LUA" -v | awk '{print $2}' ;;
    scheme) "$GUILE" -c '(display (version)) (newline)' ;;
    culebra) "$CULEBRA" --version | awk '{print $2}' ;;
    pl0) lang_pl0_cul >/dev/null && lang_version culebra ;;
    js) "$NODE" -p process.versions.node ;;
    csharp)
      "$DOTNET" --list-runtimes | awk -v want="${LANG_VERSION[csharp]}." \
        '$1 == "Microsoft.NETCore.App" && index($2, want) == 1 {v = $2} END {print v}' ;;
    go) "$GO" env GOVERSION | sed 's/^go//' ;;
    *) echo "test/languages.sh: no language '$1'" >&2; return 1 ;;
  esac
}

# Runs a compile step with its output kept out of the way, and shown only
# if it fails.
lang_build() {
  local log=$1; shift
  if ! "$@" >"$log" 2>&1; then
    cat "$log" >&2
    return 1
  fi
}

lang_command() {
  local lang=$1 scratch=$2; shift 2
  mkdir -p "$scratch"
  local file=$1
  if [ $# -gt 1 ]; then
    local last=${*: -1}
    file=$scratch/${last##*/}
    cat "$@" >"$file"
  fi
  case $lang in
    python) CMD=("$PYTHON" "$file") ;;
    ruby) CMD=("$RUBY" "$file") ;;
    lua) CMD=("$LUA" "$file") ;;
    # Without --no-auto-compile, Guile writes progress notes the first
    # time it sees a file: output of the cache's state, not the program's.
    scheme) CMD=("$GUILE" --no-auto-compile -s "$file") ;;
    culebra) CMD=("$CULEBRA" "$file") ;;
    pl0)
      local pl0_cul
      pl0_cul=$(lang_pl0_cul) || return
      CMD=("$CULEBRA" "$pl0_cul" "$file") ;;
    js) CMD=("$NODE" "$file") ;;
    # Built once, not `go run`: that compiles and links on every run.
    # `go build prog.go` names its file, which works outside a module.
    go)
      lang_build "$scratch/build.log" "$GO" build -o "$scratch/prog" "$file" || return
      CMD=("$scratch/prog") ;;
    # A project of its own: the SDK compiles every .cs in a project's
    # directory, so two programs side by side would both declare Main.
    csharp)
      cp "$file" "$scratch/Program.cs"
      cat >"$scratch/prog.csproj" <<EOF
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net${LANG_VERSION[csharp]}</TargetFramework>
    <Nullable>disable</Nullable>
    <AssemblyName>prog</AssemblyName>
    <InvariantGlobalization>true</InvariantGlobalization>
  </PropertyGroup>
</Project>
EOF
      lang_build "$scratch/build.log" \
        "$DOTNET" build "$scratch/prog.csproj" -c Release -o "$scratch/out" || return
      CMD=("$DOTNET" "$scratch/out/prog.dll") ;;
    *) echo "test/languages.sh: no language '$lang'" >&2; return 1 ;;
  esac
}

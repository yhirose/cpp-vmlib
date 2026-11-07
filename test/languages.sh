# The real language each example front end imitates, and how to run a
# program under it -- the one place that knows. test/oracle.sh runs it on
# a front end's samples, in that front end's goldens test, to check the
# goldens against it; bench/languages/run.sh times it next to the front
# end; and .github/actions/languages installs it in CI at the versions
# below. Sourced, not run.
#
# A language is known by the key its front end's add_front_end gives as
# LANG: mini-python's is `python`, and PL/0's is `pl0` -- culebra running
# its own PL/0 interpreter, pl0.cul, which this repository does not
# contain.
#
#   LANG_VERSION[lang]   the version whose output the goldens are, and the
#                        only one lang_pinned accepts (CI installs exactly
#                        these). Below is why each is pinned as tightly as
#                        it is.
#   lang_name LANG       prints what its implementation is called.
#   lang_pinned LANG     the version gate test/oracle.sh runs behind.
#   lang_command LANG SCRATCH FILE...
#                        sets the array CMD to the command that runs the
#                        program FILE... (paths as the caller's directory
#                        sees them), compiling it first, into the directory
#                        SCRATCH, for a language that has a compile step.
#                        More than one FILE is one program, concatenated in
#                        order into SCRATCH under the last one's name --
#                        what mini-js's own CLI does with the prelude its
#                        samples share. An empty SCRATCH makes a directory
#                        only where one is needed, a compile or files to
#                        join, and names it in LANG_SCRATCH for the caller
#                        to remove. Fails, with the reason on stderr, when
#                        the compile does. Running CMD starts nothing but
#                        the program itself, so bench/languages times the
#                        language rather than this file.
#
# Each implementation's command is resolved once, into LANG_BIN, and the
# environment overrides any of them by the variable named there, the way
# CC names a C compiler: to run a version installed under another name
# (PYTHON=python3.14) or not on PATH at all. PL/0's is culebra's, running
# the file PL0_CUL.

if [ "${BASH_VERSINFO[0]}" -lt 4 ] ||
   { [ "${BASH_VERSINFO[0]}" -eq 4 ] && [ "${BASH_VERSINFO[1]}" -lt 4 ]; }; then
  echo "test/languages.sh: needs bash 4.4 or later, found $BASH_VERSION" >&2
  return 1
fi

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
# go 1.27.3: to the patch, since what CI installs is that release's own
#   tarball.
# The rest are pinned at the release the language is named by (Lua 5.4,
# Guile 3.0) or at a current one (Node's LTS), so that what checks the
# goldens does not move under them when a newer one comes out.
declare -A LANG_VERSION=(
  [python]=3.14 [ruby]=3.3 [lua]=5.4 [scheme]=3.0 [culebra]=0.8.0 [js]=24
  [csharp]=8.0 [go]=1.27.3
)
LANG_VERSION[pl0]=${LANG_VERSION[culebra]}

# Debian and Ubuntu name Lua's binary by its version (lua5.4); elsewhere it
# is plain `lua`.
declare -A LANG_BIN=(
  [python]=${PYTHON:-python3}
  [ruby]=${RUBY:-ruby}
  [lua]=${LUA:-$(command -v "lua${LANG_VERSION[lua]}" || echo lua)}
  [scheme]=${GUILE:-guile}
  [culebra]=${CULEBRA:-culebra}
  [js]=${NODE:-node}
  [csharp]=${DOTNET:-dotnet}
  [go]=${GO:-go}
)
LANG_BIN[pl0]=${LANG_BIN[culebra]}

# Fails, saying so, unless LANG is a language this file knows.
lang_known() {
  if [ -z "${LANG_BIN[$1]:-}" ]; then
    echo "test/languages.sh: no language '$1'" >&2
    return 1
  fi
}

# PL/0's interpreter is a culebra program from culebra's own repository.
lang_pl0_cul() {
  if [ ! -f "${PL0_CUL:-}" ]; then
    echo "PL0_CUL must name pl0.cul (examples/languages/pl0/ in culebra's" \
         "repository) from its v${LANG_VERSION[pl0]} tag" >&2
    return 1
  fi
  echo "$PL0_CUL"
}

lang_name() {
  echo "${LANG_BIN[$1]##*/}"
}

# The version of LANG that would run, for lang_pinned.
lang_version() {
  local bin=${LANG_BIN[$1]}
  case $1 in
    python) "$bin" -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])' ;;
    ruby) "$bin" -e 'puts RUBY_VERSION' ;;
    lua) "$bin" -v | awk '{print $2}' ;;
    scheme) "$bin" -c '(display (version)) (newline)' ;;
    culebra) "$bin" --version | awk '{print $2}' ;;
    pl0) lang_pl0_cul >/dev/null && lang_version culebra ;;
    js) "$bin" -p process.versions.node ;;
    csharp)
      "$bin" --list-runtimes | awk -v want="${LANG_VERSION[csharp]}." \
        '$1 == "Microsoft.NETCore.App" && index($2, want) == 1 {v = $2} END {print v}' ;;
    go) "$bin" env GOVERSION | sed 's/^go//' ;;
  esac
}

# The version gate: succeeds, naming the implementation and its version,
# when the LANG that would run is the release LANG_VERSION pins; otherwise
# says on stderr what it found instead, and fails. A goldens check refuses
# rather than compares: another version's differences would be the
# version's.
lang_pinned() {
  lang_known "$1" || return
  local lang=$1 want=${LANG_VERSION[$1]} have
  have=$(lang_version "$lang") || have=
  if [ "$have" != "$want" ] && [[ $have != "$want".* ]]; then
    echo "$lang: the goldens are $(lang_name "$lang") $want's output, and" \
         "this is ${have:-no $(lang_name "$lang") at all}" >&2
    return 1
  fi
  echo "$(lang_name "$lang") $have"
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

# Makes lang_command's `scratch` (its local, which this sees) a directory
# to write into: the caller's, or for a caller that gave none, a new one,
# named in LANG_SCRATCH.
lang_room() {
  if [ -z "$scratch" ]; then
    scratch=$(mktemp -d)
    LANG_SCRATCH=$scratch
  fi
  mkdir -p "$scratch"
}

lang_command() {
  lang_known "$1" || return
  local lang=$1 scratch=$2; shift 2
  LANG_SCRATCH=
  local file=$1
  if [ $# -gt 1 ]; then
    lang_room
    local last=${*: -1}
    file=$scratch/${last##*/}
    cat "$@" >"$file"
  fi
  case $lang in
    # Without --no-auto-compile, Guile writes progress notes the first
    # time it sees a file: output of the cache's state, not the program's.
    scheme) CMD=("${LANG_BIN[scheme]}" --no-auto-compile -s "$file") ;;
    pl0)
      local pl0_cul
      pl0_cul=$(lang_pl0_cul) || return
      CMD=("${LANG_BIN[pl0]}" "$pl0_cul" "$file") ;;
    # Built once, not `go run`: that compiles and links on every run.
    # `go build prog.go` names its file, which works outside a module.
    go)
      lang_room
      lang_build "$scratch/build.log" \
        "${LANG_BIN[go]}" build -o "$scratch/prog" "$file" || return
      CMD=("$scratch/prog") ;;
    # A project of its own: the SDK compiles every .cs in a project's
    # directory, so two programs side by side would both declare Main.
    csharp)
      lang_room
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
        "${LANG_BIN[csharp]}" build "$scratch/prog.csproj" -c Release \
          -o "$scratch/out" || return
      CMD=("${LANG_BIN[csharp]}" "$scratch/out/prog.dll") ;;
    *) CMD=("${LANG_BIN[$lang]}" "$file") ;;
  esac
}

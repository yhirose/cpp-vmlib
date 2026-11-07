# Each sample's output must match its golden file, captured once from an
# oracle this repository does not contain -- the real language, or for
# PL/0 culebra's own PL/0 interpreter: an independent implementation, not
# just a second run of this one. Self-consistency alone would not have
# caught cpp-peglib's own ODD bug (pl0.cc's interpreter and JIT agree with
# each other and are both wrong about it). Every front end's samples are
# checked this way; its README says what its oracle is and how the goldens
# were made.
#
#   cmake -DBIN=<binary> -DDIR=<samples directory> -DEXT=<source extension>
#         -DORACLE=<what the goldens came from> [-DLEAD=<file>]
#         -P run_samples.cmake
#
# Every *.EXT under DIR is a case, run from its own directory by its file
# name alone -- the name a position or a chunk name gives, the way the
# golden was captured -- and must exit 0 having printed exactly
# DIR/golden/<case>.txt. A case may sit in a subdirectory of its own
# (mini-go's do: each is a standalone `package main`, and two side by side
# would both declare `func main`, which `go build` rejects). <case>.stdin
# beside it, when there is one, is its input. LEAD -- mini-js's prelude --
# is bound ahead of each case in its own directory, as
# test/run_sharing.cmake binds it, and is not a case.

# Every argument is required: the cases are a recursive glob of DIR, and a
# call that left DIR out would walk the whole filesystem from its root.
foreach(arg BIN DIR EXT ORACLE)
  if("${${arg}}" STREQUAL "")
    message(FATAL_ERROR "run_samples.cmake: -D${arg} is required")
  endif()
endforeach()

file(GLOB_RECURSE sources ${DIR}/*.${EXT})
if(LEAD)
  list(REMOVE_ITEM sources ${LEAD})
  get_filename_component(lead_dir ${LEAD} DIRECTORY)
endif()
list(SORT sources)
if(NOT sources)
  message(FATAL_ERROR "no *.${EXT} samples under ${DIR}")
endif()

set(failed 0)
foreach(src ${sources})
  get_filename_component(dir ${src} DIRECTORY)
  get_filename_component(name ${src} NAME)
  get_filename_component(s ${src} NAME_WE)
  set(lead)
  if(LEAD AND dir STREQUAL lead_dir)
    set(lead ${LEAD})
  endif()
  set(input)
  if(EXISTS ${dir}/${s}.stdin)
    set(input INPUT_FILE ${dir}/${s}.stdin)
  endif()
  execute_process(COMMAND ${BIN} ${lead} ${name}
                  WORKING_DIRECTORY ${dir}
                  ${input}
                  OUTPUT_VARIABLE out ERROR_VARIABLE err
                  RESULT_VARIABLE rc)

  if(NOT rc EQUAL 0)
    message(SEND_ERROR "${s}: exited ${rc}\n--stdout--\n${out}--stderr--\n${err}")
    set(failed 1)
    continue()
  endif()

  set(golden ${DIR}/golden/${s}.txt)
  if(NOT EXISTS ${golden})
    message(SEND_ERROR "${s}: no golden file at ${golden}")
    set(failed 1)
  else()
    file(READ ${golden} want)
    if(NOT out STREQUAL want)
      message(SEND_ERROR
              "${s}: output does not match the golden file\n"
              "--- want ---\n${want}--- got ---\n${out}")
      set(failed 1)
    endif()
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "sample output does not match ${ORACLE}")
endif()
list(LENGTH sources n)
message(STATUS "${n} samples OK (matches ${ORACLE})")

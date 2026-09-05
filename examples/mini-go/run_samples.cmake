# Each sample's output must match the golden file, captured once from a
# real `go run` -- an independent implementation, not just a second run of
# this one. See README.md for what this front end exists to prove: vmlib's
# Fixed-width integers, `float`, Static calls, Struct fields and Switch
# recipes hold against Go itself, not just against this repository's own
# tests.
#
# Each sample lives in its own subdirectory (samples/<name>/<name>.go)
# because it is a real, standalone `package main` -- two of them side by
# side in one directory would both declare `func main`, which `go build`
# (though not the single-file `go run` this script itself uses) rejects.

set(SAMPLES ints floats structs switch goroutines)

set(dir ${SRC}/samples)
set(failed 0)

foreach(s ${SAMPLES})
  execute_process(COMMAND ${MINI_GO} ${s}.go
                  WORKING_DIRECTORY ${dir}/${s}
                  OUTPUT_VARIABLE out ERROR_VARIABLE err
                  RESULT_VARIABLE rc)

  if(NOT rc EQUAL 0)
    message(SEND_ERROR "${s}: exited ${rc}\n--stdout--\n${out}--stderr--\n${err}")
    set(failed 1)
    continue()
  endif()

  set(golden ${dir}/golden/${s}.txt)
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
  message(FATAL_ERROR "mini-go sample output does not match go run")
endif()
message(STATUS "mini-go samples OK (matches go run)")

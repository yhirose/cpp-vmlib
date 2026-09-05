# Each sample's output must match the golden file, captured once from
# `dotnet` -- an independent implementation this repository does not
# contain. See README.md for what this front end exists to prove: that a
# statically-typed, class-based language erases its types on the way into
# the IR and then needs nothing the runtime does not already have --
# inheritance and virtual dispatch out of objects, `using` out of Scope
# and Defer, `yield return` out of a generator, and `async`/`await` out of
# a coroutine and the scheduler.

set(SAMPLES basics shapes properties resources iterators tasks)

set(dir ${SRC}/samples)
set(failed 0)

foreach(s ${SAMPLES})
  execute_process(COMMAND ${MINI_CSHARP} ${s}.cs
                  WORKING_DIRECTORY ${dir}
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
  message(FATAL_ERROR "mini-csharp sample output does not match dotnet")
endif()
message(STATUS "mini-csharp samples OK (matches dotnet)")

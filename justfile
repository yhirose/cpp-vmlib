# cpp-vmlib task runner. `just --list` for the short version.

# List available recipes.
default:
    @just --list

# Configure (if needed) and build everything: the vmlib test suite, the
# example front ends, and the benchmark harness.
build:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVMLIB_BUILD_EXAMPLES=ON
    cmake --build build -j

# The vmlib + front-end test suite.
test: build
    ctest --test-dir build --output-on-failure

# vmlib executor microbenchmarks (bench/bench.cc) -- calls, tail calls,
# variable traffic, arithmetic, strings, field access, allocation.
# Extra args pass straight through, e.g. `just bench --reps 5 call_fib`.
bench *args: build
    ./build/bench/bench {{args}}

# The same fib(25) through each front end's own binary and, where actually
# installed, the real language it imitates (bench/languages/run.sh).
bench-lang *args: build
    bench/languages/run.sh {{args}}

# ASan/UBSan/LeakSanitizer, in their own build directory so this never
# touches the normal one.
sanitizers:
    cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DVMLIB_BUILD_EXAMPLES=ON \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
    cmake --build build-asan -j
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        ctest --test-dir build-asan --output-on-failure

# Every allocation collects -- the premature-sweep class of bug, not only
# test_gc.cc's arranged scenes. deep_calls is excluded: its hundred-
# thousand live frames make an every-allocation collect quadratic, which
# is not a bug this lane could catch (see .github/workflows/ci.yml).
gc-stress: build
    COREIR_GC_STRESS=1 ctest --test-dir build --output-on-failure --timeout 900 -E deep_calls

# Remove both build directories.
clean:
    rm -rf build build-asan

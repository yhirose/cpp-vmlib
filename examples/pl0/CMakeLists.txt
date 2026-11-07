# A target that knows what a PEG is, as every example front end does.
# peglib.h lives under this directory; the other front ends reach it from
# here, and vmlib itself never does. No EXT, so no tests of its own here:
# PL/0's are the main suite's (test/CMakeLists.txt).
add_front_end(pl0)

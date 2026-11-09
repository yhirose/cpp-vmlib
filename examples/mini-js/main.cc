#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-js", "FILE.js", mini_js::bind_source);
  // More than one file is concatenated, in order, into one program -- which
  // is what `node a.js b.js` does not do, but what makes the samples able to
  // share one prelude (samples/prelude.js) without this subset needing a
  // module system it has no other use for. The oracle side of the same
  // comparison is `cat prelude.js sample.js > tmp.js && node tmp.js`; see
  // test/languages.sh.
  fe.concatenate = true;
  fe.run.natives = mini_js::stdlib();
  fe.run.describe = mini_js::describe;
  return driver::main(argc, argv, fe);
}

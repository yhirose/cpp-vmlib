// A trap caught and thrown again is reported where it first failed (line
// 6), not at the `throw e`.
let u;
function f() {
  try {
    u.x;
  } catch (e) {
    throw e;
  }
}
f();

// Another error type, thrown inside a function: node reports
// "RangeError: too deep", at the throw.
function check(n) {
  if (n > 3) {
    throw new RangeError("too deep");
  }
}
check(4);

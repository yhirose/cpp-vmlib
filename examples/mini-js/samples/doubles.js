// How a number prints: the shortest digits that round trip, in
// JavaScript's layout. Plain digits from 1e-6 up to (not including) 1e21,
// an exponent outside that ("1e-7", "1e+21"), never a ".0". String(n), a
// concatenation and join agree; console.log(-0) is "-0" where
// String(-0) is "0".
const big = 1e300 * 1e300;
const nan = big - big;
const values = [
  0, -0, 0.1, 0.5, 1, 100000, 123456789,
  0.0001, 0.00001, 0.000001, 1e-7, 1.5e-10, 0.00012345,
  999999999999999, 1e15, 1e16, 1e17, 1e20, 1e21, 1e22,
  12345678901234567, 0.1 + 0.2, 1 / 3, 2.5e-5,
  5e-324, 1.7976931348623157e308, 9007199254740992, 18446744073709552000,
  big, -big, nan, -1.5,
];
for (const v of values) {
  console.log(String(v));
}
console.log(values.slice(7, 13).join(" "));
console.log(String(1e21) + " " + String(0.0000001) + " " + String(100000) + " " + String(-0));
console.log("a" + 1e21 + " " + 1e-7);
console.log(-0, 0, 1e21, 1e-7);
console.log(-0);

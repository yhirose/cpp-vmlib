// How a number prints: the shortest digits that round trip, in
// JavaScript's layout. Plain digits from 1e-6 up to (not including) 1e21,
// an exponent outside that ("1e-7", "1e+21"), never a ".0". String(n), a
// concatenation and join agree; console.log(-0) is "-0" where
// String(-0) is "0".
const big = 1e300 * 1e300;
const nan = big - big;
const values = [
  0, -0, 0.1, 100000, 0.000001, 1e-7, 1e20, 1e21,
  12345678901234567, 5e-324, big, -big, nan,
];
for (const v of values) {
  console.log(String(v));
}
console.log(values.slice(4, 8).join(" "));
console.log(String(1e21) + " " + String(0.0000001) + " " + String(100000) + " " + String(-0));
console.log("a" + 1e21 + " " + 1e-7);
console.log(-0, 0, 1e21, 1e-7);
console.log(-0);

// Rest parameters and spread. A function's `...rest` is the IR's
// Func::rest_param, and a call with `...xs` among its arguments is a
// Tag::CallSpread over the arguments before it and one array of the rest:
// one at each end of a call whose count is decided when it runs.

function third(a, b, c) { return c; }
function count(...xs) { return xs.length; }
function tail(first, ...rest) { return fmt([first, rest]); }

// Rest parameters, in each kind of function.
console.log(count(), count(1, 2, 3), tail(1), tail(1, 2, 3));
const arrow = (...xs) => xs.length;
console.log(arrow(), arrow('a', 'b'));
function* each(...xs) { for (const x of xs) yield x; }
const seen = [];
for (const x of each(4, 5, 6)) seen.push(x);
console.log(fmt(seen));
// A rest array is a new one on every call, even from a spread.
const xs = [1, 2, 3];
function same(...ys) { return ys === xs; }
console.log(same(...xs), count(...xs));

// Spread, anywhere among the arguments and as many times as you like.
console.log(third(...xs), third(0, ...xs), third(...[9], 8, ...[7]));
console.log(third(...[], 1, 2, 3), count(...xs, ...xs), count(...[]));
console.log(fmt([...xs]), fmt([0, ...xs, 4]), fmt([...xs, ...xs]));
const copy = [...xs];
copy.push(99);
console.log(fmt(xs), fmt(copy));
// Each spread is read where it stands, before the arguments after it run.
const grow = [1];
console.log(count(...grow, grow.push(2), ...grow));

// Methods, constructors and arrows over a spread.
class Point {
  constructor(...coords) { this.coords = coords; }
  sum(...more) {
    let s = 0;
    for (const c of this.coords) s += c;
    for (const m of more) s += m;
    return s;
  }
}
const pt = new Point(...[1, 2], 3);
console.log(fmt(pt.coords), pt.sum(), pt.sum(...[10, 20]));
const obj = { f: (...args) => args.length };
console.log(obj.f(...xs, 4));

// console.log and push take any number of values, a spread among them.
console.log(...xs);
console.log('x', ...xs, 'y');
const arr = [];
console.log(arr.push(1, 2), arr.push(...xs), arr.push(...arr), fmt(arr));

// An async function's rest parameter reaches its body.
async function later(...vals) {
  await null;
  return vals.length;
}
later(...xs, 4).then((n) => console.log('later', n));

// Recursion that hands its rest parameter on by spreading it.
function down(n, ...rest) {
  if (n === 0) return rest.length;
  return down(n - 1, ...rest);
}
console.log(down(500, 'a', 'b'));

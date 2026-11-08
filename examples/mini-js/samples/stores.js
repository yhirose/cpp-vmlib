// o[k] and o[k] = v: a property the object has, lacks, or holds undefined
// under (found, so the prototype chain is not asked); an array's element,
// at the end and past it; a store that is itself a value; a Map's size.
const o = {a: 1};
o.a = 5;
o.b = 6;
o["c"] = 7;
console.log(o.a, o.b, o.c, o.zzz);

// undefined stored is found: the class's property of that name is shadowed
class P {
  constructor() { this.x = 1; }
  get() { return 7; }
}
const p = new P();
console.log(typeof p.get, p.x, p.nope);
p.get = undefined;
console.log(p.get);
p.x = 2;
p.y = 3;
console.log(p.x, p.y);

const u = {};
u.k = undefined;
console.log(u.k);
u.k = 3;
console.log(u.k);

// arrays: in range, at the length, and an element that is not there
const xs = [10, 20, 30];
xs[0] = 11;
xs[3] = 40;
console.log(xs[0], xs[1], xs[3], xs[4], xs.length);

// an assignment is its value
let y;
y = o.a = 9;
console.log(y, o.a);
function put(a, k, v) { return a[k] = v; }
console.log(put(o, "d", 4), o.d, put(xs, 4, 50), xs[4]);
console.log(put(xs, 1, 21), xs[1]);

// compound stores read then write, each of the receiver and key once
let calls = 0;
function which() { calls++; return o; }
which().a += 10;
which()["b"] *= 2;
console.log(o.a, o.b, calls);

// a Map's size is the count, and a store to it keeps nothing
const m = new Map();
m.set("a", 1);
m.size = 5;
console.log(m.size);
m.set("b", 2);
console.log(m.size, m.get("a"));
const s = new Set([1, 2]);
s.size = 9;
console.log(s.size);

// numeric keys on an object are strings
const n = {};
n[1] = "one";
n[2.5] = "two and a half";
console.log(n[1], n["1"], n[2.5], Object.keys(n).join(","));

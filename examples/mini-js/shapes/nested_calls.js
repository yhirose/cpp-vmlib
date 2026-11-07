// A method call's arguments are compiled once, however deep the calls
// nest: the call is made with `this` or without it, and each of the two
// reads the arguments from where they were put, rather than compiling
// them a second time -- which, five calls deep, put each constant in the
// bytecode 32 times. A spread is the same.
class A {
  f(x) { return x + 1; }
}
const a = new A();
console.log(a.f(a.f(a.f(a.f(a.f(7777))))));
console.log(a.f(...[a.f(a.f(8888))]));

// `class`, `this` and `new`. A class's methods are its prototype, one
// closure per member, built at the class declaration's own lexical
// position -- the same reason examples/mini-python's and
// examples/mini-ruby's method tables are built where the `class`/`def`
// stands rather than at `new`. An instance holds its fields; a call
// through it finds the method on its class and hands it the instance as
// `this`.

class Point {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
  norm2() {
    return this.x * this.x + this.y * this.y;
  }
  label(tag) {
    return tag + '(' + this.x + ', ' + this.y + ')';
  }
  scaled(k) {
    return new Point(this.x * k, this.y * k);
  }
}

const p = new Point(3, 4);
show('fields', [p.x, p.y]);
show('norm2', p.norm2());
show('label', p.label('P'));
show('chained', p.scaled(2).norm2());
show('constructor property', p.constructor === Point);

// A method reassigned on one instance does not touch another -- the one
// instance's own property shadows its class's method, and only there.
const q = new Point(1, 1);
q.norm2 = function () { return 'shadowed'; };
show('shadowed vs plain', [q.norm2(), p.norm2()]);

// this inside a method sees the instance it was called on, and an arrow
// defined inside a method closes over it lexically.
class Counter {
  constructor() {
    this.n = 0;
  }
  bump() {
    this.n = this.n + 1;
    return this;
  }
  bumper() {
    return () => {
      this.n = this.n + 1;
      return this.n;
    };
  }
}

const c = new Counter();
c.bump().bump();
show('chained mutation', c.n);
const inc = c.bumper();
show('arrow closes over this', [inc(), inc(), c.n]);

// Instances in an array, each answering for itself.
const pts = [];
for (let i = 1; i <= 3; i = i + 1) {
  pts.push(new Point(i, i));
}
const norms = [];
for (const one of pts) {
  norms.push(one.norm2());
}
show('array of instances', norms);

// A class closing over what was around it when it was declared.
function makeAdder(base, step) {
  class Adder {
    constructor(step) {
      this.step = step;
    }
    apply(v) {
      return v + base + this.step;
    }
  }
  return new Adder(step).apply(1);
}
show('class over an outer variable', makeAdder(10, 5));

// `this` is scoped to a class method here, not to every function called
// through a property the way real JavaScript's dynamic binding is -- an
// object literal's own function property still works exactly as it did
// before classes existed, closing over a variable rather than reading a
// receiver.
let count = 0;
const plain = {
  inc: function () { count = count + 1; return count; },
};
show('plain object method', [plain.inc(), plain.inc()]);

// A property set on the class is the class's own -- a static, in
// JavaScript -- and not what its instances' methods are.
class Greeter {
  hello() { return 'hi'; }
}
const early = new Greeter();
Greeter.hello = function () { return 'static'; };
const late = new Greeter();
show('static apart', [early.hello(), late.hello(), Greeter.hello()]);

// Even one named like where the class once kept its methods.
Greeter.$methods = {};
show('static named like the table', [new Greeter().hello()]);
// A class's own keys are its statics, and nothing it keeps for itself.
show('class keys', Object.keys(Greeter));

// A constructor that returns an object makes that the result of `new`;
// one that returns anything else still gives the instance.
class Swap {
  constructor(v) { this.mine = true; return v; }
}
show('constructor result', [new Swap({ other: 1 }).other,
                            new Swap([7]).length, new Swap(5).mine,
                            new Swap('s').mine, new Swap().mine]);

// An instance holds its fields and nothing else -- its methods are its
// class's, reached through it -- so Object.keys lists the fields alone,
// and a printed instance shows them alone.
show('instance keys', Object.keys(new Point(1, 2)));
show('instance', new Point(1, 2));

// `new` reads the class it is handed when it runs, so a class reached
// through any name constructs, one a function returned included.
function makeAdderClass(base) {
  class Adder {
    constructor(step) {
      this.step = step;
    }
    apply(v) {
      return v + base + this.step;
    }
  }
  return Adder;
}
const Add10 = makeAdderClass(10);
show('new through a returned class', new Add10(5).apply(1));

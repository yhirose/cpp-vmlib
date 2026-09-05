// `class`, `this` and `new`. A class here is a plain object holding one
// closure per member (`constructor` included, under that name), built at
// the class declaration's own lexical position -- the same reason
// examples/mini-python's and examples/mini-ruby's method tables are built
// where the `class`/`def` stands rather than at `new`. `new ClassName(...)`
// copies every key the table has onto a fresh object and calls whichever
// one landed under `constructor`, so it never needs to know in advance
// what a class declares.

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

// A method reassigned on one instance does not touch another -- each
// instance got its own copy of the closures at construction time.
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

// A class closing over what was around it when it was declared. `new`
// needs a class it can see by name at the call site (see README.md), so
// this instantiates `Adder` where it is declared rather than through an
// alias a function returned.
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

// What the VM itself traps on reaches `catch` as the Error V8 throws for the
// same fault: a name and a message. JavaScript coerces where the VM refuses,
// so the faults left are a property of undefined, a call of a non-function
// and the stack. A call's message names the callee as V8 prints it
// ("o.f is not a function"): the binder spells the callee and the VM hands
// it back with the trap (README, "A call of a non-function"). null is
// undefined here, so the properties are read from undefined.

function full(f) {
  try {
    f();
  } catch (e) {
    console.log(e.name + ": " + e.message);
  }
}

let u;
const o = {};
const five = 5;

full(() => u.x);
full(() => o.a.b);
full(() => u[0]);
full(() => { u.y = 1; });
full(() => { u[2] = 1; });

// The callee's spelling, one shape each. A string index prints as a member.
const xs = [1];
const k = 0;
const q = { a: {}, g: () => ({}) };
class C {}
class H {
  m() {
    return this.zz();
  }
}
full(() => o.f());
full(() => five());
full(() => u());
full(() => q.a.b());
full(() => xs[0]());
full(() => xs[k]());
full(() => o["f"]());
full(() => q.g().z());
full(() => q.g()());
full(() => (o.f)());
full(() => (1)());
full(() => new C().zz());
full(() => five(...xs));
full(() => o.f(...xs));
full(() => new H().m());

function forever() {
  forever();
}
full(forever);

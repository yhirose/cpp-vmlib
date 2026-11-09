// What the VM itself traps on reaches `catch` as the Error V8 throws for the
// same fault: a name and a message. JavaScript coerces where the VM refuses,
// so the faults left are a property of undefined, a call of a non-function
// and the stack. A call's message names the callee ("o.f is not a function"),
// which the VM does not know, so only the name is shown; and null is
// undefined here, so the properties are read from undefined.
function name(f) {
  try {
    f();
  } catch (e) {
    console.log(e.name);
  }
}

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

name(() => o.f());
name(() => five());
name(() => u());

function forever() {
  forever();
}
full(forever);

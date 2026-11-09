// A trap the program does not catch ends it with the Error node would
// throw: "TypeError: Cannot read properties of undefined (reading 'x')"
// (node adds the source line and a stack).
let u;
u.x;

// A string cannot hold a raw line break (it needs a backslash before it):
// node says "SyntaxError: Invalid or unexpected token".
console.log("a
b");

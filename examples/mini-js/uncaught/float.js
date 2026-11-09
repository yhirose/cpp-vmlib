// A number that is not an Error is reported as String(n) writes it: node
// says "1e-7", where the shortest-digits text of the VM would say 1e-07.
throw 1e-7;

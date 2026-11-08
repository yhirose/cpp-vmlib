function run(n) {
  let xs = [];
  for (let k = 0; k < 1000; k++) xs.push(0);
  let d = {a: 0, b: 0};
  let i = 0;
  while (i < n) {
    xs[i % 1000] = i;
    d["a"] = i;
    d["b"] = i * 2;
    i = i + 1;
  }
  return xs[7] + xs[999] + d["a"] + d["b"];
}
console.log(run(200000));

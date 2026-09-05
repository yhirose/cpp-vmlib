// async / await and a Promise, over the library's three scheduler
// primitives: CoroCreate, CoroYield and Enqueue. Nothing about promises
// is in the library -- what is in there is a coroutine that can park a
// whole stack of frames and a FIFO of jobs to wake it from, and the rest
// (what settles what, in what order) is written in this front end's own
// IR because it is JavaScript's rule and not every language's.

const log = [];
function tick(s) { log.push(s); }

// An async function's body runs synchronously up to its first `await`,
// and the call returns a promise at that point -- so 'body start' is
// logged before 'after call', and 'body end' long after it.
async function body() {
  tick('body start');
  await null;
  tick('body end');
  return 'body value';
}
const p = body();
tick('after call');
show('call answers a promise', typeof p);

async function add(a, b) { return a + (await b); }

async function main() {
  tick('main start');
  show('awaited', await p);
  show('awaited a value', await 41 + 1);
  show('nested await', await add(1, Promise.resolve(2)));

  // A rejected promise arrives as a throw at the await.
  try {
    await Promise.reject(new Error('rejected'));
    tick('not reached');
  } catch (e) {
    show('caught from await', e.message);
  }

  // The executor form: resolve is a closure over the promise's own cell.
  const made = await new Promise(function (resolve) {
    tick('executor ran');
    resolve('from executor');
  });
  show('executor', made);

  // A chain, with a handler-less link the rejection passes straight
  // through, a handler that throws, and a catch that recovers.
  const chained = await Promise.resolve(1)
    .then((v) => v + 1)
    .then((v) => { throw new Error('at ' + String(v)); })
    .then((v) => 'skipped ' + String(v))
    .catch((e) => 'recovered from ' + e.message);
  show('chain', chained);

  // Returning a promise from a handler adopts it rather than nesting it.
  const adopted = await Promise.resolve('x').then(() => Promise.resolve('inner'));
  show('adopted', adopted);

  tick('main end');
  return 'main value';
}

main().then((v) => {
  show('main resolved', v);
  show('order', log);
});
tick('sync tail');

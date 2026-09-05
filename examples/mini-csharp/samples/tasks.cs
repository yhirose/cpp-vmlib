// async / await over Task. The lowering is the same three funcs
// examples/mini-js uses for a Promise -- the method the source declared
// (which builds the Task, spawns a coroutine and returns), the body, and
// the coroutine entry that settles the Task -- which is worth noticing:
// the statically-typed language and the dynamic one wanted exactly the
// same thing from CoroCreate, CoroYield and Enqueue.

using System;
using System.Collections.Generic;
using System.Threading.Tasks;

class Program {
  static List<string> Log;

  static void Note(string s) {
    Log.Add(s);
  }

  static async Task<int> AddAsync(int a, int b) {
    Note("add start");
    await Task.Yield();
    Note("add resumed");
    return a + b;
  }

  static async Task<string> Chain(int n) {
    int v = await AddAsync(n, 1);
    int w = await AddAsync(v, 10);
    return "chained " + w;
  }

  static async Task Boom() {
    await Task.Yield();
    throw new Exception("from async");
  }

  static async Task<int> Caught() {
    try {
      await Boom();
      return -1;
    } catch (Exception e) {
      Note("caught " + e.Message);
      return 0;
    }
  }

  // An async method's body runs synchronously up to its first `await`,
  // which is why "body start" is noted before "after call".
  static async Task<int> Eager() {
    Note("body start");
    await Task.Yield();
    Note("body end");
    return 1;
  }

  static async Task Run() {
    Note("run start");
    Console.WriteLine(await AddAsync(1, 2));
    Console.WriteLine(await Chain(1));
    Console.WriteLine(await Caught());

    // A value awaited that is not a Task settles immediately.
    Console.WriteLine(await Task.FromResult(99));

    Note("run end");
  }

  static void Main() {
    Log = new List<string>();

    Task<int> eager = Eager();
    Note("after call");
    Console.WriteLine(eager.Result);

    Run().Wait();
    Console.WriteLine(string.Join(", ", Log));
  }
}

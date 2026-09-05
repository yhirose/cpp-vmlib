// `yield return`. In C# the compiler builds a state machine for an
// iterator method; here it is Func::is_generator and Tag::Yield, and the
// state machine is the suspended frame the runtime already had -- which
// is the point of the top-level README listing `yield return` among the
// things a managed language gets "without changes".

using System;
using System.Collections.Generic;

class Program {
  static IEnumerable<int> Squares(int n) {
    for (int i = 0; i < n; i++) {
      yield return i * i;
    }
  }

  static IEnumerable<int> Countdown(int from) {
    while (from > 0) {
      yield return from;
      from = from - 1;
    }
  }

  static IEnumerable<string> Interleave(int n) {
    for (int i = 0; i < n; i++) {
      yield return "a" + i;
      yield return "b" + i;
    }
  }

  // An iterator is lazy: nothing in the body runs until the loop asks.
  static List<string> Trace;

  static IEnumerable<int> Noisy() {
    Trace.Add("started");
    yield return 1;
    Trace.Add("middle");
    yield return 2;
    Trace.Add("finished");
  }

  // `yield break` ends it early.
  static IEnumerable<int> UpTo(int limit) {
    for (int i = 0; i < 100; i++) {
      if (i >= limit) {
        yield break;
      }
      yield return i;
    }
  }

  // One iterator driving another, which is how they compose.
  static IEnumerable<int> Doubled(IEnumerable<int> src) {
    foreach (int v in src) {
      yield return v * 2;
    }
  }

  static string Join(IEnumerable<int> xs) {
    List<int> out1 = new List<int>();
    foreach (int v in xs) {
      out1.Add(v);
    }
    return string.Join(",", out1);
  }

  static void Main() {
    Console.WriteLine(Join(Squares(5)));
    Console.WriteLine(Join(Countdown(4)));
    Console.WriteLine(Join(UpTo(3)));
    Console.WriteLine(Join(Doubled(Squares(4))));

    List<string> words = new List<string>();
    foreach (string s in Interleave(2)) {
      words.Add(s);
    }
    Console.WriteLine(string.Join(" ", words));

    Trace = new List<string>();
    IEnumerable<int> lazy = Noisy();
    Console.WriteLine("before: " + string.Join(",", Trace));
    int total = 0;
    foreach (int v in lazy) {
      total = total + v;
    }
    Console.WriteLine("after: " + string.Join(",", Trace) + " total " + total);

    // Leaving a foreach early stops the iterator where it is.
    List<int> partial = new List<int>();
    foreach (int v in Squares(100)) {
      if (v > 9) {
        break;
      }
      partial.Add(v);
    }
    Console.WriteLine(string.Join(",", partial));

    // foreach over an ordinary List goes through the same path.
    List<int> plain = new List<int>();
    plain.Add(7);
    plain.Add(8);
    int sum = 0;
    foreach (int v in plain) {
      sum = sum + v;
    }
    Console.WriteLine(sum + " " + plain.Count);
  }
}

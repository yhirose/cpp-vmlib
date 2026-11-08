// Add and Wait are a List's, a Dictionary's or a Task's, or a class's own
// method of the name; which runs is known only once the receiver is, and
// each of the three ways is a call of its own. The arguments run once,
// into slots each way reads, rather than compiled into each: three Adds
// deep, the constant is in the bytecode once, where it was 27 times, and
// a Dictionary's Add over one is 6.
using System;
using System.Collections.Generic;

class Bag {
  public int Add(int x) { return x + 1; }
}

class Program {
  static void Main() {
    var b = new Bag();
    Console.WriteLine(b.Add(b.Add(b.Add(7777))));
    var d = new Dictionary<string, int>();
    d.Add("k", b.Add(8888));
    Console.WriteLine(d["k"]);
  }
}

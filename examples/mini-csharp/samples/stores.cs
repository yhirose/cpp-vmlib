// a[i] = v and d[k] = v: an element in range and out of it, a Dictionary
// key it has, lacks or holds null under (found, so no KeyNotFoundException),
// fields of an object, and an assignment that is itself a value.

using System;
using System.Collections.Generic;

class Box {
  public int n;
  public string label;
  public Box(int start) { n = start; }
}

class Stores {
  static int Calls = 0;
  static Dictionary<string, string> Table = new Dictionary<string, string>();
  static Dictionary<string, string> Which() { Calls++; return Table; }

  static void Main() {
    List<int> xs = new List<int> { 0, 0, 0 };
    xs[0] = 11;
    xs[2] = 33;
    Console.WriteLine(xs[0] + " " + xs[1] + " " + xs[2]);
    try {
      xs[3] = 1;
    } catch (Exception) {
      Console.WriteLine("out of range");
    }
    try {
      xs[-1] = 1;
    } catch (Exception) {
      Console.WriteLine("out of range");
    }

    Dictionary<string, string> d = new Dictionary<string, string>();
    d["a"] = "x";
    d["b"] = "y";
    d["a"] = "z";
    Console.WriteLine(d["a"] + d["b"] + d.Count);
    d["n"] = null;
    Console.WriteLine(d["n"] == null);
    Console.WriteLine(d.Count);
    try {
      string s = d["missing"];
      Console.WriteLine(s);
    } catch (Exception) {
      Console.WriteLine("KeyNotFound");
    }

    Dictionary<int, int> sq = new Dictionary<int, int>();
    for (int i = 0; i < 5; i++) sq[i] = i * i;
    sq[2] = 0;
    Console.WriteLine(sq[1] + sq[2] + sq[4] + sq.Count);

    Box b = new Box(1);
    b.n = 2;
    b.label = "boxed";
    b.n += 5;
    Console.WriteLine(b.n + b.label);
    Box none = null;
    try {
      none.n = 1;
    } catch (Exception) {
      Console.WriteLine("null receiver");
    }

    // an assignment is its value
    int y;
    y = xs[1] = 7;
    Console.WriteLine(y + " " + xs[1]);
    Console.WriteLine(b.n = 9);
    Console.WriteLine(d["c"] = "fresh");
    Console.WriteLine(d["c"]);

    // compound stores run the receiver once
    Table["k"] = "a";
    Which()["k"] += "b";
    Console.WriteLine(Table["k"] + Calls);
  }
}

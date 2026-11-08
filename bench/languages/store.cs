using System;
using System.Collections.Generic;

class Program {
  static long Run(long n) {
    List<long> xs = new List<long>();
    long k = 0;
    while (k < 1000) {
      xs.Add(0);
      k = k + 1;
    }
    Dictionary<string, long> d = new Dictionary<string, long>();
    d["a"] = 0;
    d["b"] = 0;
    long i = 0;
    while (i < n) {
      xs[(int)(i % 1000)] = i;
      d["a"] = i;
      d["b"] = i * 2;
      i = i + 1;
    }
    return xs[7] + xs[999] + d["a"] + d["b"];
  }

  static void Main() {
    Console.WriteLine(Run(200000));
  }
}

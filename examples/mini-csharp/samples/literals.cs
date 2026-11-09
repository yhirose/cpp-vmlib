// Integer literals. A literal is typed by its value (int, uint, long,
// ulong), and the minus in front of one is folded into it, which is how
// the smallest long is written at all. (A literal past long needs a ulong,
// which this front end has not: see errors/.)
using System;

class Program {
  static void Main() {
    long max = 9223372036854775807;
    long min = -9223372036854775808;
    long min2 = -9223372036854775807 - 1;
    Console.WriteLine(max);
    Console.WriteLine(min);
    Console.WriteLine(min2);
    Console.WriteLine(-2147483648);
    Console.WriteLine(2147483648);
    Console.WriteLine(5 - -3);
    Console.WriteLine(-(-7));
  }
}

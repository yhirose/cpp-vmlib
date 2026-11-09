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

    // String escapes. \x takes up to four hex digits, greedily: "\x41B" is
    // one character (U+041B). Only lengths are shown for the control
    // characters.
    Console.WriteLine("\x41\x4a\x4A\x0041B\x41g");
    Console.WriteLine("\x41" + "B");
    Console.WriteLine("\x4g".Length);
    Console.WriteLine("\u0041\u00e9\u20AC\U0001F600\uD83D\uDE00");
    Console.WriteLine("\uD83D\uDE00" == "\U0001F600");
    Console.WriteLine("\a\b\f\n\r\t\v\0\\\"\'".Length);
    Console.WriteLine("\0123".Length);
    Console.WriteLine("it\'s \"quoted\"");
  }
}

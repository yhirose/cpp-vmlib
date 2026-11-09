// How a double prints: the shortest digits that round trip, in C#'s
// layout. Plain digits from 1E-4 up to (not including) 1E17, an exponent
// outside that with a capital E and a two-digit minimum ("1E+17",
// "1E-05"), never a ".0", and -0 as "-0". Console.WriteLine, ToString and
// string concatenation agree.
using System;
using System.Collections.Generic;

class Program {
  static void Main() {
    double big = 1.0e300 * 1.0e300;
    List<double> values = new List<double> {
      0.0, -0.0, 0.1, 100000.0, 0.0001, 0.00001, 1.0e16, 1.0e17,
      12345678901234567.0, 5.0e-324, 1.7976931348623157e308, -1.5,
    };
    foreach (double v in values) {
      Console.WriteLine(v);
    }
    Console.WriteLine(1.0e17.ToString() + " " + 0.00001 + " " + 100000.0);
    Console.WriteLine("" + 1.0e17 + " " + 0.00001 + " " + 100000.0 + " " + -0.0);
    Console.WriteLine(big > 1.0e308);
  }
}

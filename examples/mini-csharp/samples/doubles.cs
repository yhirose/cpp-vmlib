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
      0.0, -0.0, 0.1, 0.5, 1.0, 100000.0, 123456789.0,
      0.0001, 0.00001, 0.00012345, 1.0e-7, 1.5e-10,
      999999999999999.0, 1.0e15, 1.0e16, 1.0e17, 1.0e20, 1.0e22,
      12345678901234567.0, 0.1 + 0.2, 1.0 / 3.0, 2.5e-5,
      5.0e-324, 1.7976931348623157e308, 9007199254740992.0,
      18446744073709551616.0, -1.5,
    };
    foreach (double v in values) {
      Console.WriteLine(v);
    }
    Console.WriteLine(1.0e17.ToString() + " " + 0.00001 + " " + 100000.0);
    Console.WriteLine("" + 1.0e17 + " " + 0.00001 + " " + 100000.0 + " " + -0.0);
    Console.WriteLine(big > 1.0e308);
  }
}

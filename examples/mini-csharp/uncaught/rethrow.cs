// A trap caught and thrown again is reported where it first failed (line
// 9), not at the `throw e`.
using System;

class Program {
  static void Main() {
    int zero = 0;
    try {
      Console.WriteLine(1 / zero);
    } catch (Exception e) {
      throw e;
    }
  }
}

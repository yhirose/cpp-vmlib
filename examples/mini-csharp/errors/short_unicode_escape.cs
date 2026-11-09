// \u takes exactly four hex digits: "error CS1009: Unrecognized escape sequence".
using System;

class Program {
  static void Main() {
    Console.WriteLine("\u43");
  }
}

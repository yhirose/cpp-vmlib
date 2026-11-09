// \U takes a code point up to 10FFFF: "error CS1009: Unrecognized escape sequence".
using System;

class Program {
  static void Main() {
    Console.WriteLine("\U00110000");
  }
}

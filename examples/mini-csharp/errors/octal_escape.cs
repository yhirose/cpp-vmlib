// C# has no octal escapes: \101 is "error CS1009: Unrecognized escape sequence".
using System;

class Program {
  static void Main() {
    Console.WriteLine("\101");
  }
}

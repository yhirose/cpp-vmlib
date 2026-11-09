// There is no \q: csharp says "error CS1009: Unrecognized escape sequence".
using System;

class Program {
  static void Main() {
    Console.WriteLine("a\qb");
  }
}

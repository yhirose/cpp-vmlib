// 9223372036854775808 is a ulong in C# (long.MaxValue + 1), which dotnet
// prints as is. This front end has no unsigned 64-bit integer, so the
// literal is refused where it is read rather than clamped to
// 9223372036854775807.
using System;

class Program {
  static void Main() {
    Console.WriteLine(9223372036854775808);
  }
}

// A regular string literal cannot hold a line break: "error CS1010: Newline in constant".
using System;

class Program {
  static void Main() {
    Console.WriteLine("a
b");
  }
}

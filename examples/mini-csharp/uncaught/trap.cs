// A trap the program does not catch ends it with the exception .NET would
// throw: dotnet reports "Unhandled exception. System.DivideByZeroException:
// Attempted to divide by zero." (and a stack trace).
using System;

class Program {
  static void Main() {
    int zero = 0;
    Console.WriteLine(1 / zero);
  }
}

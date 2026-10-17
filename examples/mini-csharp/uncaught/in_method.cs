// Thrown in a method Main calls: dotnet reports
// "Unhandled exception. System.Exception: negative: -3", at the throw.
using System;

class Program {
  static void Check(int n) {
    if (n < 0) {
      throw new Exception("negative: " + n);
    }
  }

  static void Main() {
    Check(-3);
  }
}

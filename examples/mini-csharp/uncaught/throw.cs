// An exception thrown in Main: dotnet reports
// "Unhandled exception. System.Exception: boom".
using System;

class Program {
  static void Main() {
    throw new Exception("boom");
  }
}

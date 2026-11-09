// What the VM itself traps on reaches `catch` as the exception .NET throws
// for the same fault, with .NET's message. A catch here names no type (every
// catch takes everything), so the class is shown by the uncaught cases in
// uncaught/; the messages are .NET 8's.
using System;
using System.Collections.Generic;

class Node {
  public int value;
}

class Traps {
  static void Main() {
    int zero = 0;
    List<int> xs = new List<int> { 1, 2, 3 };
    string s = "abc";
    string none = null;
    Node node = null;
    Dictionary<string, int> d = new Dictionary<string, int>();

    try { Console.WriteLine(10 / zero); }
    catch (Exception e) { Console.WriteLine(e.Message); }

    try { Console.WriteLine(10 % zero); }
    catch (Exception e) { Console.WriteLine(e.Message); }

    try { Console.WriteLine(xs[5]); }
    catch (Exception e) { Console.WriteLine(e.Message); }

    try { xs[-1] = 0; }
    catch (Exception e) { Console.WriteLine(e.Message); }

    try { Console.WriteLine(s[7]); }
    catch (Exception e) { Console.WriteLine(e.Message); }

    try { Console.WriteLine(node.value); }
    catch (Exception e) { Console.WriteLine(e.Message); }

    try { node.value = 1; }
    catch (Exception e) { Console.WriteLine(e.Message); }

    try { Console.WriteLine(none.Length); }
    catch (Exception e) { Console.WriteLine(e.Message); }

    try { Console.WriteLine(d["missing"]); }
    catch (Exception e) { Console.WriteLine(e.Message); }

    // A catch that is never reached by a trap leaves the program alone.
    try { Console.WriteLine(10 / 5); }
    catch (Exception) { Console.WriteLine("unreachable"); }
  }
}

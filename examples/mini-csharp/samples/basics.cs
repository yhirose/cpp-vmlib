// The operator set, and the four places where C#'s answer is already the
// VM's answer. `/` and `%` on two ints truncate toward zero, which is
// what BinOp::Div and BinOp::Mod do; a whole double prints without a
// point, which is what to_display does; and int + int stays int while
// int + double widens, which is what eval_binop does. So unlike every
// other front end here, this one carries no arithmetic helper and no
// float-formatting rule -- see README.md.

using System;
using System.Collections.Generic;

class Program {
  static int Fib(int n) {
    if (n < 2) {
      return n;
    }
    return Fib(n - 1) + Fib(n - 2);
  }

  static string Classify(int n) {
    return n < 0 ? "negative" : n == 0 ? "zero" : "positive";
  }

  static void Main() {
    // Integer division and remainder, including the negative cases that
    // separate truncation from flooring.
    Console.WriteLine(7 / 2);
    Console.WriteLine(-7 / 2);
    Console.WriteLine(7 % 3);
    Console.WriteLine(-7 % 3);

    // One double anywhere widens the whole expression.
    Console.WriteLine(1.0 / 4);
    Console.WriteLine(8.0 / 4);
    Console.WriteLine(1 + 2 * 3);
    Console.WriteLine((double)7 / 2);
    Console.WriteLine((int)3.9);
    Console.WriteLine((int)(-3.9));

    // Only bool is a condition here, so there is nothing to decide about
    // whether 0 or "" is false.
    bool a = true;
    bool b = false;
    Console.WriteLine(a && !b);
    Console.WriteLine(a || b);
    Console.WriteLine(!a);
    Console.WriteLine(3 < 4);
    Console.WriteLine(4 <= 4);

    string s = "hello";
    Console.WriteLine(s + ", " + "world");
    Console.WriteLine(s.Length);
    Console.WriteLine(s == "hello");
    Console.WriteLine(s != "world");
    Console.WriteLine("count: " + 3 + "!");
    Console.WriteLine("sum: " + (1 + 2));

    Console.WriteLine(Classify(-2));
    Console.WriteLine(Classify(0));
    Console.WriteLine(Classify(9));

    int total = 0;
    for (int i = 1; i <= 10; i++) {
      if (i % 3 == 0) {
        continue;
      }
      if (i > 8) {
        break;
      }
      total += i;
    }
    Console.WriteLine(total);

    int n = 5;
    int fact = 1;
    while (n > 1) {
      fact *= n;
      n--;
    }
    Console.WriteLine(fact);

    Console.WriteLine(Fib(15));

    List<int> xs = new List<int> { 3, 1, 2 };
    xs.Add(4);
    Console.WriteLine(xs.Count);
    Console.WriteLine(xs[0] + xs[3]);
    xs[0] = 30;
    int sum = 0;
    foreach (int x in xs) {
      sum += x;
    }
    Console.WriteLine(sum);
    Console.WriteLine(string.Join("-", xs));

    List<string> words = new List<string>();
    words.Add("one");
    words.Add("two");
    Console.WriteLine(string.Join(" ", words));
  }
}

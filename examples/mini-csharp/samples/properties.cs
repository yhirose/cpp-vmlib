// A user-declared property: an auto property backed by a hidden field the
// binder invents, and a full `get`/`set` with a body of its own. Both
// compile to `get_Name`/`set_Name` methods -- literally what the real C#
// compiler names them -- found by the same `$mfind` walk a method call
// uses, which is why `override` on one needs no code of its own. See
// README.md.

using System;

class Counter {
  public int Value { get; set; } = 0;

  public Counter() {
  }

  public void Bump() {
    Value = Value + 1;
  }
}

class Temperature {
  private double celsius;

  public double Celsius {
    get { return celsius; }
    set { celsius = value; }
  }

  public double Fahrenheit {
    get { return celsius * 9.0 / 5.0 + 32.0; }
    set { celsius = (value - 32.0) * 5.0 / 9.0; }
  }
}

class Shape {
  public virtual string Label {
    get { return "shape"; }
  }
}

class Square : Shape {
  public double Side { get; set; }

  public Square(double side) {
    Side = side;
  }

  public override string Label {
    get { return "square of side " + Side; }
  }
}

class Program {
  static void Main() {
    Counter c = new Counter();
    Console.WriteLine(c.Value);
    c.Bump();
    c.Bump();
    Console.WriteLine(c.Value);
    c.Value += 10;
    Console.WriteLine(c.Value);

    Temperature t = new Temperature();
    t.Celsius = 100.0;
    Console.WriteLine(t.Fahrenheit);
    t.Fahrenheit = 32.0;
    Console.WriteLine(t.Celsius);

    Shape s = new Square(3.0);
    Console.WriteLine(s.Label);
  }
}

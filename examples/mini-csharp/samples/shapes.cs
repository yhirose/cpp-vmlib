// Classes, inheritance and virtual dispatch -- which no other front end
// here has. The mechanism is one link: a class table carries its base's
// table, and a method lookup walks the chain. `override` is then not a
// feature, it is what happens when the walk finds the derived entry first.

using System;
using System.Collections.Generic;

class Shape {
  public string Name;

  public Shape(string name) {
    Name = name;
  }

  public virtual double Area() {
    return 0.0;
  }

  public virtual string Kind() {
    return "shape";
  }

  // Calls a virtual method from the *base* class, so what runs depends on
  // what the object actually is -- the whole point of the exercise.
  public string Describe() {
    return Name + " is a " + Kind() + " of area " + Area();
  }
}

class Rect : Shape {
  public double W;
  public double H;

  public Rect(string name, double w, double h) : base(name) {
    W = w;
    H = h;
  }

  public override double Area() {
    return W * H;
  }

  public override string Kind() {
    return "rectangle";
  }
}

class Square : Rect {
  public Square(string name, double side) : base(name, side, side) {
  }

  // Two levels down: this overrides Rect's override, and Area comes from
  // Rect without being restated.
  public override string Kind() {
    return "square";
  }
}

class Circle : Shape {
  public double R;

  public Circle(string name, double r) : base(name) {
    R = r;
  }

  public override double Area() {
    return 3.0 * R * R;
  }

  public override string Kind() {
    return "circle";
  }
}

class Plain : Shape {
  // No constructor of its own: the synthesized one still chains to the
  // base's.
  public Plain() : base("plain") {
  }
}

class Program {
  static void Main() {
    Shape s = new Shape("s0");
    Console.WriteLine(s.Describe());

    Shape r = new Rect("r1", 3.0, 4.0);
    Console.WriteLine(r.Describe());
    Console.WriteLine(r.Area());
    Console.WriteLine(r.Kind());

    Shape q = new Square("q1", 5.0);
    Console.WriteLine(q.Describe());
    Console.WriteLine(q.Area());

    Shape c = new Circle("c1", 2.0);
    Console.WriteLine(c.Describe());

    Console.WriteLine(new Plain().Describe());

    // A collection of base-typed references, each answering for itself.
    List<Shape> all = new List<Shape>();
    all.Add(r);
    all.Add(q);
    all.Add(c);
    double total = 0.0;
    foreach (Shape one in all) {
      total = total + one.Area();
      Console.WriteLine(one.Kind() + " " + one.Area());
    }
    Console.WriteLine("total " + total);

    // Fields are reachable through a base-typed reference too, and an
    // inherited one needs no redeclaration.
    Rect rr = new Rect("r2", 2.0, 6.0);
    Console.WriteLine(rr.Name + " " + rr.W + " " + rr.H);
    rr.W = 10.0;
    Console.WriteLine(rr.Area());
  }
}

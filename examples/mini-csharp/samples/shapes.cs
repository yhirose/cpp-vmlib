// Classes, inheritance and virtual dispatch, the C# way round. The
// mechanism is one link: a class table carries its base's table, and a
// method lookup walks the chain. `override` is then not a feature, it is
// what happens when the walk finds the derived entry first.

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
  // An empty-bodied constructor whose only job is to chain to the
  // base's.
  public Plain() : base("plain") {
  }
}

// Object.ToString, overridden: what Console.WriteLine and string
// concatenation call too.
class Box {
  public int V;
  public Box(int v) { V = v; }
  public override string ToString() { return "Box(" + V + ")"; }
}

class Bare { }

// A method named like a library one (List.Add) is still this class's own.
class Bag {
  public List<int> Items = new List<int>();
  public void Add(int x) { Items.Add(x * 10); }
}

// Construction order: field initializers, then the base's constructor --
// implicitly its parameterless one -- then the body. A's constructor makes
// a virtual call, so B's override runs before B's own constructor does,
// and sees B's field already initialized.
class A {
  public int X = 5;
  public A() { Console.WriteLine("A ctor " + Describe()); }
  public virtual string Describe() { return "A"; }
}

class B : A {
  public string Tag = "tagged";
  public B() { Console.WriteLine("B ctor"); }
  public override string Describe() { return "B " + Tag; }
}

// No constructor at all: the one C# synthesizes still initializes the
// fields and chains to A's.
class C : A {
  public int Y = 7;
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

    Box box = new Box(3);
    Console.WriteLine(box.ToString());
    Console.WriteLine(box);
    Console.WriteLine("got " + box);
    Console.WriteLine(new Bare().ToString());

    Bag bag = new Bag();
    bag.Add(2);
    Console.WriteLine(bag.Items[0]);

    B b = new B();
    Console.WriteLine(b.X);
    C cc = new C();
    Console.WriteLine(cc.X + cc.Y);
  }
}

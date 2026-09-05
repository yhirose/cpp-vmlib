// `using` and `try`/`finally`. Tag::Defer runs its callable "however it
// exits: falling through, Break, Continue, Return, or an unwinding throw"
// -- which is IDisposable's contract word for word, so `using` is a Scope
// and a Defer and nothing else.

using System;
using System.Collections.Generic;

class Res : IDisposable {
  public string Tag;
  static List<string> Log;

  public Res(string tag) {
    Tag = tag;
    Note("open " + tag);
  }

  public void Dispose() {
    Note("close " + Tag);
  }

  public int Value() {
    return 42;
  }

  public static void Start() {
    Log = new List<string>();
  }

  public static void Note(string s) {
    Log.Add(s);
  }

  public static string Dump() {
    return string.Join(", ", Log);
  }
}

class Program {
  static void Plain() {
    using (Res r = new Res("plain")) {
      Res.Note("body " + r.Value());
    }
  }

  static void Nested() {
    using (Res a = new Res("outer")) {
      using (Res b = new Res("inner")) {
        Res.Note("body");
      }
    }
  }

  static int ThroughReturn() {
    using (Res r = new Res("return")) {
      Res.Note("body");
      return 7;
    }
  }

  static void ThroughThrow() {
    using (Res r = new Res("throw")) {
      throw new Exception("boom");
    }
  }

  static void ThroughBreak() {
    for (int i = 0; i < 3; i++) {
      using (Res r = new Res("loop")) {
        if (i == 1) {
          break;
        }
        Res.Note("body " + i);
      }
    }
  }

  static void Main() {
    Res.Start();
    Plain();
    Console.WriteLine(Res.Dump());

    Res.Start();
    Nested();
    Console.WriteLine(Res.Dump());

    Res.Start();
    Console.WriteLine(ThroughReturn());
    Console.WriteLine(Res.Dump());

    Res.Start();
    try {
      ThroughThrow();
    } catch (Exception e) {
      Res.Note("caught " + e.Message);
    } finally {
      Res.Note("finally");
    }
    Console.WriteLine(Res.Dump());

    Res.Start();
    ThroughBreak();
    Console.WriteLine(Res.Dump());

    // try/finally with no catch at all: the finally still runs on the way
    // past, and the exception continues.
    Res.Start();
    try {
      try {
        throw new Exception("inner");
      } finally {
        Res.Note("inner finally");
      }
    } catch (Exception e) {
      Res.Note("outer caught " + e.Message);
    }
    Console.WriteLine(Res.Dump());
  }
}

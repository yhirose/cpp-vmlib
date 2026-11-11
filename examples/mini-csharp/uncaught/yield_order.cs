// Pins mini-csharp's single-threaded ordering, not a guarantee of real C#
// (there the continuation runs on a thread-pool thread, so the "before"
// read races with it): what follows `await Task.Yield()` runs after the
// caller's next statement, and by `.Wait()` it has run. A front end that
// ran it inline would report "before=1". dotnet reports
// "Unhandled exception. System.Exception: ..." for the throw below.
using System;
using System.Threading.Tasks;

class Program {
  static int F;

  static async Task Eager() {
    await Task.Yield();
    F = 1;
  }

  static void Main() {
    Task t = Eager();
    int before = F;
    t.Wait();
    throw new Exception("before=" + before + " after=" + F);
  }
}

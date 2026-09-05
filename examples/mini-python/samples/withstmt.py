# `with`. A context manager's __exit__ must run however the block is left
# -- falling through, `return`, `break`, or an unwinding exception -- which
# is Tag::Defer's contract word for word, so that is what it lowers to.

class Tracer:
    def __init__(self, name, log):
        self.name = name
        self.log = log

    def __enter__(self):
        self.log.append("enter " + self.name)
        return self

    def __exit__(self, a, b, c):
        self.log.append("exit " + self.name)
        return False

log = []
with Tracer("plain", log) as t:
    log.append("body " + t.name)
print(log)

# Nested, and left in the order they were entered.
log = []
with Tracer("outer", log):
    with Tracer("inner", log):
        log.append("body")
print(log)

# Out through a return: the value is decided before the deferred block
# runs, and the deferred block does not change it.
log = []

def through_return():
    with Tracer("return", log):
        log.append("body")
        return "returned"

print(through_return(), log)

# Out through a break, which is the exit path a duplicated block would
# most easily miss.
log = []
for i in range(3):
    with Tracer("loop" + str(i), log):
        if i == 1:
            break
        log.append("body " + str(i))
print(log)

# Out through an exception nothing here catches: __exit__ still runs, and
# the exception continues afterwards.
log = []

def through_raise():
    with Tracer("raise", log):
        raise ValueError("thrown")

try:
    through_raise()
except Exception as e:
    log.append("caught " + str(e))
print(log)

# A manager that is not bound to a name at all.
log = []
with Tracer("anonymous", log):
    log.append("body")
print(log)

# The manager can hand back something other than itself.
class Opened:
    def __init__(self, payload):
        self.payload = payload

    def __enter__(self):
        return self.payload

    def __exit__(self, a, b, c):
        return False

with Opened([1, 2, 3]) as items:
    print(len(items), items[0])

# float() of a string reads Python's grammar: whitespace around it, a sign,
# inf, infinity and nan in any case, digits with a single underscore between
# two of them, a fraction and an exponent. Each line is its own check.
def show(text):
    try:
        print(repr(text), float(text))
    except Exception as e:
        print(repr(text), type(e).__name__, e)


for text in ["1.5", " 1.5 ", "1e3", "1_0", "1e1_0", " 1 ", "5.", ".5e-3",
             "1e+5", "00.5", "0_0", "-0", "1_000.000_1e-0_2",
             "\t\n\v\f\r 2\r"]:
    show(text)
for text in ["inf", "-inf", "+Infinity", "INF", "nan", "-nan", "iNf", "1e999"]:
    show(text)
for text in ["", "x", "1__0", "_1", "1_", "1_e5", "1._5", "1_.5", "0x1p3",
             "+-1", "infinit", "inf_", "nan(1)", "1e", "e5", ".", "1 2",
             "--1", ".e1", "1e5.5", "1,5"]:
    show(text)

# A class whose __init__ never says super() still has its arguments as the
# exception's text: python3 ends with "NoSuper: 5".
class NoSuper(Exception):
    def __init__(self, x):
        self.x = x


raise NoSuper(5)

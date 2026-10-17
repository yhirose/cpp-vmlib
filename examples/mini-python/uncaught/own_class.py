# The program's own exception class, raised inside a function: python3
# ends with "AppError: disk full", and points at the raise.
class AppError(Exception):
    pass


def save():
    raise AppError("disk full")


save()

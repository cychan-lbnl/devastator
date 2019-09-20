import brutal

class PanicError(BaseException):
  def __init__(me, message):
    BaseException.__init__(me, message)

def panic_unless(ok, fmtstr='', *fmtargs, **fmtkws):
  if not ok:
    raise PanicError(fmtstr.format(*fmtargs, **fmtkws))

def panic(fmtstr='', *fmtargs, **fmtkws):
  raise PanicError(fmtstr.format(*fmtargs, **fmtkws))

"""
brutal.noiselog: Logs errors during the course of process execution.
"""
def _everything():
  import hashlib
  import os
  import sys

  import brutal

  def by_name(x):
    import hashlib
    h = hashlib.sha1()
    x_name = x.__name__
    x_mod = x.__module__
    h.update(('fn-name.%x.%x.%s%s' % (len(x_mod), len(x_name), x_mod, x_name)).encode('utf-8'))
    x._brutal_digest_memo = h.digest()
    return x
  
  def export(obj):
    globals()[obj.__name__] = obj
    brutal.__dict__[obj.__name__] = obj
    return by_name(obj)
  
  RESET = '\x1b[0m'
  RED = '\x1b[31m'
  YELLOW = '\x1b[33m'
  BLUE = '\x1b[34m'
  WHITE = '\x1b[37m'

  if os.environ.get('TERM','') == 'dumb':
    RESET = ''
    RED = ''
    YELLOW = ''
    BLUE = ''
    WHITE = ''
  
  def terminal_rows_cols():
    import struct
    
    def ioctl_GWINSZ(fd):
      try:
        import fcntl
        import termios
        return struct.unpack('hh', fcntl.ioctl(fd, termios.TIOCGWINSZ, '1234'))
      except:
        return None
    
    ans = ioctl_GWINSZ(0) or ioctl_GWINSZ(1) or ioctl_GWINSZ(2)
    
    if ans is None:
      try:
        term_id = os.ctermid()
        try:
          fd = os.open(term_id, os.O_RDONLY)
          ans = ioctl_GWINSZ(fd)
        finally:
          os.close(fd)
      except:
        pass
    
    if ans is None:
      try:
        with os.popen('stty size', 'r') as f:
          ans = f.read().split()
      except:
        pass
    
    if ans is None:
      return (25, 50)
    ans = tuple(map(int, ans))
    if ans[0] <= 0 or ans[1] <= 0:
      return (25, 50)
    return ans
  
  isatty = sys.stdout.isatty() and sys.stderr.isatty()
  t_rows_cols = terminal_rows_cols() if isatty else None
  isatty = isatty and t_rows_cols is not None
  
  if not isatty:
    t_rows, t_cols = 25, 50
  else:
    t_rows, t_cols = t_rows_cols
  
  BAR = '~'*t_cols
  
  _fatal = [None]
  _log = [] # [(title,message)]

  class NoisyError(Exception):
    pass
  
  @export
  def error(title, message=''):
    e = NoisyError(title + '\n'*(message != ''))
    _fatal[0] = e
    pair = (title, message)
    
    if pair not in _log:
      _log.append(pair)
      
      if isatty:
        if _log[0] is pair:
          sys.stderr.write(RED + '*** Something FAILED! ***' + RESET + '\n')
      else:
        sys.stderr.write(''.join([
          '~'*50, '\n',
          RED + title + RESET,
          '\n\n'*(message != ''),
          message,
          (not message.endswith('\n'))*'\n'
        ]))

    raise e

  @export
  def error_unless(ok, title, message=''):
    if not ok: error(title, message)
  
  @export
  def warning(message):
    show("WARNING: " + message)

  @export
  def warning_unless(ok, message):
    if not ok: show("WARNING: " + message)

  @export
  def show(title, message=''):
    """
    Print error message to stderr and display in the abort log if an
    aborting error occurs elsewhere.
    """
    pair = (title, message)
    
    if pair not in _log:
      _log.append((title, message))
      
      sys.stderr.write(''.join([
        BAR, '\n',
        RED + title + RESET,
        '\n\n'*(message != ''),
        message,
        '\n'*(not message.endswith('\n')),
        #BAR, '\n'
      ]))
  
  @export
  def aborted(exception, tb=None):
    """
    Called from the top-level main() code to indicate that an uncaught
    exception was about to abort execution. This call will then display
    the error log and invoke os._exit(1).
    """

    nice_error = isinstance(exception, Exception) and not isinstance(exception, AssertionError)

    if nice_error:
      from . import coflow
      coflow.shutdown()
    
    if isinstance(exception, KeyboardInterrupt):
      os._exit(1)
    else:
      from traceback import format_exception
      
      if exception is not None and not isinstance(exception, NoisyError):
        from traceback import format_exception
        if tb is None:
          tb = sys.exc_info()[2]
        _log.append((
          'Uncaught exception... \\m/ BRUTAL \\m/',
          ''.join(format_exception(type(exception), exception, tb))
        ))

      if isatty:
        t_rows, t_cols = terminal_rows_cols()
      else:
        t_cols = 50

      BAR = '~'*t_cols
      text = '\n'.join(
        ''.join([
          BAR, '\n',
          RED, title, RESET,
          '\n\n'*(message!=''),
          message,
          '\n'*(not message.endswith('\n'))
        ])
        for title,message in _log
      )
        
      if isatty:
        import re
        text_plain = re.sub(r'(\x9b|\x1b\[)[0-?]*[ -\/]*[@-~]', '', text)
      else:
        text_plain = text
      
      text_rows = sum(map(lambda line: (len(line)+t_cols-1)/t_cols, text_plain.split('\n')))

      if isatty and text_rows >= t_rows-3:
        try:
          import subprocess as sp
          pager = os.environ.get('PAGER','less -R').split()
          less = sp.Popen(pager, stdin=sp.PIPE)
          less.communicate(text.encode('utf-8'))
        except KeyboardInterrupt:
          pass
      else:
        sys.stderr.write(text)
      
      if nice_error:
        sys.exit(1)
      else:
        os._exit(1)

_everything()
del _everything

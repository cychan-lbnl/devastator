"""
brutal.tool: Contains the top-level main() function for the
command-line tool. The first argv argument to this process (sys.argv[1])
will name the rule to invoke, with the following argv arguments used
as arguments to that rule.
"""

def main():
  import os
  import re
  import sys

  from . import coflow
  from . import noiselog
  from . import memodb
  from . import rulefile
  from . import process
  
  def parse_arg(s):
    if s.startswith('%'):
      return eval(s[1:])
    for ty in (int, float):
      try:
        val = ty(s)
        if str(val) == s:
          return val
      except ValueError:
        pass
    return s
  
  def trim_doc(name, val):
    if hasattr(val, '__doc__'):
      docstring = val.__doc__
    else:
      return 'No __doc__ string available for "%s" which has value "%r".' % (name, val)

    if not docstring:
      return 'No __doc__ string available for "%s".' % name
    
    # Convert tabs to spaces (following the normal Python rules)
    # and split into a list of lines:
    lines = docstring.expandtabs().splitlines()
    # Determine minimum indentation (first line doesn't count):
    indent = sys.maxint
    for line in lines[1:]:
      stripped = line.lstrip()
      if stripped:
        indent = min(indent, len(line) - len(stripped))
    # Remove indentation (first line is special):
    trimmed = [lines[0].strip()]
    if indent < sys.maxint:
      for line in lines[1:]:
        trimmed.append(line[indent:].rstrip())
    # Strip off trailing and leading blank lines:
    while trimmed and not trimmed[-1]:
      trimmed.pop()
    while trimmed and not trimmed[0]:
      trimmed.pop(0)
    # Return a single string:
    return '\n'.join(trimmed)
  
  i = 1
  while 1:
    if len(sys.argv) == i:
      exit()
    m = re.match('([A-Za-z0-9-_]+)=', sys.argv[i])
    if not m: break
    os.environ[m.group(1)] = sys.argv[i][len(m.group(0)):]
    i += 1

  hooks = rulefile.cli_hooks()
  
  if len(sys.argv) == i:
    exit()
  cmd = sys.argv[i]
  i += 1
  
  if cmd == 'help':
    if len(sys.argv) == i:
      lines = []
      for key in sorted(hooks.keys()):
        if key.startswith('_'):
          continue
          
        val = hooks[key]
        while hasattr(val, '__wrapped__'):
          val = val.__wrapped__

        if type(val) is type(lambda:0):
          code = val.__code__
          toks = [key] + ['<%s>'%arg for arg in code.co_varnames[0:code.co_argcount]]
          lines.append(' '.join(toks))
        elif type(val) is type:
          pass
        elif hasattr(val, '__call__') and hasattr(val.__call__,'__func__'):
          code = val.__call__.__func__.__code__
          toks = [key] + ['<%s>'%arg for arg in code.co_varnames[1:code.co_argcount]]
          lines.append(' '.join(toks))

      sys.stdout.write(
        'Usage:\n'+
        '  brutal <command> <args...>\n'+
        'Available commands:\n'+
          ''.join(['  %s\n'%ln for ln in lines])+
        'Or:\n'+
        '  brutal help <command>\n'
      )
      return 0
      
    else:
      cmd = sys.argv[i]
      val = hooks.get(cmd)
      if val is None:
        sys.stderr.write('Unknown command name "%s".\n' % cmd)
        return 1
      else:
        sys.stdout.write(trim_doc(cmd, val)+'\n')
        return 0
  else:
    try:
      fn = hooks[cmd]
    except KeyError:
      sys.stderr.write(
        ('Unknown command name "%s", to list commands use:\n' % cmd) +
        '  brutal help\n'
      )
      return 1

    if not hasattr(fn, '__call__'):
      fn = (lambda val: lambda: val)(fn)
    
    try:
      @coflow.mbind(fn(*map(parse_arg, sys.argv[i:])))
      def printed(ans):
        if ans is None:
          pass
        elif isinstance(ans, str):
          sys.stdout.write(ans)
        elif isinstance(ans, (bool, int, float)):
          sys.stdout.write(str(ans))
        elif (isinstance(ans, (tuple, list, set, frozenset))
          and all(type(x) in (bool,int,float,str) for x in ans)):
          import shlex
          if isinstance(ans, (set, frozenset)):
            try: ans = sorted(ans)
            except: pass
          sys.stdout.write(
            ' '.join([shlex.quote(str(x)) for x in ans]) + '\n'
          )
        else:
          sys.stdout.write(repr(ans)+'\n')
      printed.wait()
    
    except BaseException as e:
      noiselog.aborted(e)

  return 0

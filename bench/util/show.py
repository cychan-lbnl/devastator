#!/usr/bin/env python3

"""
show.py [-i|-out-csv] [<report-file>...] [<name>=<val>...]
        [-rel <name>=<val>...]

Parse the given <report-file>'s (defaults to just "report.out")
and plot their dependent variables.

The "<name>=<val>" assignments are used to filter the dataset
before processing. (Consider using "app=<app-name>" if more
than one app's are in the dataset).

If "-rel" is present, then all "<name>=<val>" pairs after it
are used as the denominator to produce relative data.

If "-i" is present then instead of showing plots, an
interactive python shell will be entered which has all the
dependent variables from the report files loaded as global
variables. Each is an instance of Table (from table.py).

If "-out-csv" is present then plain-text tabular data is dumped
to stdout. Good for importing into spreadsheets.

"""

import sys

if '-h' in sys.argv:
  print(__doc__)
  sys.exit()

import table as tab

plot = tab.plot
T = tab.T

reportfiles = set()
dimvals = {}

tab.xdims['opnew'] = dict(title='Allocator')
tab.xdims['ranks'] = dict(title='Ranks',rank=3)

for arg in sys.argv[1:]:
  if arg.startswith('-'):
    pass
  elif '=' not in arg:
    reportfiles.add(arg)

if len(reportfiles) == 0:
  reportfiles = ['report.out']

for arg in sys.argv[1:]:
  if arg == '-rel':
    break
  elif arg.startswith('-'):
    pass
  elif '=' in arg:
    x,y = arg.split('=')
    x = x.strip()
    y = y.split(',')
    def intify(s):
      try: return int(s)
      except: return s
    y = map(lambda y: intify(y.strip()), y)
    dimvals[x] = y

tabs = {}
for report in reportfiles:
  def row(xs={}, ys={}):
    xys = dict(xs)
    xys.update(ys)
    if all(xys[x] in dimvals[x] for x in xys if x in dimvals):
      fact = dict(xys)
      for x in ys:
        fact.pop(x, None)
      for x in ys:
        fact1 = dict(fact)
        fact1[x] = xys[x]
        tabs[x] = tabs.get(x, [])
        tabs[x].append(fact1)

  exec(open(report).read(), dict(row=row))

for name in list(tabs.keys()):
  t, = tab.tables([name], tabs[name])
  
  def mean(seq):
    return float(sum(seq))/len(seq)
  def stdev(seq):
    from math import sqrt
    bar = mean(seq)
    return sqrt(sum([((x-bar)/bar)**2 for x in seq]))
  
  reducer = mean
  #reducer = max
  
  g = t.group(t.dims - frozenset(['trial']))
  t = T(lambda t: reducer(t.values()))(g)
  tabs[name] = t
  
if '-rel' in sys.argv[1:]:
  relative_to = {}
  for arg in sys.argv[sys.argv.index('-rel')+1:]:
    if '=' in arg:
      x,y = arg.split('=')
      x = x.strip()
      y = y.strip()
      try: y = int(y)
      except: pass
      relative_to[x] = y
  
  for name in tabs:
    tabs[name] = tabs[name] / tabs[name].split(**relative_to)[0]
    
else:
  relative_to = {}

titles = dict(
  bw = 'Bandwidth',
  secs = 'Elapsed (s)',
  send_per_rank_per_sec = 'Send/Rank/Sec',
  execute_per_rank_per_sec = 'Execute/Rank/Sec',
  commit_per_rank_per_sec = 'Commit/Rank/Sec'
)

if '-i' in sys.argv[1:]:
  for name in tabs:
    globals()[name] = tabs[name]
  import code
  code.interact(local=globals())
else:
  for name in tabs:
    t = tabs[name]
    title = titles.get(name, name)
    if relative_to:
      title += " relative to %s"%tab.pretty(relative_to)
    
    if '-out-csv' in sys.argv[1:]:
      trivs = t.dims_trivial()
      if trivs:
        title += '\n  where:' + tab.pretty(trivs)
      
      out = sys.stdout.write
      out(title + '\n')
      dims = sorted(t.dims - set(t.dims_trivial()))
      out(' '.join(['%10s'%d for d in dims + [name]]) + '\n')
      out('-'*(11*len(dims) + 10) + '\n')
      for row,val in sorted(t):
        def pr(x):
          if x is None:
            return ''
          elif type(x) in (int,long):
            if len(str(x)) < 10:
              return str(x)
            else:
              return '%.4g' % x
          elif type(x) is float:
            return '%.3e' % x
          else:
            return str(x)
        out(','.join(['%10s'%pr(row.get(d)) for d in dims] + ['%10s'%pr(val)]) + '\n')
      out('\n')
    else:
      plot(t, title=title)  

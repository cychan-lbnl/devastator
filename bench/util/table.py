"""
This file gives you the `Table` class for holding sampled data, and the
`plot(Table)` function for easily visualizing it.

A table is just like a database table: it consists of rows and columns, but,
with the added structure that their is exactly one column pertaining to the
dependent variable (the measured one) which is unnamed, and the other named
columns pertaining to the independent variables (a.k.a dimensions).

With database tables all rows conform to the same set of columns, but this 
rigidity of structure is not the case with our tables. Rows may have different 
dimensions so long as the following holds: for any two rows there exists at 
least one common dimension to which each row assigns a different value. This is 
called "disjointness". We interpret the dimension assignments of each row as 
logical conjunctions: the row {a=1, b=2} means that statement "a=1 AND b=2" was 
true of the world at the time the sample was measured. By requiring 
disjointness we ensure our dependent variable is single-valued since no two 
rows in the table could ever possibly match the same world.

Arithmetic and other common operators between tables (+, -, *, /, __call__) have
been overloaded to apply element-wise to the dependent variable. An inner-join 
is used to align and broadcast values for tables of different dimensions. This 
is the *killer feature*. The python package "xarray" does this too, but over a 
more rigidly structured data model.
"""

from functools import reduce

emptyset = frozenset()
_internal = object()

def scalar(x):
  """
  Return a table containing a single row that matches all worlds, mapping them
  uniformly to the value given.
  """
  return Table(holes=emptyset, tabs={emptyset:(({},), (x,))})

def to_table(x):
  """
  Produce scalar table from argument unless its already a table.
  """
  if isinstance(x, Table):
    return x
  else:
    return scalar(x)

T = to_table

def tables(dependent_vars, rows):
  """
  Produce a tuple of tables, one for each value of `dependent_vars`, from the
  sequence of rows where dependent variables have been included with the
  independent variables, e.g.:
  
    # Builds table pertaining to dependent variable y
    y, = tables(['y'], [dict(x=1, y=101), dict(x=2, y=102)])
  """
  dependent_vars = tuple(dependent_vars)
  args = {x: [] for x in dependent_vars}
  
  for row in rows:
    fact = dict(row)
    
    for x in dependent_vars:
      try: del fact[x]
      except KeyError: pass
    
    for x in dependent_vars:
      try:
        val = row[x]
        args[x].append((fact, val))
      except KeyError:
        pass
  
  return tuple(Table(args[x]) for x in dependent_vars)

class Table(object):
  """
  To construct a Table, pass it an iterable sequence of pairs `(fact,val)`
  where each `fact` is a dictionary assigning dimension to value and `val` is
  the dependent sample associated with the row, e.g.:
    
    # Build the table mapping x to `x**2` for x in (1,2,3)
    Table([
      (dict(x=1), 1),
      (dict(x=2), 4),
      (dict(x=3), 9)
    ])
    
    # Build the table mapping x,y to `x**2 + y**2` for (x,y) in [(1,1),(2,1),(3,2)]
    Table([
      (dict(x=1, y=1), 2),
      (dict(x=2, y=1), 5),
      (dict(x=3, y=2), 15)
    ])
  """
  def __init__(me, arg=_internal, tabs={}, holes=emptyset):
    if arg is _internal:
      me._tabs = tabs
      me.dims = reduce(lambda a,b: a|b, tabs, emptyset)
      me.dims_common = reduce(lambda a,b: a&b, tabs, me.dims)
      me.holes = holes
    elif isinstance(arg, Table):
      me._tabs = arg._tabs
      me.dims = arg.dims
      me.dims_common = arg.dims_common
      me.holes = arg.holes
    else:
      tabs = {}
      bags = {}
      for fact,val in arg:
        fact = dict(fact)
        dims = frozenset(fact)
        
        if dims not in tabs:
          tabs[dims] = ([],[])
          bags[dims] = set()
        
        rs,vs = tabs[dims]
        rs.append(fact)
        vs.append(val)
      
        items = frozenset(fact.items())
        if items in bags[dims]:
          raise Exception("Duplicate data point for %r."%fact)
        bags[dims].add(items)
      
      me._tabs = tabs
      me.dims = reduce(lambda a,b: a|b, tabs, emptyset)
      me.dims_common = reduce(lambda a,b: a&b, tabs, me.dims)
      me.holes = holes
  
  def __len__(me):
    """Number of rows in table."""
    n = 0
    for rows,_ in me._tabs.values():
      n += len(rows)
    return n
  
  def __iter__(me):
    """Iterate the (row,val) pairs of table."""
    for rows,vals in me._tabs.values():
      for rowval in zip(rows,vals):
        yield rowval
  
  def values(me):
    ans = []
    for rows,vals in me._tabs.values():
      ans += vals
    return ans
  
  def dims_trivial(me):
    """
    Return a dictionary of dimensions to values for all dimensions which
    have exactly the same value across all rows.
    """
    dims = me.dims
    bins = {x:set() for x in dims}
    for rows,vals in me._tabs.values():
      for row in rows:
        for x in dims:
          if x in row:
            bins[x].add(row[x])
    return {x:bins[x].pop() for x in bins if 1 == len(bins[x])}
  
  def split(me, **fact):
    """
    Returns tuple of two tables where the first consists of all the rows matching
    `fact` but with those dimensions dropped (since they're now trivial), and the
    second table is like this one but with the split-out rows discarded.
    """
    sdims = tuple(fact.keys())
    svals = list(map(fact.__getitem__, sdims))
    tabs0, tabs1 = {}, {}
    
    for dims,(rows,vals) in me._tabs.items():
      rows0, vals0 = [], []
      tabs0[dims - frozenset(sdims)] = (rows0,vals0)
      
      rows1, vals1 = [], []
      tabs1[dims] = (rows1,vals1)
      
      for row,val in zip(rows,vals):
        try: match = list(map(row.__getitem__, sdims)) == svals
        except KeyError: match = False
        
        if match:
          row0 = dict(row)
          for x in sdims:
            del row0[x]
          rows0.append(row0)
          vals0.append(val)
        else:
          rows1.append(row)
          vals1.append(val)
      
    t0 = Table(holes=me.holes, tabs=tabs0)
    t1 = Table(holes=me.holes, tabs=tabs1)
    
    return t0, t1
  
  def group(me, dims):
    """
    Given a set of dimensions, return an "outer" table where the row facts
    contain only those dimensions and the values are tables consisting of all
    rows which collapsed (via dimension dropping) to the outer row.
    """
    dims = frozenset(dims)
    grps = {}
    for tdims,(rows,vals) in me._tabs.items():
      gdims = dims & tdims
      gdimstup = tuple(sorted(gdims))
      for r,v in zip(rows,vals):
        g = (gdims, gdimstup, tuple(map(r.__getitem__, gdimstup)))
        if g not in grps:
          grps[g] = {}
        grp = grps[g]
        
        if tdims not in grp:
          grp[tdims] = ([],[])
        subrows, subvals = grp[tdims]
        
        subrows.append(r)
        subvals.append(v)
    
    tabs = {}
    for (gdims,gdimstup,gdimsvals),grp in grps.items():
      if gdims not in tabs:
        tabs[gdims] = ([],[])
      rows,vals = tabs[gdims]
      rows.append(dict(zip(gdimstup, gdimsvals)))
      vals.append(Table(holes=me.holes, tabs=grp))
    
    return Table(holes=emptyset, tabs=tabs)
  
  def __add__(a, b): return _map2_tt(lambda a,b: a+b, a, b)
  def __radd__(b, a): return _map2_tt(lambda a,b: a+b, a, b)
  __iadd__ = __add__
  
  def __sub__(a, b): return _map2_tt(lambda a,b: a-b, a, b)
  def __rsub__(b, a): return _map2_tt(lambda a,b: a-b, a, b)
  __isub__ = __sub__
  
  def __mul__(a, b): return _map2_tt(lambda a,b: a*b, a, b)
  def __rmul__(b, a): return _map2_tt(lambda a,b: a*b, a, b)
  __imul__ = __mul__
  
  def __div__(a, b): return _map2_tt(lambda a,b: a/b, a, b)
  def __rdiv__(b, a): return _map2_tt(lambda a,b: a/b, a, b)
  __idiv__ = __div__
  
  def __pow__(a, b): return _map2_tt(lambda a,b: a**b, a, b)
  def __rpow__(b, a): return _map2_tt(lambda a,b: a**b, a, b)
  __ipow__ = __pow__
  
  def __pos__(me): return me
  def __neg__(me): return _map2_tt(lambda x: -x, me)
  
  def __call__(me, *args, **kws):
    args_acc = Table(holes=emptyset, tabs={emptyset:(({},), ((),))})
    for a in args:
      args_acc = _map2_tt(lambda acc,a: acc + (a,), args_acc, a)
    
    kws_acc = Table(holes=emptyset, tabs={emptyset:(({},), ({},))})
    for k,v in kws.items():
      def fn(k):
        def fn(acc, v):
          acc = dict(acc)
          acc[k] = v
          return acc
        return fn
      kws_acc = _map2_tt(fn(k), kws_acc, v)
    
    both = _map2_tt(lambda a,kw: (a,kw), args_acc, kws_acc)
    
    return _map2_tt(lambda fn,both: fn(*both[0], **both[1]), me, both)

class _TheFormula(object):
  """
  The `Formula` object allows you to build "infinitary" formula tables where 
  the the dependent variable is expressed as an expression of the dimensions. 
  Formula tables have most of the features of discrete tables, especially the 
  operator overloading sugar.
  
  The first way to construct formulas is by accessing an attribute of
  `Formula`, which returns a table that evaluates to the dimension of the
  attribute's name, e.g.:
    
    # dist2 will always evaulate to the squared distance from (0,0) to (x,y),
    # for dimensions x and y.
    dist2 = Formula.x**2 + Formula.y**2
    
    # Create table with a single datapoint for x and y
    t = Table([(dict(x=1, y=2), 3)])
    
    # Returns a table where the single value at x=1 y=2 equals: 3 + 1**2 + 2**2 = 8
    t + dist2
  
  The other way to build a formula is to call `Formula` on a function/lambda.
  The table returned will compute its value by matching the argument names of
  the function against dimension names.
    
    # Equivalent construction of `dist2`
    dist2 = Formula(lambda x,y: x**2 + y**2)
  """
  
  def __getattr__(me, name):
    return Table(
      holes=frozenset((name,)),
      tabs={emptyset:(
        ((),),
        (lambda row: row[name],)
      )}
    )
  
  def __call__(me, fn):
    co = fn.__code__
    holes = frozenset(co.co_varnames[0:co.co_argcount])
    return Table(
      holes=holes,
      tabs={emptyset:(
        ((),),
        (lambda row: fn(**{x:y for x,y in row.items() if x in holes}),)
      )}
    )

Formula = _TheFormula()
Fr = Formula

def pretty(x):
  if type(x) is dict:
    return '{'+', '.join(['%s=%s'%(pretty(k),pretty(v)) for k,v in sorted(x.items())])+'}'
  elif isinstance(x, (int,float)):
    return '%.4g'%x
  else:
    return str(x)

def pretty_size(x):
  x = float(x)
  if abs(x) < 1<<10:
    return '%.4g'%x
  elif abs(x) < 1<<20:
    return '%.4gK'%(x/(1<<10))
  elif abs(x) < 1<<30:
    return '%.4gM'%(x/(1<<20))
  elif abs(x) < 1<<40:
    return '%.4gG'%(x/(1<<30))
  elif abs(x) < 1<<50:
    return '%.4gT'%(x/(1<<40))
  else:
    return '%.4gP'%(x/(1<<50))

xdims = dict(
  x = dict(rank=0, title='Generic X', pretty=pretty),
  size = dict(rank=1, title='Size', pretty=pretty_size)
)

def plot(t, title=''):
  """
  Generate and show a plot of the given table. The X-axis will be chosen by
  matching the dimensions of the table against the global `xdims` dict. The
  Y-axis is simply the table's dependent variable. All other dimensions will be
  lumped together yielding a separate curve for each fact they produce.
  """
  
  import matplotlib.pyplot as pyplot
  
  dims_trivial = t.dims_trivial()
  
  try:
    def rank_of(x):
      if x in dims_trivial: return 102
      if x in xdims: return xdims[x].get('rank',101)
      return 100
    xdim = sorted(t.dims, key=rank_of)[0]
  except IndexError:
    raise Exception("No suitable x-dimension found among %r, given table.xdims=%r."%(t.dims, xdims))
  
  gs = t.group(t.dims - frozenset([xdim]))
  
  xs_all = set()
  for row,_ in t:
    xs_all.add(row[xdim])
  xs_all = sorted(xs_all)
  
  tick_of = lambda x: xs_all.index(x)
  xticks = list(range(len(xs_all)))
  xlabels = [xdims.get(xdim,{}).get('pretty',pretty)(xs_all[i]) for i in xticks]
  if len(xs_all) > 8:
    xlabels = [xlabels[i] if i%(len(xs_all)/8) == 0 else '' for i in range(len(xlabels))]
  
  pyplot.xticks(xticks, xlabels)
  
  def pretty_dict(d):
    ss = ['%s=%s'%(x,y) for x,y in d.items()]
    w = max(map(len, ss))
    ss = map(lambda s: s.rjust(w), ss)
    return ', '.join(ss)
  
  pyplot.title(title + ('\nwhere: ' if title else '') + pretty(dims_trivial))
  pyplot.xlabel(xdims.get(xdim,{}).get('title',xdim))
  
  legwth = {}
  for g,curve in gs:
    for x,y in g.items():
      legwth[x] = max(legwth.get(x,0), len('%s=%s'%(x,y)))
  
  gs = sorted(gs, key=lambda gc: sorted(gc[0].items()))
  for g,curve in gs:
    xys = []
    for row,y in curve:
      xys.append((row[xdim],y))
    xys.sort()
    xs, ys = zip(*xys)
      
    label = '   '.join(
      [('%s=%s'%(x,y)).ljust(legwth[x]) for x,y in sorted(g.items()) if x not in dims_trivial]
    )
    if len(ys) == 1:
      pyplot.bar(x=tick_of(xs[0]), height=ys[0], label=label)
    else:
      pyplot.plot(list(map(tick_of, xs)), ys, label=label)
  
  pyplot.ylim(bottom=0)
  if len(t.dims) != 1+len(dims_trivial):
    pyplot.legend()
  pyplot.show()


def _map2_tt(fn, a, b):
  a = to_table(a)
  b = to_table(b)
  
  atabs, abigholes = a._tabs.items(), a.holes
  btabs, bbigholes = b._tabs.items(), b.holes
  ctabs = {}
  
  for adims, (arows, avals) in atabs:
    aholes = abigholes - adims
    for bdims, (brows, bvals) in btabs:
      bholes = bbigholes - bdims
      
      cdims = adims | bdims
      if cdims not in ctabs:
        ctabs[cdims] = ([], [])
      crows, cvals = ctabs[cdims]
      choles = (aholes | bholes) - cdims
      
      idims = tuple(adims & bdims)
      same_dims = len(adims) == len(idims) and len(bdims) == len(idims)

      bhash = {}
      for brow,bval in zip(brows,bvals):
        irow = tuple(map(brow.__getitem__, idims))
        if irow not in bhash:
          bhash[irow] = [(brow, bval)]
        else:
          bhash[irow].append((brow, bval))
      
      bhash_get = bhash.get
      crows_append = crows.append
      cvals_append = cvals.append
      
      a_unhole = aholes and len(aholes - bdims) == 0
      b_unhole = bholes and len(bholes - adims) == 0
      
      for arow,aval in zip(arows,avals):
        irow = tuple(map(arow.__getitem__, idims))
        for brow,bval in bhash_get(irow, ()):
          if same_dims:
            crow = arow
          else:
            crow = dict(arow)
            crow.update(brow)
          
          crows_append(crow)
          
          if choles:
            if not aholes:
              cvals_append((lambda a,b: lambda row: fn(a, b(row)))(aval, bval))
            elif not bholes:
              cvals_append((lambda a,b: lambda row: fn(a(row), b))(aval, bval))
            else:
              cvals_append((lambda a,b: lambda row: fn(a(row), b(row)))(aval, bval))
          else:
            aval1 = aval
            if a_unhole: aval1 = aval(crow)
            if b_unhole: bval = bval(crow)
            cvals_append(fn(aval1, bval))
  
  cbigholes = (abigholes | bbigholes) - reduce(lambda a,b: a&b, ctabs, emptyset)
  return Table(tabs=ctabs, holes=cbigholes)

if 0:
  y, = tables(['y'], [
    dict(size=2**x, y=x, uxx=1) for x in range(6)
  ])
  z, = tables('y', [
    dict(z=z, zzz='hi'*z, y=z, v=2) for z in range(4)
  ])
  plot((100 + y + Fr.z + z)*y)
  plot(y*y + z*z)

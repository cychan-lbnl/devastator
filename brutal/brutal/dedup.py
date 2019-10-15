def _everything():
  import builtins
  import os
  import sys
  import types

  if __name__ != '__main__':
    import brutal
  
  BaseException = builtins.BaseException
  dict = builtins.dict
  getattr = builtins.getattr
  int = builtins.int
  isinstance = builtins.isinstance
  len = builtins.len
  list = builtins.list
  min = builtins.min
  range = builtins.range
  sorted = builtins.sorted
  set = builtins.set
  str = builtins.str
  tuple = builtins.tuple
  type = builtins.type
  zip = builtins.zip

  os_path_sep = os.path.sep
  
  def export(fn):
    globals()[fn.__name__] = fn
    if __name__ != '__main__':
      brutal.__dict__[fn.__name__] = fn
    return fn

  encoders = {}
  decoders = {}
  
  trivial_types = (type(None), bool, int, float, bytes)
  
  @export
  def make_encoder(table):
    cache = {}
    if tuple not in table:
      table[tuple] = {}
    tuple_table = table[tuple]
    
    def unique(x):
      ty = type(x)
      if ty not in table:
        table[ty] = {}
      ty_tab = table[ty]
      if x not in ty_tab:
        ty_tab[x] = x
      else:
        x = ty_tab[x]
      return x

    def unique_tuple(x):
      if x not in tuple_table:
        tuple_table[x] = x
      else:
        x = tuple_table[x]
      return x
    
    def encode(value):
      ty = type(value)
      if ty in trivial_types:
        return unique(value)
      elif ty in encoders:
        return unique(encoders[ty](value, encode, unique, cache))
      else:
        getstate = getattr(ty, '__getstate__', None)
        if getstate:
          st = getstate(value)
          if type(st) is tuple:
            return unique_tuple((0, ty) + tuple(map(encode, st)))
          else:
            return unique_tuple((1, ty, encode(st)))
        else:
          d = value.__dict__
          return unique_tuple((2, ty, len(d)) + tuple(map(unique, d.keys())) + tuple(map(encode, d.values())))
    
    return encode
  
  @export
  def make_decoder(table):
    tuple_table = {}
    table[tuple] = tuple_table
    
    def unique(x):
      ty = type(x)
      if ty not in table:
        table[ty] = {}
      ty_tab = table[ty]
      if x not in ty_tab:
        ty_tab[x] = x
      else:
        x = ty_tab[x]
      return x
    
    def decode(rec):
      if type(rec) is tuple:
        if rec in tuple_table:
          return tuple_table[rec]
        else:
          val = decoders[rec[0]](rec, decode, unique)
          tuple_table[rec] = val
          return val
      else:
        return rec

    return decode
  
  if 0:
    @export
    def encode(x, _): return x
    @export
    def decode(x, _): return x
  
  def decode_setstate_tuple(rec, decode, unique):
    ty = rec[1]
    val = ty.__new__(ty)
    ty.__setstate__(val, map(decode, rec[2:]))
    return val
  decoders[0] = decode_setstate_tuple

  def decode_setstate_nontuple(rec, decode, unique):
    ty = rec[1]
    val = ty.__new__(ty)
    ty.__setstate__(val, decode(rec[2]))
    return val
  decoders[1] = decode_setstate_nontuple

  def decode_setdict(rec, decode, unique):
    ty = rec[1]
    val = ty.__new__(ty)
    n = rec[2]
    keys = map(unique, rec[3:3+n])
    vals = map(decode, rec[3+n:])
    val.__dict__.update(dict(zip(keys, vals)))
    return val
  decoders[2] = decode_setstate_nontuple

  op_bump = [3]

  def make():
    op = op_bump[0]
    op_bump[0] += 1
    rpartition = str.rpartition
    unpartition = '%s' + os_path_sep + '%s'
    def dec(rec, decode, unique):
      return unique(unpartition % (rec[1], rec[2]))
    def enc(x, encode, unique, cache):
      if x in cache:
        return cache[x]
      xs = rpartition(x, os_path_sep)
      if xs[1]:
        ans = (op, unique(xs[0]), unique(xs[2]))
      else:
        ans = unique(x)
      cache[x] = ans
      return ans
    return enc, op, dec
  enc, op, dec = make()
  encoders[str] = enc
  decoders[op] = dec
  
  for ty in (tuple, list):
    def make(ty):
      op = op_bump[0]
      op_bump[0] += 1
      def dec(rec, decode, _):
        return ty(map(decode, rec[1:]))
      def enc(x, encode, _, __):
        return (op,) + tuple(map(encode, x))
      return enc, op, dec
    enc, op, dec = make(ty)
    encoders[ty] = enc
    decoders[op] = dec

  for ty in (set, frozenset):
    def make(ty):
      op = op_bump[0]
      op_bump[0] += 1
      def dec(rec, decode, _):
        return ty(map(decode, rec[1:]))
      def enc(x, encode, _, __):
        return (op,) + tuple(map(encode, sorted(x)))
      return enc, op, dec
    enc, op, dec = make(ty)
    encoders[ty] = enc
    decoders[op] = dec

  def make():
    op = op_bump[0]
    op_bump[0] += 1
    def dec(rec, decode, unique):
      n = rec[1]
      keys = map(decode, rec[2:2+n])
      vals = map(decode, rec[2+n:])
      return dict(zip(keys, vals))
    def enc(x, encode, unique, _):
      keys = sorted(x.keys())
      vals = [x[k] for k in keys]
      return (op, len(x)) + tuple(map(encode, keys)) + tuple(map(encode, vals))
    return enc, op, dec
  enc, op, dec = make()
  encoders[dict] = enc
  decoders[op] = dec
  
_everything()
del _everything

if __name__ == '__main__':
  s = 'abc/def'
  val = [0,1,2,s,{s+'a':0,s+s:1}]
  val += [tuple(val)]
  etab = {}
  y = make_encoder(etab)(val)
  dtab = {}
  x = make_decoder(dtab)(y)

  print('val:',val)
  print('y  :',y)
  print('x  :',x)
  print('etab:')
  for k,v in etab.items():
    print(k, ' :: ',v)

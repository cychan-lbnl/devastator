"""
brutal.digest: Tools for hashing arbitrary python values.
"""

digest_zero = b'\0'*20

def _everything():
  from array import array
  import binascii
  import builtins
  from collections import deque
  import hashlib
  import sys
  import types

  if __name__ != '__main__':
    from . import coflow
    import brutal

  from .panic import panic, panic_unless, PanicError
  
  BaseException = builtins.BaseException
  getattr = builtins.getattr
  int = builtins.int
  isinstance = builtins.isinstance
  len = builtins.len
  min = builtins.min
  range = builtins.range
  sorted = builtins.sorted
  set = builtins.set
  str = builtins.str
  type = builtins.type
  zip = builtins.zip
  
  hexlify = binascii.hexlify
  unhexlify = binascii.unhexlify
  
  hashlib_sha1 = hashlib.sha1
  
  import os
  stdlib_dir = os.path.dirname(os.__file__) + os.path.sep
  sys_modules = sys.modules
  
  def export(fn):
    globals()[fn.__name__] = fn
    if __name__ != '__main__':
      brutal.__dict__[fn.__name__] = fn
    return fn
  
  @export
  def by_name(*xs):
    if len(xs) != 1 or isinstance(xs[0], (str, bytes)):
      def fn(f):
        panic_unless(type(f) in (types.FunctionType, type))
        h = hashlib_sha1()
        h.update(b'names(%r)' % (xs,))
        f._brutal_digest_memo = b'H' + h.digest()
        return f
      return fn
    else:
      f = xs[0]
      panic_unless(type(f) in (type, types.FunctionType))
      h = hashlib_sha1()
      f_name = f.__name__
      f_mod = f.__module__
      h.update(b'name(%r,%r)' % (f_mod, f_name))
      f._brutal_digest_memo = b'H' + h.digest()
      return f

  destructurers = {}
  get_destructurer = destructurers.get
  default_destructurer = None # definition down below
  
  @export
  @by_name
  def destructurer(ty):
    panic_unless(type(ty) is type)
    def register(fn):
      destructurers[ty] = fn
      return fn
    return register
  
  @export
  @by_name
  def digest_sum(*xs):
    c = 0
    for x in xs:
      c += int(hexlify(x), 16)
    c = '%x' % c
    if len(c) > 40:
      c = c[-40:]
    c = '0'*(40-len(c)) + c
    return unhexlify(c)

  @export
  @by_name
  def digest_subtract(a, b):
    a = int(hexlify(a), 16)
    b = int(hexlify(b), 16)
    c = a + (1<<128) - b
    c = '%x' % c
    if len(c) > 40:
      c = c[-40:]
    c = '0'*(40-len(c)) + c
    return unhexlify(c)
  
  @export
  @by_name
  def indigestible_destructurer(x, buf, work):
    name = getattr(x, '__name__', '')
    if name:
      name = ', name='+name
    raise TypeError('Cannot digest value of type: '+str(type(x))+name)

  @export
  @by_name
  def indigestible_type(ty):
    try: by_name(ty)
    except TypeError: pass
    destructurers[ty] = indigestible_destructurer
    return ty

  class IndigestibleFunctionError(Exception):
    pass
  
  @export
  @by_name
  def indigestible_function(fn):
    fn._brutal_digest_memo = IndigestibleFunctionError()
    return fn
  
  @export
  @by_name
  def by_other(other):
    def annotator(ty_or_fn):
      panic_unless(type(ty_or_fn) in (type, types.FunctionType))
      ty_or_fn._brutal_digest_other = other
      return ty_or_fn
    return annotator

  indigestible_type(types.GeneratorType)

  leaf_types = set([type(None), bool, int, float, str, bytes, bytearray, array])
  leaf_singletons = {None:b'N', True:b'T', False:b'F'}
  for i in range(10):
    leaf_singletons[i] = chr(ord('0') + i).encode()

  persistent_memo_types = ()
  counter = [0]

  def _ingest(hasher, values, buf=None, memo_map=None):
    try:
      work_stack = list(values)
      work_stack_pop = work_stack.pop

      # open_stack = [open_val, id(open_val), buf_ix, memo_log_ix, cycle_lev, scc, close ...]
      open_stack = [1<<40, None, -1]
      open_levs = 0
      open_map = {} # open_map[id(x)] = open_stack.index(x)
      open_map_pop = open_map.pop
      buf = buf or bytearray()
      buf_origin = len(buf)
      memo_map = memo_map or {} # memo_map[id(x)] = (x, buf_begin_ix, buf_end_ix) | (x, bytes)
      memo_log = array('Q', []) # [id(value)]
      memo_log_append = memo_log.append
      
      if counter[0]:
        print('ingest begin', counter)
        counter[0]+=1
      
      while work_stack:
        val = work_stack_pop()
        val_ty = type(val)
        
        if val_ty in leaf_types:
          if val in leaf_singletons:
            buf += leaf_singletons[val]
          else:
            buf += b'(%r)' % val
        else:
          val_id = id(val)
          
          if val_id in open_map: # part of an active scc
            open_lev = open_map[val_id]
            open_lev_off = 7*open_lev
            scc = open_stack[3+5 + open_lev_off] or [open_stack[3+1 + open_lev_off]]
            for lev in range(open_levs-1, open_lev, -1):
              lev_off = 7*lev
              lev_id = open_stack[3+1 + lev_off]
              scc_lev = open_map[lev_id]
              panic_unless(open_lev <= scc_lev)
              if scc_lev == lev:
                scc1 = open_stack[3+5 + lev_off] or (lev_id,)
                for x in scc1:
                  open_map[x] = open_lev
                open_stack[3+5 + lev_off] = None
                scc += scc1
            open_stack[3+5 + open_lev_off] = scc

            buf += b'scc(%x,%x)' % (open_levs - open_lev, scc.index(val_id))
            
            if open_lev < open_stack[-3]:
              open_stack[-3] = open_lev
          
          elif val_id in memo_map: # previously memoized
            memo_tup = memo_map[val_id]
            if len(memo_tup) == 2:
              buf += memo_tup[1]
            else:
              buf += buf[memo_tup[1]:memo_tup[2]]
          
          else:
            open_map[val_id] = open_levs
            open_levs += 1
            open_stack += (val, val_id, len(buf), len(memo_log), open_levs, None, len(work_stack))
            memo_log_append(val_id)
            try:
              get_destructurer(val_ty, default_destructurer)(val, buf, work_stack)
            except TypeError:
              lev = 0
              while lev < open_levs:
                obj = open_stack[-7*lev-7]
                print('lev',lev,type(obj),getattr(obj,'__name__',None))
                lev += 1
              raise

        while len(work_stack) == open_stack[-1]:
          val, val_id, buf_ix, memo_ix, cy_lev, scc, _ = open_stack[-7:]
          del open_stack[-7:]

          open_levs -= 1
          scc = scc or ((val_id,) if open_map[val_id] == open_levs else ())
          
          for x_id in scc:
            del open_map[x_id]
          
          if open_levs <= cy_lev:
            buf_len = len(buf)
            
            if buf_len - buf_ix < 256:
              memo_map[val_id] = (val, buf_ix, buf_len)
            else:
              h1 = hashlib_sha1()

              buf_mem = memoryview(buf)
              h1.update(buf_mem[buf_ix:])
              buf_mem.release()

              s = b'H' + h1.digest()
              memo_map[val_id] = (val, s)
              buf[buf_ix:] = s

              if type(val) in persistent_memo_types:
                val._brutal_digest_memo = s
              
              for i in range(memo_ix+1, len(memo_log)):
                x_id = memo_log[i]
                if x_id: # delete from map only if object was memo'd
                  del memo_map[x_id]
              
              del memo_log[memo_ix:]
          else:
            memo_log[memo_ix] = 0 # indicate that it wasn't memo'd
            if open_stack[-3] > cy_lev: # max parent's cycle depth with ours
              open_stack[-3] = cy_lev
      
      if buf_origin:
        buf_mem = memoryview(buf)
        hasher.update(buf_mem[buf_origin:])
        buf_mem.release()
      else:
        hasher.update(buf)
      
      if counter[0]:
        print('ingest done')
      
      return hasher
    except Exception as e:
      raise PanicError() from e

  @export
  @by_name
  def ingest(hasher, *values):
    return _ingest(hasher, values)

  @export
  @by_name
  def digest_of(*values):
    """
    Produce a SHA1 digest of the given values.
    """
    return _ingest(hashlib_sha1(), values).digest()

  @export
  @by_name
  def hexdigest_of(*values):
    """
    Produce a SHA1 digest of the given values.
    """
    return _ingest(hashlib_sha1(), values).hexdigest()

  @export
  @by_name
  def digests_of(*values):
    buf = bytearray()
    memo_map = {}
    return [_ingest(hashlib_sha1(), (x,), buf, memo_map).digest() for x in values]

  @export
  @by_name
  def digests_of_run(*values):
    h = hashlib_sha1()
    buf = bytearray()
    memo_map = {}
    return [_ingest(h, (x,), buf, memo_map).digest() for x in values]
  
  @destructurer(list)
  def de(x, buf, work):
    if x:
      buf += b'l(%x)' % len(x)
      work += x
    else:
      buf += b'L'

  @destructurer(tuple)
  def de(x, buf, work):
    if x:
      buf += b'tu(%x)' % len(x)
      work += x
    else:
      buf += b't0'
  
  @destructurer(set)
  def de(x, buf, work):
    buf += b'set(%x)' % len(x)
    work += sorted(x)
  
  @destructurer(frozenset)
  def de(x, buf, work):
    buf += b'fset(%x)' % len(x)
    work += sorted(x)
  
  @destructurer(dict)
  def de(x, buf, work):
    if x:
      buf += b'd(%x)' % len(x)
      keys = sorted(x.keys())
      work += keys
      work += [x[k] for k in keys]
    else:
      buf += b'D'
  
  @destructurer(types.CodeType)
  def de(x, buf, work):
    buf += b'code(%x,%x,%x)' % (len(x.co_code), x.co_flags, x.co_argcount)
    buf += x.co_code
    work += (
      x.co_consts or (),
      x.co_names,
      x.co_varnames,
      x.co_freevars,
      x.co_cellvars
    )
  
  @destructurer(types.ModuleType)
  def de(x, buf, work):
    buf += b'mod(%r)' % x.__name__
  
  import dis
  if hasattr(dis, '_unpack_opargs'):
    unpack_opargs = dis._unpack_opargs
  else:
    import opcode
    op_has_arg = opcode.HAVE_ARGUMENT
    op_EXTENDED_ARG = opcode.opmap['EXTENDED_ARG']
    def unpack_opargs(code):
      extended_arg = 0
      n = len(code)
      i = 0
      while i < n:
        op = code[i]
        offset = i
        i += 1
        arg = None
        if op >= op_has_arg:
          arg = code[i] + code[i+1]*256 + extended_arg
          extended_arg = 0
          i += 2
          if op == op_EXTENDED_ARG:
            extended_arg = arg*65536
        yield (offset, op, arg)

  class CodeOnGlobals(object):
    def __init__(me, fn_code, fn_globals):
      me.fn_code = fn_code
      me.fn_globals = fn_globals

  CodeOnGlobals_memo = {}
  persistent_memo_types += (CodeOnGlobals,)
  
  @destructurer(CodeOnGlobals)
  def de(me, buf, work):
    me_dict = me.__dict__
    if '_brutal_digest_memo' in me_dict:
      buf += me_dict['_brutal_digest_memo']
      return
    
    import opcode
    LOAD_GLOBAL = opcode.opmap['LOAD_GLOBAL']
    LOAD_ATTR = opcode.opmap['LOAD_ATTR']
    ModuleType = types.ModuleType

    fn_code = me.fn_code
    fn_globals = me.fn_globals
    gvars = {}
    co_more = [fn_code]
    
    while co_more:
      co = co_more.pop()
      code_type = type(co)
      for lit in co.co_consts:
        if type(lit) is code_type:
          co_more.append(lit)
      
      co_code = co.co_code
      co_names = co.co_names
      co_len = len(co_code)
      i = 0
      arg_ex = 0
      
      co_attrs = set()
      co_gvars = {}
      co_mods = set()
      for _,op,arg in unpack_opargs(co_code):
        if op == LOAD_GLOBAL:
          name = co_names[arg]
          #if fn.__name__ == 'touches' and fn.__module__ =='__main__':
          #  print 'GLOBAL',name
          if name in fn_globals:
            val = fn_globals[name]
            if isinstance(val, ModuleType):
              co_mods.add(val)
            else:
              co_gvars[name] = fn_globals[name]
        elif op == LOAD_ATTR:
          co_attrs.add(arg)
      
      co_mods0 = set(co_mods)
      while co_mods0:
        co_mods1 = set()
        for mod in co_mods:
          mod = mod.__dict__
          for attr in co_attrs:
            if attr in mod and attr != '__init__':
              val = mod[attr]
              if isinstance(val, ModuleType):
                co_mods1.add(val)
              else:
                co_gvars[name] = fn_globals[name]
        co_mods |= co_mods1
        co_mods0 = co_mods1
      
      gvars.update(co_gvars)

    buf += b'cg(%x)' % len(gvars)
    work += (fn_code,)
    keys = sorted(gvars.keys())
    if keys:
      buf += (','.join(keys)).encode()
      buf += b','
      work += [gvars[k] for k in keys]

  persistent_memo_types += (types.FunctionType,)
  panic_unless(types.FunctionType is types.LambdaType)

  @destructurer(types.FunctionType)
  def de(fn, buf, work):
    fn_dict = fn.__dict__

    if '_brutal_digest_memo' in fn_dict:
      buf += fn_dict['_brutal_digest_memo']
      return
    
    if '_brutal_digest_other' in fn_dict:
      buf += b'fn-o'
      work += (fn_dict['_brutal_digest_other'],)
      return
    
    fn_mod = fn.__module__
    fn_modfile = getattr(sys_modules[fn_mod], '__file__', '')
    
    if fn_mod == 'builtins' or fn_modfile.startswith(stdlib_dir):
      buf += b'fn-sys(%r,%r)' % (fn_mod, fn.__name__)
    else:
      fn_code = fn.__code__
      fn_globals = fn.__globals__
      key = (fn_code, id(fn_globals))

      if key not in CodeOnGlobals_memo:
        CodeOnGlobals_memo[key] = (CodeOnGlobals(fn_code, fn_globals), fn_globals)
      
      work += (CodeOnGlobals_memo[key][0],)
      fn_closure = fn.__closure__ or ()
      buf += b'fn(%x)' % len(fn_closure)
      work += [cell.cell_contents for cell in fn_closure]
      
  # instance bound method
  @destructurer(types.MethodType)
  def de(x, buf, work):
    buf += b'ibm'
    work += (x.__func__, x.__self__)
  
  # built-in unbound method
  @destructurer(type(list.append))
  def de(x, buf, work):
    x_class = x.__objclass__
    buf += b'bum(%r,%r,%r)' % (x_class.__module__, x_class.__name__, x.__name__)
  
  # built-in bound method
  @destructurer(type([].append))
  def de(x, buf, work):
    buf += b'bbm(%r)' % x.__name__
    work += (x.__self__,)
  
  persistent_memo_types += (type,)

  @destructurer(type)
  def de(ty, buf, work):
    ty_mod = ty.__module__
    ty_name = ty.__name__
    ty_modfile = getattr(sys_modules[ty_mod], '__file__', '')

    if ty_mod == 'builtins' or ty_modfile.startswith(stdlib_dir):
      buf += b'ty-sys(%r,%r)' % (ty_mod, ty_name)
    else:
      ty_dict = ty.__dict__

      if '_brutal_digest_memo' in ty_dict:
        buf += ty_dict['_brutal_digest_memo']
        return
      
      if '_brutal_digest_other' in ty_dict:
        buf += b'ty-o'
        work += (ty_dict['_brutal_digest_other'],)
        return
      
      mbrs = sorted([
        (mx,my) for mx,my in ty_dict.items()
        if not(mx.startswith('__') and mx.endswith('__')) or
          mx in ('__init__','__call__','__getitem__','__getattr__')
      ])
      buf += b'ty(%r,%r,%x)' % (ty_mod, ty_name, len(mbrs))
      work += mbrs

  @export
  @by_name
  def default_destructurer(x, buf, work):
    ty = type(x)
    ty_mod = ty.__module__
    ty_name = ty.__name__

    x_dict = getattr(x, '__dict__', None)
    
    if x_dict and '_brutal_digest_memo' in x_dict:
      buf += x_dict['_brutal_digest_memo']
      return
    
    if isinstance(x, BaseException):
      buf += b'ex(%r,%r)' % (ty_mod, ty_name)
    elif 1: #ty is getattr(sys.modules[ty_mod], ty_name, None):
      __getstate__ = getattr(x, '__getstate__', None)
      if __getstate__ is not None:
        buf += b'ob1'
        work += (__getstate__(),)
      else:
        if x_dict is not None:
          keys = sorted(x_dict.keys())
          vals = list(map(x_dict.__getitem__, keys))
        else:
          keys = sorted(getattr(ty, '__slots__', ()))
          kvs = [(k, getattr(x, k, buf)) for k in keys]
          kvs = [kv for kv in kvs if kv[1] is not buf]
          keys, vals = zip(*kvs) if kvs else ((),())
        buf += b'ob2(%x)' % len(keys)
        buf += ','.join(keys).encode()
        buf += b':'
        work += vals
    else:
      panic()
      # buf += b'?'

  if __name__ == '__main__':
    def show(name, val):
      if 1:
        print(name,':',hexdigest_of(val))
      else:
        print('BEGIN',name)
        h = hexdigest_of(val)
        print('END',name, ':', val, '::', h,'\n')
    
    if 1:
      def test(a,b,eq=True):
        print('a','-'*30)
        a=hexdigest_of(a)
        print('b','-'*30)
        b=hexdigest_of(b)
        panic_unless(a == b if eq else a != b)
      x=[0,1,2,'hi',{'a':4,'b':(1,2,(3,),())}]
      y=[0,1,2,'hi',{'a':4,'b':(1,2,(3,),())}]
      z=[0,1,2,'hi',{'a':4,'b':(1,2,(3,),())}]
      x[0]=y
      y[0]=x
      test(x,y)
      x[0]=y
      y[0]=z
      z[0]=x
      test(x,z)
      test(y,z)
      z[3]=z
      test(y,z,False)
      
    if 0:
      def f(x):
        def g(y):
          return x+y+c
        return g
      c=1
      show('f @ c=1:', f)
      c=2
      show('f @ c=2:', f)
      c=1
      show('f @ c=1:', f)
    
    if 0:
      import os
      print('abspath:', hexdigest_of(os.path.abspath))
    
      show('f(0):', f(0))
      show('f(1):', f(1))
      show('f(0):', f(0))
      
      h =hashlib.sha1()
      eat(h, 'a')
      eat(h, 'b')
      print('eat(a);eat(b):', h.hexdigest())
      
      h =hashlib.sha1()
      eat(h, 'a','b')
      print('eat(a,b):', h.hexdigest())
    
    if 1:
      def a(x): return b(x)+c(x)+1
      def b(x): return c(x)+a(x)+2
      def c(x): return a(x)+b(x)+3
      
      show('a',a)
      show('b',b)
      show('c',c)
      
      show('a',a)
      show('b',b)
      show('c',c)
      
      def a(x): return b(x)+c(x)+1
      def b(x): return c(x)+a(x)+2
      def c(x): return a(x)+b(x)+3
      
      show('c',c)
      show('a',a)
      show('b',b)
      
      show('c',c)
      show('a',a)
      show('b',b)
      
      
_everything()
del _everything

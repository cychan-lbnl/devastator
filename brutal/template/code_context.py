# This is a imported by a brutal rulefile, and therefor
# gets "brutal" for free.
assert brutal

def _merge_flags(a, b):
  if a:
    if b:
      return a + [x for x in b if x not in a]
    else:
      return a
  else:
    return b

def _merge_ppdefs(a, b):
  if a:
    if b:
      ans = {}
      for x in set(a.keys()) | set(b.keys()):
        u = a[x] if x in a else None
        v = b[x] if x in b else None
        if u is None and v is None:
          continue
        if u is None:
          ans[x] = v
        elif v is None or str(u) == str(v):
          ans[x] = u
        else:
          raise AssertionError(
            'CodeContext merger attempted with differing values for ' +
            'preprocessor defines (pp_defines): symbol "%s", values: %r and %r.' % (x, a, b)
          )
      return ans
    else:
      return a
  else:
    return b

cg_optlev_order = ('0','g','s','1','2','3','fast')

def _merge_optlev(a, b):
  if a and b:
    return cg_optlev_order[min(cg_optlev_order.index(a),
                               cg_optlev_order.index(b))]
  else:
    return a or b

_empty_list = []
_empty_dict = {}
_empty_froset = frozenset()

class CodeContext(object):
  _state_fields = (
    'compilers',
    'pp_angle_dirs',
    'pp_quote_dirs',
    'pp_defines',
    'pp_misc',
    'cg_optlev',
    'cg_misc',
    'ld_misc',
    'lib_paths',
    'lib_misc'
  )
  
  def __init__(me,
               compiler=None,
               compilers=_empty_froset,
               pp_angle_dirs=_empty_list,
               pp_quote_dirs=_empty_list,
               pp_defines=_empty_dict,
               pp_misc=_empty_list,
               cg_optlev='',
               cg_misc=_empty_list,
               ld_misc=_empty_list,
               lib_paths=_empty_list,
               lib_misc=_empty_list):

    if compiler:
      compilers = compilers | frozenset(((compiler,),) if type(compiler) is str else (tuple(compiler),))
    
    if pp_defines:
      pp_defines = {str(x): str(y) for x,y in pp_defines.items() if y is not None}

    cg_optlev = str(cg_optlev)
    
    locvars = locals()
    fields = CodeContext._state_fields
    me.__dict__.update({field: locvars[field] for field in fields})
    me.digest = b'H' + brutal.digest_of(*[locvars[field] for field in fields])
    me._brutal_digest_memo = me.digest

  def __or__(a, b):
    return CodeContext(
      compilers = (a.compilers | b.compilers) or _empty_froset,
      pp_angle_dirs = _merge_flags(a.pp_angle_dirs, b.pp_angle_dirs),
      pp_quote_dirs = _merge_flags(a.pp_quote_dirs, b.pp_quote_dirs),
      pp_defines = _merge_ppdefs(a.pp_defines, b.pp_defines),
      pp_misc = _merge_flags(a.pp_misc, b.pp_misc),
      cg_optlev = _merge_optlev(a.cg_optlev, b.cg_optlev),
      cg_misc = _merge_flags(a.cg_misc, b.cg_misc),
      ld_misc = _merge_flags(a.ld_misc, a.ld_misc),
      lib_paths = _merge_flags(a.lib_paths, b.lib_paths),
      lib_misc = _merge_flags(a.lib_misc, b.lib_misc)
    )
  
  def with_dependencies(me, *dependencies):
    acc = None
    for d in dependencies:
      if acc:
        acc |= d
      else:
        acc = d
    return (acc|me) if acc else me

  def with_updates(me, **kws):
    fields = CodeContext._state_fields
    d = me.__dict__
    d = dict(zip(fields, [d[x] for x in fields]))
    d.update(kws)
    return CodeContext(**d)
  
  def __getstate__(me):
    return tuple(me.__dict__[x] for x in CodeContext._state_fields)

  def __setstate__(me, s):
    me.__init__(**dict(zip(CodeContext._state_fields, s)))
  
  def __eq__(a, b): return a.digest == b.digest
  def __ne__(a, b): return a.digest != b.digest
  def __lt__(a, b): return a.digest <  b.digest
  def __le__(a, b): return a.digest <= b.digest
  def __gt__(a, b): return a.digest >  b.digest
  def __ge__(a, b): return a.digest >= b.digest

  def __hash__(me): return hash(me.digest)

  def compiler(me):
    cs = me.compilers
    if len(cs) == 0: raise AssertionError("No compiler exists in CodeContext.")
    if len(cs)  > 1: raise AssertionError("Compiler is not unique in CodeContext: "+', '.join(map(repr, cs)))
    return list(next(iter(cs)))
  
  def pp_flags(me):
    return (['-I'+dir for dir in me.pp_angle_dirs] +
            ['-iquote'+dir for dir in me.pp_quote_dirs] +
            ['-D%s=%s'%(x, me.pp_defines[x]) for x in sorted(me.pp_defines)] +
            me.pp_misc)

  def cg_flags(me):
    return (['-O%s'%me.cg_optlev] if me.cg_optlev!='' else []) + me.cg_misc

  def ld_flags(me):
    return me.ld_misc

  def lib_flags(me):
    dirs, names = ((),()) if not me.lib_paths else zip(*[brutal.os.path.split(path) for path in me.lib_paths])
    dirs = list(dirs)
    names = list(names)
    for nm in names:
      brutal.error_unless(nm[:3]=='lib' and nm[-2:]=='.a', 'Malformed library name (ought to be like lib*.a): '+nm)
    return me.lib_misc + ['-L'+dir for dir in dirs] + ['-l'+nm[3:-2] for nm in names]

  def __str__(me):
    return repr(me)

  def __repr__(me):
    return (
      'CodeContext(' +
      ''.join(['\n %s=%r,'%(x,y) for x,y in zip(CodeContext._state_fields, me.__getstate__())]) +
      ')'
    )

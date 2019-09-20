def _everything():
  import binascii
  import builtins
  import os
  import re
  import sys
  import types

  from . import coflow
  from . import memodb
  from . import digest
  from . import noiselog
  from . import opsys
  from .panic import panic, panic_unless
  import brutal
  panic_unless(brutal.coflow is coflow)
  
  def export(fn):
    globals()[fn.__name__] = fn
    brutal.__dict__[fn.__name__] = fn
    digest.by_name(fn)
    return fn
  
  __import__ = builtins.__import__
  AttributeError = builtins.AttributeError
  float = builtins.float
  dict = builtins.dict
  getattr = builtins.getattr
  int = builtins.int
  isinstance = builtins.isinstance
  IOError = builtins.IOError
  KeyError = builtins.KeyError
  len = builtins.len
  list = builtins.list
  NameError = builtins.NameError
  object = builtins.object
  open = builtins.open
  setattr = builtins.setattr
  str = builtins.str
  ValueError = builtins.ValueError

  os_path_abspath = os.path.abspath
  os_path_basename = os.path.basename
  os_path_dirname = os.path.dirname
  os_path_exists = os.path.exists
  os_path_join = os.path.join
  os_path_relpath = os.path.relpath
  os_path_samefile = os.path.samefile

  digest_of = digest.digest_of

  path_root = os.environ.get('BRUTAL_ROOT', os_path_join(os.getcwd(), 'brutal.py'))
  path_root = os_path_abspath(path_root)
  
  node_cache = {}
  node_root = None # defined below
  node_empty = (None, {}, None)

  BrutalModuleProxy = None # defined below

  imported_files = []
  globals()['imported_files'] = imported_files
    
  class Finder(object):
    def find_module(me, name, path=None):
      #print('find_module',name,path,'here',me.here)

      if path is None:
        return None
      
      if name == 'brutal':
        class Loader(object):
          def load_module(me, name):
            return brutal_module_proxy(me.here)
        return Loader()
      else:
        parts = name.rpartition('.')
        if re.match('brutal_rulefile_'+'[0-9a-f]'*16, parts[0]):
          file_path = os_path_join(path[0], parts[2]+'.py')
          if opsys.exists(file_path):
            class Loader(object):
              def load_module(me, name):
                return import_file(file_path, owner_package_name=parts[0])
            return Loader()
          else:
            return None
        else:
          return None
  
  def import_interposer(name, globals={}, locals={}, fromlist=[], level=0):
    here = globals.get('__brutal_here__')
    if here is not None:
      #print('import_interposer name='+name)
      sys.meta_path[0:0] = (Finder(),)
      mod = __import__(name, globals, locals, fromlist, level)
      del sys.meta_path[0]
    else:
      mod = __import__(name, globals, locals, fromlist, level)
    return mod
  
  builtins.__import__ = import_interposer

  @export
  def import_file(path, owner_package_name=None):
    _, _, m = node_at(path, owner_package_name=owner_package_name)
    return m

  def node_at(path_rule, path_rule_add=None, owner_package_name=None, explicit_parent=None):
    panic_unless(not(owner_package_name and explicit_parent))
    
    if path_rule_add:
      path_rule = os_path_join(path_rule, path_rule_add)
    path_rule = os_path_abspath(path_rule)
    
    if path_rule in node_cache:
      return node_cache[path_rule]

    path_here = os_path_dirname(path_rule)
    
    if owner_package_name:
      owner_mod = sys.modules[owner_package_name]
      if owner_mod.__file__ not in node_cache:
        print(path_rule, owner_package_name, node_cache)
      parent = node_cache[owner_mod.__file__]
    elif explicit_parent:
      owner_mod = None
      parent = node_at(explicit_parent)
    else:
      owner_mod = None
      
      try:
        same_as_root = os_path_samefile(path_root, path_rule)
      except OSError:
        same_as_root = False

      path_up = os_path_dirname(path_here)
      
      if same_as_root:
        nil = {}
        parent = node_empty
      elif path_here == path_up:
        parent = node_at(path_root)
      else:
        parent = node_at(path_up, 'brutal.py')
      
    try:
      with open(path_rule, 'r'): pass
      readable = True
    except IOError:
      readable = False
    
    if not readable:
      node = parent
      node_cache[path_rule] = node
    else:
      if owner_mod:
        mod_name = owner_package_name + '.' + os_path_basename(path_rule)[:-3]
      else:
        mod_name = 'brutal_rulefile_' + digest.hexdigest_of(path_rule)[:16]
      
      mod = types.ModuleType(mod_name)
      defs = mod.__dict__
      _, parent_defs, _ = parent
      defs.update(parent_defs)
      defs.update({
        '__name__': mod_name,
        '__package__': (owner_package_name if owner_package_name else mod_name),
        '__file__': path_rule,
        '__path__': None if owner_package_name else [path_here],
        '__brutal_rule_file__': path_rule,
        '__brutal_here__': path_here,
        '__brutal_parent_node__': parent,
        'brutal': brutal_module_proxy(path_here)
      })
      
      sys.modules[mod_name] = mod
      node = (parent, defs, mod)
      node_cache[path_rule] = node
      
      if not(owner_package_name or explicit_parent):
        imported_files.append(path_rule)
      
      with open(path_rule,'r') as f:
        code = compile(f.read(), path_rule, 'exec')
      exec(code, defs, None)
    
    return node

  rule_args = {}
  rule_cli = {}

  @memodb.traced
  def locate_rule_fn(fn_name, path_rule, path_arg_ix, args, kws):
    if 'PATH' in kws:
      kws['PATH'] = path = os_path_abspath(kws['PATH'])
      path = os_path_dirname(path)
      path_add = 'brutal.py'
    elif path_arg_ix is not None:
      if path_arg_ix >= len(args):
        panic('Required argument "PATH" not present.')
      path = os_path_abspath(args[path_arg_ix])
      args = list(args)
      args[path_arg_ix] = path
      path = os_path_dirname(path)
      path_add = 'brutal.py'
    else:
      path = path_rule
      path_add = ''
    
    par_nd, defs, _ = node_at(path, path_add)
    
    while fn_name not in defs:
      panic_unless(par_nd is not None)
      par_nd, defs = par_nd

    return (defs[fn_name]._brutal_rule_fn, args, kws)

  def rule(fn=None, caching=None, cli=None, traced=None):
    def make_rule(fn):
      fn_name = fn.__name__
      unwrapped = fn
      while hasattr(unwrapped, '__wrapped__'):
        unwrapped = unwrapped.__wrapped__
      co_argcount = unwrapped.__code__.co_argcount
      co_argnames = unwrapped.__code__.co_varnames[0:co_argcount]
      
      par_nd = unwrapped.__globals__['__brutal_parent_node__']
      path_rule = unwrapped.__globals__['__brutal_rule_file__']
      
      path_arg_ix = None
      if 'PATH' in co_argnames:
        path_arg_ix = co_argnames.index('PATH')

      if rule_args.get(fn_name, co_argnames) != co_argnames:
        panic(
          "Rule definition '%s:%s' may not change argument names wrt other rule instances." %
          (path_rule, fn_name)
        )
      rule_args[fn_name] = co_argnames
      
      if cli is not None and rule_cli.get(fn_name, cli) != cli:
        panic(
          "Rule definition '%s:%s' may not change 'cli' attribute." %
          (path_rule, fn_name)
        )
      rule_cli[fn_name] = cli or rule_cli.get(fn_name)

      if caching == 'process':
        memo = {}
        fn_core = fn
        @digest.by_other(fn_core)
        def proxy(*args, **kws):
          key = (args, kws)
          try: hash(key)
          except TypeError: key = digest.digest_of(key)
          
          if key in memo:
            return memo[key]
          else:
            ans = coflow.Result(fn_core(*args, **kws))
            memo[key] = ans
          return ans.value()
        
        proxy.__doc__ = fn.__doc__
        proxy.__name__ = fn_name
        proxy.__module__ = fn.__module__
        proxy.__wrapped__ = fn
        fn = proxy
      elif caching == 'file':
        fn = memodb.memoized(fn)
      elif caching is None:
        pass
      else:
        panic("brutal.rule(): invalid argument: caching=%r"%caching)

      if traced:
        if caching == 'file':
          panic("brutal.rule(): invalid argument: caching=%r, traced=%r"%(caching, traced))
        fn = memodb.traced(fn)
      
      def resolving_proxy(*args, **kws):
        fn, args, kws = locate_rule_fn(fn_name, path_rule, path_arg_ix, args, kws)
        return fn(*args, **kws)
      resolving_proxy._brutal_rule_fn = fn
      resolving_proxy.__wrapped__ = fn
      
      par_defs = par_nd[1]
      
      if fn_name in par_defs:
        resolving_proxy.parent_rule = par_defs[fn_name]._brutal_rule_fn
      else:
        resolving_proxy.parent_rule = None
      
      return resolving_proxy
    
    if fn is not None:
      return make_rule(fn)
    else:
      return make_rule

  proxy_memo = {}
  def brutal_module_proxy(here):
    if here not in proxy_memo:
      name = 'brutal_proxy_' + digest.hexdigest_of(here)[:16]
      mod = types.ModuleType(name)
      mod.__dict__.update(brutal.__dict__)
      mod.__dict__.update({
        'here': lambda *relpath: os_path_join(here, *relpath),
        'rule': rule,
        'import_tree': lambda path: node_at(path, 'brutal.py', None, os_path_join(here,'brutal.py'))[2]
      })
      sys.modules[name] = mod
      proxy_memo[here] = mod
    return proxy_memo[here]

  @export
  def cli_hooks():
    _, root_defs, _ = node_at(os.getcwd(), 'brutal.py')
    
    ans = dict(root_defs)
    for nm in root_defs.keys():
      do_del = nm in ('brutal','__brutal_rule_file__','__brutal_parent_node__')
      do_del |= type(root_defs[nm]) is type(sys)
      if do_del: del ans[nm]
    
    for nm,cli in rule_cli.items():
      if cli is not None:
        ans[cli] = root_defs[nm]
    return ans

_everything()
del _everything

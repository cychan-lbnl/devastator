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
  
  export = digest.exporter(globals())
  
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
  hexdigest_of = digest.hexdigest_of

  path_root = os.environ.get('BRUTAL_ROOT', os_path_join(os.getcwd(), 'brutal.py'))
  path_root = os_path_abspath(path_root)
  
  node_cache = {}
  node_root = None # defined below
  node_empty = (None, {}, None)

  BrutalModuleProxy = None # defined below

  imported_files = []
  globals()['imported_files'] = imported_files
    
  @digest.by_name
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
  
  @digest.by_name
  def import_interposer(name, globals={}, locals={}, fromlist=[], level=0):
    here = globals.get('_brutal_here')
    if here is not None:
      #print('import_interposer name='+name)
      sys.meta_path[0:0] = (Finder(),)
      mod = __import__(name, globals, locals, fromlist, level)
      del sys.meta_path[0]
    else:
      mod = __import__(name, globals, locals, fromlist, level)
    return mod
  
  builtins.__import__ = import_interposer

  @digest.by_name
  class ChildModule(types.ModuleType):
    def __init__(me, name, here, parent_here):
      types.ModuleType.__init__(me, name)
      me._brutal_here = here
      me._brutal_parent_here = parent_here
      me._brutal_imported = False
      
    def __getattr__(me, name):
      if not me._brutal_imported:
        panic('Child modules cannot be accessed from the toplevel scope of the importing module.')
      else:
        return me.__dict__[name]

  proxy_module_memo = {}
  child_module_memo = {}

  @digest.by_name
  def brutal_module_proxy(here):
    if here not in proxy_module_memo:
      name = 'brutal_proxy_' + hexdigest_of(here)[:16]
      mod = types.ModuleType(name)
      mod.__dict__.update(brutal.__dict__)

      child_modules = []
      def import_child(path):
        if path not in child_module_memo:
          if mod._brutal_exec_done:
            panic("brutal.import_child(): Function only available during the parent's toplevel import scope.")
          name = 'brutal_child_' + hexdigest_of(path)[:16]
          submod = ChildModule(name, path, here)
          sys.modules[name] = submod
          child_module_memo[path] = submod
          child_modules.append(submod)
        else:
          submod = child_module_memo[path]
          if submod._brutal_parent_here != here: panic(
            'Attempted import of child rulefile from two different parent directories: "{0}", "{1}"',
            submod._brutal_parent_here, here
          )
        
        return submod
      
      mod.__dict__.update({
        'here': lambda *relpath: os_path_join(here, *relpath),
        'rule': rule,
        '_child_modules': child_modules,
        'import_child': import_child,
        '_brutal_exec_done': False
      })
      sys.modules[name] = mod
      proxy_module_memo[here] = mod
    
    return proxy_module_memo[here]

  @digest.by_name
  def import_child_module(submod):
    _, subdefs, _ = node_at(
      submod._brutal_here, 'brutal.py', None,
      os_path_join(submod._brutal_parent_here, 'brutal.py')
    )
    submod.__dict__.update({
      x: (y if isinstance(y, ChildModule) else None) or getattr(y, '_brutal_scoped_rule', None) or y
      for x,y in subdefs.items()
    })
    submod._brutal_imported = True
  
  @export
  def import_file(path, owner_package_name=None):
    _, _, m = node_at(path, owner_package_name=owner_package_name)
    return m

  @digest.by_name
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
        mod_name = 'brutal_rulefile_' + hexdigest_of(path_rule)[:16]
      
      mod = types.ModuleType(mod_name)
      defs = mod.__dict__
      _, parent_defs, _ = parent
      defs.update(parent_defs)
      brutal_proxy = brutal_module_proxy(path_here)
      defs.update({
        '__name__': mod_name,
        '__package__': (owner_package_name if owner_package_name else mod_name),
        '__file__': path_rule,
        '__path__': None if owner_package_name else [path_here],
        '_brutal_rulefile': path_rule,
        '_brutal_here': path_here,
        '_brutal_parent_node': parent,
        'brutal': brutal_proxy
      })
      
      sys.modules[mod_name] = mod
      node = (parent, defs, mod)
      node_cache[path_rule] = node
      
      if not(owner_package_name or explicit_parent):
        imported_files.append(path_rule)
      
      with open(path_rule,'r') as f:
        code = compile(f.read(), path_rule, 'exec')
      exec(code, defs, None)

      if not(owner_package_name or explicit_parent):
        defs['_brutal_exec_done'] = True
        for submod in brutal_proxy._child_modules:
          import_child_module(submod)
    
    return node

  rule_args = {}
  rule_cli = {}

  @memodb.traced
  def locate_rule_fn(fn_name, path_top, path_bottom, scoped):
    par_nd, defs, _ = nd = node_at(path_bottom)
    
    while fn_name not in defs:
      panic_unless(par_nd is not None, '"{0}" not found starting at "{1}"', fn_name, path)
      par_nd, defs, _ = nd = par_nd
    
    if scoped:
      if path_top == path_root:
        path_here = path_root
        path_add = None
      else:
        path_here = os_path_dirname(path_top)
        path_add = 'brutal.py'

      nd_here = node_at(path_here, path_add)
      is_subnode = False

      while nd is not None:
        if nd is nd_here:
          is_subnode = True
          break
        nd = nd[0]

      if not is_subnode:
        defs = nd_here[1]

    return defs[fn_name]._brutal_rule_fn
        
  @digest.by_name
  def prepare_rule_call(fn_name, path_top, scoped, path_arg_ix, args, kws):
    if 'PATH' in kws:
      kws['PATH'] = path = os_path_abspath(kws['PATH'])
      path = os_path_join(os_path_dirname(path), 'brutal.py')
    elif path_arg_ix is not None:
      if path_arg_ix >= len(args):
        panic('Required argument "PATH" not present.')
      path = os_path_abspath(args[path_arg_ix])
      args = list(args)
      args[path_arg_ix] = path
      path = os_path_join(os_path_dirname(path), 'brutal.py')
    else:
      path = path_top

    fn = locate_rule_fn(fn_name, path_top, path, scoped)
    return (fn, args, kws)

  @digest.by_name
  def rule(fn=None, caching=None, cli=None, traced=None):
    def make_rule(fn):
      fn_name = fn.__name__
      unwrapped = fn
      while hasattr(unwrapped, '__wrapped__'):
        unwrapped = unwrapped.__wrapped__
      co_argcount = unwrapped.__code__.co_argcount
      co_argnames = unwrapped.__code__.co_varnames[0:co_argcount]
      
      unwrapped__globals__ = unwrapped.__globals__
      par_nd = unwrapped__globals__['_brutal_parent_node']
      path_rule = unwrapped__globals__['_brutal_rulefile']
      
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

      if traced:
        if caching == 'file':
          panic("brutal.rule(): invalid argument: caching=%r, traced=%r"%(caching, traced))
        fn = memodb.traced(fn)
      else:
        if caching == 'memory':
          fn = coflow.memoized(fn)
        elif caching == 'file':
          fn = memodb.memoized(fn)
        elif caching is None:
          pass
        else:
          panic("brutal.rule(): invalid argument: caching=%r"%caching)
        
        fn.as_named = lambda *a,**kw: memodb.Named(fn(*a,**kw))
      
      def make_resolving_proxy(fn, scoped):
        def resolving_proxy(*args, **kws):
          fn, args, kws = prepare_rule_call(fn_name, path_rule, scoped, path_arg_ix, args, kws)
          return fn(*args, **kws)

        def as_named(*args, **kws):
          fn, args, kws = prepare_rule_call(fn_name, path_rule, scoped, path_arg_ix, args, kws)
          return fn.as_named(*args, **kws)

        resolving_proxy._brutal_rule_fn = fn
        resolving_proxy.__wrapped__ = fn
        resolving_proxy.as_named = as_named
        return resolving_proxy

      resolving_proxy = make_resolving_proxy(fn, False)
      resolving_proxy._brutal_scoped_rule = make_resolving_proxy(fn, True)
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

  @export
  def cli_hooks():
    _, root_defs, _ = node_at(os.getcwd(), 'brutal.py')
    
    ans = dict(root_defs)
    for nm in root_defs.keys():
      do_del = nm in ('brutal','_brutal_rulefile','_brutal_parent_node')
      do_del |= type(root_defs[nm]) is type(sys)
      if do_del: del ans[nm]
    
    for nm,cli in rule_cli.items():
      if cli is not None:
        ans[cli] = root_defs[nm]
    return ans

_everything()
del _everything

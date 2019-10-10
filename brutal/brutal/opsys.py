def _everything():
  import os
  import shutil
  import stat
  import tempfile
  
  os_link = os.link
  os_listdir = os.listdir
  os_lstat = os.lstat
  os_makedirs = os.makedirs
  os_path_abspath = os.path.abspath
  os_path_dirname = os.path.dirname
  os_path_exists = os.path.exists
  os_path_isdir = os.path.isdir
  os_path_isfile = os.path.isfile
  os_path_islink = os.path.islink
  os_path_join = os.path.join
  os_path_normcase = os.path.normcase
  os_path_normpath = os.path.normpath
  os_path_relpath = os.path.relpath
  os_path_sep = os.path.sep
  os_path_split = os.path.split
  os_readlink = os.readlink
  os_remove = os.remove
  os_rmdir = os.rmdir
  os_stat = os.stat
  os_symlink = os.symlink

  shutil_copyfile = shutil.copyfile
  shutil_copymode = shutil.copymode

  stat_S_ISLNK = stat.S_ISLNK
  
  for x in dir(os):
    if not x.startswith('_'):
      globals()[x] = os.__dict__[x]

  for x in dir(shutil):
    if not x.startswith('_'):
      globals()[x] = shutil.__dict__[x]
    
  def memoize(fn):
    memo = {}
    memo_get = memo.get
    def proxy(x):
      y = memo_get(x, memo_get)
      if y is memo_get:
        y = fn(x)
        memo[x] = y
      return y
    proxy.__doc__ = fn.__doc__
    proxy.__name__ = fn.__name__
    proxy.__wrapped__ = getattr(fn, '__wrapped__', fn)
    return proxy
  
  def memoize_true(fn):
    memo = {}
    memo_get = memo.get
    memo_pop = memo.pop
    lru = []
    lru_append = lru.append
    lru_index = lru.index
    def proxy(x):
      y = memo_get(x, memo_get)
      if y is memo_get:
        y = fn(x)
        memo[x] = y
        if not y:
          if len(lru) > 50:
            memo_pop(lru[0])
            del lru[0]
          lru_append(x)
      elif not y:
        del lru[lru_index(x)]
        lru_append(x)
      return y
    proxy.__doc__ = fn.__doc__
    proxy.__name__ = fn.__name__
    proxy.__wrapped__ = fn
    return proxy
  
  def export(fn):
    globals()[fn.__name__] = fn
    return fn
  
  with tempfile.NamedTemporaryFile(prefix='TmP') as f:
    is_case_sensitive = not os_path_exists(f.name.lower())
  
  globals()['is_case_sensitive'] = is_case_sensitive

  os_path_exists_memoized = memoize(os_path_exists)
  
  @export
  @memoize
  def isfile(path):
    return os_path_isfile(path)
  
  @export
  @memoize
  def isdir(path):
    return os_path_isdir(path)

  @export
  @memoize
  def listdir(path):
    """A memoized version of `os.listdir`. More performant, but only useful
    if you don't expect the queried directory to change during this
    program's lifetime."""
    return frozenset(os_listdir(path))
  
  @export
  def rmtree(path):
    """Delete file or directory-tree at path."""
    try:
      os_remove(path)
    except OSError as e:
      if e.errno == 21: # is a directory
        try:
          for f in os_listdir(path):
            rmtree(os_path_join(path, f))
          os_rmdir(path)
        except OSError:
          pass
  
  @export
  @memoize
  def lstat(path):
    """
    Memoized version of os.lstat.
    """
    return os_lstat(path)
  
  @export
  def islink(path):
    return stat_S_ISLNK(lstat(path).st_mode)
  
  @export
  @memoize
  def unsym_once(path):
    """
    Like os.realpath, but will remove only one level of symlink
    from `path` and will not make relative paths absolute.
    Also memoized.
    """
    try:
      head, tail = os_path_split(path)
      
      if head == path:
        return head
      
      head1 = unsym_once(head)
      
      if head1 == head:
        if islink(path):
          return os_path_normpath(os_path_join(head, os_readlink(path)))
        else:
          return path
      else:
        return os_path_join(head1, tail)
    
    except OSError as e:
      if e.errno == 2: # No such file or directory
        return path
      else:
        raise
  
  @export
  @memoize
  def realpath(path):
    """
    Memoized version of os.path.realpath.
    """
    path = os_path_abspath(path)
    while True:
      path1 = unsym_once(path)
      if path == path1:
        return path
      path = path1
  
  @export
  @memoize
  def mtime(path):
    """
    Does a better job than os.path.getmtime because it takes the
    maximum mtime value over all symlinks encountered in `path`.
    Returns -1 for non-existent file. Also memoized.
    """
    if not exists(path):
      return -1
    
    latest_mtime = lstat(path).st_mtime
    while True:
      path1 = unsym_once(path)
      if path == path1: break
      latest_mtime = max(latest_mtime, lstat(path1).st_mtime)
      path = path1
    return latest_mtime

  @export
  def path_within_any(path, *prefix_paths):
    for pre in prefix_paths:
      rel = os_path_relpath(path, pre)
      if path == pre or not rel.startswith('..' + os_path_sep):
        return True
    return False
  
  @export
  def files_equal(a, b, false_negatives=False):
    if a == b:
      return True
    try:
      sa = os_stat(a)
      sb = os_stat(b)
      if sa.st_dev == sb.st_dev and sa.st_ino == sb.st_ino:
        return True
      if sa.st_size != sb.st_size or sa.st_mode != sb.st_mode:
        return False
      if false_negatives:
        return False
      with open(a, 'rb') as fa:
        with open(b, 'rb') as fb:
          size = sa.st_size
          n = 8192
          while size > 0:
            size -= n
            bufa = fa.read(n)
            bufb = fb.read(n)
            if bufa != bufb:
              return False
      return True
    except OSError:
      return False
  
  @export
  def link_or_copy(src, dst, overwrite=False):
    try:
      try:
        os_link(src, dst)
      except OSError as e:
        if e.errno == 18: # cross-device link
          shutil_copyfile(src, dst)
          shutil_copymode(src, dst)
        else:
          raise
    except OSError as e:
      if e.errno == 17: # File exists
        if files_equal(src, dst):
          return
        if overwrite:
          rmtree(dst)
          link_or_copy(src, dst)
        else:
          raise
      else:
        raise
  
  @export
  def ensure_dirs(path):
    d = os_path_dirname(path)
    if not os_path_exists(d):
      os_makedirs(d)
  
  @export
  def mktree(path, entries, symlinks=True):
    def enter(path, entries):
      try: os_makedirs(path)
      except OSError: pass
      
      for e_name, e_val in entries.items():
        path_and_name = os_path_join(path, e_name)
        
        if isinstance(e_val, dict):
          enter(path_and_name, e_val)
        else:
          if symlinks:
            os_symlink(e_val, path_and_name)
          else:
            if os_path_isfile(e_val):
              link_or_copy(e_val, path_and_name)
            elif os_path_isdir(e_val):
              enter(
                path_and_name,
                dict((nm, os_path_join(e_val,nm)) for nm in listdir(e_val))
              )
            elif os_path_islink(e_val):
              target = os_readlink(e_val)
              os_symlink(target, path_and_name)
    
    enter(path, entries)

  @export
  @memoize_true
  def exists(path):
    """
    A memoized version of `os.path.exists` that also respects
    correct upper/lower caseing in the filename (a mismatch in case
    reports as non-existence). More performant than `os.path.exists`,
    but only useful if you don't expect the existence of queried files
    to change during this program's lifetime.
    """
    head, tail = os_path_split(path)
    head_exists = os_path_exists_memoized(head)
    
    if head == path:
      return head_exists

    if head != '' and not head_exists:
      return False
    
    return tail in ('','.','..') or tail in listdir(head or '.')

  # Case-sensitive system:
  if is_case_sensitive:
    @export
    def realcase(path):
      """
      Returns the real upper/lower caseing of the given filename as
      stored in the filesystem. Your system is case-sensitive so this
      is the identity function.
      """
      return path
  
  # Case-insensitive system:
  else:
    @export
    @memoize
    def realcase():
      """
      Returns the real upper/lower caseing of the given filename as
      stored in the filesystem.
      """
      
      head, tail = os_path_split(path)
      if head == path:
        return path
      else:
        sibs = listdir(head)
        sibmap = dict((os_path_normcase(sib), sib) for sib in sibs)
        normtail = os_path_normcase(tail)
        return os_path_join(realcase(head), sibmap.get(normtail, tail))

_everything()
del _everything

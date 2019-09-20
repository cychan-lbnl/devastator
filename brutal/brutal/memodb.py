"""
brutal.memodb: Memoization of coflowhronous functions to a database file.
"""
def _everything():
  # Useful in debugging to leave temporary files/dirs in tact after
  # brutal exits so they may be autopsied.
  DONT_REMOVE_TEMPS = False

  import binascii
  import builtins
  import pickle
  import hashlib
  import os
  import shlex
  import sys
  import traceback
  import types
    
  import brutal
  from . import coflow
  from . import dedup
  from . import digest
  from . import opsys
  from .panic import panic, panic_unless
  
  if 1: # export & common stuff
    def export(x):
      globals()[x.__name__] = x
      brutal.__dict__[x.__name__] = x
      return x
    
    BaseException = builtins.BaseException
    dict = builtins.dict
    dir = builtins.dir
    Exception = builtins.Exception
    getattr = builtins.getattr
    isinstance = builtins.isinstance
    iter = builtins.iter
    KeyError = builtins.KeyError
    len = builtins.len
    list = builtins.list
    object = builtins.object
    OSError = builtins.OSError
    set = builtins.set
    sorted = builtins.sorted
    str = builtins.str
    super = builtins.super
    TypeError = builtins.TypeError
    ValueError = builtins.ValueError
    zip = builtins.zip
    
    types_FunctionType = types.FunctionType
    types_MethodType = types.MethodType
    
    os_link = os.link
    os_listdir = os.listdir
    os_makedirs = os.makedirs
    os_remove = os.remove
    os_rmdir = os.rmdir
    os_symlink = os.symlink
    os_walk = os.walk
    
    os_path_abspath = os.path.abspath
    os_path_join = os.path.join
    os_path_normpath = os.path.normpath
    os_path_sep = os.path.sep
    
    hexlify = binascii.hexlify
    
    coflow_Failure = coflow.Failure
    coflow_Future = coflow.Future
    coflow_coroutine = coflow.coroutine
    coflow_Promise = coflow.Promise
    coflow_Result = coflow.Result
    
    hashlib_sha1 = hashlib.sha1
    
    opsys_exists = opsys.exists
    opsys_link_or_copy = opsys.link_or_copy
    opsys_mtime = opsys.mtime
    opsys_rmtree = opsys.rmtree
    
    ingest = digest.ingest
    digest_of = digest.digest_of
    hexdigest_of = digest.hexdigest_of
    zero = digest.digest_zero
    
    empty_dict = {}
    empty_froset = frozenset()
  
  if not DONT_REMOVE_TEMPS:
    DONT_REMOVE_TEMPS = os.environ.get('BRUTAL_DEBUG', False)
  
  class Db(object): pass
  the_db = Db()
  the_db.initialized = False
  
  path_cwd = os.getcwd()
  path_site = os.environ.get('BRUTAL_SITE', '') or path_cwd
  path_db = os_path_join(path_site, '.brutal', 'db')
  path_art = os_path_join(path_site, '.brutal', 'art')
  
  @digest.by_name
  def artifact_path(pre, art_id, suf):
    pre += '.' if pre and not pre.endswith('.') else ''
    suf =  ('.' if suf and not suf.startswith('.') else '')  + suf
    return os_path_join(path_art, '%s%x%s'%(pre, art_id, suf))
  
  DB_TAG_BRANCH = 0
  DB_TAG_PRUNE = 1
  DB_TAG_FILE_DIGEST = 2
  DB_TAG_ART = 3
  DB_TAG_IMPORT = 4
  
  @digest.by_name
  def _db():
    db = the_db
    if not db.initialized:
      db_initialize()
      brutal.at_shutdown(save)
    return db
  
  @digest.by_name
  def db_initialize():
    db = the_db
    
    db.initialized = True
    db.dedup_encoder_table = {}
    # Tree = {name_dig: (0, full_dig, fn_id, arg1, arg2, Tree)
    #                 | (1, full_dig, changed:Promise)
    #                 | (2, full_dig, cfull, ename, efull, result_vals, result_kws, artifacts)
    #                 | (3, full_dig, cfull, ename, efull, Failure)}
    db.tree = {}
    db.files = {} # {dedup_encode(path): (mtime, fname, ffull)}
    db.art_id_bump = 0
    db.arts = {} # {art_id: (pre,suf,refn)}
    db.failed_name_logs = []
    db.pending_n = 0
    db.size_head = 0
    db.size_tail = 0
    db.lock = coflow.CriticalSection()
    db.quiesced = db.lock.make_condition(lambda: db.pending_n == 0)
    db.rule_files_seen = set()
    
    try: os.makedirs(path_art)
    except OSError: pass

    from . import rulefile
    
    db.dedup_decode = dedup.make_decoder({})

    if not opsys.exists(path_db):
      with open(path_db, 'wb') as f:
        pickle.dump(rulefile.imported_files, f, protocol=2)
        pickle.dump((db.dedup_encoder_table, db.tree, db.files, db.art_id_bump, db.arts), f, protocol=2)
      db.dedup_encode = dedup.make_encoder(db.dedup_encoder_table)
      db.size_head = 0
      db.size_tail = 0
    else:
      with open(path_db, 'rb') as f:
        pickle_load = pickle.load

        rule_files = pickle_load(f)
        for path in rule_files:
          rulefile.import_file(path)
        
        db.dedup_encoder_table, db.tree, db.files, db.art_id_bump, db.arts = pickle_load(f)
        db.size_head = f.tell()
        db.dedup_encode = dedup_encode = dedup.make_encoder(db.dedup_encoder_table)

        while True:
          try:
            record = pickle_load(f)
            record_tag = record[0]
            
            if record_tag == DB_TAG_BRANCH:
              _, log, cfull, ename, efull, result_vals, result_kws, arts = record
              tip = db.tree
              _, _, _, name, full = log[0]

              cfull = dedup_encode(cfull)
              ename = dedup_encode(ename)
              efull = dedup_encode(efull)
              name = dedup_encode(name)
              full = dedup_encode(full)
              
              for (next_fn_id, next_arg1, next_arg2, next_name, next_full) in log[1:]:
                next_name = dedup_encode(next_name)
                next_full = dedup_encode(next_full)
                next_fn_id = dedup_encode(next_fn_id)
                next_arg1 = dedup_encode(next_arg1)
                next_arg2 = dedup_encode(next_arg2)
                node = tip.get(name)
                if node is None or node[1] != full:
                  next_tip = {}
                  tip[name] = (0, full, next_fn_id, next_arg1, next_arg2, next_tip)
                  tip = next_tip
                else:
                  tag, _, node_fn_id, node_arg1, node_arg2, next_tip = node
                  panic_unless((node_fn_id, node_arg1, node_arg2) == (next_fn_id, next_arg1, next_arg2))
                  tip = next_tip
                name = next_name
                full = next_full

              result_vals = dedup_encode(result_vals)
              result_kws = dedup_encode(result_kws)
              tip[name] = (2, full, cfull, ename, efull, result_vals, result_kws, arts)
              
              for art_id in arts:
                pre, suf, refn = db.arts[art_id]
                db.arts[art_id] = (pre, suf, refn+1)

            elif record_tag == DB_TAG_PRUNE:
              _, name_log = record
              tip = db.tree
              fan_tip = tip
              fan_name = name_log[0]
              for name in name_log[:-1]:
                if len(tip) > 1:
                  fan_tip = tip
                  fan_name = name
                tip = tip[name][5]
              
              def prune_arts(node):
                tag = node[0]
                if tag == 0:
                  for name,node1 in node[5].items():
                    prune_arts(node1)
                elif tag == 2:
                  for art_id in node[7]:
                    if art_id not in db.arts:
                      panic('brutal internal error: assert art_id in db.arts')
                    pre, suf, refn = db.arts[art_id]
                    if refn != 1:
                      db.arts[art_id] = (pre, suf, refn-1)
                    else:
                      del db.arts[art_id]
                else:
                  panic()
              
              prune_arts(tip[name_log[-1]])
              
              if len(tip) > 1:
                del tip[name_log[-1]]
              else:
                del fan_tip[fan_name]
            
            elif record_tag == DB_TAG_FILE_DIGEST:
              _, apath, mtime, name, full = record
              db.files[dedup_encode(apath)] = (mtime, dedup_encode(name), dedup_encode(full))
              
            elif record_tag == DB_TAG_ART:
              _, pre, suf = record
              pre = dedup_encode(pre)
              art_id = db.art_id_bump
              db.art_id_bump += 1
              db.arts[art_id] = (pre, suf, 0)
            
            elif record_tag == DB_TAG_IMPORT:
              _, path = record
              rulefile.import_file(path)
            
            else:
              panic()
          
          except EOFError:
            db.size_tail = f.tell()
            break

      
  @digest.by_name
  def db_append_record(*rec):
    db = _db()
    from . import rulefile
    unseen_rules = db.rule_files_seen - set(rulefile.imported_files)
    db.rule_files_seen |= unseen_rules
    recs = [(DB_TAG_IMPORT, path) for path in unseen_rules]
    recs += (rec,)
    with open(path_db, 'ab', 0) as f:
      for rec in recs:
        #print('\n\n\nRECORD ',rec)
        rec = pickle.dumps(tuple(rec), protocol=2)
        f.write(rec)
      db.size_tail = f.tell()

  @export
  @digest.by_name
  @coflow_coroutine
  def save():
    db = the_db
    if not db.initialized:
      return

    #print('awaiting quiesced')
    release = yield db.lock.acquire(cond=db.quiesced)
    #print('got quiesced')
    
    try:
      if 0.33*db.size_head < db.size_tail - db.size_head:
        # prune failures
        for name_log in db.failed_name_logs:
          tip = db.tree
          fan_tip = tip
          fan_name = name_log[0]
          for name in name_log:
            if len(tip) > 1:
              fan_tip = tip
              fan_name = name
            node = tip[name]
            if node[0] == 0:
              tip = node[5]
            else:
              panic_unless(node[0] == 3) # failed path must end in Failure
              tip = None
          del fan_tip[fan_name]

        del db.failed_name_logs[:]

        from . import rulefile

        import sys
        sys.setrecursionlimit(1<<20)
        with open(path_db, 'wb') as f:
          try:
            pickle.dump(rulefile.imported_files, f, protocol=2)
            pickle.dump((db.dedup_encoder_table, db.tree, db.files, db.art_id_bump, db.arts), f, protocol=2)
          except TypeError:
            raise
          db.size_head = db.size_tail = f.tell()
          db.rule_files_seen = set(rulefile.imported_files)
    finally:
      release()

    while all_temps:
      tmp = all_temps.pop()
      opsys_rmtree(tmp)

    yield False
  
  @digest.by_name
  def query_file_digests(apath):
    db = _db()
    enc_apath = db.dedup_encode(apath)
    now_mtime = opsys_mtime(apath)

    if enc_apath in db.files:
      rec_mtime, fname, ffull = db.files[enc_apath]
    else:
      rec_mtime, fname, ffull = (-2, None, None)
    
    if rec_mtime != now_mtime:
      h = hashlib_sha1()
      h.update(b'%r' % apath)
      fname = h.digest()
      if now_mtime != -1:
        h.update(b'y') # exists?
        with open(apath, 'rb') as f:
          for chk in iter(lambda: f.read(8192), b''):
            h.update(chk)
      else:
        h.update(b'n') # exists?
      
      ffull = h.digest()
      
      if now_mtime != -1:
        db.files[enc_apath] = (now_mtime, fname, ffull)
        db_append_record(DB_TAG_FILE_DIGEST, apath, now_mtime, fname, ffull)
    
    return fname, ffull
  
  @digest.by_name
  class TraceShadow(coflow.Shadow):
    def key_of(me, rec):
      return id(rec)
    def emit(me, proxy, args, kws, cname, cfull, ename, efull):
      coflow.Shadow.emit(me, (proxy, args, kws, cname, cfull, ename, efull))
  TraceShadow = TraceShadow()
  
  @digest.by_name
  class ArtsShadow(coflow.Shadow):
    def key_of(me, rec):
      return rec
  ArtsShadow = ArtsShadow()
  
  builtin_traces = {}
  user_traces = {}
  
  @digest.by_name
  def encode_trace_id(fn):
    if isinstance(fn, str):
      panic_unless(fn in builtin_traces)
      return fn
    return fn._brutal_trace_id
  
  @digest.by_name
  def decode_trace_id(tr_id):
    if tr_id in builtin_traces:
      return builtin_traces[tr_id]
    else:
      name, meat = tr_id
      names = user_traces.get(name, None)
      if names is None:
        return None
      if len(names) == 1:
        return tuple(names.values())[0]
      return names.get(meat, None)
  
  @digest.by_name
  def tree_digests(fn_id, cname, cfull, ename, efull):
    h = hashlib_sha1()
    h.update(b'%r:%b%b' % (fn_id, cname, ename))
    name = h.digest()
    h.update(cfull + efull)
    full = h.digest()
    return name, full
  
  traced_hash_digest = binascii.unhexlify(b"b13b4fbd38684ba882c10426b457dc90")
  
  @export
  @digest.by_name
  def traced(fn):
    memo = {}
    def resultoid_and_digests(args, kws):
      arg_dig = digest_of(args, kws)
      
      if arg_dig in memo:
        return memo[arg_dig]

      log = []
      @coflow.capture_effects(
        (coflow.effect(TraceShadow, log.append),), False,
        fn, *args, **kws)
      def resultoid_and_digests(result):
        h = ingest(hashlib_sha1(), arg_dig, result)
        cname = cfull = h.digest()

        elog = sorted(set([(r[5],r[6]) for r in log]))

        if len(elog) == 0:
          ename, efull = zero, zero
        elif len(elog) == 1:
          ename, efull = elog[0]
        else:
          enames, efulls = zip(*elog)
          h = hashlib_sha1()
          h.update(b'%x.' % len(enames))
          h.update(b''.join(enames))
          ename = h.digest()
          h.update(b''.join(efulls))
          efull = h.digest()
        
        return (result.value, cname, cfull, ename, efull)

      memo[arg_dig] = resultoid_and_digests
      return resultoid_and_digests

    @digest.by_name(traced_hash_digest)
    def proxy(*args, **kws):
      @coflow.after(resultoid_and_digests, args, kws)
      def result(res_and_digs):
        resultoid, cname, cfull, ename, efull = res_and_digs.value()
        TraceShadow.emit(proxy, args, kws, cname, cfull, ename, efull)
        return resultoid()
      return result
  
    proxy._brutal_resultoid_and_digests = resultoid_and_digests
    proxy.__name__ = fn.__name__
    proxy.__doc__ = fn.__doc__
    proxy.__wrapped__ = fn
    
    tr_name = (fn.__module__, fn.__name__)
    tr_meat = digest_of(fn.__code__, fn.__closure__ or ())
    proxy._brutal_trace_id = (tr_name, tr_meat)
    if tr_name not in user_traces:
      user_traces[tr_name] = {}
    user_traces[tr_name][tr_meat] = resultoid_and_digests
    
    return proxy
  
  if 1: # depend_apath & depend_file
    def depend_apath_resultoid_and_digests():
      dummy = lambda:None
      def depend_apath_resultoid_and_digests(apath, _):
        cname, cfull = query_file_digests(apath)
        return (dummy, cname, cfull, zero, zero)
      return depend_apath_resultoid_and_digests
    depend_apath_resultoid_and_digests = depend_apath_resultoid_and_digests()
    
    builtin_traces['#apath'] = depend_apath_resultoid_and_digests
    
    def depend_apath(apath):
      _, cname, cfull, ename, efull = depend_apath_resultoid_and_digests(apath, None)
      TraceShadow.emit('#apath', apath, None, cname, cfull, ename, efull)
    
    @export
    @digest.by_name
    def depend_file(*paths):
      for p in paths:
        depend_apath(os_path_abspath(p))
  
  if 1: # depend_fact
    def depend_fact_resultoid_and_digests(factdict):
      efull = digest_of(factdict)
      return (lambda:None, zero, zero, efull, efull)
    
    builtin_traces['#fact'] = depend_fact_resultoid_and_digests
    
    @export
    @digest.by_name
    def depend_fact(key=None, val=None, **kws):
      if key is not None:
        kws[key] = val
      _, cname, cfull, ename, efull = depend_fact_resultoid_and_digests(kws)
      TraceShadow.emit('#fact', kws, None, cname, cfull, ename, efull)
  
  if 1: # env
    def env_resultoid_and_digests():
      memo = {}
      def env_resultoid_and_digests(name, default):
        key = (name, default)
        try: hash(key)
        except TypeError: key = hexdigest_of(key)
        
        if key in memo:
          return memo[key]
        
        s = os.environ.get(name, None)

        def parse(s):
          if s.startswith('%'):
            return eval(s[1:])
          for ty in (int, float):
            try: val = ty(s); break
            except: pass
          return s
        
        if s is None or s == '':
          val = default
        elif s.startswith('%'):
          val = eval(s[1:])
        elif type(default) is bool:
          val = {'0':False, 'false':False, '1':True, 'true':True}[s.lower()]
        elif type(default) in (int, float):
          val = eval(s)
        elif type(default) in (tuple, list, set, frozenset):
          val = type(default)([parse(x) for x in shlex.split(s)])
        elif type(default) is dict:
          import re
          val = {}
          toks = shlex.split(s)
          for tok in toks:
            m = re.match('([^=:]+)[=:]', tok)
            if not m: break
            x = parse(m.group(1))
            y = parse(tok[len(m.group(0)):])
            val[x] = y
        else:
          val = s
        
        full = digest_of(name, default, val)
        ans = (lambda:val, full, full, zero, zero)
        memo[key] = ans
        return ans
      return env_resultoid_and_digests
    env_resultoid_and_digests = env_resultoid_and_digests()
    
    builtin_traces['#env'] = env_resultoid_and_digests

    @digest.by_name
    class Env(object):
      def __getattr__(me, name):
        return lambda default=None: me(name, default)
      
      def __call__(me, name, default=None):
        val_fn, full, full, zero, zero = env_resultoid_and_digests(name, default)
        TraceShadow.emit('#env', name, default, full, full, zero, zero)
        return val_fn()
    
    brutal.env = globals()['env'] = Env()
  
  @export
  @digest.by_name
  def memoized(fn):
    @coflow_coroutine
    def proxy(*args, **kws):
      resultoid, cname, cfull, ename, efull = yield memo_resultoid_and_digests(fn, args, kws)
      TraceShadow.emit(proxy, args, kws, cname, cfull, ename, efull)
      yield resultoid()
    
    resultoid_and_digests = lambda args,kws: memo_resultoid_and_digests(fn, args, kws)
    
    proxy._brutal_digest_memo = traced_hash_digest
    proxy._brutal_resultoid_and_digests = resultoid_and_digests
    proxy.__name__ = fn.__name__
    proxy.__doc__ = fn.__doc__
    proxy.__wrapped__ = fn
    
    tr_name = (fn.__module__, fn.__name__)
    tr_meat = digest_of(fn.__code__, fn.__closure__ or ())
    proxy._brutal_trace_id = (tr_name, tr_meat)
    if tr_name not in user_traces:
      user_traces[tr_name] = {}
    user_traces[tr_name][tr_meat] = resultoid_and_digests
    
    return proxy
  
  @export
  @digest.by_name
  def mkpath(prefix='', suffix=''):
    _, prefix = os.path.split(prefix)
    panic_unless(os_path_sep not in suffix)
    
    db = _db()
    art_id = db.art_id_bump
    db.art_id_bump += 1
    db.arts[art_id] = (db.dedup_encode(prefix), suffix, 0)
    ArtsShadow.emit(art_id)
    apath = artifact_path(prefix, art_id, suffix)
    
    db_append_record(DB_TAG_ART, prefix, suffix)
    
    opsys_rmtree(apath)
    return apath
  
  all_temps = set()
  
  @export
  @digest.by_name
  def mktemp(me, *mkstemp_args, **mkstemp_kws):
    import tempfile
    fd, path = tempfile.mkstemp(*mkstemp_args, **mkstemp_kws)
    os.close(fd)
    os.remove(path)
    all_temps.add(path)
    return path
  
  @export
  @digest.by_name
  def mkstemp(me, *mkstemp_args, **mkstemp_kws):
    import tempfile
    fd, path = tempfile.mkstemp(*mkstemp_args, **mkstemp_kws)
    all_temps.add(path)
    return fd, path
  
  @export
  @digest.by_name
  def mkdtemp(me):
    import tempfile
    path = tempfile.mkdtemp()
    all_temps.add(path)
    return path
  
  @coflow_coroutine
  @digest.by_name
  def memo_resultoid_and_digests(memo_fn, memo_args, memo_kws):
    db = _db()
    db_lock_release = yield db.lock.acquire()
    db_dedup_decode = db.dedup_decode
    db_dedup_encode = db.dedup_encode
    
    # run down tree
    tip = db.tree
    memo_cfull = name = full = digest_of(memo_fn, memo_args, memo_kws)
    log = [(None, None, None, name, full)] # [(fn_id, arg1, arg2, name, full),...]
    edigs = set()
    do_prune = False

    prev_node = None
    node = None
    
    while True:
      prev_node = node
      node = tip[name] if name in tip else None
      tag = None if node is None else node[0]

      if node is None or node[1] != full:
        if node is not None:
          if node[0] == 1:
            panic('brutal internal error: Same trace and instance generated different full hashes.')
          do_prune = True
        if 0:
          print('failed, %s not in %r: '%(hexlify(name), [hexlify(x) for x in tip]),'\n',
                memo_fn.__name__,memo_args,'\n ',
                prev_node[2:5] if prev_node else None)
        break # jump to execute
        
      elif tag == 1:
        db_lock_release()
        _, _, subtree_changed = node
        yield subtree_changed
        db_lock_release = yield db.lock.acquire()
        
      elif tag in (2,3):
        db_lock_release()
        
        #print('\n\nmemo result',memo_fn.__name__,memo_args)
        
        if tag == 2:
          _, _, cfull, ename, efull, res_vals, res_kws, _ = node
          res_vals = db_dedup_decode(res_vals)
          res_kws = db_dedup_decode(res_kws)
          yield lambda:coflow_Result(*res_vals, **res_kws), cfull, cfull, ename, efull
        else:
          _, _, cfull, ename, efull, failure = node
          yield lambda:failure, cfull, cfull, ename, efull
        return
      
      elif tag == 0:
        _, _, fn_id, arg1, arg2, tip1 = node
        fn_id = db_dedup_decode(fn_id)
        arg1 = db_dedup_decode(arg1)
        arg2 = db_dedup_decode(arg2)

        trace_fn = decode_trace_id(fn_id)
        
        if trace_fn is None:
          do_prune = True
          break
        
        db_lock_release()
        
        #print(memo_fn.__name__, memo_args,': tracing',fn_id)
        
        _, cname, cfull, ename, efull = yield trace_fn(arg1, arg2)
        edigs.add((ename, efull))
        name, full = tree_digests(fn_id, cname, cfull, ename, efull)
          
        db_lock_release = yield db.lock.acquire()
        tip = tip1
        log.append((fn_id, arg1, arg2, name, full))
    
    if do_prune:
      # commit prune record to file
      db_append_record(DB_TAG_PRUNE, tuple(zip(*log))[3])
      
      # delete orphaned artifacts
      def prune_arts(node):
        tag = node[0]
        if tag == 0:
          for name,node1 in node[5].items():
            prune_arts(node1)
        elif tag == 1:
          panic('brutal internal error: Same trace and instance generated different full hashes.')
        elif tag == 2:
          for art_id in node[7]:
            if art_id not in db.arts:
              panic('brutal internal error: assert art_id in db.arts')
            pre, suf, refn = db.arts[art_id]
            if refn != 1:
              db.arts[art_id] = (pre, suf, refn-1)
            else:
              del db.arts[art_id]
              prefix = db_dedup_decode(pre)
              path_art = artifact_path(prefix, art_id, suf)
              opsys_rmtree(path_art)
      
      prune_arts(node)
    
    tip[name] = (1, full, coflow_Promise())
    #print('pending++',db.pending_n,memo_fn.__name__,memo_args)
    db.pending_n += 1
    db_lock_release()
    
    #-- execute -----------------------------------------------------------

    inserter_state = [tip, name, full]
    def inserter(rec):
      fn, arg1, arg2, cname, cfull, ename, efull = rec
      edigs.add((ename, efull))

      fn_id = encode_trace_id(fn)
      name1, full1 = tree_digests(fn_id, cname, cfull, ename, efull)

      if fn_id == '#fact':
        panic('`brutal.depend_fact` can only be called within a traced function.')

      log.append((fn_id, arg1, arg2, name1, full1))
      
      tip0, name0, full0 = inserter_state
      _, _, changed0 = tip0[name0]
      tip1 = {name1: (1, full1, coflow_Promise())}
      name0 = db_dedup_encode(name0)
      full0 = db_dedup_encode(full0)
      fn_id = db_dedup_encode(fn_id)
      arg1 = db_dedup_encode(arg1)
      arg2 = db_dedup_encode(arg2)
      tip0[name0] = (0, full0, fn_id, arg1, arg2, tip1)
      changed0.satisfy()
      
      inserter_state[:] = (tip1, name1, full1)

    arts = set()

    result = coflow.capture_effects(
        (coflow.effect(TraceShadow, inserter),
         coflow.effect(ArtsShadow, arts.add)), False,
        memo_fn, *memo_args, **memo_kws
      )(lambda result: result)

    yield coflow.mbind_wrapped(result)(lambda fu: None)
    result = result.result()

    tip, name, full = inserter_state

    #print memo_fn.__name__ + repr(memo_args)
    #for i,r in zip(range(len(log)), log):
    #  print '  log '+str(i)+':'+repr((r[0],r[1],r[2],hexlify(r[3]),hexlify(r[4])))
    
    # add result to call hash
    h = hashlib_sha1()
    h.update(memo_cfull)
    memo_cfull = ingest(h, result).digest()
    
    # compute ename, efull from log
    if len(edigs) == 0:
      ename, efull = zero, zero
    elif len(edigs) == 1:
      ename, efull = edigs.pop()
    else:
      h = hashlib_sha1()
      enames, efulls = zip(*sorted(edigs))
      h.update(b'%x.' % len(enames))
      h.update(b''.join(enames))
      ename = h.digest()
      h.update(b''.join(efulls))
      efull = h.digest()
    
    # finish tree
    db_lock_release = yield db.lock.acquire()
    db.pending_n -= 1
    #print('pending--',memo_fn.__name__,memo_args)
    
    _, _, _, name, full = log[-1]
    _, _, subtree_changed = tip[name]

    name = db_dedup_encode(name)
    full = db_dedup_encode(full)
    
    if isinstance(result, coflow_Result):
      res_vals, res_kws = result.values(), result.kws()
      enc_vals = db_dedup_encode(res_vals)
      enc_kws = db_dedup_encode(res_kws)
      tip[name] = (2, full, memo_cfull, ename, efull, enc_vals, enc_kws, arts)
      for art_id in arts:
        pre, suf, refn = db.arts[art_id]
        db.arts[art_id] = (pre, suf, refn+1)
      db_append_record(DB_TAG_BRANCH, log, memo_cfull, ename, efull, res_vals, res_kws, arts)
    elif isinstance(result, coflow_Failure):
      tip[name] = (3, full, memo_cfull, ename, efull, result)
      db.failed_name_logs.append(tuple(zip(*log))[3])
    else:
      panic()
    
    db_lock_release()
    subtree_changed.satisfy()
    
    yield (lambda:result, memo_cfull, memo_cfull, ename, efull)
  
_everything()
del _everything

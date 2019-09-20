"""
brutal.coflow: Asynchronous programming constructs based on futures and
coroutines.
"""
def _everything():
  import builtins
  import collections
  import os
  import sys
  import traceback
  import types

  import brutal
  from . import digest
  from .panic import panic, panic_unless
  
  def export(obj, name=None):
    name = name or obj.__name__
    globals()[name] = obj
    brutal.__dict__[name] = obj
    return obj
  
  BaseException = builtins.BaseException
  dict = builtins.dict
  Exception = builtins.Exception
  getattr = builtins.getattr
  isinstance = builtins.isinstance
  iter = builtins.iter
  KeyError = builtins.KeyError
  KeyboardInterrupt = builtins.KeyboardInterrupt
  len = builtins.len
  list = builtins.list
  object = builtins.object
  set = builtins.set
  super = builtins.super
  
  deque = collections.deque
  
  types_GeneratorType = types.GeneratorType

  the_empty_dict = {}

  @export
  @digest.by_name
  class Job(object):
    def __init__(me):
      me._cancelled = False
    
    def launch(me): pass
    def join(me, cancelled): pass
    def cancel(me, launched): pass

  jobs_launched = deque()
  jobs_waiting = deque()
  
  concurrency_limit = os.environ.get("BRUTAL_THREADS","")
  try: concurrency_limit = int(concurrency_limit)
  except: concurrency_limit = 0
  
  if concurrency_limit <= 0:
    import multiprocessing
    concurrency_limit = multiprocessing.cpu_count()
    if 4 <= concurrency_limit:
      concurrency_limit = int(.75*concurrency_limit)

  export(concurrency_limit, 'concurrency_limit')

  @export
  @digest.by_name
  def submit_job(job):
    ans = Promise()
    if len(jobs_launched) <= concurrency_limit:
      try:
        job.launch()
      except Exception as e:
        ans.satisfy(Failure(e))
      else:
        jobs_launched.append((job, ans))
    else:
      jobs_waiting.append((job, ans))

    return ans

  shutdown_cbs = []

  @export
  def at_shutdown(cb):
    shutdown_cbs.append(cb)
    return cb

  @export
  def shutdown():
    any_busy = True
    while any_busy:
      any_busy = False
      for got in [cb() for cb in shutdown_cbs]:
        busy = wait(futurize(got)).value()
        any_busy = any_busy or busy
    del shutdown_cbs[:]
  
  import atexit
  atexit.register(shutdown)

  @at_shutdown
  def mass_cancellation():
    #print("coflow.mass_cancellation")
    
    for job,ans in jobs_launched:
      if not job._cancelled:
        job._cancelled = True
        job.cancel(True)

    for job,ans in jobs_waiting:
      job._cancelled = True
      job.cancel(False)

    jobs_launched.extend(jobs_waiting)
    jobs_waiting.clear()
    return not not jobs_launched
  
  actives = deque()
  actives_append = actives.append
  actives_popleft = actives.popleft
  actives_extend = actives.extend
  
  progressing = [0]

  top = [{}, set()] # [shadows, effects]
  globals()['top'] = top
  
  def progress():
    if progressing[0] or not actives:
      return
    progressing[0] += 1
    old_shadows, old_effects = top
    
    try:
      while actives:
        top[:] = (None, None)
        actives_popleft()._fire()
    finally:
      top[:] = (old_shadows, old_effects)
      progressing[0] -= 1

  def wait(until):
    while True:
      progressing[0] += 1
      old_shadows, old_effects = top
      
      try:
        while actives:
          top[:] = (None, None)
          actives_popleft()._fire()
      except Exception as e:
        raise PanicError("Unacceptable error in future engine.") from e
      finally:
        top[:] = (old_shadows, old_effects)
        progressing[0] -= 1
      
      if until._done:
        return until._result
      
      if jobs_launched:
        old_shadows, old_effects = top
        top[:] = ({}, None)
        
        try:
          sats = []
          job, ans = jobs_launched.popleft()
          try:
            got = job.join(job._cancelled)
          except Exception as e:
            sats.append((ans, Failure(e)))
          else:
            sats.append((ans, Result(got)))
          
          while jobs_waiting:
            job_ans = jobs_waiting.popleft()
            job, ans = job_ans
            try:
              job.launch()
            except Exception as e:
              sats.append((ans, Failure(e)))
            else:
              jobs_launched.append(job_ans)
              break
          
          for fu,res in sats:
            fu.satisfy(res)
        
        finally:
          top[:] = (old_shadows, old_effects)
      else:
        panic_unless(not jobs_waiting)
        
        if type(until) is Coroutine:
          raise PanicError("wait() on unsatisfiable future: " + str(until._gen.gi_code))
        else:
          raise PanicError("wait() on unsatisfiable future: " + str(until))

  def enter_done(fu, fu_shadows, result):
    result = result._result
    result_shadows = result._shadows
    for sh in result_shadows:
      fu_shadows[sh] = (1, result_shadows[sh], fu_shadows[sh]) if sh in fu_shadows else result_shadows[sh]
    
    del fu._dep
    del fu._effects
    if not isinstance(fu, (Promise, CaptureEffects)):
      del fu._fire

    fu._result = result._shadows_changed(fu_shadows)
    fu._shadows = fu_shadows
    fu._done = True
    
    actives_extend(fu._sucs)
    progress()
  
  def fresh(fu):
    progress()
    return fu

  def merge_shadows(dst_sh, dst_e, src_sh):
    for sh in src_sh:
      a = dst_sh[sh] if sh in dst_sh else None
      b = src_sh[sh]
      if a is not b:
        dst_sh[sh] = b = b if not a else (1, b, a)
        for e in dst_e:
          if sh is e.shadow:
            e(b)
  
  def spread_effects(effs, fu):
    #print('spread','-'*40)
    while True:
      done = fu._done

      if not done:
        fu_effs = fu._effects
        if isinstance(fu, CaptureEffects) and not fu._keep_shadows:
          #print('  cap block',list(type(sh).__name__ for sh in fu._captures))
          effs = set(e for e in effs if e not in fu_effs and e.shadow not in fu._captures)
        else:
          effs = effs - fu_effs if fu_effs else effs
        fu_effs |= effs
        if not effs: return
        #if isinstance(fu, Coroutine) and (fu._gen.__name__ =='discover_srcs' or 'effect inserter on TraceShadow' in list(e.__name__ for e in effs)):
        #  print('  coro',list(e.__name__ for e in effs),'to',fu._gen.gi_code)
        
      shadows = fu._shadows
      for e in effs:
        e_shadow = e.shadow
        e(shadows[e_shadow] if e_shadow in shadows else None)

      if done: return
      fu = fu._dep
      if fu is None: return
  
  @export
  @digest.by_name
  class Shadow(object):
    def key_of(me, rec):
      return rec
    
    def emit(me, x):
      top_shadows, top_effects = top
      top_shadows[me] = t = (0, x, top_shadows[me] if me in top_shadows else None)
      for e in top_effects:
        if e.shadow is me:
          e(t)
    
    def of(me, fu):
      panic_unless(fu._done)
      flat = []
      effect(flat.append)(fu._shadows.get(me))
      return flat
    
    def stripped_from(me, fu):
      fu = fu._result
      fush = fu._shadows
      if not fush:
        return (), fu
      fush = dict(fush)
      flat = []
      effect(flat.append)(fush.pop(me, None))
      return flat, fu._shadows_changed(fush or the_empty_dict)

  @export
  @digest.by_name
  def effect(sh, fn):
    seen_trees = {}
    seen_keys = {}
    sh_key_of = sh.key_of
    def consume(t):
      s = [1, t]
      iters = 0
      while s:
        iters += 1
        panic_unless(iters < 100000)
        op, t = s[-2:]
        del s[-2:]
        if op:
          if t:
            tid = id(t)
            if tid not in seen_trees:
              seen_trees[tid] = t
              tag, kid, subtree = t
              if tag:
                s += (1, kid, 1, subtree)
              else:
                s += (0, kid, 1, subtree)
        else:
          #print('leaf',t)
          key = sh_key_of(t)
          if key not in seen_keys:
            seen_keys[key] = 0
            fn(t)
    consume.shadow = sh
    return consume
        
  @export
  @digest.by_name
  class Future(object):
    """
    The base class for all future-like types. Futures represent an
    eventually available collection of values with a structure
    matching the arguments needed for calling general python
    functions: a positional argument list and a keywords argument list.
    The common case of a future representing a single value will have a
    positional list of length one containing that value, and no keywords
    arguments. Futures may also represent a single thrown exception
    value.
    """
    __slots__ = ('_done','_result','_shadows')
    
    def __rshift__(arg, lam):
      """
      Schedule `lam` to execute with the values provided by this future
      as its arguments. The return of `lam` will wrapped in a future
      if it isn't already a future. The return of this function will be
      a proxy for that future eventually produced by `lam`. If this future
      represnet an exceptional return, then `lam` will not be called and
      the resulting future of this operation will also have the same
      exceptional value.
      """
      return fresh(Mbind(arg, lam, wrapped=False))
    
    def success(me):
      """
      Determine if this future resulted in a successful return value.
      This future must be in its ready state.
      """
      return isinstance(me._result, Result)
    
    def result(me):
      """
      Retrieve the final future instance represented by this future.
      The final future will be an instance of either the Result or
      Failure future subclasses. This future must be in its ready state.
      """
      return me._result
    
    def value(me):
      """
      Return first positional value of this future. If this future has
      no positional values, `None` will be returned. This future must be
      ready
      """
      return me._result.value()
    
    def values(me):
      """
      Return the tuple of positional values. This future must be ready.
      """
      return me._result.values()
    
    def kws(me):
      """
      Return the dictionary of keywords values. This future must be ready.
      """
      return me._result.kws()
    
    def explode(me):
      """
      If this future represents an exceptional value, raises that
      exception. Otherwise no-op. This future must be ready.
      """
      return me._result.explode()
    
    def __getitem__(me, i):
      """
      If `i` is a string then returns that keyword value of the future.
      If `i` is an integer then returns that positional value of the future.
      This future must be ready.
      """
      return me._result[i]
    
    def wait(me):
      """
      Cause this thread to make progress until this future is ready.
      Returns `value()` of this future.
      """
      return wait(me).value()
    
    def wait_futurized(me):
      """
      Cause this thread to make progress until this future is ready.
      Returns `result()` of this future.
      """
      return wait(me)
  
  @export
  @digest.by_name
  class Result(Future):
    """
    Future subclass representing a final-state positional value list
    and a keywords value dictionary.
    """
    __slots__ = Future.__slots__ + ('_value','_val_seq','_val_kws')
    
    def __init__(me, *values_seq, **values_kws):
      """
      Construct this future to have the same positional and keywords
      values that this function was called with.
      """
      me._done = True
      me._result = me
      me._shadows = dict(top[0]) or the_empty_dict
      me._val_seq = values_seq
      me._val_kws = values_kws

      me._value = (me if values_kws else
                   None if not values_seq else
                   values_seq[0] if 1 == len(values_seq) else
                   me)
    
    def value(me):
      merge_shadows(top[0], (), me._shadows)
      return me._value
    
    def values(me):
      merge_shadows(top[0], (), me._shadows)
      return me._val_seq

    def kws(me):
      merge_shadows(top[0], (), me._shadows)
      return me._val_kws
    
    def __getitem__(me, i):
      merge_shadows(top[0], (), me._shadows)
      if isinstance(i, str):
        return me._val_kws[i]
      else:
        return me._val_seq[i]
    
    def __getstate__(me):
      return (me._val_seq, me._val_kws)

    def __setstate__(me, s):
      me.__init__(*s[0], **s[1])

    def _shadows_changed(me, shadows):
      ans = Result.__new__(Result)
      ans._done = True
      ans._result = ans
      ans._shadows = shadows
      ans._val_seq = me._val_seq
      ans._val_kws = me._val_kws
      ans._value = me._value
      return ans

  @digest.destructurer(Result)
  def destructurer(x, buf, work):
    buf += b'fures'
    work += (x._val_seq, x._val_kws)
  
  @export
  @digest.by_name
  class Failure(Future):
    """
    Future subclass representing a final state exceptional value raised
    from a given traceback.
    """
    __slots__ = Future.__slots__ + ('exception','traceback')
    
    def __init__(me, exception, traceback=None):
      """
      Construct this future to represent the given exception value and
      traceback. If `traceback` is absent or None, then it will be
      initialized to the current traceback in `sys.exc_info()`.
      """
      me._done = True
      me._result = me
      me._shadows = dict(top[0]) or the_empty_dict
      me.exception = exception
      me.traceback = traceback if traceback is not None else sys.exc_info()[2]
    
    def explode(me):
      merge_shadows(top[0], (), me._shadows)
      raise me.exception
    
    value = explode
    values = explode
    kws = explode
    
    def __getitem__(me, i):
      me.explode()
    
    def __getstate__(me):
      return me.exception
    
    def __setstate__(me, s):
      me.__init__(exception=s, traceback=None)

    def _shadows_changed(me, shadows):
      ans = Failure.__new__(Failure)
      ans._done = True
      ans._result = ans
      ans._shadows = shadows
      ans.exception = me.exception
      ans.traceback = me.traceback
      return ans

  @digest.destructurer(Failure)
  def destructurer(x, buf, work):
    buf += b'fufail'
    work += (x.exception,)
  
  @export
  @digest.indigestible_type
  class Promise(Future):
    """
    A promise is a user-modifiable single-assignment future which is
    explicitly satisifed to represent another future or given value.
    """
    __slots__ = Future.__slots__ + ('_sucs','_dep','_effects')
    
    def __init__(me):
      me._done = False
      me._sucs = []
      me._dep = None
      me._shadows = dict(top[0])
      me._effects = set()
    
    def _fire(me):
      arg = me._dep._result
      enter_done(me, me._shadows, arg)
    
    def satisfy(me, *args, **kwargs):
      """
      Set this promise to represent the future obtained from 
      `futurize(*args, **kwargs)`.
      """
      merge_shadows(me._shadows, me._effects, top[0])
      me._dep = dep = futurize(*args, **kwargs)
      if dep._done:
        actives_append(me)
        progress()
      else:
        dep._sucs.append(me)

  @export
  @digest.indigestible_type
  class Mbind(Future):
    __slots__ = Future.__slots__ + ('_fire','_sucs','_dep','_effects')
    
    def __init__(me, arg, lam, wrapped):
      me._done = False
      me._sucs = []
      me._shadows = me_shadows = dict(top[0])
      me._effects = me_effects = set()
      me._dep = arg
      if arg._done:
        actives_append(me)
      else:
        arg._sucs.append(me)
      
      def fire1():
        arg_result = arg._result
        me._dep = None

        # begin_scope
        top[:] = (me_shadows, me_effects)
        merge_shadows(me_shadows, (), arg_result._shadows)
        
        try:
          if wrapped:
            proxied = lam(arg_result)
          else:
            if isinstance(arg_result, Result):
              proxied = lam(*arg_result._val_seq, **arg_result._val_kws)
            elif isinstance(arg_result, Failure):
              proxied = arg_result
            else:
              panic()
          proxied = futurize(proxied)
        except Exception as e:
          proxied = Failure(e)

        spread_effects(me_effects, proxied)
        
        # enter proxying state
        if proxied._done:
          enter_done(me, me_shadows, proxied)
        else:
          me._fire = make_fire2(proxied)
          me._dep = proxied
          proxied._sucs.append(me)
      
      def make_fire2(proxied):
        def fire():
          enter_done(me, me_shadows, proxied)
        return fire
      
      # register method
      me._fire = fire1

  @export
  @digest.by_name
  def mbind(*args, **kws):
    """
    Futurize the given arguments, when they are ready apply the decorated
    function to them. The result of that function (lifted to a future
    if not already) will be assigned over the definition of the funciton.
    ```
    @mbind(1, 2)
    def foo(one, two):
      return ...
    # foo is now a future representing the eventual return of the
    # function that was assigned to `foo` at decoration time.
    ```
    """
    def proxy(fn):
      ans = Mbind(futurize(*args, **kws), fn, wrapped=False)
      progress()
      return ans
    return proxy
  
  @export
  @digest.by_name
  def mbind_wrapped(*args, **kws):
    """
    Futurize the given arguments, when that future is ready apply the
    decorated function directly to it as a future (not unwrapping its
    values as arguments). The result of that function (lifted to a future
    if not already) will be assigned over the definition of the funciton.
    ```
    @mbind(1, 2)
    def result(one_and_two):
      return ...
    # result is now a future representing the eventual return of the
    # function that was assigned to `result` at decoration time.
    ```
    """
    def proxy(fn):
      ans = Mbind(futurize(*args, **kws), fn, wrapped=True)
      progress()
      return ans
    return proxy
  
  none_result = Result(None)
  
  @export
  @digest.indigestible_type
  class Coroutine(Future):
    __slots__ = Future.__slots__ + ('_fire','_sucs','_dep','_effects','__name__','_gen')
    
    def __init__(me, gen):
      me._done = False
      me._sucs = []
      me._shadows = me_shadows = dict(top[0])
      me._effects = me_effects = set()
      me.__name__ = gen.__name__
      me._gen = gen
      
      me_shadows_update = me_shadows.update
      gen_send = gen.send
      gen_throw = gen.throw

      me._dep = none_result
      
      def fire():
        arg = me._dep._result
        me._dep = None

        # begin_scope
        top[:] = (me_shadows, me_effects)
        
        while True:
          merge_shadows(me_shadows, (), arg._shadows)
          
          try:
            if isinstance(arg, Result):
              arg = gen_send(arg._value)
            elif isinstance(arg, Failure):
              arg = gen_throw(arg.exception, None, arg.traceback)
            else:
              panic()
          except StopIteration as e:
            panic_unless(e.value is None)
            stopped = True
          except Exception as e:
            arg = Failure(e)
            stopped = True
          else:
            if isinstance(arg, Future):
              spread_effects(me_effects, arg)
            else:
              arg = Result(arg)
            stopped = False
          
          if stopped:
            enter_done(me, me_shadows, arg)
            break
          elif arg._done:
            arg = arg._result
          else:
            me._dep = arg
            arg._sucs.append(me)
            break
      
      # register method
      me._fire = fire

  @export
  @digest.by_name
  def coroutine(fn):
    """
    Convert the given generator function `fn` into a coroutine. The
    returned value is a future representing the last yielded value from
    `fn` or its raised exception. Intermediate yields from `fn` will be
    interpreted as futures (via `futurize`) and waited for non-blockingly.
    When an intermediate future is ready, the generator will be resumed
    with the single positional value of that future returned from the yield.
    """
    @digest.by_other(fn)
    def fn1(*a,**kw):
      ans = Coroutine(fn(*a,**kw))
      actives_append(ans)
      progress()
      return ans
    fn1.__name__ = fn.__name__
    fn1.__module__ = fn.__module__
    fn1.__doc__ = fn.__doc__
    fn1.__wrapped__ = fn
    return fn1

  @digest.by_name
  @coroutine
  def when_all(*args, **kws):
    args1 = []
    for a in args:
      a = yield a
      args1.append(a)
    kws1 = {}
    for k in kws:
      val = yield kws[k]
      kws1[k] = val
    yield Result(*args1, **kws1)

  @export
  @digest.by_name
  def futurize(*args, **kws):
    """
    Wrap and collect the given function arguments into a single returned
    future. If all of `args` and `kws` are non-futures then a future
    representing those positional and keywords values will be returned.
    Otherwise, the future returned will have its positional values
    represent the concatenated positional values from the
    `map(futurize, args)` list of futures, and the returned future's 
    keywords arugments will be a best-effort attempt to merge the keywords
    values from each future in `args` along with each of the non-futures
    or singly-valued futures in `kws`.
    """
    if not kws:
      if not args:
        return Result()
      elif len(args) == 1:
        x = args[0]
        if isinstance(x, Future):
          return x
        elif isinstance(x, types_GeneratorType):
          return fresh(Coroutine(x))
        else:
          return Result(x)

    return when_all(*args, **kws)
  
  @export
  @digest.by_name
  def wrapped(fn, *args, **kws):
    """
    Immediately evaluate `fn(*args,**kws)`. If it returns a future
    then that is the return value of `wrapped`. Otherwise, if `fn`
    returns a value it is returned as a singly-valued (Result) future.
    If `fn` raises, it is returned in an exceptional (Failure) future.
    """
    try:
      ans = fn(*args,**kws)
      if not isinstance(ans, Future):
        ans = Result(ans)
    except Exception as e:
      ans = Failure(e)
    return ans

  @export
  @digest.by_name
  def after(before, *args, **kws):
    def proxy(aft):
      try:
        ans = before(*args, **kws)
        if isinstance(ans, Future):
          ans = Mbind(ans, aft, wrapped=True)
          progress()
          return ans
        else:
          return aft(Result(ans))
      except Exception as e:
        return aft(Failure(e))
    return proxy

  @export
  @digest.indigestible_type
  class CaptureEffects(Future):
    __slots__ = Future.__slots__ + ('_sucs','_dep','_effects','_captures','_keep_shadows')
    
    def __init__(me, shadows, effects, captures, keep_shadows, dep):
      me._done = False
      me._sucs = []
      me._shadows = shadows
      me._effects = effects
      me._captures = captures
      me._keep_shadows = keep_shadows
      me._dep = dep

      if dep._done:
        actives_append(me)
      else:
        dep._sucs.append(me)

      spread_effects(effects, dep)
    
    def _fire(me):
      shs = me._shadows
      dep = me._dep._result
      
      if not me._keep_shadows:
        dep_shs = dict(dep._shadows)
        for sh in me._captures:
          shs.pop(sh, None)
          dep_shs.pop(sh, None)
        dep = dep._shadows_changed(dep_shs)
      
      enter_done(me, shs, dep)
  
  @export
  @digest.by_name
  def capture_effects(effects, keep_shadows, fn, *args, **kws):
    captures = set(e.shadow for e in effects)
    def proxy(aft):
      old_sh, old_effs = top
      top[0] = new_sh = {}
      top[1] = new_effs = set(effects)
      new_effs.update(e for e in old_effs if e.shadow not in captures)
      try:
        ans = fn(*args, **kws)
      except Exception as e:
        if not keep_shadows:
          for sh in captures:
            new_sh.pop(sh, None)
        ans = Failure(e) # captures new_sh as shadows from top
        top[:] = (old_sh, old_effs)
        return aft(ans)
      else:
        if not isinstance(ans, Future):
          if not keep_shadows:
            for sh in captures:
              new_sh.pop(sh, None)
          ans = Result(ans) # captures new_sh as shadows from top
          top[:] = (old_sh, old_effs)
          return aft(ans)
        else:
          ans = Mbind(CaptureEffects(new_sh, set(effects), captures, keep_shadows, ans), aft, 1) # wrapped=1
          top[:] = (old_sh, old_effs)
          progress()
          return ans
    return proxy
  
  @export
  @digest.indigestible_type
  class CriticalSection(object):
    """
    A future-based lock for coroutines.
    """
    
    def __init__(me):
      class Condition(object):
        def __init__(me, test):
          me._test = test
          me._pros = deque()
      
      me.make_condition = Condition
      me.trivial_condition = Condition(lambda:True)
      me._conds = set()
      me._held = False

      def releaser():
        panic_unless(me._held)
        
        yep = False
        for cond in me._conds:
          if cond._test():
            yep = True
            break

        if yep:
          pros = cond._pros
          pro = pros.popleft()
          if not pros:
            me._conds.discard(cond)
          pro.satisfy(releaser)
        else:
          me._held = False
      
      me._releaser = releaser
      
    def acquire(me, cond=None):
      """
      Request the eventual acquisition of the lock. The return value is
      a singly-valued future containing the `releaser` callback. When
      the future is ready, the caller has the lock. To release the lock,
      the caller must invoke the no-argument releaser callback delivered
      in the future.
      ```
      lock = CriticalSection()
      @coroutine
      def foo():
        # Before critical section
        # ...
        release = yield lock.acquire()
        # In critical section
        # ...
        release()
        # After critical section
        # ...
      """
      cond = cond or me.trivial_condition
      panic_unless(type(cond) is me.make_condition)
      
      if me._held or not cond._test():
        pro = Promise()
        cond._pros.append(pro)
        if cond not in me._conds:
          me._conds.add(cond)
      else:
        pro = Result(me._releaser)
        me._held = True

      return pro

_everything()
del _everything

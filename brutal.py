assert easy_cxx_is_root

@brutal.rule(caching='memory', traced=1)
def c_compiler():
  cc = brutal.env('CC', [])
  if cc: return cc
  
  NERSC_HOST = brutal.env('NERSC_HOST', None)
  if NERSC_HOST: return ['cc']
  
  return ['gcc']

@brutal.rule(caching='memory', traced=1)
def cxx_compiler():
  cxx = brutal.env('CXX', [])
  if cxx: return cxx

  NERSC_HOST = brutal.env('NERSC_HOST', None)
  if NERSC_HOST: return ['CC']

  return ['g++']

@brutal.rule(caching='memory')
def cxx14_flags():
  cxx = cxx_compiler()
  _, pp_defs = compiler_version_and_pp_defines(cxx, 'c++').wait()
  
  std_ver = int(pp_defs['__cplusplus'].rstrip('L'))
  if std_ver < 201400:
    return ['-std=c++14']
  else:
    return []

@brutal.rule
def sources_from_includes_enabled(PATH):
  return '.brutal/' not in PATH and brutal.os.path_within_any(PATH,
    '.',
    brutal.here('bench'),
    brutal.here('src'),
    brutal.here('test'),
  )

def base_env():
  debug = brutal.env('debug', 0)
  optlev = brutal.env('optlev', 0 if debug else 3)
  syms = brutal.env('syms', 1 if debug else 0)
  opnew = brutal.env('opnew', 'libc' if debug else 'deva', universe=['libc','deva','jemalloc'])
  asan = brutal.env('asan', 1 if debug else 0)
  dummy = brutal.env('dummy', 0)
  drain_timer = brutal.env('drain_timer', 0)
  timeline = brutal.env('timeline', 0)
  return debug, optlev, syms, opnew, asan, dummy, drain_timer, timeline

@brutal.rule
def base_cg_flags():
  debug, optlev, syms, opnew, asan, dummy, drain_timer, timeline = base_env()

  asan_flags = ['-fsanitize=address'] if asan else []
  cg_misc = brutal.env('CXX_CGFLAGS', [])

  return (
      (['-flto'] if optlev == 3 else []) +
      (['-g'] if syms else []) +
      asan_flags
    ) + cg_misc

def code_context_base():
  debug, optlev, syms, opnew, asan, dummy, drain_timer, timeline = base_env()

  pp_misc = brutal.env('CXX_PPFLAGS', [])
  asan_flags = ['-fsanitize=address'] if asan else []
  pp_angle_dirs = brutal.env('PP_DIRS', [])
  lib_dirs = brutal.env('LIB_DIRS', [])
  lib_names = brutal.env('LIB_NAMES', [])
  
  return CodeContext(
    compiler = cxx_compiler(),
    pp_angle_dirs = [brutal.here('src')] + pp_angle_dirs,
    pp_misc = cxx14_flags() + pp_misc,
    cg_optlev = optlev,
    cg_misc = base_cg_flags() + ['-Wno-unknown-warning-option','-Wno-aligned-new'],
    ld_misc = (['-flto'] if optlev == 3 else []) + asan_flags,
    lib_dirs = lib_dirs,
    lib_names = lib_names,
    pp_defines = {
      'DEBUG': 1 if debug else 0,
      'NDEBUG': None if debug else 1,
      'DEVA_OPNEW_'+opnew.upper(): 1,
      'DEVA_DUMMY_EXEC': 1 if dummy else 0,
      'DRAIN_TIMER': 1 if drain_timer else 0,
      'TIMELINE': 1 if timeline else 0
    }
  )

@brutal.rule
def code_context(PATH):
  cxt = code_context_base()

  def get_world():
    return brutal.env('world', universe=('threads','gasnet'))
  
  def get_thread_n():
    world = get_world()
    if world == 'threads':
      return brutal.env('ranks',2)
    elif world == 'gasnet':
      return brutal.env('workers',2)+1
  
  if PATH == brutal.here('src/devastator/threads.hxx'):
    impl = brutal.env('tmsg', universe=('spsc','mpsc'))
    talloc = brutal.env('talloc', universe=('opnew-asym','opnew-sym','epoch'))
    cxt |= CodeContext(pp_defines={
      'DEVA_THREADS_'+impl.upper(): 1,
      'DEVA_THREADS_ALLOC_' + talloc.replace('-','_').upper(): 1
    })
  
  elif PATH == brutal.here('src/devastator/threads/message_spsc.hxx'):
    tsigbits = brutal.env('tsigbits', universe=(0,8,16,32))
    if tsigbits == 0:
      tsigbits = 64*8/get_thread_n()
      tsigbits = (8,16,32)[sum([x < tsigbits for x in (8,16)])]

    tprefetch = brutal.env('tprefetch', universe=(0,1,2))
    torder = brutal.env('torder', universe=('dfs','bfs'))
    
    cxt |= CodeContext(pp_defines={
      'DEVA_THREADS_SPSC_BITS': tsigbits,
      'DEVA_THREADS_SPSC_PREFETCH': tprefetch,
      'DEVA_THREADS_SPSC_ORDER_'+torder.upper(): 1
    })

  elif PATH == brutal.here('src/devastator/threads/signal_slots.hxx'):
    tsigreap = brutal.env('tsigreap', universe=('memcpy','simd','atom'))
    
    cxt |= CodeContext(pp_defines={
      'DEVA_THREADS_SIGNAL_REAP_'+tsigreap.upper(): 1
    })
  
  elif PATH == brutal.here('src/devastator/threads/message_mpsc.hxx'):
    cxt |= CodeContext(pp_defines={
      'DEVA_THREADS_MPSC_RAIL_N': brutal.env('trails', 1)
    })
  
  elif PATH == brutal.here('src/devastator/world.hxx'):
    world = get_world()
    cxt |= CodeContext(pp_defines={'DEVA_WORLD':1})
    
    if world == 'threads':
      cxt |= CodeContext(pp_defines={
        'DEVA_WORLD_THREADS': 1,
        'DEVA_THREAD_N': get_thread_n()
      })
    
    elif world == 'gasnet':
      cxt |= CodeContext(pp_defines={
        'DEVA_WORLD_GASNET': 1,
        'DEVA_PROCESS_N': brutal.env('procs',2),
        'DEVA_WORKER_N': brutal.env('workers',2),
        'DEVA_THREAD_N': get_thread_n()
      })
    
  elif PATH == brutal.here('src/devastator/diagnostic.cxx'):
    version = brutal.git_describe(brutal.here())
    cxt |= CodeContext(pp_defines={'DEVA_GIT_VERSION': '"'+version+'"'})
  
  return cxt

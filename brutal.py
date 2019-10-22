assert easy_cxx_is_root

@brutal.rule(caching='memory', traced=1)
def c_compiler():
  while 1:
    cc = brutal.env('CC', [])
    if cc: break

    NERSC_HOST = brutal.env('NERSC_HOST', None)
    if NERSC_HOST:
      cc = ['cc']
      break

    cc = ['gcc']
    break

  ver_text = brutal.process(cc + ['--version'], show=0, capture_stdout=1).wait()
  brutal.depend_fact('CC --version', ver_text)
  return cc

@brutal.rule(caching='memory', traced=1)
def cxx_compiler():
  while 1:
    cxx = brutal.env('CXX', [])
    if cxx: break

    NERSC_HOST = brutal.env('NERSC_HOST', None)
    if NERSC_HOST:
      cxx = ['CC']
      break

    cxx = ['g++']
    break

  text = brutal.process(cxx + ['--version'], show=0, capture_stdout=1)
  text = text.wait()
  brutal.depend_fact('CXX --version', text)
  
  text = brutal.process(
    cxx + ['-x','c++','-E','-'],
    stdin='__cplusplus', capture_stdout=1, show=0
  )
  text = text.wait()
  
  for line in text.split('\n'):
    if line and not line.startswith('#'):
      std_ver = int(line.rstrip('L'))
      if std_ver < 201400:
        return cxx + ['-std=c++14']
      else:
        return cxx

  brutal.error('Invalid preprocessor output:', text)

@brutal.rule
def sources_from_includes_enabled(PATH):
  return brutal.os.path_within_any(PATH, brutal.here('src'), brutal.here('test'))

def code_context_base():
  debug = brutal.env('debug', 0)
  optlev = brutal.env('optlev', 0 if debug else 3)
  syms = brutal.env('syms', 1 if debug else 0)
  opnew = brutal.env('opnew', 0 if debug else 1)
  
  return CodeContext(
    compiler = cxx_compiler(),
    pp_angle_dirs = [brutal.here('src')],
    cg_optlev = optlev,
    cg_misc = (
      (['-flto'] if optlev == 3 else []) +
      (['-g'] if syms else []) +
      ['-Wno-unknown-warning-option','-Wno-aligned-new','-march=native']
    ),
    ld_misc = ['-flto'] if optlev == 3 else [],
    pp_defines = {
      'DEBUG': 1 if debug else 0,
      'NDEBUG': None if debug else 1,
      'DEVA_OPNEW': opnew
    }
  )

@brutal.rule
def code_context(PATH):
  cxt = code_context_base()

  def get_world():
    world = brutal.env('world', default='threads')
    brutal.error_unless(world in ('threads','gasnet'), '"world" must be one of ["threads","gasnet"], not "{0}".', world)
    return world
  
  def get_thread_n():
    world = get_world()
    if world == 'threads':
      return brutal.env('ranks',2)
    elif world == 'gasnet':
      return brutal.env('workers',2)+1
  
  if PATH == brutal.here('src/devastator/threads.hxx'):
    impl = brutal.env('tmsg', 'spsc')
    brutal.error_unless(impl in ('spsc','mpsc'), '"tmsg" must be one of ["spsc","mpsc"], not "{0}".', impl)
    
    cxt |= CodeContext(pp_defines={
      'DEVA_THREADS_MESSAGE_'+impl.upper(): 1
    })
  
  if PATH == brutal.here('src/devastator/threads/message_spsc.hxx'):
    tsigbits = brutal.env('tsigbits', 0)
    brutal.error_unless(tsigbits in (0,8,16,32), '"tsigbits" must be one of [0,8,16,32], not "{0}".', tsigbits)

    if tsigbits == 0:
      tsigbits = 64*8/get_thread_n()
      tsigbits = (8,16,32)[sum([x < tsigbits for x in (8,16)])]
    
    cxt |= CodeContext(pp_defines={
      'DEVA_THREADS_MESSAGE_SIGNAL_BITS': tsigbits
    })

  if PATH == brutal.here('src/devastator/threads/message_mpsc.hxx'):
    cxt |= CodeContext(pp_defines={
      'DEVA_THREADS_MESSAGE_RAIL_N': brutal.env('trails', 1)
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

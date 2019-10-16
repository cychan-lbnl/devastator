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
      'NDEBUG': None if debug else 1
    }
  )

@brutal.rule
def code_context(PATH):
  cxt = code_context_base()

  if PATH == brutal.here('src/devastator/threads.hxx'):
    cxt = cxt.with_pp_defines(DEVA_THREADS_SPSC=1)
  
  if PATH == brutal.here('src/devastator/world.hxx'):
    world = brutal.env('world', default='threads')
    cxt |= CodeContext(pp_defines={'DEVA_WORLD':1})
    
    if world == 'threads':
      cxt |= CodeContext(pp_defines={
        'DEVA_WORLD_THREADS': 1,
        'DEVA_THREAD_N': brutal.env('ranks',2)
      })
    
    elif world == 'gasnet':
      cxt |= CodeContext(pp_defines={
        'DEVA_WORLD_GASNET': 1,
        'DEVA_PROCESS_N': brutal.env('procs',2),
        'DEVA_WORKER_N': brutal.env('workers',2),
        'DEVA_THREAD_N': '((DEVA_WORKER_N)+1)'
      })
    
  elif PATH == brutal.here('src/devastator/diagnostic.cxx'):
    version = brutal.git_describe(brutal.here())
    cxt |= CodeContext(pp_defines={'DEVA_GIT_VERSION': '"'+version+'"'})
  
  return cxt

assert easy_cxx_is_root

@brutal.rule(caching='process', traced=1)
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

@brutal.rule(caching='process', traced=1)
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

  ver_text = brutal.process(cxx + ['--version'], show=0, capture_stdout=1).wait()
  brutal.depend_fact('CXX --version', ver_text)
  return cxx

@brutal.rule
def sources_from_includes_enabled(PATH):
  return brutal.os.path_within_any(PATH, brutal.here('src'), brutal.here('test'))

def code_context_base():
  kws = {}
  kws['compiler'] = cxx_compiler()
  kws['pp_angle_dirs'] = [brutal.here('src')]

  debug = brutal.env('debug', 0)
  optlev = brutal.env('optlev', 0 if debug else 3)
  syms = brutal.env('syms', 1 if debug else 0)
  
  kws['cg_optlev'] = optlev

  kws['cg_misc'] = ['-g'] if syms else []
  kws['cg_misc'] += ['-Wno-aligned-new']
  kws['cg_misc'] += ['-march=native']
  
  kws['pp_defines'] = {
    'DEBUG': 1 if debug else 0,
    'NDEBUG': None if debug else 1
  }

  return CodeContext(**kws)

@brutal.rule
def code_context(PATH):
  kws = {}
  kws['pp_defines'] = {}
  
  if PATH == brutal.here('src/devastator/world.hxx'):
    world = brutal.env('world', default='threads')

    if world == 'threads':
      kws['pp_defines'].update({
        'WORLD_THREADS': 1,
        'RANK_N': brutal.env('ranks',2)
      })
    elif world == 'gasnet':
      kws['pp_defines'].update({
        'WORLD_GASNET': 1,
        'PROCESS_N': brutal.env('procs',2),
        'WORKER_N': brutal.env('workers',2)
      })

  elif PATH == brutal.here('src/devastator/diagnostic.cxx'):
    version = brutal.process(
        ['git','describe','--dirty','--always','--tags'],
        capture_stdout=1, show=0, cwd=brutal.here()
      ).wait().strip()
    kws['pp_defines'] = {'DEVA_GIT_VERSION': '"'+version+'"'}
  
  return code_context_base() | CodeContext(**kws)

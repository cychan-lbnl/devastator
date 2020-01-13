from .gasnet import gasnet_context
from .jemalloc import jemalloc_context

@brutal.rule
@brutal.coroutine
def code_context(PATH):
  kws = {}
  dependencies = []
  
  if PATH == brutal.here('pthread.h'):
    kws['lib_misc'] = ['-pthread']

  elif PATH == brutal.here('gasnetex.h'):
    url, cross, conduit, sync = gasnet_opts()
    gasnet = yield gasnet_context(
      url, cross, conduit, sync,
      cc = c_compiler(),
      cxx = cxx_compiler(),
      debug = brutal.env('debug', 0)
    )
    dependencies += (gasnet,)
    kws['pp_defines'] = {'DEVA_GASNET_PRESENT':1}

  elif PATH == brutal.here('jemalloc.h'):
    jemalloc = yield jemalloc_context()
    dependencies += (jemalloc,)
    kws['pp_defines'] = {'DEVA_JEMALLOC_PRESENT':1}

  yield CodeContext(**kws).with_dependencies(*dependencies)

@brutal.rule
def gasnet_opts():
  url = brutal.env('DEVA_GASNET', "http://mantis.lbl.gov/nightly/unlisted/GASNet-stable.tar.gz")
  conduit = 'smp'
  cross = None
  nersc = brutal.env('NERSC_HOST',None)
  if nersc == 'cori':
    conduit = 'aries'
    cross = 'cross-configure-cray-aries-slurm'
  conduit = brutal.env('DEVA_GASNET_CONDUIT', conduit)
  sync = 'seq'
  return (url, cross, conduit, sync)

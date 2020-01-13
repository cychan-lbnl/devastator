import shlex

@brutal.rule(caching='file')
def jemalloc_source():
  url = "https://github.com/jemalloc/jemalloc/releases/download/5.2.1/jemalloc-5.2.1.tar.bz2"
  tgz = brutal.download(url)
  return brutal.untar(tgz)

@brutal.rule(caching='file')
@brutal.coroutine
def jemalloc_context():
  src_dir = yield jemalloc_source()
  
  build_dir = brutal.mkpath('jemalloc-build')
  brutal.os.mkdir(build_dir)
  
  install_dir = brutal.mkpath('jemalloc-install')

  cc = c_compiler()
  cxx = cxx_compiler()
  cg_flags = base_cg_flags()
  
  configure = brutal.os.path.join(src_dir, 'configure')
  configure = [configure, '--prefix', install_dir, '--with-jemalloc-prefix=je_', '--enable-cxx', '--disable-fill', '--disable-shared']
  
  yield brutal.process(configure, cwd=build_dir, env_add={'CC':cc, 'CFLAGS':cg_flags, 'CXX':cxx, 'CXXFLAGS': cxx14_flags() + cg_flags})
  yield brutal.process(['make','install'], cwd=build_dir)

  jemalloc_config = brutal.os.path.join(install_dir, 'bin', 'jemalloc-config')

  pp_misc = yield brutal.process([jemalloc_config, '--cppflags'])
  pp_misc = shlex.split(pp_misc)
  
  lib_misc = yield brutal.process([jemalloc_config, '--libs'])
  lib_misc = shlex.split(lib_misc)
  
  yield CodeContext(
    compiler = cxx,
    pp_misc = pp_misc,
    #lib_misc = ['-L'+brutal.os.path.join(install_dir, 'lib'), '-ljemalloc'] + lib_misc
    lib_misc = [brutal.os.path.join(install_dir, 'lib', 'libjemalloc.a')] + lib_misc
  )

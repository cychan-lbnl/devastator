@brutal.rule(caching='file')
def gasnet_source(url):
  tgz = brutal.download(url)
  src_dir = brutal.untar(tgz)
  return src_dir

@brutal.rule(caching='file')
@brutal.coroutine
def gasnet_configured(url, cross, cc, cxx, debug):
  src_dir = yield gasnet_source(url)
  build_dir = brutal.mkpath('gasnet-build')
  brutal.os.mkdir(build_dir)

  if cross:
    script = brutal.os.path.join(src_dir, cross)
    brutal.os.symlink(brutal.os.path.join(src_dir,'other','contrib',cross), script)
  else:
    script = brutal.os.path.join(src_dir, 'configure')

  flags = ['--disable-parsync','--disable-mpi','--without-mpicc']
  if brutal.os.environ.get('NERSC_HOST') == "perlmutter":
    flags += ['--with-ofi-provider=cxi']
  flags += ['--enable-debug'] if debug else []

  yield brutal.process([script] + flags, cwd=build_dir, env_add={'CC':cc, 'CXX':cxx})
  yield build_dir

@brutal.rule(caching='file')
@brutal.coroutine
def gasnet_context(url, cross, conduit, sync, cc, cxx, debug):
  build_dir = yield gasnet_configured(url, cross, cc, cxx, debug)

  cmd = ['make','-j', brutal.concurrency_limit]
  yield brutal.process(cmd, cwd=brutal.os.path.join(build_dir, '%s-conduit'%conduit))

  makefile = brutal.os.path.join(*(
    [build_dir] + ['%s-conduit'%conduit, '%s-%s.mak'%(conduit,sync)]
  ))
  
  def extract(varname):
    import subprocess as sp
    import shlex
    p = sp.Popen(['make','--no-print-directory','-f','-','gimme'], stdin=sp.PIPE, stdout=sp.PIPE, stderr=sp.PIPE, close_fds=True)
    tmp = ('include {0}\n' + 'gimme:\n' + '\t@echo $({1})\n').format(makefile, varname)
    val, _ = p.communicate(tmp.encode())
    if p.returncode != 0:
      brutal.error('Makefile %s not found.'%makefile)
    val = val.decode().strip(' \t\n')
    return shlex.split(val)

  yield CodeContext(
    compiler = cxx,
    ld_misc = extract('GASNET_LDFLAGS'),
    pp_misc = extract('GASNET_CXXCPPFLAGS'),
    cg_misc = extract('GASNET_CXXFLAGS'),
    lib_misc = extract('GASNET_LIBS')
  )

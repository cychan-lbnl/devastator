# This is a imported by a brutal rulefile, and therefor
# gets "brutal" for free.
assert brutal

from .code_context import CodeContext

easy_cxx_is_root = True

c_exts = ('.c',)
cxx_exts = ('.cpp','.cxx','.c++','.C','.C++')
source_exts = c_exts + cxx_exts
header_exts = ('.h','.hpp','.hxx','.h++')

@brutal.rule
def uuifdef():
  import uuid
  u = uuid.uuid4().hex
  return ("#ifndef _%s\n"%u +
          "#define _%s\n"%u +
          "#endif\n")

@brutal.rule
def c_compiler():
  return None

@brutal.rule
def cxx_compiler():
  return None

@brutal.rule
def code_context(PATH):
  return CodeContext()

@brutal.rule
def sources_from_includes_enabled(PATH):
  return False

@brutal.rule
def sources_for_include(PATH):
  if not sources_from_includes_enabled(PATH):
    return set()
  
  inc_base, inc_ext = brutal.os.path.splitext(PATH)
  
  if inc_ext in header_exts:
    return set([inc_base + ext for ext in find_src_exts(inc_base)])
  else:
    return set()

@brutal.rule(caching='file')
@brutal.coroutine
def includes(PATH, compiler, pp_flags):
  brutal.depend_file(PATH)
  cmd = compiler + pp_flags + ['-MM','-MT','x',PATH]
  mk = yield brutal.process(cmd, capture_stdout=True)
  mk = mk[mk.index(":")+1:]
  import shlex
  incs = shlex.split(mk.replace("\\\n",""))[1:] # first is source file
  incs = list(map(brutal.os.path.abspath, incs))
  brutal.depend_file(*incs)
  incs = list(map(brutal.os.realpath, incs))
  yield incs

@brutal.rule(caching='file')
@brutal.coroutine
def compile(PATH, compiler, pp_flags, cg_flags):
  out = brutal.mkpath(PATH, '.o')
  brutal.depend_file(PATH)
  incs = includes(PATH, compiler, pp_flags)
  cmd = compiler + pp_flags + cg_flags + ['-c',PATH,'-o',out]
  yield brutal.process(cmd)
  incs = yield incs
  brutal.depend_file(*incs)
  yield out

def find_src_exts(base):
  ans = []
  for ext in source_exts:
    path = base + ext
    brutal.depend_file(path)
    if brutal.os.exists(path):
      ans.append(ext)
  return ans

@brutal.traced
@brutal.coroutine
def discovery(main_src):
  if sources_from_includes_enabled(main_src):
    srcs_todo = [main_src]
  else:
    srcs_todo = []
  srcs_seen = {main_src: None}
  incs_todo = []
  incs_seen = set()
  
  memo_cxt = {}
  cxt_big = CodeContext()
  pp_flags = cxt_big.pp_flags()

  splitext = brutal.os.path.splitext
  
  while srcs_todo or incs_todo:
    cxt_old = cxt_big
    cxts = [(p,code_context(p)) for p in (srcs_todo + incs_todo) if p not in memo_cxt]
    for (path,cxt) in cxts:
      cxt = yield cxt
      memo_cxt[path] = cxt
      if path in srcs_todo:
        cxt_big |= cxt.with_updates(pp_defines={})
      else:
        cxt_big |= cxt
    
    del incs_todo[:]

    if cxt_big != cxt_old:
      pp_flags = cxt_big.pp_flags()
      srcs_todo = [main_src]
      srcs_seen = {main_src: None}
      incs_seen = set()
    else:
      srcs_temp = srcs_todo
      srcs_todo = []
      while srcs_temp:
        src = srcs_temp.pop()
        incs = yield includes(src, memo_cxt[src].compiler(), pp_flags)
        srcs_seen[src] = incs
        incs = [inc for inc in incs if inc not in incs_seen]
        incs_todo += incs
        incs_seen |= set(incs)
        for inc in incs:
          for src1 in sources_for_include(inc):
            if src1 not in srcs_seen:
              srcs_todo += (src1,)
              srcs_seen[src1] = None

  for src in list(srcs_seen.keys()):
    cxt = memo_cxt[src]
    for f in sorted(srcs_seen[src]):
      cxt |= memo_cxt[f]
    srcs_seen[src] = cxt

  yield (srcs_seen, cxt_big)

@brutal.rule(caching='file', cli='exe')
@brutal.coroutine
def executable(PATH):
  srcs, cxt_big = yield discovery(PATH)
  
  objs = {}
  for src in srcs:
    cxt = srcs[src]
    big_pp_flags = (cxt_big | cxt).pp_flags()
    objs[src] = compile(src, cxt.compiler(), big_pp_flags, cxt.cg_flags())
  
  for src in objs:
    objs[src] = yield objs[src]
  objs = list(objs.values())

  ld = cxt_big.compiler()
  ld_flags = cxt_big.ld_flags()
  lib_flags = cxt_big.lib_flags()
  
  exe = brutal.mkpath(PATH, suffix='.exe')
  cmd = ld + ld_flags + ['-o',exe] + objs + lib_flags
  yield brutal.process(cmd)
  yield exe
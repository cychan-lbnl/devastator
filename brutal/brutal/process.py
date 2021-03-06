"""
brutal.process: Asynchronous execution of child processes.

Similar in function to the subprocess module. Completion of the child 
process is returned in a `brutal.coflow.Future`. Child processes are 
executed against pseudo-terminals to capture their ASCII color codes. 
Failure is reported to `brutal.noiselog`.
"""
def _everything():
  import fcntl
  import os
  import re
  import select
  import shlex
  import signal
  import struct
  import sys
  import threading
  import time

  import brutal
  from . import coflow
  from . import digest
  from . import noiselog
  from .panic import panic, panic_unless
  
  export = digest.exporter(globals())

  class Job(coflow.Job):
    def __init__(me, go):
      super(Job, me).__init__()
      me.go = go
      me.pid = None
      
    def launch(me):
      me.wait_n = 0
      me.outputs = {}
      me.io_done = threading.Lock()
      me.io_done.acquire()
      me.go(me)

    def satisfy(me):
      me.wait_n -= 1
      if me.wait_n == 0:
        me.io_done.release()
    
    def join(me, cancelled):
      if not cancelled:
        try:
          _, status = os.waitpid(me.pid, 0)
        except:
          os.kill(me.pid, signal.SIGTERM)
          raise

        me.io_done.acquire()

        return (status, me.outputs['stdout'], me.outputs['stderr'])
      else:
        raise Exception("brutal.process(): cancelled")
    
    def cancel(me, launched):
      if launched:
        try: os.kill(me.pid, signal.SIGTERM)
        except: pass
  
  io_cond = threading.Condition(threading.Lock())
  io_r = {} # {fd:([buf],outname,job)}
  io_w = {} # {fd:(rev_bufs,job)}
  io_thread_box = [None]

  force_mute = False

  @digest.by_name
  def io_thread_fn():
    try:
      io_cond.acquire()
      while 1:
        if 0 == len(io_r) + len(io_w):
          if io_thread_box[0] is None:
            break
          io_cond.wait()
          continue
        
        fds_r = list(io_r.keys())
        fds_w = list(io_w.keys())

        io_cond.release()
        fds_r, fds_w, _ = select.select(fds_r, fds_w, [])
        io_cond.acquire()
        
        for fd in fds_r:
          try:
            buf = os.read(fd, 8<<10)
          except OSError:
            buf = b''

          chks, outname, job = io_r[fd]
          
          if len(buf) == 0:
            del io_r[fd]
            os.close(fd)
            job.outputs[outname] = b''.join(chks).decode()
            job.satisfy()
          else:
            chks.append(buf)

        for fd in fds_w:
          rev_bufs, job = io_w[fd]
          if len(rev_bufs) != 0:
            os.write(fd, rev_bufs.pop())
          if len(rev_bufs) == 0:
            del io_w[fd]
            os.close(fd)
            job.satisfy()

      io_cond.release()

    except BaseException:
      import traceback
      traceback.print_exc()
      os._exit(1)

  @digest.by_name
  def initialize():
    if io_thread_box[0] is None:
      with io_cond:
        if io_thread_box[0] is None:
          def kill_all():
            with io_cond:
              t = io_thread_box[0]
              io_thread_box[0] = None
              io_cond.notify()
            if t: t.join()
            return not not t
            
          coflow.at_shutdown(kill_all)

          io_thread_box[0] = threading.Thread(target=io_thread_fn, args=())
          io_thread_box[0].daemon = True
          io_thread_box[0].start()

  @export
  @coflow.coroutine
  def process(args, show=True, stdin='', cwd=None, env=None, env_add=None):
    """
    Execute the `args` list of strings as a command with appropriate
    `os.execvp` variant. The stderr output of the process will be
    captured in a pseudo-terminal (not just a pipe) into a string
    buffer and logged with `noiselog` appropriately. If `cwd` is present and
    not `None` it is the directory path in which to execute the child. If `env`
    or `env_add` are present and not `None` they must be a dictionary of string
    to stringable (via str(x)) representing the environment in which to execute
    the child.
    
    This function returns a future representing the termination of
    the child process. If the child has a return code of zero, then the
    future will contain the stdout string. If the return code is non-zero then
    the future will be exceptional of type `noiselog.LoggedError`, thus
    indicating an aborting execution.
    """

    def flatten(x):
      if type(x) in (tuple, list):
        return sum((flatten(x1) for x1 in x), [])
      elif x is None:
        return []
      else:
        panic_unless(type(x) in (str,int,float))
        return [str(x)]
    
    try:
      args = flatten(args)
    except:
      print('args',args)
      raise

    if env is not None or env_add:
      def fmt(x):
        if type(x) in (tuple,list,set,frozenset):
          if type(x) in (set,frozenset):
            x = sorted(x)
          return ' '.join([shlex.quote(t) for t in x])
        elif x is None:
          return ''
        elif type(x) is bool:
          return str(int(x))
        else:
          return str(x)
      
      env_add = dict(env_add or {})
      for x in list(env_add.keys()):
        env_add[x] = fmt(env_add[x])

      if env is None:
        env = dict(os.environ)
      else:
        env = dict(env)
      for x in list(env.keys()):
        env[x] = fmt(env[x])
      env.update(env_add)

    def go(job):
      initialize()
      
      if show:
        msg = '(in '+cwd+')\n' if cwd else ''
        msg += ''.join(['%s=%s '%(shlex.quote(x), shlex.quote(y)) for x,y in (env_add or {}).items()])
        msg += ' '.join([shlex.quote(a) for a in args])
        msg += '\n'*2
        sys.stderr.write(msg)

      stdout_r, stdout_w = os.pipe()
      set_nonblock(stdout_r)

      stdin_r, stdin_w = os.pipe()
      set_nonblock(stdout_w)
      
      pid, ptfd = os.forkpty()
      
      if pid == 0: # i am child
        os.close(stdout_r)
        os.dup2(stdout_w, 1)
        os.close(stdout_w)

        os.close(stdin_w)
        os.dup2(stdin_r, 0)
        os.close(stdin_r)
        
        child_close_fds()
        
        if cwd is not None:
          os.chdir(cwd)
        
        if env is not None:
          os.execvpe(args[0], args, env)
        else:
          os.execvp(args[0], args)

      else: # i am parent
        with io_cond:
          job.pid = pid
          job.wait_n = 1
          
          io_r[ptfd] = ([], 'stderr', job)
          
          job.wait_n += 1
          os.close(stdin_r)
          io_w[stdin_w] = (reversed_bufs(stdin), job)
        
          job.wait_n += 1
          os.close(stdout_w)
          io_r[stdout_r] = ([], 'stdout', job)
          
          io_cond.notify()

    status, out, err = yield coflow.submit_job(Job(go))
    
    if status == 0:
      if show and not force_mute and len(err) != 0:
        noiselog.show(' '.join(args), err)
      yield out
    else:
      noiselog.error(' '.join(args),
        err + 
        ('\n'*(not err.endswith('\n')) + '\n< stdout >:\n')*bool(out) +
        out
      )
  
  def child_close_fds():
    try:
      fds = os.listdir('/dev/fd')
    except:
      fds = range(100)
    
    for fd in fds:
      try: fd = int(fd)
      except ValueError: continue
      if fd > 2:
        try: os.close(fd)
        except OSError: pass

  def reversed_bufs(s):
    if type(s) is str:
      s = s.encode()
    
    cn = select.PIPE_BUF
    n = (len(s) + cn-1)//cn
    ans = [None]*n
    i = 0
    while i < n:
      ans[i] = s[i*cn : (i+1)*cn]
      i += 1
    ans.reverse()
    return ans

  def set_nonblock(fd):
    fcntl.fcntl(fd, fcntl.F_SETFL,
      fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK
    )
  
_everything()
del _everything

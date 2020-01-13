from . import digest
from . import memodb
from . import noiselog
from . import opsys
from .process import process

@digest.by_name
def download(url, name=None):
  import re
  m = re.match('(http(s?)|ftp(s?))://', url)
  if m:
    name = name or url[len(m.group(0)):]
    path = memodb.mkpath(name)

    import sys
    sys.stderr.write('Downloading %s\n' % url)
    
    import urllib.request
    try:
      urllib.request.urlretrieve(url, path)
    except IOError as e:
      if e.args[0] == 'socket error':
        brutal.error('Internet troubles.',
          'Socket error "%s" when attempting to download "%s".\n'%(e.args[1].strerror, url)
        )
      else:
        raise
  
    sys.stderr.write('Finished    %s\n' % url)
    return path
  elif opsys.exists(url):
    return url
  else:
    noiselog.error('Invalid url or filesystem path: '+url)

@digest.by_name
def untar(path_tar):
  if opsys.isdir(path_tar):
    return path_tar
  
  name, ext = opsys.path.splitext(opsys.path.basename(path_tar))
  untar_dir = memodb.mkpath(name + '.d')
  import tarfile
  with tarfile.open(path_tar) as f:
    inner_dir = opsys.path.join(untar_dir, f.members[0].name)
    f.extractall(untar_dir)
  return inner_dir

@memodb.traced
def git_describe(cwd):
  version = process(
      ['git','describe','--dirty','--always','--tags'],
      cwd=cwd, show=0
    ).wait().strip()
  return memodb.Named(version, '')

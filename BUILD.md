# Devastator: Build Instructions #

```
# You have to be in *precisely* this directory for next command to work.
$> cd <devastator>

# This puts `brutal` command in your shell instance.
$> . sourceme

# The `brutal` command prints the name of the executable to stdout. We
# caputure that into a local shell variable named `exe`. Since `brutal` is
# in the shell, it can be invoked from any directory, feel free to move about.
$> exe=$(brutal <option=value>... exe <source-containing-main>)
```

Where `<option=value>...`:

  * `debug=[0|1]`: Affects default behavior of `optlev` and `syms`.
    (Default: `0`)
  
  * `optlev=[3..0]`: The `-O<optlev>` optimization level.
    (Default: `3 if debug==0 else 0`)

  * `syms=[0|1]`: Adds debug symbols to generated code. (Default: `debug`)
  
  * `world=[threads|gasnet]`: Use the gasnet backend (distributed memory machine)
    or just posix threads in a single shared memory process. (Default: threads)

  * `world=threads` backend only:

    - `ranks=<integer>`: Number of threads a.k.a ranks in devastator run.
      (Default: 2)

  * `world=gasnet` backend only:

    - `procs=<integer>`: Number of processes in devastator run. (Default: 2)

    - `workers=<integer>`: Number of worker threads per process. Devastator
      includes one hidden thread per process not accounted for by this value.
      Thus, when allocating system resources be sure to have `procs*(workers+1)`
      total number of logical CPU cores so each thread gets its own. (Default: 2)

## Brutal Cache ##

To annihilate brutal's cache of built things so that rebuild starts from scratch:

```
brutal clean
# -- OR --
rm -rf <devastator>/.brutal
```


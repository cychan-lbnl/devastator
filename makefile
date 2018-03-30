SHELL = bash
export

########################################################################

.PHONY: the-default
the-default: executable

########################################################################

devastator ?= .
devastator/src ?= $(devastator)/src/devastator
upcxx ?= $(devastator)/src/upcxx

########################################################################
# toolchain variables init

ifeq ($(debug),0)
	override debug :=
endif
override debug := $(if $(debug),1,)

syms ?= $(debug)
ifeq ($(syms),0)
	override syms :=
endif

opnew ?= $(if $(debug),,1)
ifeq ($(opnew),0)
	override opnew :=
endif

asan ?= $(and $(debug),$(if $(opnew),,1))
ifeq ($(asan),0)
	override asan :=
endif

ifeq ($(opnew_debug),0)
	override opnew_debug :=
endif

optlev ?= $(if $(debug),0,3)

# ppflags
ppflags =
ifeq ($(debug),)
	ppflags += -DNDEBUG
else
	ppflags += -DDEBUG=1
endif
ppflags += -DOPNEW_ENABLED=$(if $(opnew),1,0)
ppflags += $(if $(opnew_debug),-DOPNEW_DEBUG=$(opnew_debug),)

# cgflags
cgflags = -O$(optlev) $(if $(syms),-g,)
ifeq ($(optlev),3)
	cgflags += -flto
endif
ifneq ($(asan),)
	cgflags += -fsanitize=address
endif

# libflags
libflags = $(if $(and $(or $(debug),$(asan)),$(NERSC_HOST)),-dynamic,)

# if a better c++ compiler can't be found
CXX ?= g++
ifneq ($(NERSC_HOST),)
  cxx = CC
else
  cxx = $(CXX)
endif

########################################################################
# world variables

world ?= $(if $(NERSC_HOST),gasnet,threads)
threads ?= 2
procs ?= 2
workers ?= 2

ifeq ($(world),gasnet)
	include $(devastator)/ext/gasnet/makefile
	
	ppflags += -DWORLD_GASNET \
	           -DPROCESS_N=$(procs) \
	           -DWORKER_N=$(workers) \
	           $(GASNET_CXXCPPFLAGS)
	
	cgflags += -pthread
	libflags += $(GASNET_LIBS)
else
	ppflags += -DWORLD_THREADS \
	           -DRANK_N=$(threads)
	
	cgflags += -pthread
endif

########################################################################
# dependencies

devastator/tmsg.deps = \
  $(devastator/src)/diagnostic.hxx \
  $(devastator/src)/opnew.hxx \
  $(devastator/src)/opnew_fwd.hxx \
  $(devastator/src)/tmsg.hxx \
  $(upcxx)/utility.hpp

devastator/tmsg.srcs = \
  $(devastator/src)/diagnostic.cxx \
  $(devastator/src)/opnew.cxx \
  $(devastator/src)/tmsg.cxx

devastator/world_threads.deps = \
  $(devastator/tmsg.deps)

devastator/world_threads.srcs = \
  $(devastator/tmsg.srcs)

devastator/world_gasnet.deps = \
  $(devastator/tmsg.deps) \
	$(upcxx)/bind.hpp \
	$(upcxx)/command.hpp \
	$(upcxx)/diagnostic.hpp \
	$(upcxx)/packing.hpp \
	$(upcxx)/reflection.hpp \
	$(upcxx)/utility.hpp \
	$(devastator/src)/world_gasnet.hxx \
	$(gasnet_fragment)

devastator/world_gasnet.srcs = \
  $(devastator/tmsg.srcs) \
  $(upcxx)/diagnostic.cpp \
  $(upcxx)/packing.cpp \
  $(devastator/src)/world_gasnet.cxx

devastator/world.deps = \
  $(devastator/world_$(world).deps) \
  $(devastator/src)/reduce.hxx

devastator/world.srcs = \
  $(devastator/world_$(world).srcs) \
  $(devastator/src)/reduce.cxx

devastator/pdes.deps = \
  $(devastator/src)/intrusive_min_heap.hxx \
	$(devastator/src)/gvt.hxx \
	$(devastator/src)/pdes.hxx \
	$(devastator/src)/queue.hxx \
	$(devastator/world.deps)

devastator/pdes.srcs = \
  $(devastator/src)/pdes.cxx \
  $(devastator/src)/gvt.cxx \
  $(devastator/world.srcs)

########################################################################

ppflags += -I$(devastator)/src

ppflags := $($(app).ppflags) $(ppflags)
cgflags := $($(app).cgflags) $(cgflags)
libflags := $($(app).libflags) $(libflags)

########################################################################

exe.deps = $($(app).deps)
exe.srcs = $(shell python -c "import sys; sys.stdout.write(' '.join(sorted(set('$($(app).srcs)'.split()))))")

exe_name.0 = exe/$(app).$(world)
ifeq ($(world),gasnet)
	exe_name.1 = $(procs)x$(workers)
else
	exe_name.1 = $(threads)
endif
exe_name.2 = O$(optlev)$(if $(syms),g,)

exe_name = $(exe_name.0).$(exe_name.1).$(exe_name.2)

########################################################################

.SECONDARY:
.PHONY: executable
executable: $(build_makefiles)
	@build_makefiles= $(MAKE) $(foreach x,$(build_makefiles) makefile,-f $(x)) $(exe_name)

exe:
	@mkdir exe
	@ln -sf $(devastator)/run run

$(exe_name): exe $(exe.srcs) $(exe.deps) $(devastator)/makefile
	$(cxx) -std=c++14 $(ppflags) $(cgflags) -o $(exe_name) $(exe.srcs) $(libflags)

.PHONY: cmd-to-%
cmd-to-%: executable
	@if [[ $$NERSC_HOST != '' && $(world) == gasnet ]]; then \
		echo srun -n $(procs) -c $$((1+$(workers))) $(srun_args) ./$(exe_name) 1>&$*; \
	elif [[ $$NERSC_HOST != '' && $(world) == threads ]]; then \
		echo srun -n 1 -c $(threads) $(srun_args) ./$(exe_name) 1>&$*; \
	else \
		echo ./$(exe_name) 1>&$*; \
	fi

.PHONY: clean
clean:
	rm -rf exe

.PHONY: deepclean
deepclean: clean
	rm -rf $(devastator)/ext/gasnet/{install.*,build.*,source}

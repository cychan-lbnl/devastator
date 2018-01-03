SHELL = bash
export

########################################################################

.PHONY: the-default
the-default: executable

########################################################################

devastator ?= .
devastator/src ?= $(devastator)/src/devastator
upcxx ?= $(devastator)/src/upcxx

ifeq ($(debug),0)
	override debug :=
endif

ifeq ($(syms),0)
	override syms :=
endif

optlev ?= 3
ppflags =
cgflags =

ifeq ($(debug),)
	ifeq ($(optlev),3)
		cgflags += -flto
	endif
else
	override syms := 1
	override optlev := 0
	ppflags += -DOPNEW_ENABLED=0
	#cgflags += -fsanitize=address
endif

# if a better c++ compiler can't be found
CXX ?= g++
ifneq ($(NERSC_HOST),)
  cxx = CC
else
  cxx = $(CXX)
endif

world ?= $(if $(NERSC_HOST),gasnet,threads)
threads ?= 2
procs ?= 2
workers ?= 2

devastator/tmsg.deps = \
  $(devastator/src)/diagnostic.hxx \
  $(devastator/src)/opnew.hxx \
  $(devastator/src)/opnew_fwd.hxx \
  $(devastator/src)/tmsg.hxx

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

ifeq ($(world),gasnet)
	include $(devastator)/ext/gasnet/makefile
	
	ppflags = -DWORLD_GASNET \
	          -DPROCESS_N=$(procs) \
	          -DWORKER_N=$(workers) \
	          $(GASNET_CXXCPPFLAGS)
	
	libflags = $(GASNET_LIBS) -pthread
else
	ppflags = -DWORLD_THREADS \
	          -DRANK_N=$(threads)
	
	libflags = -pthread
endif

ppflags += -I$(devastator)/src

cgflags += -O$(optlev) $(if $(syms),-g,)


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
executable:
	@$(MAKE) $(foreach x,$(ext_makeparts) makefile,-f $(x)) $(exe_name)

exe:
	@mkdir -p exe

$(exe_name): exe $(exe.srcs) $(exe.deps) $(devastator)/makefile $(ext_makeparts)
	$(cxx) -std=c++14 $(ppflags) $(cgflags) $(exe.srcs) -o $(exe_name) $(libflags)

.PHONY: run
run: $(exe_name)
	./$(exe_name)

.PHONY: clean
clean:
	rm -rf exe

.PHONY: deepclean
deepclean: clean
	rm -rf $(devastator)/ext/gasnet/{install.*,build.*,source}

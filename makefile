SHELL = bash

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

ifeq ($(debug),)
	ifeq ($(optlev),3)
		override cgflags += -flto
	endif
else
	override syms := 1
	override optlev := 0
	override cgflags += -fsanitize=address
endif

world ?= threads
threads ?= 2
procs ?= 1
workers ?= 2

# if a better c++ compiler can't be found
CXX ?= g++
ifneq ($(NERSC_HOST),)
  cxx = CC
else
  cxx = $(CXX)
endif

devastator/tmessage.hxxs = \
  $(devastator/src)/diagnostic.hxx \
  $(devastator/src)/opnew.hxx \
  $(devastator/src)/tmessage.hxx

devastator/tmessage.cxxs = \
  $(devastator/src)/diagnostic.cxx \
  $(devastator/src)/opnew.cxx \
  $(devastator/src)/tmessage.cxx

devastator/world_threads.hxxs = \
  $(devastator/tmessage.hxxs)

devastator/world_threads.cxxs = \
  $(devastator/tmessage.cxxs)

devastator/world_gasnet.hxxs = \
  $(devastator/tmessage.hxxs) \
	$(upcxx)/command.hpp \
	$(upcxx)/diagnostic.hpp \
	$(upcxx)/packing.hpp \
	$(upcxx)/reflection.hpp \
	$(upcxx)/utility.hpp \
	$(devastator/src)/world_gasnet.hxx

devastator/world_gasnet.cxxs = \
  $(devastator/tmessage.cxxs) \
  $(upcxx)/diagnostic.cpp \
  $(upcxx)/packing.cpp \
  $(devastator/src)/world_gasnet.cxx

devastator/world.hxxs = \
  $(devastator/world_$(world).hxxs)

devastator/world.cxxs = \
  $(devastator/world_$(world).cxxs)

devastator/pdes.hxxs = \
  $(devastator/src)/intrusive_min_heap.hxx \
	$(devastator/src)/gvt.hxx \
	$(devastator/src)/pdes.hxx \
	$(devastator/src)/queue.hxx \
	$(devastator/world.hxxs)

devastator/pdes.cxxs = \
  $(devastator/src)/pdes.cxx \
  $(devastator/src)/gvt.cxx \
  $(devastator/world.cxxs)


########################################################################

ifeq ($(world),gasnet)
	include $(devastator)/ext/gasnet/makefile

	ppflags += -DWORLD_GASNET
	ppflags += -DPROCESS_N=$(procs)
	ppflags += -DWORKER_N=$(workers)
	ppflags += $(GASNET_CXXCPPFLAGS)
	
	libflags = $(GASNET_LIBS) -pthread
else
	ppflags += -DWORLD_THREADS
	ppflags += -DRANK_N=$(threads)
	
	libflags = -pthread
endif

ppflags += -I$(devastator)/src

override cgflags += -O$(optlev) $(if $(syms),-g,)


########################################################################

exe.hxxs = $($(app).hxxs)
exe.cxxs = $(shell python -c "import sys; sys.stdout.write(' '.join(sorted(set('$($(app).cxxs)'.split()))))")

exe_name.0 = exe/$(app).$(world)
ifeq ($(world),gasnet)
	exe_name.1 = $(procs)-$(workers)
else
	exe_name.1 = $(threads)
endif
exe_name.2 = O$(optlev)$(if $(syms),g,)

exe_name = $(exe_name.0).$(exe_name.1).$(exe_name.2)

########################################################################

.SECONDARY:
#.PHONY: executable
executable: $(exe_name)

exe:
	mkdir -p exe

$(exe_name): makefile exe $(exe.cxxs) $(exe.hxxs)
	$(cxx) -std=c++11 $(ppflags) $(cgflags) $(exe.cxxs) -o $(exe_name) $(libflags)

.PHONY: run
run: $(exe_name)
	./$(exe_name)

.POHNY: clean
clean:
	rm -rf exe

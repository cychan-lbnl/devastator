SHELL = bash

# USER TODO: Make sure a brutal.py matching the one in this directory exists in
# yours as well!

# abs path to devastator
deva := ../..
deva := $(shell cd $(deva); echo $$PWD)

# canonicalize configuration parameters
debug ?= 0
world ?= threads
ifeq ($(world),threads)
	ranks ?= 2
else
	procs ?= 2
	workers ?= 2
endif

# populate this with configuration variables for devastator, this logic uses
# the same names devastator expects, but doesn't have to, e.g. the app could
# expect "foo=..." from the user and give that to devastator as "bar=$(foo)"
deva_conf := $(if $(debug),debug=$(debug)) \
             $(if $(world),world=$(world)) \
             $(if $(ranks),ranks=$(ranks)) \
             $(if $(procs),procs=$(procs)) \
             $(if $(workers),workers=$(workers))
deva_conf := $(strip $(deva_conf))

# function for invoking "brutal $(deva_conf) makevar $(1) $(2)"
deva_var = $(shell cd $(deva); . sourceme; cd $(PWD); DEVA=$(deva) brutal $(deva_conf) makevar $(1) $(2))

# extract build flags from devastator given that deva_includes.hpp
# includes all the functionality this app needs from devastator
deva_cxx = $(call deva_var, CXX, deva_includes.hpp)
deva_ppflags = $(call deva_var, PPFLAGS, deva_includes.hpp)
deva_cgflags = $(call deva_var, CGFLAGS, deva_includes.hpp)
deva_ldflags = $(call deva_var, LDFLAGS, deva_includes.hpp)
deva_objs = $(call deva_var, OBJS, deva_includes.hpp)
deva_libflags = $(call deva_var, LIBFLAGS, deva_includes.hpp)

# perhaps more key=val parameters for your app?
app_conf := $(deva_conf)
# list of root files involved in build
app_srcs := main.cpp
# list of non-root files involved in build
app_deps := deva_includes.hpp

# make app_conf nice for filenames
empty :=
space := $(empty) $(empty)
app_conf_trim := $(subst $(space),.,$(app_conf))

# the application build logic
exe/main.$(app_conf_trim): $(app_srcs) $(app_deps)
	@mkdir -p exe
	$(deva_cxx) $(deva_ldflags) -o "$@" $(deva_ppflags) $(deva_cgflags) $(app_srcs) $(deva_objs) $(deva_libflags)
	
.PHONY: clean
clean:
	rm -rf exe

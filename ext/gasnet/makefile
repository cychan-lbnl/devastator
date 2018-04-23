gasnet_src = GASNet-EX-collaborator-snapshot
gasnet_tgz = $(gasnet_src).tar.gz
gasnet_url = http://mantis.lbl.gov/nightly/unlisted/$(gasnet_tgz)

gasnet_debug =
ifeq ($(optlev),0)
	gasnet_debug = $(debug)
endif

gasnet_suffix = $(if $(NERSC_HOST),$(NERSC_HOST).,)$(if $(gasnet_debug),debug,fast)

gasnet_install := ext/gasnet/install.$(gasnet_suffix)
gasnet_install_abs := $(shell cd $(devastator); pwd)/$(gasnet_install)
gasnet_install := $(devastator)/$(gasnet_install)

gasnet_config_args := --prefix=$(gasnet_install_abs)
gasnet_config_args += $(if $(gasnet_debug),--enable-debug,)
gasnet_config_args += --disable-parsync --disable-par
gasnet_config_args += --disable-mpi --disable-udp

gasnet_conduit ?= $(if $(NERSC_HOST),aries,smp)
gasnet_cross ?= $(if $(NERSC_HOST),cray-aries-slurm,)

ifeq ($(gasnet_conduit),aries)
	gasnet_config_args += --with-aries-max-medium=64896
endif

gasnet_makepart = $(gasnet_install)/include/$(gasnet_conduit)-conduit/$(gasnet_conduit)-seq.mak

#gasnet_config_env += CXXFLAGS="$(cgflags)"

$(gasnet_makepart): $(devastator)/ext/gasnet/makefile
	@cd $(devastator)/ext/gasnet; \
	wget -O $(gasnet_tgz) $(gasnet_url); \
	rm -rf source; \
	tar xzf $(gasnet_tgz); \
	mv $(gasnet_src) source; \
	rm -rf $(gasnet_tgz) build.$(gasnet_suffix); \
	mkdir build.$(gasnet_suffix); \
	if [[ "$(gasnet_cross)" ]]; then \
		cd source; \
		ln -sf other/contrib/cross-configure-$(gasnet_cross) .; \
		cd ../build.$(gasnet_suffix); \
		$(gasnet_config_env) ../source/cross-configure-$(gasnet_cross) $(gasnet_config_args); \
		make install; \
		echo "Built GASNet with cross=$(gasnet_cross)"; \
	else \
		cd build.$(gasnet_suffix); \
		CXX=$(cxx) $(gasnet_config_env) ../source/configure $(gasnet_config_args); \
		make install; \
		echo "Built GASNet."; \
	fi; \
	cd ..; \
	echo;

build_makefiles += $(gasnet_makepart)

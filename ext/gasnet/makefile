gasnet_src = GASNet-EX-collaborator-snapshot
gasnet_tgz = $(gasnet_src).tar.gz
gasnet_url = http://mantis.lbl.gov/nightly/unlisted/$(gasnet_tgz)

gasnet_suffix = $(if $(NERSC_HOST),$(NERSC_HOST).,)$(if $(debug),debug,fast)

gasnet_install := ext/gasnet/install.$(gasnet_suffix)
gasnet_install_abs := $(shell cd $(devastator); pwd)/$(gasnet_install)
gasnet_install := $(devastator)/$(gasnet_install)

gasnet_config_args := --prefix=$(gasnet_install_abs)
gasnet_config_args += $(if $(debug),--enable-debug,)
gasnet_config_args += --disable-parsync --disable-par
gasnet_config_args += --disable-mpi --disable-udp

ifeq ($(NERSC_HOST),)
  gasnet_makepart = $(gasnet_install)/include/smp-conduit/smp-seq.mak
else
  gasnet_makepart = $(gasnet_install)/include/aries-conduit/aries-seq.mak
	gasnet_config_args += --with-aries-max-medium=65472
endif

#gasnet_config_env += CXXFLAGS="$(cgflags)"

$(gasnet_makepart): $(devastator)/ext/gasnet/makefile
	@cd $(devastator)/ext/gasnet; \
	wget -O $(gasnet_tgz) $(gasnet_url); \
	rm -rf source; \
	tar xzf $(gasnet_tgz); \
	mv $(gasnet_src) source; \
	rm -rf $(gasnet_tgz) build.$(gasnet_suffix); \
	mkdir build.$(gasnet_suffix); \
	case "$$NERSC_HOST" in \
	edison|cori) \
		cd source; \
		ln -sf other/contrib/cross-configure-cray-aries-slurm .; \
		cd ../build.$(gasnet_suffix); \
		$(gasnet_config_env) ../source/cross-configure-cray-aries-slurm $(gasnet_config_args); \
		make install; \
		echo "Built GASNet for NERSC=$$NERSC_HOST"; \
		;; \
	*) \
		cd build.$(gasnet_suffix); \
		CXX=$(cxx) $(gasnet_config_env) ../source/configure $(gasnet_config_args); \
		make install; \
		echo "Built GASNet."; \
		;; \
	esac; \
	cd ..; \
	echo;

build_makefiles += $(gasnet_makepart)

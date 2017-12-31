gasnet_src = GASNet-EX-collaborator-snapshot
gasnet_tgz = $(gasnet_src).tar.gz
gasnet_url = http://mantis.lbl.gov/nightly/unlisted/$(gasnet_tgz)

gasnet_install = $(devastator)/ext/gasnet/install
gasnet_install_abs = $(shell cd $(devastator); pwd)/ext/gasnet/install

gasnet_config_args = --prefix=$(gasnet_install_abs)
gasnet_config_args += --disable-parsync --disable-par
gasnet_config_args += --disable-mpi --disable-udp

ifeq ($(NERSC_HOST),)
  gasnet_fragment = $(gasnet_install)/include/smp-conduit/smp-seq.mak
else
  gasnet_fragment = $(gasnet_install)/include/aries-conduit/aries-seq.mak
endif

$(gasnet_fragment): $(devastator)/ext/gasnet/makefile
	@cd $(devastator)/ext/gasnet; \
	wget -O $(gasnet_tgz) $(gasnet_url); \
	tar xzf $(gasnet_tgz); \
	mkdir -p build; \
	if [[ $$NERSC_HOST = "''" ]]; then \
		cd $(gasnet_src); \
		ln -sf other/contrib/cross-configure-cray-aries-slurm .; \
		cd ../build; \
		../$(gasnet_src)/cross-configure-cray-aries-slurm $(gasnet_config_args); \
		make install; \
	else \
		cd build; \
		CXX=$(cxx) ../$(gasnet_src)/configure $(gasnet_config_args); \
		make install; \
	fi; \
	rm -rf $(gasnet_tgz)

include $(gasnet_fragment)

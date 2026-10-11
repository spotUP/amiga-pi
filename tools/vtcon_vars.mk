# Prints a variable of vtcon's Makefile: make -C VTCON_DIR -f Makefile -f <this> print-VC
# (amiga-pi takes vbcc's command line, the netinclude and the TLS choice from there, not a copy)
print-%:
	@echo '$($*)'

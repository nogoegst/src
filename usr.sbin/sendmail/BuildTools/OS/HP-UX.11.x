#	@(#)HP-UX.11.x	8.7	(Berkeley)	3/30/1998
define(`confCC', `cc -Ae')
define(`confMAPDEF', `-DNDBM -DNIS -DMAP_REGEX')
define(`confENVDEF', `-DV4FS -DHPUX11 ')
define(`confOPTIMIZE', `+O3')
define(`confLIBS', `-ldbm -lnsl')
define(`confSHELL', `/usr/bin/sh')
define(`confSTDIR', `/etc/mail')
define(`confHFDIR', `/usr/share/lib')
define(`confINSTALL', `${BUILDBIN}/install.sh')
define(`confSBINGRP', `mail')

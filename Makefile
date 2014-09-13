INCLUDEDIRS := -I.
INCLUDEDIRS += -I$(shell /usr/local/pgsql/bin/pg_config  --includedir-server)
INCLUDEDIRS += -I$(shell /usr/local/pgsql/bin/pg_config  --includedir)
LIBDIR = -L$(shell /usr/local/pgsql/bin/pg_config  --libdir)
LIBINSTALL = $(shell /usr/local/pgsql/bin/pg_config  --pkglibdir)

timestampandtz.so: timestampandtz.c Makefile
	gcc -fpic -o timestampandtz.o -c timestampandtz.c $(INCLUDEDIRS)
	gcc -bundle -flat_namespace -undefined suppress -o timestampandtz.so timestampandtz.o $(LIBDIR) -lpq -lpgcommon -lpgport -lpgtypes
	cp timestampandtz.so $(LIBINSTALL)

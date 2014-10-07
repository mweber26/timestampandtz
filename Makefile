MODULES = timestampandtz
EXTENSION = timestampandtz
EXTVERSION  = $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\\([^']*\\)'/\\1/")
DATA = $(wildcard *--*.sql)
DOCS = README.md
REGRESS = tests

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

all: $(EXTENSION)--$(EXTVERSION).sql

timestampandtz.o : to_char.c zones.c

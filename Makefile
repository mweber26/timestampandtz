MODULES = timestampandtz
EXTENSION = timestampandtz
DATA = timestampandtz.sql
DOCS = README.md
REGRESS = tests

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

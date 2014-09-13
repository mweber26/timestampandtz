create type timestampandtz;
create function timestampandtz_in(cstring) returns timestampandtz as 'timestampandtz.so' LANGUAGE C IMMUTABLE STRICT;
create function timestampandtz_out(timestampandtz) returns cstring as 'timestampandtz.so' LANGUAGE C IMMUTABLE STRICT;
create function timestampandtz_recv(internal) returns timestampandtz as 'timestampandtz.so' LANGUAGE C IMMUTABLE STRICT;
create function timestampandtz_send(timestampandtz) returns bytea as 'timestampandtz.so' LANGUAGE C IMMUTABLE STRICT;
create function pg_catalog.timezone(text, timestampandtz) returns timestamp as 'timestampandtz.so', 'timestampandtz_timezone' LANGUAGE C IMMUTABLE STRICT;
create type timestampandtz ( internallength = 12, input = timestampandtz_in, output = timestampandtz_out, send = timestampandtz_send, receive = timestampandtz_recv );

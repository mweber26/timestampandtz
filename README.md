# timestampandtz

A postgresql date/time type that stores both the timestamp and the timezone.

### Input/Output

The type supports both input and output from text format both with a specified timezone and with the implicit session/database time zone.  The standard timestamp date parsing is supported  with the implicit session timezone:

```sql
postgres=# select '9/1/2014 10:15pm'::timestampandtz;
          timestampandtz          
----------------------------------
 2014-09-01 22:15:00 @ US/Eastern
(1 row)
```

The format for the input/output of direct timestampandtz is **[date string] @ [timezone]**.  Using this format, the type also supports a forced timezone (doesn't matter what the session time zone is set to):

```sql
postgres=# select '9/1/2014 10:15pm @ US/Pacific'::timestampandtz;
          timestampandtz          
----------------------------------
 2014-09-01 22:15:00 @ US/Pacific
(1 row)
```

You can see that the output is always the local time with the timezone displayed.

### Binary format

The internal binary format of timestampandtz is 10 bytes with the standard 8-byte timestamp (in UTC) combines with a 2-byte timezone ID.  The timezone IDs are fixed, and are simply a list of timezones from *pg_timezone_names* with each timezone assigned a specific fixed ID. 

### Casts

The type supports casting from both timestamp and timestamptz and to both timestamp and timestamptz.

#### Cast to timestamp

Casts to timestamp are always done from the local timestamp of the timestampandtz.  Since the "at time zone" clause is also supported, the default, un-adorned conversion is to UTC:

```sql
postgres=# select '9/1/2014 10:15pm @ US/Eastern'::timestampandtz::timestamp;
      timestamp      
---------------------
 2014-09-01 22:15:00
(1 row)

postgres=# select '9/1/2014 10:15pm @ US/Eastern'::timestampandtz at time zone 'utc';
      timestamp      
---------------------
 2014-09-02 02:15:00
(1 row)
```

#### Cast to timestamptz

Casts to timestamptz follow the same rules as normal timestamptz.  We convert our local timestamp to the UTC and the display routine for timestamptz takes over converting it to the local session timezone:

```sql
postgres=# set time zone 'US/Eastern';
SET
postgres=# select '9/1/2014 10:15pm @ US/Eastern'::timestampandtz::timestamptz;
      timestamptz       
------------------------
 2014-09-01 22:15:00-04
(1 row)
```

#### Cast from timestamp

Casts from timestamp are takes as a local timestamp converted to the session time zone:

```sql
postgres=# select '9/1/2014 10:15pm'::timestamp::timestampandtz;
          timestampandtz          
----------------------------------
 2014-09-01 22:15:00 @ US/Eastern
(1 row)
```

#### Cast from timestamptz

Casts from timestamp are takes as a local timestamp converted to the session time zone:

```sql
postgres=# select '9/1/2014 10:15pm'::timestamptz::timestampandtz;
          timestampandtz          
----------------------------------
 2014-09-01 22:15:00 @ US/Eastern
(1 row)
```

### Compares

All comparison operations are complete and act as point-in-time comparisons, so they are immutable (unlike timestamptz which relys on the current sesssion time zone):

```sql
postgres=# select '9/1/2014 10:15pm @ US/Eastern'::timestampandtz='9/1/2014 7:15pm @ US/Pacific'::timestampandtz;
 ?column? 
----------
 t
(1 row)
```

Using the standard set of casts support for now()/current_timestamp is preserved when using timestampandtz:

```sql
postgres=# create table times (dt timestampandtz);
CREATE TABLE
postgres=# insert into times values (now());
INSERT 0 1
postgres=# insert into times values (current_timestamp);
INSERT 0 1
postgres=# select * from times;
                   dt                    
-----------------------------------------
 2014-09-13 23:26:17.170931 @ US/Eastern
 2014-09-13 23:26:21.146286 @ US/Eastern
(2 rows)
```

### Indexing

Because we have the time zone stored with the point-in-time timestamp, full btree indexing is supported (where the comparisons occur in UTC time, but values are preserved in local time zones).

```sql
postgres=# create table times (dt timestampandtz);
CREATE TABLE
postgres=# create index ix_times_dt on times(dt);
CREATE INDEX
postgres=# insert into times values ('9/1/2014 10:15pm @ US/Eastern'::timestampandtz);
INSERT 0 1
postgres=# insert into times values ('9/1/2014 9:16pm @ US/Central'::timestampandtz);
INSERT 0 1
postgres=# insert into times values ('9/1/2014 10:17pm'::timestampandtz);
INSERT 0 1
postgres=# insert into times values ('9/1/2014 7:18pm @ US/Pacific'::timestampandtz);
INSERT 0 1
postgres=# select * from times order by dt;
                dt                
----------------------------------
 2014-09-01 22:15:00 @ US/Eastern
 2014-09-01 21:16:00 @ US/Central
 2014-09-01 22:17:00 @ US/Eastern
 2014-09-01 19:18:00 @ US/Pacific
(4 rows)
postgres=# select * from times where dt >= '2014-09-02 02:17:00 @ UTC' order by dt;
                dt                
----------------------------------
 2014-09-01 22:17:00 @ US/Eastern
 2014-09-01 19:18:00 @ US/Pacific
(2 rows)
```

### Intervals

Intervals are supported and work based on wall clocks with respect to daylight savings time.   For example, at the crossover (+3 months) of DST in the US/Eastern, the wall clock stays the same (8:15pm + 3 months is still 8:15pm) and the time zone remains the same.  The thing that changes is the internal UTC timestamp (since we crossed DST):

```sql
postgres=# select ('9-18-2014 8:15pm @ US/Eastern'::timestampandtz)::timestamp;
      timestamp      
---------------------
 2014-09-19 00:15:00
(1 row)

postgres=# select '9-18-2014 8:15pm @ US/Eastern'::timestampandtz + interval '3 months';
             ?column?             
----------------------------------
 2014-12-18 20:15:00 @ US/Eastern
(1 row)

postgres=# select ('9-18-2014 8:15pm @ US/Eastern'::timestampandtz + interval '3 months')::timestamp;
      timestamp      
---------------------
 2014-12-19 01:15:00
(1 row)
```

### Functions

#### tzmove

This function allows you to move the timezone for a timestampandtz value:

```sql
postgres=# select tzmove('9-18-2014 8:15pm @ US/Eastern'::timestampandtz, 'US/Pacific');
              tzmove              
----------------------------------
 2014-09-18 17:15:00 @ US/Pacific
(1 row)
```

#### date_trunc and date_trunc_at

Functions to perfrom date truncation on timestampandtz values. See https://github.com/mweber26/timestampandtz/wiki/date_trunc for a detailed description.

#### date_part

Function to get a date part from the local time of the timestamp.

#### at time zone

The standard **at time zone** clause works by returning a standard timestamp (no zone information) at the specified time zone:

```sql
postgres=# select '9-18-2014 8:15pm @ US/Eastern'::timestampandtz at time zone 'US/Pacific';
      timezone       
---------------------
 2014-09-18 17:15:00
(1 row)
```

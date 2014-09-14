\set ECHO 0
\i timestampandtz.sql
\set ECHO all

set time zone 'US/Eastern';
select '9-18-2014 8:15pm'::timestampandtz;
set time zone 'US/Pacific';
select '9-18-2014 8:15pm'::timestampandtz;

select '9-18-2014 8:15pm @ US/Eastern'::timestampandtz;
select '9-18-2014 8:15pm @ US/Pacific'::timestampandtz;
select '9-18-2014 8:15pm @ foobar'::timestampandtz;

select '9-18-2014 8:15pm @ US/Eastern'::timestampandtz::timestamp;
select '9-18-2014 8:15pm @ US/Pacific'::timestampandtz::timestamp;

set time zone 'US/Eastern';
select '9-18-2014 8:15pm @ US/Eastern'::timestampandtz::timestamptz;
select '9-18-2014 8:15pm @ US/Pacific'::timestampandtz::timestamptz;

select '9-18-2014 8:15pm @ US/Eastern'::timestampandtz + interval '3 hours';
select '9-18-2014 8:15pm @ US/Pacific'::timestampandtz + interval '3 hours';
select '9-18-2014 8:15pm @ US/Eastern'::timestampandtz + interval '3 days';
select '9-18-2014 8:15pm @ US/Pacific'::timestampandtz + interval '3 days';
select '9-18-2014 8:15pm @ US/Eastern'::timestampandtz + interval '3 months';
select '9-18-2014 8:15pm @ US/Pacific'::timestampandtz + interval '3 months';

select ('9-18-2014 8:15pm @ US/Eastern'::timestampandtz + interval '3 hours')::timestamp;
select ('9-18-2014 8:15pm @ US/Pacific'::timestampandtz + interval '3 hours')::timestamp;
select ('9-18-2014 8:15pm @ US/Eastern'::timestampandtz + interval '3 days')::timestamp;
select ('9-18-2014 8:15pm @ US/Pacific'::timestampandtz + interval '3 days')::timestamp;
select ('9-18-2014 8:15pm @ US/Eastern'::timestampandtz + interval '3 months')::timestamp;
select ('9-18-2014 8:15pm @ US/Pacific'::timestampandtz + interval '3 months')::timestamp;

create table times (dt timestampandtz);
create index ix_times_dt on times (dt);

insert into times values ('9-18-2014 8:15pm @ US/Eastern');
insert into times values ('9-18-2014 5:16pm @ US/Pacific');
insert into times values ('9-18-2014 8:17pm @ US/Eastern');
insert into times values ('9-18-2014 7:18pm @ US/Central');

select dt,dt::timestamp,dt::timestamptz from times order by times.dt asc;
select dt,dt at time zone 'US/Eastern' from times order by times.dt asc;
select dt,dt at time zone 'US/Central' from times order by times.dt asc;

select tzmove('9-18-2014 8:15pm @ US/Eastern'::timestampandtz, 'US/Eastern');
select tzmove('9-18-2014 8:15pm @ US/Eastern'::timestampandtz, 'US/Pacific');
select tzmove('9-18-2014 8:15pm @ US/Eastern'::timestampandtz, 'foobar');

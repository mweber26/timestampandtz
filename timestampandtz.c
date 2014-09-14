#include "postgres.h"
#include "fmgr.h"
#include "access/heapam.h"
#include "miscadmin.h"
#include "funcapi.h"
#include "pgtime.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "parser/scansup.h"
#include "access/xact.h"
#include <string.h>

PG_MODULE_MAGIC;

Datum timestampandtz_in(PG_FUNCTION_ARGS);
Datum timestampandtz_out(PG_FUNCTION_ARGS);
Datum timestampandtz_recv(PG_FUNCTION_ARGS);
Datum timestampandtz_send(PG_FUNCTION_ARGS);
Datum timestampandtz_typmodin(PG_FUNCTION_ARGS);
Datum timestampandtz_typmodout(PG_FUNCTION_ARGS);
Datum timestampandtz_pl_interval(PG_FUNCTION_ARGS);
Datum timestampandtz_mi_interval(PG_FUNCTION_ARGS);
Datum timestampandtz_mi(PG_FUNCTION_ARGS);
Datum timestampandtz_eq(PG_FUNCTION_ARGS);
Datum timestampandtz_ne(PG_FUNCTION_ARGS);
Datum timestampandtz_gt(PG_FUNCTION_ARGS);
Datum timestampandtz_ge(PG_FUNCTION_ARGS);
Datum timestampandtz_lt(PG_FUNCTION_ARGS);
Datum timestampandtz_le(PG_FUNCTION_ARGS);
Datum timestampandtz_cmp(PG_FUNCTION_ARGS);
Datum timestampandtz_scale(PG_FUNCTION_ARGS);
Datum timestampandtz_to_timestamptz(PG_FUNCTION_ARGS);
Datum timestampandtz_to_timestamp(PG_FUNCTION_ARGS);
Datum timestampandtz_timezone(PG_FUNCTION_ARGS);
Datum timestamptz_to_timestampandtz(PG_FUNCTION_ARGS);
Datum timestamp_to_timestampandtz(PG_FUNCTION_ARGS);
Datum timestampandtz_movetz(PG_FUNCTION_ARGS);
Datum timestampandtz_to_char(PG_FUNCTION_ARGS);

typedef struct TimestampAndTz {
	Timestamp time;
	short tz;
} TimestampAndTz;

#include "zones.c"
#include "to_char.c"

static void debug_tm(struct pg_tm *tm)
{
	fprintf(stderr, "%d/%d/%d %d:%d:%d\n",
		tm->tm_mon, tm->tm_mday, tm->tm_year,
		tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void EncodeSpecialTimestamp(Timestamp dt, char *str)
{
    if (TIMESTAMP_IS_NOBEGIN(dt))
        strcpy(str, EARLY);
    else if (TIMESTAMP_IS_NOEND(dt))
        strcpy(str, LATE);
    else    /* shouldn't happen */
        elog(ERROR, "invalid argument for EncodeSpecialTimestamp");
}

static Timestamp dt2local(Timestamp dt, int tz)
{
#ifdef HAVE_INT64_TIMESTAMP
    dt -= (tz * USECS_PER_SEC);
#else
    dt -= tz;
#endif
    return dt;
}

static int32 anytimestamp_typmodin(ArrayType *ta)
{
	int32		typmod;
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
	 * we're not too tense about good error message here because grammar
	 * shouldn't allow wrong number of modifiers for TIMESTAMP
	 */
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	if (*tl < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TIMESTAMP(%d)%s precision must not be negative",
						*tl, " AND TIME ZONE")));
	if (*tl > MAX_TIMESTAMP_PRECISION)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		   errmsg("TIMESTAMP(%d)%s precision reduced to maximum allowed, %d",
				  *tl, " AND TIME ZONE",
				  MAX_TIMESTAMP_PRECISION)));
		typmod = MAX_TIMESTAMP_PRECISION;
	}
	else
		typmod = *tl;

	return typmod;
}

static char *anytimestamp_typmodout(int32 typmod)
{
	if (typmod >= 0)
	{
		char *ret = palloc(6);
		sprintf(ret, "(%d)", (int) typmod);
		return ret;
	}
	else
		return "";
}

static void AdjustTimestampForTypmod(Timestamp *time, int32 typmod)
{
#ifdef HAVE_INT64_TIMESTAMP
	static const int64 TimestampScales[MAX_TIMESTAMP_PRECISION + 1] = {
		INT64CONST(1000000),
		INT64CONST(100000),
		INT64CONST(10000),
		INT64CONST(1000),
		INT64CONST(100),
		INT64CONST(10),
		INT64CONST(1)
	};

	static const int64 TimestampOffsets[MAX_TIMESTAMP_PRECISION + 1] = {
		INT64CONST(500000),
		INT64CONST(50000),
		INT64CONST(5000),
		INT64CONST(500),
		INT64CONST(50),
		INT64CONST(5),
		INT64CONST(0)
	};
#else
	static const double TimestampScales[MAX_TIMESTAMP_PRECISION + 1] = {
		1,
		10,
		100,
		1000,
		10000,
		100000,
		1000000
	};
#endif

	if (!TIMESTAMP_NOT_FINITE(*time)
		&& (typmod != -1) && (typmod != MAX_TIMESTAMP_PRECISION))
	{
		if (typmod < 0 || typmod > MAX_TIMESTAMP_PRECISION)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				  errmsg("timestamp(%d) precision must be between %d and %d",
						 typmod, 0, MAX_TIMESTAMP_PRECISION)));

		/*
		 * Note: this round-to-nearest code is not completely consistent about
		 * rounding values that are exactly halfway between integral values.
		 * On most platforms, rint() will implement round-to-nearest-even, but
		 * the integer code always rounds up (away from zero).  Is it worth
		 * trying to be consistent?
		 */
#ifdef HAVE_INT64_TIMESTAMP
		if (*time >= INT64CONST(0))
		{
			*time = ((*time + TimestampOffsets[typmod]) / TimestampScales[typmod]) *
				TimestampScales[typmod];
		}
		else
		{
			*time = -((((-*time) + TimestampOffsets[typmod]) / TimestampScales[typmod])
					  * TimestampScales[typmod]);
		}
#else
		*time = rint((double) *time * TimestampScales[typmod]) / TimestampScales[typmod];
#endif
	}
}

static Datum gen_timestamp(Timestamp stamp, int tz)
{
	TimestampAndTz *result = (TimestampAndTz *) palloc0(sizeof(TimestampAndTz));
	result->time = stamp;
	result->tz = tz;
	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(timestampandtz_in);
Datum timestampandtz_in(PG_FUNCTION_ARGS)
{
	char *str = PG_GETARG_CSTRING(0);
	int32 typmod = PG_GETARG_INT32(2);
	Timestamp timestamp;
	fsec_t fsec;
	struct pg_tm tt, *tm = &tt;
	int tz, dtype, nf, dterr;
	char *field[MAXDATEFIELDS];
	int  ftype[MAXDATEFIELDS];
	char workbuf[MAXDATELEN + MAXDATEFIELDS];
	pg_tz * tzp = NULL;
	char *tzn;
	int tzid;
	int tz_index;

	tz_index = strcspn(str, "@");
	if(tz_index < strlen(str))
	{
		/* split the date/time string and the timezone string at the @ */
		str = pstrdup(str);
		str[tz_index] = 0x00;
		tzn = pstrdup(&str[tz_index] + 1);

		/* trim end of date/time string */
		while(isspace(str[strlen(str) - 1]))
			str[strlen(str) - 1] = 0x00;

		/* trim end and start of timezone */
		while(isspace(*tzn)) tzn++;
		while(isspace(tzn[strlen(tzn) - 1])) tzn[strlen(tzn) - 1] = 0x00;

		/* find our timezone id */
		tzid = tzname_to_tzid(tzn);
	}
	else
	{
		/* find our timezone id for the current session timezone */
		tzn = pstrdup(pg_get_timezone_name(session_timezone));
		tzid = tzname_to_tzid(tzn);
	}

	if(tzid == 0)
	{
		elog(ERROR, "missing timezone ID \"%s\" while parsing timestampandtz \"%s\"", tzn, str);
		return gen_timestamp(DT_NOEND, 0);
	}

	/* standard date/time parse */
	dterr = ParseDateTime(str, workbuf, sizeof(workbuf), field, ftype, MAXDATEFIELDS, &nf);
	if(dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz);
	if(dterr != 0)
		DateTimeParseError(dterr, str, "timestamp and time zone");

	/* set the timezone and determine the offset for the parsed date time (which is local time) */
	tzp = pg_tzset(tzn);
	tz = DetermineTimeZoneOffset(tm, tzp);

	switch(dtype)
	{
		case DTK_DATE:
			if(tm2timestamp(tm, fsec, &tz, &timestamp) != 0)
				ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
		 			errmsg("timestamp out of range: \"%s\"", str)));
			break;

		case DTK_EPOCH:
			timestamp = SetEpochTimestamp();
			break;

		case DTK_LATE:
			TIMESTAMP_NOEND(timestamp);
			break;

		case DTK_EARLY:
			TIMESTAMP_NOBEGIN(timestamp);
			break;

		case DTK_INVALID:
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("date/time value \"%s\" is no longer supported", str)));
			TIMESTAMP_NOEND(timestamp);
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing timestampandtz \"%s\"",
				dtype, str);
			TIMESTAMP_NOEND(timestamp);
	}

	AdjustTimestampForTypmod(&timestamp, typmod);
	return gen_timestamp(timestamp, tzid);
}

PG_FUNCTION_INFO_V1(timestampandtz_out);
Datum timestampandtz_out(PG_FUNCTION_ARGS)
{
	TimestampAndTz *dt = (TimestampAndTz *)PG_GETARG_POINTER(0);
	char *result;
	struct pg_tm tt, *tm = &tt;
	int tz;
	fsec_t fsec;
	char buf[MAXDATELEN + 1];
	pg_tz * tzp = NULL;
	const char * tzname = NULL;

	/* does the argument have a valid timezone */
	if(dt->tz != 0)
	{
		tzname = tzid_to_tzname(dt->tz);
		tzp = pg_tzset(tzname);
	}

	if(TIMESTAMP_NOT_FINITE(dt->time))
		EncodeSpecialTimestamp(dt->time, buf);
	else if(timestamp2tm(dt->time, &tz, tm, &fsec, NULL, tzp) == 0)
		EncodeDateTime(tm, fsec, false, tz, NULL, DateStyle, buf);
	else
		ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE), errmsg("timestamp out of range")));

	/* output the string format of the local time and the set timezone */
	result = palloc0(strlen(buf) + 3 + strlen(tzname) + 1);
	strcat(result, buf);
	strcat(result, " @ ");
	strcat(result, tzname);
	PG_RETURN_CSTRING(result);
}

PG_FUNCTION_INFO_V1(timestampandtz_recv);
Datum timestampandtz_recv(PG_FUNCTION_ARGS)
{
	StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
	int32 typmod = PG_GETARG_INT32(2);
	TimestampAndTz *result;

	/* input the timestamp and the timezone id */
	result = (TimestampAndTz *) palloc0(sizeof(TimestampAndTz));
	result->time = pq_getmsgint64(buf);
	result->tz = pq_getmsgint(buf, 2);

	AdjustTimestampForTypmod(&result->time, typmod);
	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(timestampandtz_send);
Datum timestampandtz_send(PG_FUNCTION_ARGS)
{
	TimestampAndTz *arg = (TimestampAndTz *)PG_GETARG_POINTER(0);
	StringInfoData buf;

	/* output the timestamp and the timezone id */
	pq_begintypsend(&buf);
	pq_sendint64(&buf, arg->time);
	pq_sendint(&buf, arg->tz, 2);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

PG_FUNCTION_INFO_V1(timestampandtz_typmodin);
Datum timestampandtz_typmodin(PG_FUNCTION_ARGS)
{
    ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);
    PG_RETURN_INT32(anytimestamp_typmodin(ta));
}

PG_FUNCTION_INFO_V1(timestampandtz_typmodout);
Datum timestampandtz_typmodout(PG_FUNCTION_ARGS)
{
    int32 typmod = PG_GETARG_INT32(0);
    PG_RETURN_CSTRING(anytimestamp_typmodout(typmod));
}

PG_FUNCTION_INFO_V1(timestampandtz_scale);
Datum timestampandtz_scale(PG_FUNCTION_ARGS)
{
	TimestampAndTz *arg = (TimestampAndTz *)PG_GETARG_POINTER(0);
	int32 typmod = PG_GETARG_INT32(1);
	Timestamp result;

	result = arg->time;

	AdjustTimestampForTypmod(&result, typmod);
	return gen_timestamp(result, arg->tz);
}

PG_FUNCTION_INFO_V1(timestampandtz_timezone);
Datum timestampandtz_timezone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_PP(0);
	TimestampAndTz *dt = (TimestampAndTz *)PG_GETARG_POINTER(1);
	Timestamp result;
	int			tz = dt->tz;
	char		tzname[TZ_STRLEN_MAX + 1];
	char	   *lowzone;
	int			type,
				val;
	pg_tz	   *tzp;
	Timestamp time = dt->time;

	if (TIMESTAMP_NOT_FINITE(time))
		PG_RETURN_TIMESTAMPTZ(time);

	/*
	 * Look up the requested timezone.  First we look in the date token table
	 * (to handle cases like "EST"), and if that fails, we look in the
	 * timezone database (to handle cases like "America/New_York").  (This
	 * matches the order in which timestamp input checks the cases; it's
	 * important because the timezone database unwisely uses a few zone names
	 * that are identical to offset abbreviations.)
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));
	lowzone = downcase_truncate_identifier(tzname, strlen(tzname), false);

	type = DecodeSpecial(0, lowzone, &val);

	if (type == TZ || type == DTZ)
	{
		tz = val * MINS_PER_HOUR;
		result = dt2local(time, tz);
	}
	else
	{
		tzp = pg_tzset(tzname);
		if (tzp)
		{
			/* Apply the timezone change */
			struct pg_tm tm;
			fsec_t		fsec;

			if (timestamp2tm(time, &tz, &tm, &fsec, NULL, tzp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			if (tm2timestamp(&tm, fsec, NULL, &result) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not convert to time zone \"%s\"",
								tzname)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));
			result = 0;			/* keep compiler quiet */
		}
	}

	PG_RETURN_TIMESTAMP(result);
}

PG_FUNCTION_INFO_V1(timestampandtz_eq);
Datum timestampandtz_eq(PG_FUNCTION_ARGS)
{
	TimestampAndTz *left = (TimestampAndTz *)PG_GETARG_POINTER(0);
	TimestampAndTz *right = (TimestampAndTz *)PG_GETARG_POINTER(1);
	PG_RETURN_BOOL(left->time == right->time);
}

PG_FUNCTION_INFO_V1(timestampandtz_ne);
Datum timestampandtz_ne(PG_FUNCTION_ARGS)
{
	TimestampAndTz *left = (TimestampAndTz *)PG_GETARG_POINTER(0);
	TimestampAndTz *right = (TimestampAndTz *)PG_GETARG_POINTER(1);
	PG_RETURN_BOOL(left->time != right->time);
}

PG_FUNCTION_INFO_V1(timestampandtz_le);
Datum timestampandtz_le(PG_FUNCTION_ARGS)
{
	TimestampAndTz *left = (TimestampAndTz *)PG_GETARG_POINTER(0);
	TimestampAndTz *right = (TimestampAndTz *)PG_GETARG_POINTER(1);
	PG_RETURN_BOOL(left->time <= right->time);
}

PG_FUNCTION_INFO_V1(timestampandtz_lt);
Datum timestampandtz_lt(PG_FUNCTION_ARGS)
{
	TimestampAndTz *left = (TimestampAndTz *)PG_GETARG_POINTER(0);
	TimestampAndTz *right = (TimestampAndTz *)PG_GETARG_POINTER(1);
	PG_RETURN_BOOL(left->time < right->time);
}

PG_FUNCTION_INFO_V1(timestampandtz_ge);
Datum timestampandtz_ge(PG_FUNCTION_ARGS)
{
	TimestampAndTz *left = (TimestampAndTz *)PG_GETARG_POINTER(0);
	TimestampAndTz *right = (TimestampAndTz *)PG_GETARG_POINTER(1);
	PG_RETURN_BOOL(left->time >= right->time);
}

PG_FUNCTION_INFO_V1(timestampandtz_gt);
Datum timestampandtz_gt(PG_FUNCTION_ARGS)
{
	TimestampAndTz *left = (TimestampAndTz *)PG_GETARG_POINTER(0);
	TimestampAndTz *right = (TimestampAndTz *)PG_GETARG_POINTER(1);
	PG_RETURN_BOOL(left->time < right->time);
}

PG_FUNCTION_INFO_V1(timestampandtz_to_timestamptz);
Datum timestampandtz_to_timestamptz(PG_FUNCTION_ARGS)
{
	TimestampAndTz *dt = (TimestampAndTz *)PG_GETARG_POINTER(0);
	PG_RETURN_TIMESTAMP(dt->time);
}

PG_FUNCTION_INFO_V1(timestampandtz_to_timestamp);
Datum timestampandtz_to_timestamp(PG_FUNCTION_ARGS)
{
	TimestampAndTz *dt = (TimestampAndTz *)PG_GETARG_POINTER(0);
	PG_RETURN_TIMESTAMP(dt->time);
}

PG_FUNCTION_INFO_V1(timestamptz_to_timestampandtz);
Datum timestamptz_to_timestampandtz(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	char *tzn;
	int tzid;

	/* find our timezone id for the current session timezone */
	tzn = pstrdup(pg_get_timezone_name(session_timezone));
	tzid = tzname_to_tzid(tzn);

	if(tzid == 0)
	{
		elog(ERROR, "missing timezone ID \"%s\"", tzn);
		return gen_timestamp(DT_NOEND, 0);
	}

	return gen_timestamp(timestamp, tzid);
}

PG_FUNCTION_INFO_V1(timestamp_to_timestampandtz);
Datum timestamp_to_timestampandtz(PG_FUNCTION_ARGS)
{
	Timestamp timestamp = PG_GETARG_TIMESTAMP(0);
	Timestamp result;
	struct pg_tm tm;
	fsec_t fsec;
	char *tzn;
	int tzid, tz;
	pg_tz * tzp = NULL;

	/* find our timezone id for the current session timezone */
	tzn = pstrdup(pg_get_timezone_name(session_timezone));
	tzid = tzname_to_tzid(tzn);

	if(tzid == 0)
	{
		elog(ERROR, "missing timezone ID \"%s\"", tzn);
		return gen_timestamp(DT_NOEND, 0);
	}


	/* convert from the local timestamp to a local tm struct */
	if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	/* get the local offset for the tm local time */
	tzp = pg_tzset(tzn);
	tz = DetermineTimeZoneOffset(&tm, tzp);

	/* convert from the local timezone to utc timestamp */
	if (tm2timestamp(&tm, fsec, &tz, &result) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not convert to time zone \"%s\"",
						tzn)));

	return gen_timestamp(result, tzid);
}

PG_FUNCTION_INFO_V1(timestampandtz_cmp);
Datum timestampandtz_cmp(PG_FUNCTION_ARGS)
{
	TimestampAndTz * left = (TimestampAndTz *)PG_GETARG_POINTER(0);
	TimestampAndTz * right = (TimestampAndTz *)PG_GETARG_POINTER(1);

	if(left->time > right->time)
		PG_RETURN_INT32(1);
	else if(left->time < right->time)
		PG_RETURN_INT32(-1);
	else
		PG_RETURN_INT32(0);
}

PG_FUNCTION_INFO_V1(timestampandtz_pl_interval);
Datum timestampandtz_pl_interval(PG_FUNCTION_ARGS)
{
	TimestampAndTz *dt = (TimestampAndTz *)PG_GETARG_POINTER(0);
	Interval *span = PG_GETARG_INTERVAL_P(1);
	Timestamp timestamp = dt->time;
	pg_tz * tzp = NULL;
	const char * tzname = NULL;
	int	tz;

	if (TIMESTAMP_NOT_FINITE(timestamp) || dt->tz == 0)
	{
		return gen_timestamp(DT_NOEND, 0);
	}
	else
	{
		tzname = tzid_to_tzname(dt->tz);
		tzp = pg_tzset(tzname);

		if (span->month != 0)
		{
			struct pg_tm tt, *tm = &tt;
			fsec_t fsec;

			if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, tzp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			tm->tm_mon += span->month;
			if (tm->tm_mon > MONTHS_PER_YEAR)
			{
				tm->tm_year += (tm->tm_mon - 1) / MONTHS_PER_YEAR;
				tm->tm_mon = ((tm->tm_mon - 1) % MONTHS_PER_YEAR) + 1;
			}
			else if (tm->tm_mon < 1)
			{
				tm->tm_year += tm->tm_mon / MONTHS_PER_YEAR - 1;
				tm->tm_mon = tm->tm_mon % MONTHS_PER_YEAR + MONTHS_PER_YEAR;
			}

			/* adjust for end of month boundary problems... */
			if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
				tm->tm_mday = (day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]);

			/* determine the new time offset for the new day */
			tz = DetermineTimeZoneOffset(tm, tzp);

			if (tm2timestamp(tm, fsec, &tz, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		if (span->day != 0)
		{
			struct pg_tm tt,
					   *tm = &tt;
			fsec_t		fsec;
			int			julian;

			if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, tzp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			/* Add days by converting to and from julian */
			julian = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) + span->day;
			j2date(julian, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

			/* determine the new time offset for the new day */
			tz = DetermineTimeZoneOffset(tm, tzp);

			if (tm2timestamp(tm, fsec, &tz, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		timestamp += span->time;
	}

	return gen_timestamp(timestamp, dt->tz);
}

PG_FUNCTION_INFO_V1(timestampandtz_mi_interval);
Datum timestampandtz_mi_interval(PG_FUNCTION_ARGS)
{
	TimestampAndTz *dt = (TimestampAndTz *)PG_GETARG_POINTER(0);
	Interval *span = PG_GETARG_INTERVAL_P(1);
	Interval    tspan;

	tspan.month = -span->month;
	tspan.day = -span->day;
	tspan.time = -span->time;

	return DirectFunctionCall2(timestampandtz_pl_interval, gen_timestamp(dt->time, dt->tz), PointerGetDatum(&tspan));
}

PG_FUNCTION_INFO_V1(timestampandtz_movetz);
Datum timestampandtz_movetz(PG_FUNCTION_ARGS)
{
	TimestampAndTz *dt = (TimestampAndTz *)PG_GETARG_POINTER(0);
	text *zone = PG_GETARG_TEXT_PP(1);
	char tzname[TZ_STRLEN_MAX + 1];
	int tzid;

	/* find our timezone id */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));
	tzid = tzname_to_tzid(tzname);

	if(tzid == 0)
	{
		elog(ERROR, "missing timezone ID \"%s\"", tzname);
		return gen_timestamp(DT_NOEND, 0);
	}

	return gen_timestamp(dt->time, tzid);
}

PG_FUNCTION_INFO_V1(timestampandtz_mi);
Datum timestampandtz_mi(PG_FUNCTION_ARGS)
{
	TimestampAndTz *left = (TimestampAndTz *)PG_GETARG_POINTER(0);
	TimestampAndTz *right = (TimestampAndTz *)PG_GETARG_POINTER(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	if (TIMESTAMP_NOT_FINITE(left->time) || TIMESTAMP_NOT_FINITE(right->time) || left->tz == 0 || right->tz == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("cannot subtract infinite timestamps")));

	result->time = left->time - right->time;
	result->month = 0;
	result->day = 0;

	result = DatumGetIntervalP(DirectFunctionCall1(interval_justify_hours, IntervalPGetDatum(result)));
	PG_RETURN_INTERVAL_P(result);
}

PG_FUNCTION_INFO_V1(timestampandtz_trunc);
Datum timestampandtz_trunc(PG_FUNCTION_ARGS)
{
	text *units = PG_GETARG_TEXT_PP(0);
	TimestampAndTz *dt = (TimestampAndTz *)PG_GETARG_POINTER(1);
	TimestampTz result;
	int	tz;
	int	type, val;
	char *lowunits;
	fsec_t fsec;
	pg_tz * tzp = NULL;
	const char * tzname = NULL;
	struct pg_tm tt, *tm = &tt;

	if (TIMESTAMP_NOT_FINITE(dt->time) || dt->tz == 0)
		return gen_timestamp(DT_NOEND, 0);

	tzname = tzid_to_tzname(dt->tz);
	tzp = pg_tzset(tzname);

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (timestamp2tm(dt->time, &tz, tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_WEEK:
				{
					int			woy;

					woy = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);

					/*
					 * If it is week 52/53 and the month is January, then the
					 * week must belong to the previous year. Also, some
					 * December dates belong to the next year.
					 */
					if (woy >= 52 && tm->tm_mon == 1)
						--tm->tm_year;
					if (woy <= 1 && tm->tm_mon == MONTHS_PER_YEAR)
						++tm->tm_year;
					isoweek2date(woy, &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
					tm->tm_hour = 0;
					tm->tm_min = 0;
					tm->tm_sec = 0;
					fsec = 0;
					break;
				}
				/* one may consider DTK_THOUSAND and DTK_HUNDRED... */
			case DTK_MILLENNIUM:

				/*
				 * truncating to the millennium? what is this supposed to
				 * mean? let us put the first year of the millennium... i.e.
				 * -1000, 1, 1001, 2001...
				 */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 999) / 1000) * 1000 - 999;
				else
					tm->tm_year = -((999 - (tm->tm_year - 1)) / 1000) * 1000 + 1;
				/* FALL THRU */
			case DTK_CENTURY:
				/* truncating to the century? as above: -100, 1, 101... */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 99) / 100) * 100 - 99;
				else
					tm->tm_year = -((99 - (tm->tm_year - 1)) / 100) * 100 + 1;
				/* FALL THRU */
			case DTK_DECADE:

				/*
				 * truncating to the decade? first year of the decade. must
				 * not be applied if year was truncated before!
				 */
				if (val != DTK_MILLENNIUM && val != DTK_CENTURY)
				{
					if (tm->tm_year > 0)
						tm->tm_year = (tm->tm_year / 10) * 10;
					else
						tm->tm_year = -((8 - (tm->tm_year - 1)) / 10) * 10;
				}
				/* FALL THRU */
			case DTK_YEAR:
				tm->tm_mon = 1;
				/* FALL THRU */
			case DTK_QUARTER:
				tm->tm_mon = (3 * ((tm->tm_mon - 1) / 3)) + 1;
				/* FALL THRU */
			case DTK_MONTH:
				tm->tm_mday = 1;
				/* FALL THRU */
			case DTK_DAY:
				tm->tm_hour = 0;
				/* FALL THRU */
			case DTK_HOUR:
				tm->tm_min = 0;
				/* FALL THRU */
			case DTK_MINUTE:
				tm->tm_sec = 0;
				/* FALL THRU */
			case DTK_SECOND:
				fsec = 0;
				break;

			case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
				fsec = (fsec / 1000) * 1000;
#else
				fsec = floor(fsec * 1000) / 1000;
#endif
				break;
			case DTK_MICROSEC:
#ifndef HAVE_INT64_TIMESTAMP
				fsec = floor(fsec * 1000000) / 1000000;
#endif
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp with time zone units \"%s\" not "
								"supported", lowunits)));
				result = 0;
		}

		tz = DetermineTimeZoneOffset(tm, tzp);
		if (tm2timestamp(tm, fsec, &tz, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			   errmsg("timestamp with time zone units \"%s\" not recognized",
					  lowunits)));
		result = 0;
	}

	return gen_timestamp(result, dt->tz);
}

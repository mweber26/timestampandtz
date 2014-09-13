#include "postgres.h"
#include "fmgr.h"
#include "access/heapam.h"
#include "miscadmin.h"
#include "funcapi.h"
#include "server/pgtime.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "parser/scansup.h"
#include <string.h>

PG_MODULE_MAGIC;

typedef struct TimestampAndTz {
	Timestamp time;
	int tz;
} TimestampAndTz;

static void
EncodeSpecialTimestamp(Timestamp dt, char *str)
{
    if (TIMESTAMP_IS_NOBEGIN(dt))
        strcpy(str, EARLY);
    else if (TIMESTAMP_IS_NOEND(dt))
        strcpy(str, LATE);
    else    /* shouldn't happen */
        elog(ERROR, "invalid argument for EncodeSpecialTimestamp");
}

static Timestamp
dt2local(Timestamp dt, int tz)
{
#ifdef HAVE_INT64_TIMESTAMP
    dt -= (tz * USECS_PER_SEC);
#else
    dt -= tz;
#endif
    return dt;
}

PG_FUNCTION_INFO_V1(timestampandtz_in);
Datum timestampandtz_in(PG_FUNCTION_ARGS)
{
	char *str = PG_GETARG_CSTRING(0);
	TimestampAndTz *result;
	Timestamp timestamp;
	fsec_t fsec;
	struct pg_tm tt, *tm = &tt;
	int tz, dtype, nf, dterr;
	char *field[MAXDATEFIELDS];
	int  ftype[MAXDATEFIELDS];
	char workbuf[MAXDATELEN + MAXDATEFIELDS];

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf), field, ftype, MAXDATEFIELDS, &nf);
	if(dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz);
	if(dterr != 0)
		DateTimeParseError(dterr, str, "timestamp and time zone");

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
			elog(ERROR, "unexpected dtype %d while parsing timestamptz \"%s\"",
				dtype, str);
			TIMESTAMP_NOEND(timestamp);
	}

	result = (TimestampAndTz *) palloc(sizeof(TimestampAndTz));
	result->time = timestamp;
	result->tz = tz;
	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(timestampandtz_out);
Datum timestampandtz_out(PG_FUNCTION_ARGS)
{
	TimestampAndTz *dt = (TimestampAndTz *)PG_GETARG_POINTER(0);
	char       *result;
	struct pg_tm tt, *tm = &tt;
	int tz;
	fsec_t      fsec;
	char        buf[MAXDATELEN + 1];
	Timestamp time = dt->time - dt->tz * USECS_PER_SEC;

	if(TIMESTAMP_NOT_FINITE(time))
		EncodeSpecialTimestamp(time, buf);
	else if(timestamp2tm(time, NULL, tm, &fsec, NULL, NULL) == 0)
	{
		tm->tm_isdst = 0;
		EncodeDateTime(tm, fsec, true, dt->tz, NULL, DateStyle, buf);
	}
	else
		ereport(ERROR,
			(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			errmsg("timestamp out of range")));

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

PG_FUNCTION_INFO_V1(timestampandtz_recv);
Datum timestampandtz_recv(PG_FUNCTION_ARGS)
{
	StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
	TimestampAndTz *result;

	result = (TimestampAndTz *) palloc(sizeof(TimestampAndTz));
	result->time = pq_getmsgint64(buf);
	result->tz = pq_getmsgint(buf, 4);
	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(timestampandtz_send);
Datum timestampandtz_send(PG_FUNCTION_ARGS)
{
	TimestampAndTz *arg = (TimestampAndTz *)PG_GETARG_POINTER(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, arg->time);
	pq_sendint(&buf, arg->tz, 4);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
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

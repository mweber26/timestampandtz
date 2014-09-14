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
#include "access/xact.h"
#include <string.h>

PG_MODULE_MAGIC;

typedef struct TimestampAndTz {
	Timestamp time;
	short tz;
} TimestampAndTz;

static const char * tzid_to_tzname(int id);
static int tzname_to_tzid(const char *name);

#include "to_char.c"

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
	TimestampAndTz *result;

	/* input the timestamp and the timezone id */
	result = (TimestampAndTz *) palloc0(sizeof(TimestampAndTz));
	result->time = pq_getmsgint64(buf);
	result->tz = pq_getmsgint(buf, 2);
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

struct timezone_to_id {
	const char *name;
	const char *nameupper;
	int id;
};

static struct timezone_to_id timezones[] = {
	{ "Africa/Abidjan", "AFRICA/ABIDJAN", 1 },
	{ "Africa/Accra", "AFRICA/ACCRA", 2 },
	{ "Africa/Addis_Ababa", "AFRICA/ADDIS_ABABA", 3 },
	{ "Africa/Algiers", "AFRICA/ALGIERS", 4 },
	{ "Africa/Asmara", "AFRICA/ASMARA", 5 },
	{ "Africa/Asmera", "AFRICA/ASMERA", 6 },
	{ "Africa/Bamako", "AFRICA/BAMAKO", 7 },
	{ "Africa/Bangui", "AFRICA/BANGUI", 8 },
	{ "Africa/Banjul", "AFRICA/BANJUL", 9 },
	{ "Africa/Bissau", "AFRICA/BISSAU", 10 },
	{ "Africa/Blantyre", "AFRICA/BLANTYRE", 11 },
	{ "Africa/Brazzaville", "AFRICA/BRAZZAVILLE", 12 },
	{ "Africa/Bujumbura", "AFRICA/BUJUMBURA", 13 },
	{ "Africa/Cairo", "AFRICA/CAIRO", 14 },
	{ "Africa/Casablanca", "AFRICA/CASABLANCA", 15 },
	{ "Africa/Ceuta", "AFRICA/CEUTA", 16 },
	{ "Africa/Conakry", "AFRICA/CONAKRY", 17 },
	{ "Africa/Dakar", "AFRICA/DAKAR", 18 },
	{ "Africa/Dar_es_Salaam", "AFRICA/DAR_ES_SALAAM", 19 },
	{ "Africa/Djibouti", "AFRICA/DJIBOUTI", 20 },
	{ "Africa/Douala", "AFRICA/DOUALA", 21 },
	{ "Africa/El_Aaiun", "AFRICA/EL_AAIUN", 22 },
	{ "Africa/Freetown", "AFRICA/FREETOWN", 23 },
	{ "Africa/Gaborone", "AFRICA/GABORONE", 24 },
	{ "Africa/Harare", "AFRICA/HARARE", 25 },
	{ "Africa/Johannesburg", "AFRICA/JOHANNESBURG", 26 },
	{ "Africa/Juba", "AFRICA/JUBA", 27 },
	{ "Africa/Kampala", "AFRICA/KAMPALA", 28 },
	{ "Africa/Khartoum", "AFRICA/KHARTOUM", 29 },
	{ "Africa/Kigali", "AFRICA/KIGALI", 30 },
	{ "Africa/Kinshasa", "AFRICA/KINSHASA", 31 },
	{ "Africa/Lagos", "AFRICA/LAGOS", 32 },
	{ "Africa/Libreville", "AFRICA/LIBREVILLE", 33 },
	{ "Africa/Lome", "AFRICA/LOME", 34 },
	{ "Africa/Luanda", "AFRICA/LUANDA", 35 },
	{ "Africa/Lubumbashi", "AFRICA/LUBUMBASHI", 36 },
	{ "Africa/Lusaka", "AFRICA/LUSAKA", 37 },
	{ "Africa/Malabo", "AFRICA/MALABO", 38 },
	{ "Africa/Maputo", "AFRICA/MAPUTO", 39 },
	{ "Africa/Maseru", "AFRICA/MASERU", 40 },
	{ "Africa/Mbabane", "AFRICA/MBABANE", 41 },
	{ "Africa/Mogadishu", "AFRICA/MOGADISHU", 42 },
	{ "Africa/Monrovia", "AFRICA/MONROVIA", 43 },
	{ "Africa/Nairobi", "AFRICA/NAIROBI", 44 },
	{ "Africa/Ndjamena", "AFRICA/NDJAMENA", 45 },
	{ "Africa/Niamey", "AFRICA/NIAMEY", 46 },
	{ "Africa/Nouakchott", "AFRICA/NOUAKCHOTT", 47 },
	{ "Africa/Ouagadougou", "AFRICA/OUAGADOUGOU", 48 },
	{ "Africa/Porto-Novo", "AFRICA/PORTO-NOVO", 49 },
	{ "Africa/Sao_Tome", "AFRICA/SAO_TOME", 50 },
	{ "Africa/Timbuktu", "AFRICA/TIMBUKTU", 51 },
	{ "Africa/Tripoli", "AFRICA/TRIPOLI", 52 },
	{ "Africa/Tunis", "AFRICA/TUNIS", 53 },
	{ "Africa/Windhoek", "AFRICA/WINDHOEK", 54 },
	{ "America/Adak", "AMERICA/ADAK", 55 },
	{ "America/Anchorage", "AMERICA/ANCHORAGE", 56 },
	{ "America/Anguilla", "AMERICA/ANGUILLA", 57 },
	{ "America/Antigua", "AMERICA/ANTIGUA", 58 },
	{ "America/Araguaina", "AMERICA/ARAGUAINA", 59 },
	{ "America/Argentina/Buenos_Aires", "AMERICA/ARGENTINA/BUENOS_AIRES", 60 },
	{ "America/Argentina/Catamarca", "AMERICA/ARGENTINA/CATAMARCA", 61 },
	{ "America/Argentina/ComodRivadavia", "AMERICA/ARGENTINA/COMODRIVADAVIA", 62 },
	{ "America/Argentina/Cordoba", "AMERICA/ARGENTINA/CORDOBA", 63 },
	{ "America/Argentina/Jujuy", "AMERICA/ARGENTINA/JUJUY", 64 },
	{ "America/Argentina/La_Rioja", "AMERICA/ARGENTINA/LA_RIOJA", 65 },
	{ "America/Argentina/Mendoza", "AMERICA/ARGENTINA/MENDOZA", 66 },
	{ "America/Argentina/Rio_Gallegos", "AMERICA/ARGENTINA/RIO_GALLEGOS", 67 },
	{ "America/Argentina/Salta", "AMERICA/ARGENTINA/SALTA", 68 },
	{ "America/Argentina/San_Juan", "AMERICA/ARGENTINA/SAN_JUAN", 69 },
	{ "America/Argentina/San_Luis", "AMERICA/ARGENTINA/SAN_LUIS", 70 },
	{ "America/Argentina/Tucuman", "AMERICA/ARGENTINA/TUCUMAN", 71 },
	{ "America/Argentina/Ushuaia", "AMERICA/ARGENTINA/USHUAIA", 72 },
	{ "America/Aruba", "AMERICA/ARUBA", 73 },
	{ "America/Asuncion", "AMERICA/ASUNCION", 74 },
	{ "America/Atikokan", "AMERICA/ATIKOKAN", 75 },
	{ "America/Atka", "AMERICA/ATKA", 76 },
	{ "America/Bahia", "AMERICA/BAHIA", 77 },
	{ "America/Bahia_Banderas", "AMERICA/BAHIA_BANDERAS", 78 },
	{ "America/Barbados", "AMERICA/BARBADOS", 79 },
	{ "America/Belem", "AMERICA/BELEM", 80 },
	{ "America/Belize", "AMERICA/BELIZE", 81 },
	{ "America/Blanc-Sablon", "AMERICA/BLANC-SABLON", 82 },
	{ "America/Boa_Vista", "AMERICA/BOA_VISTA", 83 },
	{ "America/Bogota", "AMERICA/BOGOTA", 84 },
	{ "America/Boise", "AMERICA/BOISE", 85 },
	{ "America/Buenos_Aires", "AMERICA/BUENOS_AIRES", 86 },
	{ "America/Cambridge_Bay", "AMERICA/CAMBRIDGE_BAY", 87 },
	{ "America/Campo_Grande", "AMERICA/CAMPO_GRANDE", 88 },
	{ "America/Cancun", "AMERICA/CANCUN", 89 },
	{ "America/Caracas", "AMERICA/CARACAS", 90 },
	{ "America/Catamarca", "AMERICA/CATAMARCA", 91 },
	{ "America/Cayenne", "AMERICA/CAYENNE", 92 },
	{ "America/Cayman", "AMERICA/CAYMAN", 93 },
	{ "America/Chicago", "AMERICA/CHICAGO", 94 },
	{ "America/Chihuahua", "AMERICA/CHIHUAHUA", 95 },
	{ "America/Coral_Harbour", "AMERICA/CORAL_HARBOUR", 96 },
	{ "America/Cordoba", "AMERICA/CORDOBA", 97 },
	{ "America/Costa_Rica", "AMERICA/COSTA_RICA", 98 },
	{ "America/Creston", "AMERICA/CRESTON", 99 },
	{ "America/Cuiaba", "AMERICA/CUIABA", 100 },
	{ "America/Curacao", "AMERICA/CURACAO", 101 },
	{ "America/Danmarkshavn", "AMERICA/DANMARKSHAVN", 102 },
	{ "America/Dawson", "AMERICA/DAWSON", 103 },
	{ "America/Dawson_Creek", "AMERICA/DAWSON_CREEK", 104 },
	{ "America/Denver", "AMERICA/DENVER", 105 },
	{ "America/Detroit", "AMERICA/DETROIT", 106 },
	{ "America/Dominica", "AMERICA/DOMINICA", 107 },
	{ "America/Edmonton", "AMERICA/EDMONTON", 108 },
	{ "America/Eirunepe", "AMERICA/EIRUNEPE", 109 },
	{ "America/El_Salvador", "AMERICA/EL_SALVADOR", 110 },
	{ "America/Ensenada", "AMERICA/ENSENADA", 111 },
	{ "America/Fort_Wayne", "AMERICA/FORT_WAYNE", 112 },
	{ "America/Fortaleza", "AMERICA/FORTALEZA", 113 },
	{ "America/Glace_Bay", "AMERICA/GLACE_BAY", 114 },
	{ "America/Godthab", "AMERICA/GODTHAB", 115 },
	{ "America/Goose_Bay", "AMERICA/GOOSE_BAY", 116 },
	{ "America/Grand_Turk", "AMERICA/GRAND_TURK", 117 },
	{ "America/Grenada", "AMERICA/GRENADA", 118 },
	{ "America/Guadeloupe", "AMERICA/GUADELOUPE", 119 },
	{ "America/Guatemala", "AMERICA/GUATEMALA", 120 },
	{ "America/Guayaquil", "AMERICA/GUAYAQUIL", 121 },
	{ "America/Guyana", "AMERICA/GUYANA", 122 },
	{ "America/Halifax", "AMERICA/HALIFAX", 123 },
	{ "America/Havana", "AMERICA/HAVANA", 124 },
	{ "America/Hermosillo", "AMERICA/HERMOSILLO", 125 },
	{ "America/Indiana/Indianapolis", "AMERICA/INDIANA/INDIANAPOLIS", 126 },
	{ "America/Indiana/Knox", "AMERICA/INDIANA/KNOX", 127 },
	{ "America/Indiana/Marengo", "AMERICA/INDIANA/MARENGO", 128 },
	{ "America/Indiana/Petersburg", "AMERICA/INDIANA/PETERSBURG", 129 },
	{ "America/Indiana/Tell_City", "AMERICA/INDIANA/TELL_CITY", 130 },
	{ "America/Indiana/Vevay", "AMERICA/INDIANA/VEVAY", 131 },
	{ "America/Indiana/Vincennes", "AMERICA/INDIANA/VINCENNES", 132 },
	{ "America/Indiana/Winamac", "AMERICA/INDIANA/WINAMAC", 133 },
	{ "America/Indianapolis", "AMERICA/INDIANAPOLIS", 134 },
	{ "America/Inuvik", "AMERICA/INUVIK", 135 },
	{ "America/Iqaluit", "AMERICA/IQALUIT", 136 },
	{ "America/Jamaica", "AMERICA/JAMAICA", 137 },
	{ "America/Jujuy", "AMERICA/JUJUY", 138 },
	{ "America/Juneau", "AMERICA/JUNEAU", 139 },
	{ "America/Kentucky/Louisville", "AMERICA/KENTUCKY/LOUISVILLE", 140 },
	{ "America/Kentucky/Monticello", "AMERICA/KENTUCKY/MONTICELLO", 141 },
	{ "America/Knox_IN", "AMERICA/KNOX_IN", 142 },
	{ "America/Kralendijk", "AMERICA/KRALENDIJK", 143 },
	{ "America/La_Paz", "AMERICA/LA_PAZ", 144 },
	{ "America/Lima", "AMERICA/LIMA", 145 },
	{ "America/Los_Angeles", "AMERICA/LOS_ANGELES", 146 },
	{ "America/Louisville", "AMERICA/LOUISVILLE", 147 },
	{ "America/Lower_Princes", "AMERICA/LOWER_PRINCES", 148 },
	{ "America/Maceio", "AMERICA/MACEIO", 149 },
	{ "America/Managua", "AMERICA/MANAGUA", 150 },
	{ "America/Manaus", "AMERICA/MANAUS", 151 },
	{ "America/Marigot", "AMERICA/MARIGOT", 152 },
	{ "America/Martinique", "AMERICA/MARTINIQUE", 153 },
	{ "America/Matamoros", "AMERICA/MATAMOROS", 154 },
	{ "America/Mazatlan", "AMERICA/MAZATLAN", 155 },
	{ "America/Mendoza", "AMERICA/MENDOZA", 156 },
	{ "America/Menominee", "AMERICA/MENOMINEE", 157 },
	{ "America/Merida", "AMERICA/MERIDA", 158 },
	{ "America/Metlakatla", "AMERICA/METLAKATLA", 159 },
	{ "America/Mexico_City", "AMERICA/MEXICO_CITY", 160 },
	{ "America/Miquelon", "AMERICA/MIQUELON", 161 },
	{ "America/Moncton", "AMERICA/MONCTON", 162 },
	{ "America/Monterrey", "AMERICA/MONTERREY", 163 },
	{ "America/Montevideo", "AMERICA/MONTEVIDEO", 164 },
	{ "America/Montreal", "AMERICA/MONTREAL", 165 },
	{ "America/Montserrat", "AMERICA/MONTSERRAT", 166 },
	{ "America/Nassau", "AMERICA/NASSAU", 167 },
	{ "America/New_York", "AMERICA/NEW_YORK", 168 },
	{ "America/Nipigon", "AMERICA/NIPIGON", 169 },
	{ "America/Nome", "AMERICA/NOME", 170 },
	{ "America/Noronha", "AMERICA/NORONHA", 171 },
	{ "America/North_Dakota/Beulah", "AMERICA/NORTH_DAKOTA/BEULAH", 172 },
	{ "America/North_Dakota/Center", "AMERICA/NORTH_DAKOTA/CENTER", 173 },
	{ "America/North_Dakota/New_Salem", "AMERICA/NORTH_DAKOTA/NEW_SALEM", 174 },
	{ "America/Ojinaga", "AMERICA/OJINAGA", 175 },
	{ "America/Panama", "AMERICA/PANAMA", 176 },
	{ "America/Pangnirtung", "AMERICA/PANGNIRTUNG", 177 },
	{ "America/Paramaribo", "AMERICA/PARAMARIBO", 178 },
	{ "America/Phoenix", "AMERICA/PHOENIX", 179 },
	{ "America/Port-au-Prince", "AMERICA/PORT-AU-PRINCE", 180 },
	{ "America/Port_of_Spain", "AMERICA/PORT_OF_SPAIN", 181 },
	{ "America/Porto_Acre", "AMERICA/PORTO_ACRE", 182 },
	{ "America/Porto_Velho", "AMERICA/PORTO_VELHO", 183 },
	{ "America/Puerto_Rico", "AMERICA/PUERTO_RICO", 184 },
	{ "America/Rainy_River", "AMERICA/RAINY_RIVER", 185 },
	{ "America/Rankin_Inlet", "AMERICA/RANKIN_INLET", 186 },
	{ "America/Recife", "AMERICA/RECIFE", 187 },
	{ "America/Regina", "AMERICA/REGINA", 188 },
	{ "America/Resolute", "AMERICA/RESOLUTE", 189 },
	{ "America/Rio_Branco", "AMERICA/RIO_BRANCO", 190 },
	{ "America/Rosario", "AMERICA/ROSARIO", 191 },
	{ "America/Santa_Isabel", "AMERICA/SANTA_ISABEL", 192 },
	{ "America/Santarem", "AMERICA/SANTAREM", 193 },
	{ "America/Santiago", "AMERICA/SANTIAGO", 194 },
	{ "America/Santo_Domingo", "AMERICA/SANTO_DOMINGO", 195 },
	{ "America/Sao_Paulo", "AMERICA/SAO_PAULO", 196 },
	{ "America/Scoresbysund", "AMERICA/SCORESBYSUND", 197 },
	{ "America/Shiprock", "AMERICA/SHIPROCK", 198 },
	{ "America/Sitka", "AMERICA/SITKA", 199 },
	{ "America/St_Barthelemy", "AMERICA/ST_BARTHELEMY", 200 },
	{ "America/St_Johns", "AMERICA/ST_JOHNS", 201 },
	{ "America/St_Kitts", "AMERICA/ST_KITTS", 202 },
	{ "America/St_Lucia", "AMERICA/ST_LUCIA", 203 },
	{ "America/St_Thomas", "AMERICA/ST_THOMAS", 204 },
	{ "America/St_Vincent", "AMERICA/ST_VINCENT", 205 },
	{ "America/Swift_Current", "AMERICA/SWIFT_CURRENT", 206 },
	{ "America/Tegucigalpa", "AMERICA/TEGUCIGALPA", 207 },
	{ "America/Thule", "AMERICA/THULE", 208 },
	{ "America/Thunder_Bay", "AMERICA/THUNDER_BAY", 209 },
	{ "America/Tijuana", "AMERICA/TIJUANA", 210 },
	{ "America/Toronto", "AMERICA/TORONTO", 211 },
	{ "America/Tortola", "AMERICA/TORTOLA", 212 },
	{ "America/Vancouver", "AMERICA/VANCOUVER", 213 },
	{ "America/Virgin", "AMERICA/VIRGIN", 214 },
	{ "America/Whitehorse", "AMERICA/WHITEHORSE", 215 },
	{ "America/Winnipeg", "AMERICA/WINNIPEG", 216 },
	{ "America/Yakutat", "AMERICA/YAKUTAT", 217 },
	{ "America/Yellowknife", "AMERICA/YELLOWKNIFE", 218 },
	{ "Antarctica/Casey", "ANTARCTICA/CASEY", 219 },
	{ "Antarctica/Davis", "ANTARCTICA/DAVIS", 220 },
	{ "Antarctica/DumontDUrville", "ANTARCTICA/DUMONTDURVILLE", 221 },
	{ "Antarctica/Macquarie", "ANTARCTICA/MACQUARIE", 222 },
	{ "Antarctica/Mawson", "ANTARCTICA/MAWSON", 223 },
	{ "Antarctica/McMurdo", "ANTARCTICA/MCMURDO", 224 },
	{ "Antarctica/Palmer", "ANTARCTICA/PALMER", 225 },
	{ "Antarctica/Rothera", "ANTARCTICA/ROTHERA", 226 },
	{ "Antarctica/South_Pole", "ANTARCTICA/SOUTH_POLE", 227 },
	{ "Antarctica/Syowa", "ANTARCTICA/SYOWA", 228 },
	{ "Antarctica/Troll", "ANTARCTICA/TROLL", 229 },
	{ "Antarctica/Vostok", "ANTARCTICA/VOSTOK", 230 },
	{ "Arctic/Longyearbyen", "ARCTIC/LONGYEARBYEN", 231 },
	{ "Asia/Aden", "ASIA/ADEN", 232 },
	{ "Asia/Almaty", "ASIA/ALMATY", 233 },
	{ "Asia/Amman", "ASIA/AMMAN", 234 },
	{ "Asia/Anadyr", "ASIA/ANADYR", 235 },
	{ "Asia/Aqtau", "ASIA/AQTAU", 236 },
	{ "Asia/Aqtobe", "ASIA/AQTOBE", 237 },
	{ "Asia/Ashgabat", "ASIA/ASHGABAT", 238 },
	{ "Asia/Ashkhabad", "ASIA/ASHKHABAD", 239 },
	{ "Asia/Baghdad", "ASIA/BAGHDAD", 240 },
	{ "Asia/Bahrain", "ASIA/BAHRAIN", 241 },
	{ "Asia/Baku", "ASIA/BAKU", 242 },
	{ "Asia/Bangkok", "ASIA/BANGKOK", 243 },
	{ "Asia/Beirut", "ASIA/BEIRUT", 244 },
	{ "Asia/Bishkek", "ASIA/BISHKEK", 245 },
	{ "Asia/Brunei", "ASIA/BRUNEI", 246 },
	{ "Asia/Calcutta", "ASIA/CALCUTTA", 247 },
	{ "Asia/Choibalsan", "ASIA/CHOIBALSAN", 248 },
	{ "Asia/Chongqing", "ASIA/CHONGQING", 249 },
	{ "Asia/Chungking", "ASIA/CHUNGKING", 250 },
	{ "Asia/Colombo", "ASIA/COLOMBO", 251 },
	{ "Asia/Dacca", "ASIA/DACCA", 252 },
	{ "Asia/Damascus", "ASIA/DAMASCUS", 253 },
	{ "Asia/Dhaka", "ASIA/DHAKA", 254 },
	{ "Asia/Dili", "ASIA/DILI", 255 },
	{ "Asia/Dubai", "ASIA/DUBAI", 256 },
	{ "Asia/Dushanbe", "ASIA/DUSHANBE", 257 },
	{ "Asia/Gaza", "ASIA/GAZA", 258 },
	{ "Asia/Harbin", "ASIA/HARBIN", 259 },
	{ "Asia/Hebron", "ASIA/HEBRON", 260 },
	{ "Asia/Ho_Chi_Minh", "ASIA/HO_CHI_MINH", 261 },
	{ "Asia/Hong_Kong", "ASIA/HONG_KONG", 262 },
	{ "Asia/Hovd", "ASIA/HOVD", 263 },
	{ "Asia/Irkutsk", "ASIA/IRKUTSK", 264 },
	{ "Asia/Istanbul", "ASIA/ISTANBUL", 265 },
	{ "Asia/Jakarta", "ASIA/JAKARTA", 266 },
	{ "Asia/Jayapura", "ASIA/JAYAPURA", 267 },
	{ "Asia/Jerusalem", "ASIA/JERUSALEM", 268 },
	{ "Asia/Kabul", "ASIA/KABUL", 269 },
	{ "Asia/Kamchatka", "ASIA/KAMCHATKA", 270 },
	{ "Asia/Karachi", "ASIA/KARACHI", 271 },
	{ "Asia/Kashgar", "ASIA/KASHGAR", 272 },
	{ "Asia/Kathmandu", "ASIA/KATHMANDU", 273 },
	{ "Asia/Katmandu", "ASIA/KATMANDU", 274 },
	{ "Asia/Khandyga", "ASIA/KHANDYGA", 275 },
	{ "Asia/Kolkata", "ASIA/KOLKATA", 276 },
	{ "Asia/Krasnoyarsk", "ASIA/KRASNOYARSK", 277 },
	{ "Asia/Kuala_Lumpur", "ASIA/KUALA_LUMPUR", 278 },
	{ "Asia/Kuching", "ASIA/KUCHING", 279 },
	{ "Asia/Kuwait", "ASIA/KUWAIT", 280 },
	{ "Asia/Macao", "ASIA/MACAO", 281 },
	{ "Asia/Macau", "ASIA/MACAU", 282 },
	{ "Asia/Magadan", "ASIA/MAGADAN", 283 },
	{ "Asia/Makassar", "ASIA/MAKASSAR", 284 },
	{ "Asia/Manila", "ASIA/MANILA", 285 },
	{ "Asia/Muscat", "ASIA/MUSCAT", 286 },
	{ "Asia/Nicosia", "ASIA/NICOSIA", 287 },
	{ "Asia/Novokuznetsk", "ASIA/NOVOKUZNETSK", 288 },
	{ "Asia/Novosibirsk", "ASIA/NOVOSIBIRSK", 289 },
	{ "Asia/Omsk", "ASIA/OMSK", 290 },
	{ "Asia/Oral", "ASIA/ORAL", 291 },
	{ "Asia/Phnom_Penh", "ASIA/PHNOM_PENH", 292 },
	{ "Asia/Pontianak", "ASIA/PONTIANAK", 293 },
	{ "Asia/Pyongyang", "ASIA/PYONGYANG", 294 },
	{ "Asia/Qatar", "ASIA/QATAR", 295 },
	{ "Asia/Qyzylorda", "ASIA/QYZYLORDA", 296 },
	{ "Asia/Rangoon", "ASIA/RANGOON", 297 },
	{ "Asia/Riyadh", "ASIA/RIYADH", 298 },
	{ "Asia/Saigon", "ASIA/SAIGON", 299 },
	{ "Asia/Sakhalin", "ASIA/SAKHALIN", 300 },
	{ "Asia/Samarkand", "ASIA/SAMARKAND", 301 },
	{ "Asia/Seoul", "ASIA/SEOUL", 302 },
	{ "Asia/Shanghai", "ASIA/SHANGHAI", 303 },
	{ "Asia/Singapore", "ASIA/SINGAPORE", 304 },
	{ "Asia/Taipei", "ASIA/TAIPEI", 305 },
	{ "Asia/Tashkent", "ASIA/TASHKENT", 306 },
	{ "Asia/Tbilisi", "ASIA/TBILISI", 307 },
	{ "Asia/Tehran", "ASIA/TEHRAN", 308 },
	{ "Asia/Tel_Aviv", "ASIA/TEL_AVIV", 309 },
	{ "Asia/Thimbu", "ASIA/THIMBU", 310 },
	{ "Asia/Thimphu", "ASIA/THIMPHU", 311 },
	{ "Asia/Tokyo", "ASIA/TOKYO", 312 },
	{ "Asia/Ujung_Pandang", "ASIA/UJUNG_PANDANG", 313 },
	{ "Asia/Ulaanbaatar", "ASIA/ULAANBAATAR", 314 },
	{ "Asia/Ulan_Bator", "ASIA/ULAN_BATOR", 315 },
	{ "Asia/Urumqi", "ASIA/URUMQI", 316 },
	{ "Asia/Ust-Nera", "ASIA/UST-NERA", 317 },
	{ "Asia/Vientiane", "ASIA/VIENTIANE", 318 },
	{ "Asia/Vladivostok", "ASIA/VLADIVOSTOK", 319 },
	{ "Asia/Yakutsk", "ASIA/YAKUTSK", 320 },
	{ "Asia/Yekaterinburg", "ASIA/YEKATERINBURG", 321 },
	{ "Asia/Yerevan", "ASIA/YEREVAN", 322 },
	{ "Atlantic/Azores", "ATLANTIC/AZORES", 323 },
	{ "Atlantic/Bermuda", "ATLANTIC/BERMUDA", 324 },
	{ "Atlantic/Canary", "ATLANTIC/CANARY", 325 },
	{ "Atlantic/Cape_Verde", "ATLANTIC/CAPE_VERDE", 326 },
	{ "Atlantic/Faeroe", "ATLANTIC/FAEROE", 327 },
	{ "Atlantic/Faroe", "ATLANTIC/FAROE", 328 },
	{ "Atlantic/Jan_Mayen", "ATLANTIC/JAN_MAYEN", 329 },
	{ "Atlantic/Madeira", "ATLANTIC/MADEIRA", 330 },
	{ "Atlantic/Reykjavik", "ATLANTIC/REYKJAVIK", 331 },
	{ "Atlantic/South_Georgia", "ATLANTIC/SOUTH_GEORGIA", 332 },
	{ "Atlantic/St_Helena", "ATLANTIC/ST_HELENA", 333 },
	{ "Atlantic/Stanley", "ATLANTIC/STANLEY", 334 },
	{ "Australia/ACT", "AUSTRALIA/ACT", 335 },
	{ "Australia/Adelaide", "AUSTRALIA/ADELAIDE", 336 },
	{ "Australia/Brisbane", "AUSTRALIA/BRISBANE", 337 },
	{ "Australia/Broken_Hill", "AUSTRALIA/BROKEN_HILL", 338 },
	{ "Australia/Canberra", "AUSTRALIA/CANBERRA", 339 },
	{ "Australia/Currie", "AUSTRALIA/CURRIE", 340 },
	{ "Australia/Darwin", "AUSTRALIA/DARWIN", 341 },
	{ "Australia/Eucla", "AUSTRALIA/EUCLA", 342 },
	{ "Australia/Hobart", "AUSTRALIA/HOBART", 343 },
	{ "Australia/LHI", "AUSTRALIA/LHI", 344 },
	{ "Australia/Lindeman", "AUSTRALIA/LINDEMAN", 345 },
	{ "Australia/Lord_Howe", "AUSTRALIA/LORD_HOWE", 346 },
	{ "Australia/Melbourne", "AUSTRALIA/MELBOURNE", 347 },
	{ "Australia/NSW", "AUSTRALIA/NSW", 348 },
	{ "Australia/North", "AUSTRALIA/NORTH", 349 },
	{ "Australia/Perth", "AUSTRALIA/PERTH", 350 },
	{ "Australia/Queensland", "AUSTRALIA/QUEENSLAND", 351 },
	{ "Australia/South", "AUSTRALIA/SOUTH", 352 },
	{ "Australia/Sydney", "AUSTRALIA/SYDNEY", 353 },
	{ "Australia/Tasmania", "AUSTRALIA/TASMANIA", 354 },
	{ "Australia/Victoria", "AUSTRALIA/VICTORIA", 355 },
	{ "Australia/West", "AUSTRALIA/WEST", 356 },
	{ "Australia/Yancowinna", "AUSTRALIA/YANCOWINNA", 357 },
	{ "Brazil/Acre", "BRAZIL/ACRE", 358 },
	{ "Brazil/DeNoronha", "BRAZIL/DENORONHA", 359 },
	{ "Brazil/East", "BRAZIL/EAST", 360 },
	{ "Brazil/West", "BRAZIL/WEST", 361 },
	{ "CET", "CET", 362 },
	{ "CST6CDT", "CST6CDT", 363 },
	{ "Canada/Atlantic", "CANADA/ATLANTIC", 364 },
	{ "Canada/Central", "CANADA/CENTRAL", 365 },
	{ "Canada/East-Saskatchewan", "CANADA/EAST-SASKATCHEWAN", 366 },
	{ "Canada/Eastern", "CANADA/EASTERN", 367 },
	{ "Canada/Mountain", "CANADA/MOUNTAIN", 368 },
	{ "Canada/Newfoundland", "CANADA/NEWFOUNDLAND", 369 },
	{ "Canada/Pacific", "CANADA/PACIFIC", 370 },
	{ "Canada/Saskatchewan", "CANADA/SASKATCHEWAN", 371 },
	{ "Canada/Yukon", "CANADA/YUKON", 372 },
	{ "Chile/Continental", "CHILE/CONTINENTAL", 373 },
	{ "Chile/EasterIsland", "CHILE/EASTERISLAND", 374 },
	{ "Cuba", "CUBA", 375 },
	{ "EET", "EET", 376 },
	{ "EST", "EST", 377 },
	{ "EST5EDT", "EST5EDT", 378 },
	{ "Egypt", "EGYPT", 379 },
	{ "Eire", "EIRE", 380 },
	{ "Etc/GMT", "ETC/GMT", 381 },
	{ "Etc/GMT+0", "ETC/GMT+0", 382 },
	{ "Etc/GMT+1", "ETC/GMT+1", 383 },
	{ "Etc/GMT+10", "ETC/GMT+10", 384 },
	{ "Etc/GMT+11", "ETC/GMT+11", 385 },
	{ "Etc/GMT+12", "ETC/GMT+12", 386 },
	{ "Etc/GMT+2", "ETC/GMT+2", 387 },
	{ "Etc/GMT+3", "ETC/GMT+3", 388 },
	{ "Etc/GMT+4", "ETC/GMT+4", 389 },
	{ "Etc/GMT+5", "ETC/GMT+5", 390 },
	{ "Etc/GMT+6", "ETC/GMT+6", 391 },
	{ "Etc/GMT+7", "ETC/GMT+7", 392 },
	{ "Etc/GMT+8", "ETC/GMT+8", 393 },
	{ "Etc/GMT+9", "ETC/GMT+9", 394 },
	{ "Etc/GMT-0", "ETC/GMT-0", 395 },
	{ "Etc/GMT-1", "ETC/GMT-1", 396 },
	{ "Etc/GMT-10", "ETC/GMT-10", 397 },
	{ "Etc/GMT-11", "ETC/GMT-11", 398 },
	{ "Etc/GMT-12", "ETC/GMT-12", 399 },
	{ "Etc/GMT-13", "ETC/GMT-13", 400 },
	{ "Etc/GMT-14", "ETC/GMT-14", 401 },
	{ "Etc/GMT-2", "ETC/GMT-2", 402 },
	{ "Etc/GMT-3", "ETC/GMT-3", 403 },
	{ "Etc/GMT-4", "ETC/GMT-4", 404 },
	{ "Etc/GMT-5", "ETC/GMT-5", 405 },
	{ "Etc/GMT-6", "ETC/GMT-6", 406 },
	{ "Etc/GMT-7", "ETC/GMT-7", 407 },
	{ "Etc/GMT-8", "ETC/GMT-8", 408 },
	{ "Etc/GMT-9", "ETC/GMT-9", 409 },
	{ "Etc/GMT0", "ETC/GMT0", 410 },
	{ "Etc/Greenwich", "ETC/GREENWICH", 411 },
	{ "Etc/UCT", "ETC/UCT", 412 },
	{ "Etc/UTC", "ETC/UTC", 413 },
	{ "Etc/Universal", "ETC/UNIVERSAL", 414 },
	{ "Etc/Zulu", "ETC/ZULU", 415 },
	{ "Europe/Amsterdam", "EUROPE/AMSTERDAM", 416 },
	{ "Europe/Andorra", "EUROPE/ANDORRA", 417 },
	{ "Europe/Athens", "EUROPE/ATHENS", 418 },
	{ "Europe/Belfast", "EUROPE/BELFAST", 419 },
	{ "Europe/Belgrade", "EUROPE/BELGRADE", 420 },
	{ "Europe/Berlin", "EUROPE/BERLIN", 421 },
	{ "Europe/Bratislava", "EUROPE/BRATISLAVA", 422 },
	{ "Europe/Brussels", "EUROPE/BRUSSELS", 423 },
	{ "Europe/Bucharest", "EUROPE/BUCHAREST", 424 },
	{ "Europe/Budapest", "EUROPE/BUDAPEST", 425 },
	{ "Europe/Busingen", "EUROPE/BUSINGEN", 426 },
	{ "Europe/Chisinau", "EUROPE/CHISINAU", 427 },
	{ "Europe/Copenhagen", "EUROPE/COPENHAGEN", 428 },
	{ "Europe/Dublin", "EUROPE/DUBLIN", 429 },
	{ "Europe/Gibraltar", "EUROPE/GIBRALTAR", 430 },
	{ "Europe/Guernsey", "EUROPE/GUERNSEY", 431 },
	{ "Europe/Helsinki", "EUROPE/HELSINKI", 432 },
	{ "Europe/Isle_of_Man", "EUROPE/ISLE_OF_MAN", 433 },
	{ "Europe/Istanbul", "EUROPE/ISTANBUL", 434 },
	{ "Europe/Jersey", "EUROPE/JERSEY", 435 },
	{ "Europe/Kaliningrad", "EUROPE/KALININGRAD", 436 },
	{ "Europe/Kiev", "EUROPE/KIEV", 437 },
	{ "Europe/Lisbon", "EUROPE/LISBON", 438 },
	{ "Europe/Ljubljana", "EUROPE/LJUBLJANA", 439 },
	{ "Europe/London", "EUROPE/LONDON", 440 },
	{ "Europe/Luxembourg", "EUROPE/LUXEMBOURG", 441 },
	{ "Europe/Madrid", "EUROPE/MADRID", 442 },
	{ "Europe/Malta", "EUROPE/MALTA", 443 },
	{ "Europe/Mariehamn", "EUROPE/MARIEHAMN", 444 },
	{ "Europe/Minsk", "EUROPE/MINSK", 445 },
	{ "Europe/Monaco", "EUROPE/MONACO", 446 },
	{ "Europe/Moscow", "EUROPE/MOSCOW", 447 },
	{ "Europe/Nicosia", "EUROPE/NICOSIA", 448 },
	{ "Europe/Oslo", "EUROPE/OSLO", 449 },
	{ "Europe/Paris", "EUROPE/PARIS", 450 },
	{ "Europe/Podgorica", "EUROPE/PODGORICA", 451 },
	{ "Europe/Prague", "EUROPE/PRAGUE", 452 },
	{ "Europe/Riga", "EUROPE/RIGA", 453 },
	{ "Europe/Rome", "EUROPE/ROME", 454 },
	{ "Europe/Samara", "EUROPE/SAMARA", 455 },
	{ "Europe/San_Marino", "EUROPE/SAN_MARINO", 456 },
	{ "Europe/Sarajevo", "EUROPE/SARAJEVO", 457 },
	{ "Europe/Simferopol", "EUROPE/SIMFEROPOL", 458 },
	{ "Europe/Skopje", "EUROPE/SKOPJE", 459 },
	{ "Europe/Sofia", "EUROPE/SOFIA", 460 },
	{ "Europe/Stockholm", "EUROPE/STOCKHOLM", 461 },
	{ "Europe/Tallinn", "EUROPE/TALLINN", 462 },
	{ "Europe/Tirane", "EUROPE/TIRANE", 463 },
	{ "Europe/Tiraspol", "EUROPE/TIRASPOL", 464 },
	{ "Europe/Uzhgorod", "EUROPE/UZHGOROD", 465 },
	{ "Europe/Vaduz", "EUROPE/VADUZ", 466 },
	{ "Europe/Vatican", "EUROPE/VATICAN", 467 },
	{ "Europe/Vienna", "EUROPE/VIENNA", 468 },
	{ "Europe/Vilnius", "EUROPE/VILNIUS", 469 },
	{ "Europe/Volgograd", "EUROPE/VOLGOGRAD", 470 },
	{ "Europe/Warsaw", "EUROPE/WARSAW", 471 },
	{ "Europe/Zagreb", "EUROPE/ZAGREB", 472 },
	{ "Europe/Zaporozhye", "EUROPE/ZAPOROZHYE", 473 },
	{ "Europe/Zurich", "EUROPE/ZURICH", 474 },
	{ "GB", "GB", 475 },
	{ "GB-Eire", "GB-EIRE", 476 },
	{ "GMT", "GMT", 477 },
	{ "GMT+0", "GMT+0", 478 },
	{ "GMT-0", "GMT-0", 479 },
	{ "GMT0", "GMT0", 480 },
	{ "Greenwich", "GREENWICH", 481 },
	{ "HST", "HST", 482 },
	{ "Hongkong", "HONGKONG", 483 },
	{ "Iceland", "ICELAND", 484 },
	{ "Indian/Antananarivo", "INDIAN/ANTANANARIVO", 485 },
	{ "Indian/Chagos", "INDIAN/CHAGOS", 486 },
	{ "Indian/Christmas", "INDIAN/CHRISTMAS", 487 },
	{ "Indian/Cocos", "INDIAN/COCOS", 488 },
	{ "Indian/Comoro", "INDIAN/COMORO", 489 },
	{ "Indian/Kerguelen", "INDIAN/KERGUELEN", 490 },
	{ "Indian/Mahe", "INDIAN/MAHE", 491 },
	{ "Indian/Maldives", "INDIAN/MALDIVES", 492 },
	{ "Indian/Mauritius", "INDIAN/MAURITIUS", 493 },
	{ "Indian/Mayotte", "INDIAN/MAYOTTE", 494 },
	{ "Indian/Reunion", "INDIAN/REUNION", 495 },
	{ "Iran", "IRAN", 496 },
	{ "Israel", "ISRAEL", 497 },
	{ "Jamaica", "JAMAICA", 498 },
	{ "Japan", "JAPAN", 499 },
	{ "Kwajalein", "KWAJALEIN", 500 },
	{ "Libya", "LIBYA", 501 },
	{ "MET", "MET", 502 },
	{ "MST", "MST", 503 },
	{ "MST7MDT", "MST7MDT", 504 },
	{ "Mexico/BajaNorte", "MEXICO/BAJANORTE", 505 },
	{ "Mexico/BajaSur", "MEXICO/BAJASUR", 506 },
	{ "Mexico/General", "MEXICO/GENERAL", 507 },
	{ "NZ", "NZ", 508 },
	{ "NZ-CHAT", "NZ-CHAT", 509 },
	{ "Navajo", "NAVAJO", 510 },
	{ "PRC", "PRC", 511 },
	{ "PST8PDT", "PST8PDT", 512 },
	{ "Pacific/Apia", "PACIFIC/APIA", 513 },
	{ "Pacific/Auckland", "PACIFIC/AUCKLAND", 514 },
	{ "Pacific/Chatham", "PACIFIC/CHATHAM", 515 },
	{ "Pacific/Chuuk", "PACIFIC/CHUUK", 516 },
	{ "Pacific/Easter", "PACIFIC/EASTER", 517 },
	{ "Pacific/Efate", "PACIFIC/EFATE", 518 },
	{ "Pacific/Enderbury", "PACIFIC/ENDERBURY", 519 },
	{ "Pacific/Fakaofo", "PACIFIC/FAKAOFO", 520 },
	{ "Pacific/Fiji", "PACIFIC/FIJI", 521 },
	{ "Pacific/Funafuti", "PACIFIC/FUNAFUTI", 522 },
	{ "Pacific/Galapagos", "PACIFIC/GALAPAGOS", 523 },
	{ "Pacific/Gambier", "PACIFIC/GAMBIER", 524 },
	{ "Pacific/Guadalcanal", "PACIFIC/GUADALCANAL", 525 },
	{ "Pacific/Guam", "PACIFIC/GUAM", 526 },
	{ "Pacific/Honolulu", "PACIFIC/HONOLULU", 527 },
	{ "Pacific/Johnston", "PACIFIC/JOHNSTON", 528 },
	{ "Pacific/Kiritimati", "PACIFIC/KIRITIMATI", 529 },
	{ "Pacific/Kosrae", "PACIFIC/KOSRAE", 530 },
	{ "Pacific/Kwajalein", "PACIFIC/KWAJALEIN", 531 },
	{ "Pacific/Majuro", "PACIFIC/MAJURO", 532 },
	{ "Pacific/Marquesas", "PACIFIC/MARQUESAS", 533 },
	{ "Pacific/Midway", "PACIFIC/MIDWAY", 534 },
	{ "Pacific/Nauru", "PACIFIC/NAURU", 535 },
	{ "Pacific/Niue", "PACIFIC/NIUE", 536 },
	{ "Pacific/Norfolk", "PACIFIC/NORFOLK", 537 },
	{ "Pacific/Noumea", "PACIFIC/NOUMEA", 538 },
	{ "Pacific/Pago_Pago", "PACIFIC/PAGO_PAGO", 539 },
	{ "Pacific/Palau", "PACIFIC/PALAU", 540 },
	{ "Pacific/Pitcairn", "PACIFIC/PITCAIRN", 541 },
	{ "Pacific/Pohnpei", "PACIFIC/POHNPEI", 542 },
	{ "Pacific/Ponape", "PACIFIC/PONAPE", 543 },
	{ "Pacific/Port_Moresby", "PACIFIC/PORT_MORESBY", 544 },
	{ "Pacific/Rarotonga", "PACIFIC/RAROTONGA", 545 },
	{ "Pacific/Saipan", "PACIFIC/SAIPAN", 546 },
	{ "Pacific/Samoa", "PACIFIC/SAMOA", 547 },
	{ "Pacific/Tahiti", "PACIFIC/TAHITI", 548 },
	{ "Pacific/Tarawa", "PACIFIC/TARAWA", 549 },
	{ "Pacific/Tongatapu", "PACIFIC/TONGATAPU", 550 },
	{ "Pacific/Truk", "PACIFIC/TRUK", 551 },
	{ "Pacific/Wake", "PACIFIC/WAKE", 552 },
	{ "Pacific/Wallis", "PACIFIC/WALLIS", 553 },
	{ "Pacific/Yap", "PACIFIC/YAP", 554 },
	{ "Poland", "POLAND", 555 },
	{ "Portugal", "PORTUGAL", 556 },
	{ "ROC", "ROC", 557 },
	{ "ROK", "ROK", 558 },
	{ "Singapore", "SINGAPORE", 559 },
	{ "Turkey", "TURKEY", 560 },
	{ "UCT", "UCT", 561 },
	{ "US/Alaska", "US/ALASKA", 562 },
	{ "US/Aleutian", "US/ALEUTIAN", 563 },
	{ "US/Arizona", "US/ARIZONA", 564 },
	{ "US/Central", "US/CENTRAL", 565 },
	{ "US/East-Indiana", "US/EAST-INDIANA", 566 },
	{ "US/Eastern", "US/EASTERN", 567 },
	{ "US/Hawaii", "US/HAWAII", 568 },
	{ "US/Indiana-Starke", "US/INDIANA-STARKE", 569 },
	{ "US/Michigan", "US/MICHIGAN", 570 },
	{ "US/Mountain", "US/MOUNTAIN", 571 },
	{ "US/Pacific", "US/PACIFIC", 572 },
	{ "US/Pacific-New", "US/PACIFIC-NEW", 573 },
	{ "US/Samoa", "US/SAMOA", 574 },
	{ "UTC", "UTC", 575 },
	{ "Universal", "UNIVERSAL", 576 },
	{ "W-SU", "W-SU", 577 },
	{ "WET", "WET", 578 },
	{ "Zulu", "ZULU", 579 },
};

#define NTIMEZONES (sizeof(timezones)/sizeof(timezones[0]))

static const char *tzid_to_tzname(int id)
{
	return timezones[id - 1].name;
}

static int tzname_to_tzid(const char *name)
{
	char uppername[TZ_STRLEN_MAX + 1] = {0};
	char *p;
	int first, last, middle;

	p = uppername;
	while (*name)
		*p++ = pg_toupper((unsigned char) *name++);
	*p = '\0';

	first = 0;
	last = NTIMEZONES - 1;
	middle = (first + last) / 2;

	while(first <= last)
	{
		int cmp = strcmp(timezones[middle].nameupper, uppername);

		if(cmp < 0)
			first = middle + 1;
		else if(cmp == 0)
			return timezones[middle].id;
		else
			last = middle - 1;

		middle = (first + last) / 2;
	}

	return 0;
}

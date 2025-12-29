/* -----------------------------------------------------------------------
 * formatting.c
 *
 * src/backend/utils/adt/formatting.c
 *
 *
 *	 Portions Copyright (c) 1999-2025, PostgreSQL Global Development Group
 *
 *
 *	 TO_CHAR(); TO_TIMESTAMP(); TO_DATE(); TO_NUMBER();
 *
 *	 The PostgreSQL routines for a timestamp/int/float/numeric formatting,
 *	 inspired by the Oracle TO_CHAR() / TO_DATE() / TO_NUMBER() routines.
 *
 *
 *	 Cache & Memory:
 *	Routines use (itself) internal cache for format pictures.
 *
 *	The cache uses a static buffer and is persistent across transactions.  If
 *	the format-picture is bigger than the cache buffer, the parser is called
 *	always.
 *
 *	 NOTE for Number version:
 *	All in this version is implemented as keywords ( => not used
 *	suffixes), because a format picture is for *one* item (number)
 *	only. It not is as a timestamp version, where each keyword (can)
 *	has suffix.
 *
 *	 NOTE for Timestamp routines:
 *	In this module the POSIX 'struct tm' type is *not* used, but rather
 *	PgSQL type, which has tm_mon based on one (*non* zero) and
 *	year *not* based on 1900, but is used full year number.
 *	Module supports AD / BC / AM / PM.
 *
 *	Supported types for to_char():
 *
 *		Timestamp, Numeric, int4, int8, float4, float8
 *
 *	Supported types for reverse conversion:
 *
 *		Timestamp	- to_timestamp()
 *		Date		- to_date()
 *		Numeric		- to_number()
 *
 *
 *	Karel Zak
 *
 * TODO
 *	- better number building (formatting) / parsing, now it isn't
 *		  ideal code
 *	- use Assert()
 *	- add support for number spelling
 *	- add support for string to string formatting (we must be better
 *	  than Oracle :-),
 *		to_char('Hello', 'X X X X X') -> 'H e l l o'
 *
 * -----------------------------------------------------------------------
 */

#ifdef DEBUG_TO_FROM_CHAR
#define DEBUG_elog_output	DEBUG3
#endif

#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <wctype.h>

#ifdef USE_ICU
#include <unicode/ustring.h>
#endif

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "mb/pg_wchar.h"
#include "nodes/miscnodes.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "varatt.h"


/* ----------
 * Routines flags
 * ----------
 */
#define DCH_FLAG		0x1		/* DATE-TIME flag	*/
#define NUM_FLAG		0x2		/* NUMBER flag	*/
#define STD_FLAG		0x4		/* STANDARD flag	*/

/* ----------
 * KeyWord Index (ascii from position 32 (' ') to 126 (~))
 * ----------
 */
#define KeyWord_INDEX_SIZE		('~' - ' ')
#define KeyWord_INDEX_FILTER(_c)	((_c) <= ' ' || (_c) >= '~' ? 0 : 1)

/* ----------
 * Maximal length of one node
 * ----------
 */
#define DCH_MAX_ITEM_SIZ	   12	/* max localized day name		*/
#define NUM_MAX_ITEM_SIZ		8	/* roman number (RN has 15 chars)	*/


/* ----------
 * Format parser structs
 * ----------
 */
typedef struct
{
	const char *name;			/* suffix string		*/
	int			len,			/* suffix length		*/
				id,				/* used in node->suffix */
				type;			/* prefix / postfix		*/
} KeySuffix;

/* ----------
 * FromCharDateMode
 * ----------
 *
 * This value is used to nominate one of several distinct (and mutually
 * exclusive) date conventions that a keyword can belong to.
 */
typedef enum
{
	FROM_CHAR_DATE_NONE = 0,	/* Value does not affect date mode. */
	FROM_CHAR_DATE_GREGORIAN,	/* Gregorian (day, month, year) style date */
	FROM_CHAR_DATE_ISOWEEK,		/* ISO 8601 week date */
} FromCharDateMode;

typedef struct
{
	const char *name;
	int			len;
	int			id;
	bool		is_digit;
	FromCharDateMode date_mode;
} KeyWord;

typedef struct
{
	uint8		type;			/* NODE_TYPE_XXX, see below */
	char		character[MAX_MULTIBYTE_CHAR_LEN + 1];	/* if type is CHAR */
	uint8		suffix;			/* keyword prefix/suffix code, if any */
	const KeyWord *key;			/* if type is ACTION */
} FormatNode;

#define NODE_TYPE_END		1
#define NODE_TYPE_ACTION	2
#define NODE_TYPE_CHAR		3
#define NODE_TYPE_SEPARATOR	4
#define NODE_TYPE_SPACE		5

#define SUFFTYPE_PREFIX		1
#define SUFFTYPE_POSTFIX	2

#define CLOCK_24_HOUR		0
#define CLOCK_12_HOUR		1


/* ----------
 * Full months
 * ----------
 */
static const char *const months_full[] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December", NULL
};

static const char *const days_short[] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", NULL
};

/* ----------
 * AD / BC
 * ----------
 *	There is no 0 AD.  Years go from 1 BC to 1 AD, so we make it
 *	positive and map year == -1 to year zero, and shift all negative
 *	years up one.  For interval years, we just return the year.
 */
#define ADJUST_YEAR(year, is_interval)	((is_interval) ? (year) : ((year) <= 0 ? -((year) - 1) : (year)))

#define A_D_STR		"A.D."
#define a_d_STR		"a.d."
#define AD_STR		"AD"
#define ad_STR		"ad"

#define B_C_STR		"B.C."
#define b_c_STR		"b.c."
#define BC_STR		"BC"
#define bc_STR		"bc"

/*
 * AD / BC strings for seq_search.
 *
 * These are given in two variants, a long form with periods and a standard
 * form without.
 *
 * The array is laid out such that matches for AD have an even index, and
 * matches for BC have an odd index.  So the boolean value for BC is given by
 * taking the array index of the match, modulo 2.
 */
static const char *const adbc_strings[] = {ad_STR, bc_STR, AD_STR, BC_STR, NULL};
static const char *const adbc_strings_long[] = {a_d_STR, b_c_STR, A_D_STR, B_C_STR, NULL};

/* ----------
 * AM / PM
 * ----------
 */
#define A_M_STR		"A.M."
#define a_m_STR		"a.m."
#define AM_STR		"AM"
#define am_STR		"am"

#define P_M_STR		"P.M."
#define p_m_STR		"p.m."
#define PM_STR		"PM"
#define pm_STR		"pm"

/*
 * AM / PM strings for seq_search.
 *
 * These are given in two variants, a long form with periods and a standard
 * form without.
 *
 * The array is laid out such that matches for AM have an even index, and
 * matches for PM have an odd index.  So the boolean value for PM is given by
 * taking the array index of the match, modulo 2.
 */
static const char *const ampm_strings[] = {am_STR, pm_STR, AM_STR, PM_STR, NULL};
static const char *const ampm_strings_long[] = {a_m_STR, p_m_STR, A_M_STR, P_M_STR, NULL};

/* ----------
 * Months in roman-numeral
 * (Must be in reverse order for seq_search (in FROM_CHAR), because
 *	'VIII' must have higher precedence than 'V')
 * ----------
 */
static const char *const rm_months_upper[] =
{"XII", "XI", "X", "IX", "VIII", "VII", "VI", "V", "IV", "III", "II", "I", NULL};

static const char *const rm_months_lower[] =
{"xii", "xi", "x", "ix", "viii", "vii", "vi", "v", "iv", "iii", "ii", "i", NULL};

/* ----------
 * Roman numerals
 * ----------
 */
static const char *const rm1[] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", NULL};
static const char *const rm10[] = {"X", "XX", "XXX", "XL", "L", "LX", "LXX", "LXXX", "XC", NULL};
static const char *const rm100[] = {"C", "CC", "CCC", "CD", "D", "DC", "DCC", "DCCC", "CM", NULL};

/*
 * MACRO: Check if the current and next characters form a valid subtraction
 * combination for roman numerals.
 */
#define IS_VALID_SUB_COMB(curr, next) \
	(((curr) == 'I' && ((next) == 'V' || (next) == 'X')) || \
	 ((curr) == 'X' && ((next) == 'L' || (next) == 'C')) || \
	 ((curr) == 'C' && ((next) == 'D' || (next) == 'M')))

/*
 * MACRO: Roman numeral value, or 0 if character isn't a roman numeral.
 */
#define ROMAN_VAL(r) \
	((r) == 'I' ? 1 : \
	 (r) == 'V' ? 5 : \
	 (r) == 'X' ? 10 : \
	 (r) == 'L' ? 50 : \
	 (r) == 'C' ? 100 : \
	 (r) == 'D' ? 500 : \
	 (r) == 'M' ? 1000 : 0)

/*
 * 'MMMDCCCLXXXVIII' (3888) is the longest valid roman numeral (15 characters).
 */
#define MAX_ROMAN_LEN	15

/* ----------
 * Ordinal postfixes
 * ----------
 */
static const char *const numTH[] = {"ST", "ND", "RD", "TH", NULL};
static const char *const numth[] = {"st", "nd", "rd", "th", NULL};

/* ----------
 * Flags & Options:
 * ----------
 */
#define TH_UPPER		1
#define TH_LOWER		2

/* ----------
 * Number description struct
 * ----------
 */
typedef struct
{
	int			pre,			/* (count) numbers before decimal */
				post,			/* (count) numbers after decimal  */
				lsign,			/* want locales sign		  */
				flag,			/* number parameters		  */
				pre_lsign_num,	/* tmp value for lsign		  */
				multi,			/* multiplier for 'V'		  */
				zero_start,		/* position of first zero	  */
				zero_end,		/* position of last zero	  */
				need_locale;	/* needs it locale		  */
} NUMDesc;

/* ----------
 * Flags for NUMBER version
 * ----------
 */
#define NUM_F_DECIMAL		(1 << 1)
#define NUM_F_LDECIMAL		(1 << 2)
#define NUM_F_ZERO			(1 << 3)
#define NUM_F_BLANK			(1 << 4)
#define NUM_F_FILLMODE		(1 << 5)
#define NUM_F_LSIGN			(1 << 6)
#define NUM_F_BRACKET		(1 << 7)
#define NUM_F_MINUS			(1 << 8)
#define NUM_F_PLUS			(1 << 9)
#define NUM_F_ROMAN			(1 << 10)
#define NUM_F_MULTI			(1 << 11)
#define NUM_F_PLUS_POST		(1 << 12)
#define NUM_F_MINUS_POST	(1 << 13)
#define NUM_F_EEEE			(1 << 14)

#define NUM_LSIGN_PRE	(-1)
#define NUM_LSIGN_POST	1
#define NUM_LSIGN_NONE	0

/* ----------
 * Tests
 * ----------
 */
#define IS_DECIMAL(_f)	((_f)->flag & NUM_F_DECIMAL)
#define IS_LDECIMAL(_f) ((_f)->flag & NUM_F_LDECIMAL)
#define IS_ZERO(_f) ((_f)->flag & NUM_F_ZERO)
#define IS_BLANK(_f)	((_f)->flag & NUM_F_BLANK)
#define IS_FILLMODE(_f) ((_f)->flag & NUM_F_FILLMODE)
#define IS_BRACKET(_f)	((_f)->flag & NUM_F_BRACKET)
#define IS_MINUS(_f)	((_f)->flag & NUM_F_MINUS)
#define IS_LSIGN(_f)	((_f)->flag & NUM_F_LSIGN)
#define IS_PLUS(_f) ((_f)->flag & NUM_F_PLUS)
#define IS_ROMAN(_f)	((_f)->flag & NUM_F_ROMAN)
#define IS_MULTI(_f)	((_f)->flag & NUM_F_MULTI)
#define IS_EEEE(_f)		((_f)->flag & NUM_F_EEEE)

/* ----------
 * Format picture cache
 *
 * We will cache datetime format pictures up to DCH_CACHE_SIZE bytes long;
 * likewise number format pictures up to NUM_CACHE_SIZE bytes long.
 *
 * For simplicity, the cache entries are fixed-size, so they allow for the
 * worst case of a FormatNode for each byte in the picture string.
 *
 * The CACHE_SIZE constants are computed to make sizeof(DCHCacheEntry) and
 * sizeof(NUMCacheEntry) be powers of 2, or just less than that, so that
 * we don't waste too much space by palloc'ing them individually.  Be sure
 * to adjust those macros if you add fields to those structs.
 *
 * The max number of entries in each cache is DCH_CACHE_ENTRIES
 * resp. NUM_CACHE_ENTRIES.
 * ----------
 */
#define DCH_CACHE_OVERHEAD \
	MAXALIGN(sizeof(bool) + sizeof(int))
#define NUM_CACHE_OVERHEAD \
	MAXALIGN(sizeof(bool) + sizeof(int) + sizeof(NUMDesc))

#define DCH_CACHE_SIZE \
	((2048 - DCH_CACHE_OVERHEAD) / (sizeof(FormatNode) + sizeof(char)) - 1)
#define NUM_CACHE_SIZE \
	((1024 - NUM_CACHE_OVERHEAD) / (sizeof(FormatNode) + sizeof(char)) - 1)

#define DCH_CACHE_ENTRIES	20
#define NUM_CACHE_ENTRIES	20

typedef struct
{
	FormatNode	format[DCH_CACHE_SIZE + 1];
	char		str[DCH_CACHE_SIZE + 1];
	bool		std;
	bool		valid;
	int			age;
} DCHCacheEntry;

typedef struct
{
	FormatNode	format[NUM_CACHE_SIZE + 1];
	char		str[NUM_CACHE_SIZE + 1];
	bool		valid;
	int			age;
	NUMDesc		Num;
} NUMCacheEntry;

/* global cache for date/time format pictures */
static DCHCacheEntry *DCHCache[DCH_CACHE_ENTRIES];
static int	n_DCHCache = 0;		/* current number of entries */
static int	DCHCounter = 0;		/* aging-event counter */

/* ----------
 * For char->date/time conversion
 * ----------
 */
typedef struct
{
	FromCharDateMode mode;
	int			hh,
				pm,
				mi,
				ss,
				ssss,
				d,				/* stored as 1-7, Sunday = 1, 0 means missing */
				dd,
				ddd,
				mm,
				ms,
				year,
				bc,
				ww,
				w,
				cc,
				j,
				us,
				yysz,			/* is it YY or YYYY ? */
				clock,			/* 12 or 24 hour clock? */
				tzsign,			/* +1, -1, or 0 if no TZH/TZM fields */
				tzh,
				tzm,
				ff;				/* fractional precision */
	bool		has_tz;			/* was there a TZ field? */
	int			gmtoffset;		/* GMT offset of fixed-offset zone abbrev */
	pg_tz	   *tzp;			/* pg_tz for dynamic abbrev */
	char	   *abbrev;			/* dynamic abbrev */
} TmFromChar;

#define ZERO_tmfc(_X) memset(_X, 0, sizeof(TmFromChar))

struct fmt_tz					/* do_to_timestamp's timezone info output */
{
	bool		has_tz;			/* was there any TZ/TZH/TZM field? */
	int			gmtoffset;		/* GMT offset in seconds */
};

/* ----------
 * Debug
 * ----------
 */
#ifdef DEBUG_TO_FROM_CHAR
#define DEBUG_TMFC(_X) \
		elog(DEBUG_elog_output, "TMFC:\nmode %d\nhh %d\npm %d\nmi %d\nss %d\nssss %d\nd %d\ndd %d\nddd %d\nmm %d\nms: %d\nyear %d\nbc %d\nww %d\nw %d\ncc %d\nj %d\nus: %d\nyysz: %d\nclock: %d", \
			(_X)->mode, (_X)->hh, (_X)->pm, (_X)->mi, (_X)->ss, (_X)->ssss, \
			(_X)->d, (_X)->dd, (_X)->ddd, (_X)->mm, (_X)->ms, (_X)->year, \
			(_X)->bc, (_X)->ww, (_X)->w, (_X)->cc, (_X)->j, (_X)->us, \
			(_X)->yysz, (_X)->clock)
#define DEBUG_TM(_X) \
		elog(DEBUG_elog_output, "TM:\nsec %d\nyear %d\nmin %d\nwday %d\nhour %d\nyday %d\nmday %d\nnisdst %d\nmon %d\n",\
			(_X)->tm_sec, (_X)->tm_year,\
			(_X)->tm_min, (_X)->tm_wday, (_X)->tm_hour, (_X)->tm_yday,\
			(_X)->tm_mday, (_X)->tm_isdst, (_X)->tm_mon)
#else
#define DEBUG_TMFC(_X)
#define DEBUG_TM(_X)
#endif

/* ----------
 * Datetime to char conversion
 *
 * To support intervals as well as timestamps, we use a custom "tm" struct
 * that is almost like struct pg_tm, but has a 64-bit tm_hour field.
 * We omit the tm_isdst and tm_zone fields, which are not used here.
 * ----------
 */
struct fmt_tm
{
	int			tm_sec;
	int			tm_min;
	int64		tm_hour;
	int			tm_mday;
	int			tm_mon;
	int			tm_year;
	int			tm_wday;
	int			tm_yday;
	long int	tm_gmtoff;
};

typedef struct TmToChar
{
	struct fmt_tm tm;			/* almost the classic 'tm' struct */
	fsec_t		fsec;			/* fractional seconds */
	const char *tzn;			/* timezone */
} TmToChar;

#define tmtcTm(_X)	(&(_X)->tm)
#define tmtcTzn(_X) ((_X)->tzn)
#define tmtcFsec(_X)	((_X)->fsec)

/* Note: this is used to copy pg_tm to fmt_tm, so not quite a bitwise copy */
#define COPY_tm(_DST, _SRC) \
do {	\
	(_DST)->tm_sec = (_SRC)->tm_sec; \
	(_DST)->tm_min = (_SRC)->tm_min; \
	(_DST)->tm_hour = (_SRC)->tm_hour; \
	(_DST)->tm_mday = (_SRC)->tm_mday; \
	(_DST)->tm_mon = (_SRC)->tm_mon; \
	(_DST)->tm_year = (_SRC)->tm_year; \
	(_DST)->tm_wday = (_SRC)->tm_wday; \
	(_DST)->tm_yday = (_SRC)->tm_yday; \
	(_DST)->tm_gmtoff = (_SRC)->tm_gmtoff; \
} while(0)

/* Caution: this is used to zero both pg_tm and fmt_tm structs */
#define ZERO_tm(_X) \
do {	\
	memset(_X, 0, sizeof(*(_X))); \
	(_X)->tm_mday = (_X)->tm_mon = 1; \
} while(0)

#define ZERO_tmtc(_X) \
do { \
	ZERO_tm( tmtcTm(_X) ); \
	tmtcFsec(_X) = 0; \
	tmtcTzn(_X) = NULL; \
} while(0)

/*
 *	to_char(time) appears to to_char() as an interval, so this check
 *	is really for interval and time data types.
 */
#define INVALID_FOR_INTERVAL  \
do { \
	if (is_interval) \
		ereport(ERROR, \
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT), \
				 errmsg("invalid format specification for an interval value"), \
				 errhint("Intervals are not tied to specific calendar dates."))); \
} while(0)

/*****************************************************************************
 *			KeyWord definitions
 *****************************************************************************/

/* ----------
 * Suffixes (FormatNode.suffix is an OR of these codes)
 * ----------
 */
#define DCH_S_FM	0x01
#define DCH_S_TH	0x02
#define DCH_S_th	0x04
#define DCH_S_SP	0x08
#define DCH_S_TM	0x10

/* ----------
 * Suffix tests
 * ----------
 */
#define S_THth(_s)	((((_s) & DCH_S_TH) || ((_s) & DCH_S_th)) ? 1 : 0)
#define S_TH(_s)	(((_s) & DCH_S_TH) ? 1 : 0)
#define S_th(_s)	(((_s) & DCH_S_th) ? 1 : 0)
#define S_TH_TYPE(_s)	(((_s) & DCH_S_TH) ? TH_UPPER : TH_LOWER)

/* Oracle toggles FM behavior, we don't; see docs. */
#define S_FM(_s)	(((_s) & DCH_S_FM) ? 1 : 0)
#define S_SP(_s)	(((_s) & DCH_S_SP) ? 1 : 0)
#define S_TM(_s)	(((_s) & DCH_S_TM) ? 1 : 0)

/* ----------
 * Suffixes definition for DATE-TIME TO/FROM CHAR
 * ----------
 */
#define TM_SUFFIX_LEN	2

static const KeySuffix DCH_suff[] = {
	{"FM", 2, DCH_S_FM, SUFFTYPE_PREFIX},
	{"fm", 2, DCH_S_FM, SUFFTYPE_PREFIX},
	{"TM", TM_SUFFIX_LEN, DCH_S_TM, SUFFTYPE_PREFIX},
	{"tm", 2, DCH_S_TM, SUFFTYPE_PREFIX},
	{"TH", 2, DCH_S_TH, SUFFTYPE_POSTFIX},
	{"th", 2, DCH_S_th, SUFFTYPE_POSTFIX},
	{"SP", 2, DCH_S_SP, SUFFTYPE_POSTFIX},
	/* last */
	{NULL, 0, 0, 0}
};


/* ----------
 * Format-pictures (KeyWord).
 *
 * The KeyWord field; alphabetic sorted, *BUT* strings alike is sorted
 *		  complicated -to-> easy:
 *
 *	(example: "DDD","DD","Day","D" )
 *
 * (this specific sort needs the algorithm for sequential search for strings,
 * which not has exact end; -> How keyword is in "HH12blabla" ? - "HH"
 * or "HH12"? You must first try "HH12", because "HH" is in string, but
 * it is not good.
 *
 * (!)
 *	 - Position for the keyword is similar as position in the enum DCH/NUM_poz.
 * (!)
 *
 * For fast search is used the 'int index[]', index is ascii table from position
 * 32 (' ') to 126 (~), in this index is DCH_ / NUM_ enums for each ASCII
 * position or -1 if char is not used in the KeyWord. Search example for
 * string "MM":
 *	1)	see in index to index['M' - 32],
 *	2)	take keywords position (enum DCH_MI) from index
 *	3)	run sequential search in keywords[] from this position
 *
 * ----------
 */

typedef enum
{
	DCH_A_D,
	DCH_A_M,
	DCH_AD,
	DCH_AM,
	DCH_B_C,
	DCH_BC,
	DCH_CC,
	DCH_DAY,
	DCH_DDD,
	DCH_DD,
	DCH_DY,
	DCH_Day,
	DCH_Dy,
	DCH_D,
	DCH_FF1,					/* FFn codes must be consecutive */
	DCH_FF2,
	DCH_FF3,
	DCH_FF4,
	DCH_FF5,
	DCH_FF6,
	DCH_FX,						/* global suffix */
	DCH_HH24,
	DCH_HH12,
	DCH_HH,
	DCH_IDDD,
	DCH_ID,
	DCH_IW,
	DCH_IYYY,
	DCH_IYY,
	DCH_IY,
	DCH_I,
	DCH_J,
	DCH_MI,
	DCH_MM,
	DCH_MONTH,
	DCH_MON,
	DCH_MS,
	DCH_Month,
	DCH_Mon,
	DCH_OF,
	DCH_P_M,
	DCH_PM,
	DCH_Q,
	DCH_RM,
	DCH_SSSSS,
	DCH_SSSS,
	DCH_SS,
	DCH_TZH,
	DCH_TZM,
	DCH_TZ,
	DCH_US,
	DCH_WW,
	DCH_W,
	DCH_Y_YYY,
	DCH_YYYY,
	DCH_YYY,
	DCH_YY,
	DCH_Y,
	DCH_a_d,
	DCH_a_m,
	DCH_ad,
	DCH_am,
	DCH_b_c,
	DCH_bc,
	DCH_cc,
	DCH_day,
	DCH_ddd,
	DCH_dd,
	DCH_dy,
	DCH_d,
	DCH_ff1,
	DCH_ff2,
	DCH_ff3,
	DCH_ff4,
	DCH_ff5,
	DCH_ff6,
	DCH_fx,
	DCH_hh24,
	DCH_hh12,
	DCH_hh,
	DCH_iddd,
	DCH_id,
	DCH_iw,
	DCH_iyyy,
	DCH_iyy,
	DCH_iy,
	DCH_i,
	DCH_j,
	DCH_mi,
	DCH_mm,
	DCH_month,
	DCH_mon,
	DCH_ms,
	DCH_of,
	DCH_p_m,
	DCH_pm,
	DCH_q,
	DCH_rm,
	DCH_sssss,
	DCH_ssss,
	DCH_ss,
	DCH_tzh,
	DCH_tzm,
	DCH_tz,
	DCH_us,
	DCH_ww,
	DCH_w,
	DCH_y_yyy,
	DCH_yyyy,
	DCH_yyy,
	DCH_yy,
	DCH_y,

	/* last */
	_DCH_last_
}			DCH_poz;

typedef enum
{
	NUM_COMMA,
	NUM_DEC,
	NUM_0,
	NUM_9,
	NUM_B,
	NUM_C,
	NUM_D,
	NUM_E,
	NUM_FM,
	NUM_G,
	NUM_L,
	NUM_MI,
	NUM_PL,
	NUM_PR,
	NUM_RN,
	NUM_SG,
	NUM_SP,
	NUM_S,
	NUM_TH,
	NUM_V,
	NUM_b,
	NUM_c,
	NUM_d,
	NUM_e,
	NUM_fm,
	NUM_g,
	NUM_l,
	NUM_mi,
	NUM_pl,
	NUM_pr,
	NUM_rn,
	NUM_sg,
	NUM_sp,
	NUM_s,
	NUM_th,
	NUM_v,

	/* last */
	_NUM_last_
}			NUM_poz;

/* ----------
 * KeyWords for DATE-TIME version
 * ----------
 */
static const KeyWord DCH_keywords[] = {
/*	name, len, id, is_digit, date_mode */
	{"A.D.", 4, DCH_A_D, false, FROM_CHAR_DATE_NONE},	/* A */
	{"A.M.", 4, DCH_A_M, false, FROM_CHAR_DATE_NONE},
	{"AD", 2, DCH_AD, false, FROM_CHAR_DATE_NONE},
	{"AM", 2, DCH_AM, false, FROM_CHAR_DATE_NONE},
	{"B.C.", 4, DCH_B_C, false, FROM_CHAR_DATE_NONE},	/* B */
	{"BC", 2, DCH_BC, false, FROM_CHAR_DATE_NONE},
	{"CC", 2, DCH_CC, true, FROM_CHAR_DATE_NONE},	/* C */
	{"DAY", 3, DCH_DAY, false, FROM_CHAR_DATE_NONE},	/* D */
	{"DDD", 3, DCH_DDD, true, FROM_CHAR_DATE_GREGORIAN},
	{"DD", 2, DCH_DD, true, FROM_CHAR_DATE_GREGORIAN},
	{"DY", 2, DCH_DY, false, FROM_CHAR_DATE_NONE},
	{"Day", 3, DCH_Day, false, FROM_CHAR_DATE_NONE},
	{"Dy", 2, DCH_Dy, false, FROM_CHAR_DATE_NONE},
	{"D", 1, DCH_D, true, FROM_CHAR_DATE_GREGORIAN},
	{"FF1", 3, DCH_FF1, true, FROM_CHAR_DATE_NONE}, /* F */
	{"FF2", 3, DCH_FF2, true, FROM_CHAR_DATE_NONE},
	{"FF3", 3, DCH_FF3, true, FROM_CHAR_DATE_NONE},
	{"FF4", 3, DCH_FF4, true, FROM_CHAR_DATE_NONE},
	{"FF5", 3, DCH_FF5, true, FROM_CHAR_DATE_NONE},
	{"FF6", 3, DCH_FF6, true, FROM_CHAR_DATE_NONE},
	{"FX", 2, DCH_FX, false, FROM_CHAR_DATE_NONE},
	{"HH24", 4, DCH_HH24, true, FROM_CHAR_DATE_NONE},	/* H */
	{"HH12", 4, DCH_HH12, true, FROM_CHAR_DATE_NONE},
	{"HH", 2, DCH_HH, true, FROM_CHAR_DATE_NONE},
	{"IDDD", 4, DCH_IDDD, true, FROM_CHAR_DATE_ISOWEEK},	/* I */
	{"ID", 2, DCH_ID, true, FROM_CHAR_DATE_ISOWEEK},
	{"IW", 2, DCH_IW, true, FROM_CHAR_DATE_ISOWEEK},
	{"IYYY", 4, DCH_IYYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"IYY", 3, DCH_IYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"IY", 2, DCH_IY, true, FROM_CHAR_DATE_ISOWEEK},
	{"I", 1, DCH_I, true, FROM_CHAR_DATE_ISOWEEK},
	{"J", 1, DCH_J, true, FROM_CHAR_DATE_NONE}, /* J */
	{"MI", 2, DCH_MI, true, FROM_CHAR_DATE_NONE},	/* M */
	{"MM", 2, DCH_MM, true, FROM_CHAR_DATE_GREGORIAN},
	{"MONTH", 5, DCH_MONTH, false, FROM_CHAR_DATE_GREGORIAN},
	{"MON", 3, DCH_MON, false, FROM_CHAR_DATE_GREGORIAN},
	{"MS", 2, DCH_MS, true, FROM_CHAR_DATE_NONE},
	{"Month", 5, DCH_Month, false, FROM_CHAR_DATE_GREGORIAN},
	{"Mon", 3, DCH_Mon, false, FROM_CHAR_DATE_GREGORIAN},
	{"OF", 2, DCH_OF, false, FROM_CHAR_DATE_NONE},	/* O */
	{"P.M.", 4, DCH_P_M, false, FROM_CHAR_DATE_NONE},	/* P */
	{"PM", 2, DCH_PM, false, FROM_CHAR_DATE_NONE},
	{"Q", 1, DCH_Q, true, FROM_CHAR_DATE_NONE}, /* Q */
	{"RM", 2, DCH_RM, false, FROM_CHAR_DATE_GREGORIAN}, /* R */
	{"SSSSS", 5, DCH_SSSS, true, FROM_CHAR_DATE_NONE},	/* S */
	{"SSSS", 4, DCH_SSSS, true, FROM_CHAR_DATE_NONE},
	{"SS", 2, DCH_SS, true, FROM_CHAR_DATE_NONE},
	{"TZH", 3, DCH_TZH, false, FROM_CHAR_DATE_NONE},	/* T */
	{"TZM", 3, DCH_TZM, true, FROM_CHAR_DATE_NONE},
	{"TZ", 2, DCH_TZ, false, FROM_CHAR_DATE_NONE},
	{"US", 2, DCH_US, true, FROM_CHAR_DATE_NONE},	/* U */
	{"WW", 2, DCH_WW, true, FROM_CHAR_DATE_GREGORIAN},	/* W */
	{"W", 1, DCH_W, true, FROM_CHAR_DATE_GREGORIAN},
	{"Y,YYY", 5, DCH_Y_YYY, true, FROM_CHAR_DATE_GREGORIAN},	/* Y */
	{"YYYY", 4, DCH_YYYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"YYY", 3, DCH_YYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"YY", 2, DCH_YY, true, FROM_CHAR_DATE_GREGORIAN},
	{"Y", 1, DCH_Y, true, FROM_CHAR_DATE_GREGORIAN},
	{"a.d.", 4, DCH_a_d, false, FROM_CHAR_DATE_NONE},	/* a */
	{"a.m.", 4, DCH_a_m, false, FROM_CHAR_DATE_NONE},
	{"ad", 2, DCH_ad, false, FROM_CHAR_DATE_NONE},
	{"am", 2, DCH_am, false, FROM_CHAR_DATE_NONE},
	{"b.c.", 4, DCH_b_c, false, FROM_CHAR_DATE_NONE},	/* b */
	{"bc", 2, DCH_bc, false, FROM_CHAR_DATE_NONE},
	{"cc", 2, DCH_CC, true, FROM_CHAR_DATE_NONE},	/* c */
	{"day", 3, DCH_day, false, FROM_CHAR_DATE_NONE},	/* d */
	{"ddd", 3, DCH_DDD, true, FROM_CHAR_DATE_GREGORIAN},
	{"dd", 2, DCH_DD, true, FROM_CHAR_DATE_GREGORIAN},
	{"dy", 2, DCH_dy, false, FROM_CHAR_DATE_NONE},
	{"d", 1, DCH_D, true, FROM_CHAR_DATE_GREGORIAN},
	{"ff1", 3, DCH_FF1, true, FROM_CHAR_DATE_NONE}, /* f */
	{"ff2", 3, DCH_FF2, true, FROM_CHAR_DATE_NONE},
	{"ff3", 3, DCH_FF3, true, FROM_CHAR_DATE_NONE},
	{"ff4", 3, DCH_FF4, true, FROM_CHAR_DATE_NONE},
	{"ff5", 3, DCH_FF5, true, FROM_CHAR_DATE_NONE},
	{"ff6", 3, DCH_FF6, true, FROM_CHAR_DATE_NONE},
	{"fx", 2, DCH_FX, false, FROM_CHAR_DATE_NONE},
	{"hh24", 4, DCH_HH24, true, FROM_CHAR_DATE_NONE},	/* h */
	{"hh12", 4, DCH_HH12, true, FROM_CHAR_DATE_NONE},
	{"hh", 2, DCH_HH, true, FROM_CHAR_DATE_NONE},
	{"iddd", 4, DCH_IDDD, true, FROM_CHAR_DATE_ISOWEEK},	/* i */
	{"id", 2, DCH_ID, true, FROM_CHAR_DATE_ISOWEEK},
	{"iw", 2, DCH_IW, true, FROM_CHAR_DATE_ISOWEEK},
	{"iyyy", 4, DCH_IYYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"iyy", 3, DCH_IYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"iy", 2, DCH_IY, true, FROM_CHAR_DATE_ISOWEEK},
	{"i", 1, DCH_I, true, FROM_CHAR_DATE_ISOWEEK},
	{"j", 1, DCH_J, true, FROM_CHAR_DATE_NONE}, /* j */
	{"mi", 2, DCH_MI, true, FROM_CHAR_DATE_NONE},	/* m */
	{"mm", 2, DCH_MM, true, FROM_CHAR_DATE_GREGORIAN},
	{"month", 5, DCH_month, false, FROM_CHAR_DATE_GREGORIAN},
	{"mon", 3, DCH_mon, false, FROM_CHAR_DATE_GREGORIAN},
	{"ms", 2, DCH_MS, true, FROM_CHAR_DATE_NONE},
	{"of", 2, DCH_OF, false, FROM_CHAR_DATE_NONE},	/* o */
	{"p.m.", 4, DCH_p_m, false, FROM_CHAR_DATE_NONE},	/* p */
	{"pm", 2, DCH_pm, false, FROM_CHAR_DATE_NONE},
	{"q", 1, DCH_Q, true, FROM_CHAR_DATE_NONE}, /* q */
	{"rm", 2, DCH_rm, false, FROM_CHAR_DATE_GREGORIAN}, /* r */
	{"sssss", 5, DCH_SSSS, true, FROM_CHAR_DATE_NONE},	/* s */
	{"ssss", 4, DCH_SSSS, true, FROM_CHAR_DATE_NONE},
	{"ss", 2, DCH_SS, true, FROM_CHAR_DATE_NONE},
	{"tzh", 3, DCH_TZH, false, FROM_CHAR_DATE_NONE},	/* t */
	{"tzm", 3, DCH_TZM, true, FROM_CHAR_DATE_NONE},
	{"tz", 2, DCH_tz, false, FROM_CHAR_DATE_NONE},
	{"us", 2, DCH_US, true, FROM_CHAR_DATE_NONE},	/* u */
	{"ww", 2, DCH_WW, true, FROM_CHAR_DATE_GREGORIAN},	/* w */
	{"w", 1, DCH_W, true, FROM_CHAR_DATE_GREGORIAN},
	{"y,yyy", 5, DCH_Y_YYY, true, FROM_CHAR_DATE_GREGORIAN},	/* y */
	{"yyyy", 4, DCH_YYYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"yyy", 3, DCH_YYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"yy", 2, DCH_YY, true, FROM_CHAR_DATE_GREGORIAN},
	{"y", 1, DCH_Y, true, FROM_CHAR_DATE_GREGORIAN},

	/* last */
	{NULL, 0, 0, 0, 0}
};

/* ----------
 * KeyWords for NUMBER version
 *
 * The is_digit and date_mode fields are not relevant here.
 * ----------
 */
static const KeyWord NUM_keywords[] = {
/*	name, len, id			is in Index */
	{",", 1, NUM_COMMA},		/* , */
	{".", 1, NUM_DEC},			/* . */
	{"0", 1, NUM_0},			/* 0 */
	{"9", 1, NUM_9},			/* 9 */
	{"B", 1, NUM_B},			/* B */
	{"C", 1, NUM_C},			/* C */
	{"D", 1, NUM_D},			/* D */
	{"EEEE", 4, NUM_E},			/* E */
	{"FM", 2, NUM_FM},			/* F */
	{"G", 1, NUM_G},			/* G */
	{"L", 1, NUM_L},			/* L */
	{"MI", 2, NUM_MI},			/* M */
	{"PL", 2, NUM_PL},			/* P */
	{"PR", 2, NUM_PR},
	{"RN", 2, NUM_RN},			/* R */
	{"SG", 2, NUM_SG},			/* S */
	{"SP", 2, NUM_SP},
	{"S", 1, NUM_S},
	{"TH", 2, NUM_TH},			/* T */
	{"V", 1, NUM_V},			/* V */
	{"b", 1, NUM_B},			/* b */
	{"c", 1, NUM_C},			/* c */
	{"d", 1, NUM_D},			/* d */
	{"eeee", 4, NUM_E},			/* e */
	{"fm", 2, NUM_FM},			/* f */
	{"g", 1, NUM_G},			/* g */
	{"l", 1, NUM_L},			/* l */
	{"mi", 2, NUM_MI},			/* m */
	{"pl", 2, NUM_PL},			/* p */
	{"pr", 2, NUM_PR},
	{"rn", 2, NUM_rn},			/* r */
	{"sg", 2, NUM_SG},			/* s */
	{"sp", 2, NUM_SP},
	{"s", 1, NUM_S},
	{"th", 2, NUM_th},			/* t */
	{"v", 1, NUM_V},			/* v */

	/* last */
	{NULL, 0, 0}
};


/* ----------
 * KeyWords index for DATE-TIME version
 * ----------
 */
static const int DCH_index[KeyWord_INDEX_SIZE] = {
/*
0	1	2	3	4	5	6	7	8	9
*/
	/*---- first 0..31 chars are skipped ----*/

	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, DCH_A_D, DCH_B_C, DCH_CC, DCH_DAY, -1,
	DCH_FF1, -1, DCH_HH24, DCH_IDDD, DCH_J, -1, -1, DCH_MI, -1, DCH_OF,
	DCH_P_M, DCH_Q, DCH_RM, DCH_SSSSS, DCH_TZH, DCH_US, -1, DCH_WW, -1, DCH_Y_YYY,
	-1, -1, -1, -1, -1, -1, -1, DCH_a_d, DCH_b_c, DCH_cc,
	DCH_day, -1, DCH_ff1, -1, DCH_hh24, DCH_iddd, DCH_j, -1, -1, DCH_mi,
	-1, DCH_of, DCH_p_m, DCH_q, DCH_rm, DCH_sssss, DCH_tzh, DCH_us, -1, DCH_ww,
	-1, DCH_y_yyy, -1, -1, -1, -1

	/*---- chars over 126 are skipped ----*/
};

/* ----------
 * KeyWords index for NUMBER version
 * ----------
 */
static const int NUM_index[KeyWord_INDEX_SIZE] = {
/*
0	1	2	3	4	5	6	7	8	9
*/
	/*---- first 0..31 chars are skipped ----*/

	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, NUM_COMMA, -1, NUM_DEC, -1, NUM_0, -1,
	-1, -1, -1, -1, -1, -1, -1, NUM_9, -1, -1,
	-1, -1, -1, -1, -1, -1, NUM_B, NUM_C, NUM_D, NUM_E,
	NUM_FM, NUM_G, -1, -1, -1, -1, NUM_L, NUM_MI, -1, -1,
	NUM_PL, -1, NUM_RN, NUM_SG, NUM_TH, -1, NUM_V, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, NUM_b, NUM_c,
	NUM_d, NUM_e, NUM_fm, NUM_g, -1, -1, -1, -1, NUM_l, NUM_mi,
	-1, -1, NUM_pl, -1, NUM_rn, NUM_sg, NUM_th, -1, NUM_v, -1,
	-1, -1, -1, -1, -1, -1

	/*---- chars over 126 are skipped ----*/
};

/* ----------
 * Number processor struct
 * ----------
 */
typedef struct NUMProc
{
	bool		is_to_char;
	NUMDesc    *Num;			/* number description		*/

	int			sign,			/* '-' or '+'			*/
				sign_wrote,		/* was sign write		*/
				num_count,		/* number of write digits	*/
				num_in,			/* is inside number		*/
				num_curr,		/* current position in number	*/
				out_pre_spaces, /* spaces before first digit	*/

				read_dec,		/* to_number - was read dec. point	*/
				read_post,		/* to_number - number of dec. digit */
				read_pre;		/* to_number - number non-dec. digit */

	char	   *number,			/* string with number	*/
			   *number_p,		/* pointer to current number position */
			   *inout,			/* in / out buffer	*/
			   *inout_p,		/* pointer to current inout position */
			   *last_relevant,	/* last relevant number after decimal point */

			   *L_negative_sign,	/* Locale */
			   *L_positive_sign,
			   *decimal,
			   *L_thousands_sep,
			   *L_currency_symbol;
} NUMProc;

/* Return flags for DCH_from_char() */
#define DCH_DATED	0x01
#define DCH_TIMED	0x02
#define DCH_ZONED	0x04

/*
 * These macros are used in NUM_processor() and its subsidiary routines.
 * OVERLOAD_TEST: true if we've reached end of input string
 * AMOUNT_TEST(s): true if at least s bytes remain in string
 */
#define OVERLOAD_TEST	(Np->inout_p >= Np->inout + input_len)
#define AMOUNT_TEST(s)	(Np->inout_p <= Np->inout + (input_len - (s)))


/* ----------
 * Functions
 * ----------
 */
static const KeyWord *index_seq_search(const char *str, const KeyWord *kw,
									   const int *index);
static const KeySuffix *suff_search(const char *str, const KeySuffix *suf, int type);
static bool is_separator_char(const char *str);
static void NUMDesc_prepare(NUMDesc *num, FormatNode *n);
static void parse_format(FormatNode *node, const char *str, const KeyWord *kw,
						 const KeySuffix *suf, const int *index, uint32 flags, NUMDesc *Num);

static void DCH_to_char(FormatNode *node, bool is_interval,
						TmToChar *in, char *out, Oid collid);

#ifdef DEBUG_TO_FROM_CHAR
static void dump_index(const KeyWord *k, const int *index);
static void dump_node(FormatNode *node, int max);
#endif

static const char *get_th(char *num, int type);
static char *str_numth(char *dest, char *num, int type);
static DCHCacheEntry *DCH_cache_getnew(const char *str, bool std);
static DCHCacheEntry *DCH_cache_search(const char *str, bool std);
static DCHCacheEntry *DCH_cache_fetch(const char *str, bool std);


/* ----------
 * Fast sequential search, use index for data selection which
 * go to seq. cycle (it is very fast for unwanted strings)
 * (can't be used binary search in format parsing)
 * ----------
 */
static const KeyWord *
index_seq_search(const char *str, const KeyWord *kw, const int *index)
{
	int			poz;

	if (!KeyWord_INDEX_FILTER(*str))
		return NULL;

	if ((poz = *(index + (*str - ' '))) > -1)
	{
		const KeyWord *k = kw + poz;

		do
		{
			if (strncmp(str, k->name, k->len) == 0)
				return k;
			k++;
			if (!k->name)
				return NULL;
		} while (*str == *k->name);
	}
	return NULL;
}

static const KeySuffix *
suff_search(const char *str, const KeySuffix *suf, int type)
{
	const KeySuffix *s;

	for (s = suf; s->name != NULL; s++)
	{
		if (s->type != type)
			continue;

		if (strncmp(str, s->name, s->len) == 0)
			return s;
	}
	return NULL;
}

static bool
is_separator_char(const char *str)
{
	/* ASCII printable character, but not letter or digit */
	return (*str > 0x20 && *str < 0x7F &&
			!(*str >= 'A' && *str <= 'Z') &&
			!(*str >= 'a' && *str <= 'z') &&
			!(*str >= '0' && *str <= '9'));
}

/* ----------
 * Prepare NUMDesc (number description struct) via FormatNode struct
 * ----------
 */
static void
NUMDesc_prepare(NUMDesc *num, FormatNode *n)
{
	if (n->type != NODE_TYPE_ACTION)
		return;

	if (IS_EEEE(num) && n->key->id != NUM_E)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("\"EEEE\" must be the last pattern used")));

	switch (n->key->id)
	{
		case NUM_9:
			if (IS_BRACKET(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"9\" must be ahead of \"PR\"")));
			if (IS_MULTI(num))
			{
				++num->multi;
				break;
			}
			if (IS_DECIMAL(num))
				++num->post;
			else
				++num->pre;
			break;

		case NUM_0:
			if (IS_BRACKET(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"0\" must be ahead of \"PR\"")));
			if (!IS_ZERO(num) && !IS_DECIMAL(num))
			{
				num->flag |= NUM_F_ZERO;
				num->zero_start = num->pre + 1;
			}
			if (!IS_DECIMAL(num))
				++num->pre;
			else
				++num->post;

			num->zero_end = num->pre + num->post;
			break;

		case NUM_B:
			if (num->pre == 0 && num->post == 0 && (!IS_ZERO(num)))
				num->flag |= NUM_F_BLANK;
			break;

		case NUM_D:
			num->flag |= NUM_F_LDECIMAL;
			num->need_locale = true;
			/* FALLTHROUGH */
		case NUM_DEC:
			if (IS_DECIMAL(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("multiple decimal points")));
			if (IS_MULTI(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"V\" and decimal point together")));
			num->flag |= NUM_F_DECIMAL;
			break;

		case NUM_FM:
			num->flag |= NUM_F_FILLMODE;
			break;

		case NUM_S:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" twice")));
			if (IS_PLUS(num) || IS_MINUS(num) || IS_BRACKET(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"PL\"/\"MI\"/\"SG\"/\"PR\" together")));
			if (!IS_DECIMAL(num))
			{
				num->lsign = NUM_LSIGN_PRE;
				num->pre_lsign_num = num->pre;
				num->need_locale = true;
				num->flag |= NUM_F_LSIGN;
			}
			else if (num->lsign == NUM_LSIGN_NONE)
			{
				num->lsign = NUM_LSIGN_POST;
				num->need_locale = true;
				num->flag |= NUM_F_LSIGN;
			}
			break;

		case NUM_MI:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"MI\" together")));
			num->flag |= NUM_F_MINUS;
			if (IS_DECIMAL(num))
				num->flag |= NUM_F_MINUS_POST;
			break;

		case NUM_PL:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"PL\" together")));
			num->flag |= NUM_F_PLUS;
			if (IS_DECIMAL(num))
				num->flag |= NUM_F_PLUS_POST;
			break;

		case NUM_SG:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"SG\" together")));
			num->flag |= NUM_F_MINUS;
			num->flag |= NUM_F_PLUS;
			break;

		case NUM_PR:
			if (IS_LSIGN(num) || IS_PLUS(num) || IS_MINUS(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"PR\" and \"S\"/\"PL\"/\"MI\"/\"SG\" together")));
			num->flag |= NUM_F_BRACKET;
			break;

		case NUM_rn:
		case NUM_RN:
			if (IS_ROMAN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"RN\" twice")));
			num->flag |= NUM_F_ROMAN;
			break;

		case NUM_L:
		case NUM_G:
			num->need_locale = true;
			break;

		case NUM_V:
			if (IS_DECIMAL(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"V\" and decimal point together")));
			num->flag |= NUM_F_MULTI;
			break;

		case NUM_E:
			if (IS_EEEE(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"EEEE\" twice")));
			if (IS_BLANK(num) || IS_FILLMODE(num) || IS_LSIGN(num) ||
				IS_BRACKET(num) || IS_MINUS(num) || IS_PLUS(num) ||
				IS_ROMAN(num) || IS_MULTI(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"EEEE\" is incompatible with other formats"),
						 errdetail("\"EEEE\" may only be used together with digit and decimal point patterns.")));
			num->flag |= NUM_F_EEEE;
			break;
	}

	if (IS_ROMAN(num) &&
		(num->flag & ~(NUM_F_ROMAN | NUM_F_FILLMODE)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("\"RN\" is incompatible with other formats"),
				 errdetail("\"RN\" may only be used together with \"FM\".")));
}

/* ----------
 * Format parser, search small keywords and keyword's suffixes, and make
 * format-node tree.
 *
 * for DATE-TIME & NUMBER version
 * ----------
 */
static void
parse_format(FormatNode *node, const char *str, const KeyWord *kw,
			 const KeySuffix *suf, const int *index, uint32 flags, NUMDesc *Num)
{
	FormatNode *n;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, "to_char/number(): run parser");
#endif

	n = node;

	while (*str)
	{
		int			suffix = 0;
		const KeySuffix *s;

		/*
		 * Prefix
		 */
		if ((flags & DCH_FLAG) &&
			(s = suff_search(str, suf, SUFFTYPE_PREFIX)) != NULL)
		{
			suffix |= s->id;
			if (s->len)
				str += s->len;
		}

		/*
		 * Keyword
		 */
		if (*str && (n->key = index_seq_search(str, kw, index)) != NULL)
		{
			n->type = NODE_TYPE_ACTION;
			n->suffix = suffix;
			if (n->key->len)
				str += n->key->len;

			/*
			 * NUM version: Prepare global NUMDesc struct
			 */
			if (flags & NUM_FLAG)
				NUMDesc_prepare(Num, n);

			/*
			 * Postfix
			 */
			if ((flags & DCH_FLAG) && *str &&
				(s = suff_search(str, suf, SUFFTYPE_POSTFIX)) != NULL)
			{
				n->suffix |= s->id;
				if (s->len)
					str += s->len;
			}

			n++;
		}
		else if (*str)
		{
			int			chlen;

			if ((flags & STD_FLAG) && *str != '"')
			{
				/*
				 * Standard mode, allow only following separators: "-./,':; ".
				 * However, we support double quotes even in standard mode
				 * (see below).  This is our extension of standard mode.
				 */
				if (strchr("-./,':; ", *str) == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
							 errmsg("invalid datetime format separator: \"%s\"",
									pnstrdup(str, pg_mblen(str)))));

				if (*str == ' ')
					n->type = NODE_TYPE_SPACE;
				else
					n->type = NODE_TYPE_SEPARATOR;

				n->character[0] = *str;
				n->character[1] = '\0';
				n->key = NULL;
				n->suffix = 0;
				n++;
				str++;
			}
			else if (*str == '"')
			{
				/*
				 * Process double-quoted literal string, if any
				 */
				str++;
				while (*str)
				{
					if (*str == '"')
					{
						str++;
						break;
					}
					/* backslash quotes the next character, if any */
					if (*str == '\\' && *(str + 1))
						str++;
					chlen = pg_mblen(str);
					n->type = NODE_TYPE_CHAR;
					memcpy(n->character, str, chlen);
					n->character[chlen] = '\0';
					n->key = NULL;
					n->suffix = 0;
					n++;
					str += chlen;
				}
			}
			else
			{
				/*
				 * Outside double-quoted strings, backslash is only special if
				 * it immediately precedes a double quote.
				 */
				if (*str == '\\' && *(str + 1) == '"')
					str++;
				chlen = pg_mblen(str);

				if ((flags & DCH_FLAG) && is_separator_char(str))
					n->type = NODE_TYPE_SEPARATOR;
				else if (isspace((unsigned char) *str))
					n->type = NODE_TYPE_SPACE;
				else
					n->type = NODE_TYPE_CHAR;

				memcpy(n->character, str, chlen);
				n->character[chlen] = '\0';
				n->key = NULL;
				n->suffix = 0;
				n++;
				str += chlen;
			}
		}
	}

	n->type = NODE_TYPE_END;
	n->suffix = 0;
}

/* ----------
 * DEBUG: Dump the FormatNode Tree (debug)
 * ----------
 */
#ifdef DEBUG_TO_FROM_CHAR

#define DUMP_THth(_suf) (S_TH(_suf) ? "TH" : (S_th(_suf) ? "th" : " "))
#define DUMP_FM(_suf)	(S_FM(_suf) ? "FM" : " ")

static void
dump_node(FormatNode *node, int max)
{
	FormatNode *n;
	int			a;

	elog(DEBUG_elog_output, "to_from-char(): DUMP FORMAT");

	for (a = 0, n = node; a <= max; n++, a++)
	{
		if (n->type == NODE_TYPE_ACTION)
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_ACTION '%s'\t(%s,%s)",
				 a, n->key->name, DUMP_THth(n->suffix), DUMP_FM(n->suffix));
		else if (n->type == NODE_TYPE_CHAR)
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_CHAR '%s'",
				 a, n->character);
		else if (n->type == NODE_TYPE_END)
		{
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_END", a);
			return;
		}
		else
			elog(DEBUG_elog_output, "%d:\t unknown NODE!", a);
	}
}
#endif							/* DEBUG */

/*****************************************************************************
 *			Private utils
 *****************************************************************************/

/* ----------
 * Return ST/ND/RD/TH for simple (1..9) numbers
 * type --> 0 upper, 1 lower
 * ----------
 */
static const char *
get_th(char *num, int type)
{
	int			len = strlen(num),
				last;

	last = *(num + (len - 1));
	if (!isdigit((unsigned char) last))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("\"%s\" is not a number", num)));

	/*
	 * All "teens" (<x>1[0-9]) get 'TH/th', while <x>[02-9][123] still get
	 * 'ST/st', 'ND/nd', 'RD/rd', respectively
	 */
	if ((len > 1) && (num[len - 2] == '1'))
		last = 0;

	switch (last)
	{
		case '1':
			if (type == TH_UPPER)
				return numTH[0];
			return numth[0];
		case '2':
			if (type == TH_UPPER)
				return numTH[1];
			return numth[1];
		case '3':
			if (type == TH_UPPER)
				return numTH[2];
			return numth[2];
		default:
			if (type == TH_UPPER)
				return numTH[3];
			return numth[3];
	}
}

/* ----------
 * Convert string-number to ordinal string-number
 * type --> 0 upper, 1 lower
 * ----------
 */
static char *
str_numth(char *dest, char *num, int type)
{
	if (dest != num)
		strcpy(dest, num);
	strcat(dest, get_th(num, type));
	return dest;
}

/*****************************************************************************
 *			upper/lower/initcap functions
 *****************************************************************************/

/*
 * If the system provides the needed functions for wide-character manipulation
 * (which are all standardized by C99), then we implement upper/lower/initcap
 * using wide-character functions, if necessary.  Otherwise we use the
 * traditional <ctype.h> functions, which of course will not work as desired
 * in multibyte character sets.  Note that in either case we are effectively
 * assuming that the database character encoding matches the encoding implied
 * by LC_CTYPE.
 */

/*
 * collation-aware, wide-character-aware lower function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
str_tolower(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;
	pg_locale_t mylocale;

	if (!buff)
		return NULL;

	if (!OidIsValid(collid))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for %s function",
						"lower()"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	mylocale = pg_newlocale_from_collation(collid);

	/* C/POSIX collations use this path regardless of database encoding */
	if (mylocale->ctype_is_c)
	{
		result = asc_tolower(buff, nbytes);
	}
	else
	{
		const char *src = buff;
		size_t		srclen = nbytes;
		size_t		dstsize;
		char	   *dst;
		size_t		needed;

		/* first try buffer of equal size plus terminating NUL */
		dstsize = srclen + 1;
		dst = palloc(dstsize);

		needed = pg_strlower(dst, dstsize, src, srclen, mylocale);
		if (needed + 1 > dstsize)
		{
			/* grow buffer if needed and retry */
			dstsize = needed + 1;
			dst = repalloc(dst, dstsize);
			needed = pg_strlower(dst, dstsize, src, srclen, mylocale);
			Assert(needed + 1 <= dstsize);
		}

		Assert(dst[needed] == '\0');
		result = dst;
	}

	return result;
}

/*
 * collation-aware, wide-character-aware upper function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
str_toupper(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;
	pg_locale_t mylocale;

	if (!buff)
		return NULL;

	if (!OidIsValid(collid))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for %s function",
						"upper()"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	mylocale = pg_newlocale_from_collation(collid);

	/* C/POSIX collations use this path regardless of database encoding */
	if (mylocale->ctype_is_c)
	{
		result = asc_toupper(buff, nbytes);
	}
	else
	{
		const char *src = buff;
		size_t		srclen = nbytes;
		size_t		dstsize;
		char	   *dst;
		size_t		needed;

		/* first try buffer of equal size plus terminating NUL */
		dstsize = srclen + 1;
		dst = palloc(dstsize);

		needed = pg_strupper(dst, dstsize, src, srclen, mylocale);
		if (needed + 1 > dstsize)
		{
			/* grow buffer if needed and retry */
			dstsize = needed + 1;
			dst = repalloc(dst, dstsize);
			needed = pg_strupper(dst, dstsize, src, srclen, mylocale);
			Assert(needed + 1 <= dstsize);
		}

		Assert(dst[needed] == '\0');
		result = dst;
	}

	return result;
}

/*
 * collation-aware, wide-character-aware initcap function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
str_initcap(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;
	pg_locale_t mylocale;

	if (!buff)
		return NULL;

	if (!OidIsValid(collid))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for %s function",
						"initcap()"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	mylocale = pg_newlocale_from_collation(collid);

	/* C/POSIX collations use this path regardless of database encoding */
	if (mylocale->ctype_is_c)
	{
		result = asc_initcap(buff, nbytes);
	}
	else
	{
		const char *src = buff;
		size_t		srclen = nbytes;
		size_t		dstsize;
		char	   *dst;
		size_t		needed;

		/* first try buffer of equal size plus terminating NUL */
		dstsize = srclen + 1;
		dst = palloc(dstsize);

		needed = pg_strtitle(dst, dstsize, src, srclen, mylocale);
		if (needed + 1 > dstsize)
		{
			/* grow buffer if needed and retry */
			dstsize = needed + 1;
			dst = repalloc(dst, dstsize);
			needed = pg_strtitle(dst, dstsize, src, srclen, mylocale);
			Assert(needed + 1 <= dstsize);
		}

		Assert(dst[needed] == '\0');
		result = dst;
	}

	return result;
}

/*
 * collation-aware, wide-character-aware case folding
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
str_casefold(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;
	pg_locale_t mylocale;

	if (!buff)
		return NULL;

	if (!OidIsValid(collid))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for %s function",
						"lower()"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	if (GetDatabaseEncoding() != PG_UTF8)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("Unicode case folding can only be performed if server encoding is UTF8")));

	mylocale = pg_newlocale_from_collation(collid);

	/* C/POSIX collations use this path regardless of database encoding */
	if (mylocale->ctype_is_c)
	{
		result = asc_tolower(buff, nbytes);
	}
	else
	{
		const char *src = buff;
		size_t		srclen = nbytes;
		size_t		dstsize;
		char	   *dst;
		size_t		needed;

		/* first try buffer of equal size plus terminating NUL */
		dstsize = srclen + 1;
		dst = palloc(dstsize);

		needed = pg_strfold(dst, dstsize, src, srclen, mylocale);
		if (needed + 1 > dstsize)
		{
			/* grow buffer if needed and retry */
			dstsize = needed + 1;
			dst = repalloc(dst, dstsize);
			needed = pg_strfold(dst, dstsize, src, srclen, mylocale);
			Assert(needed + 1 <= dstsize);
		}

		Assert(dst[needed] == '\0');
		result = dst;
	}

	return result;
}

/*
 * ASCII-only lower function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
asc_tolower(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
		*p = pg_ascii_tolower((unsigned char) *p);

	return result;
}

/*
 * ASCII-only upper function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
asc_toupper(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
		*p = pg_ascii_toupper((unsigned char) *p);

	return result;
}

/*
 * ASCII-only initcap function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
asc_initcap(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;
	int			wasalnum = false;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
	{
		char		c;

		if (wasalnum)
			*p = c = pg_ascii_tolower((unsigned char) *p);
		else
			*p = c = pg_ascii_toupper((unsigned char) *p);
		/* we don't trust isalnum() here */
		wasalnum = ((c >= 'A' && c <= 'Z') ||
					(c >= 'a' && c <= 'z') ||
					(c >= '0' && c <= '9'));
	}

	return result;
}

/* convenience routines for when the input is null-terminated */

static char *
str_tolower_z(const char *buff, Oid collid)
{
	return str_tolower(buff, strlen(buff), collid);
}

static char *
str_toupper_z(const char *buff, Oid collid)
{
	return str_toupper(buff, strlen(buff), collid);
}

static char *
str_initcap_z(const char *buff, Oid collid)
{
	return str_initcap(buff, strlen(buff), collid);
}

static char *
asc_tolower_z(const char *buff)
{
	return asc_tolower(buff, strlen(buff));
}

static char *
asc_toupper_z(const char *buff)
{
	return asc_toupper(buff, strlen(buff));
}

/* asc_initcap_z is not currently needed */


/* ----------
 * Skip TM / th in FROM_CHAR
 *
 * If S_THth is on, skip two chars, assuming there are two available
 * ----------
 */
#define SKIP_THth(ptr, _suf) \
	do { \
		if (S_THth(_suf)) \
		{ \
			if (*(ptr)) (ptr) += pg_mblen(ptr); \
			if (*(ptr)) (ptr) += pg_mblen(ptr); \
		} \
	} while (0)


#ifdef DEBUG_TO_FROM_CHAR
/* -----------
 * DEBUG: Call for debug and for index checking; (Show ASCII char
 * and defined keyword for each used position
 * ----------
 */
static void
dump_index(const KeyWord *k, const int *index)
{
	int			i,
				count = 0,
				free_i = 0;

	elog(DEBUG_elog_output, "TO-FROM_CHAR: Dump KeyWord Index:");

	for (i = 0; i < KeyWord_INDEX_SIZE; i++)
	{
		if (index[i] != -1)
		{
			elog(DEBUG_elog_output, "\t%c: %s, ", i + 32, k[index[i]].name);
			count++;
		}
		else
		{
			free_i++;
			elog(DEBUG_elog_output, "\t(%d) %c %d", i, i + 32, index[i]);
		}
	}
	elog(DEBUG_elog_output, "\n\t\tUsed positions: %d,\n\t\tFree positions: %d",
		 count, free_i);
}
#endif							/* DEBUG */

/* ----------
 * Process a TmToChar struct as denoted by a list of FormatNodes.
 * The formatted data is written to the string pointed to by 'out'.
 * ----------
 */
static void
DCH_to_char(FormatNode *node, bool is_interval, TmToChar *in, char *out, Oid collid)
{
	FormatNode *n;
	char	   *s;
	struct fmt_tm *tm = &in->tm;
	int			i;

	/* cache localized days and months */
	cache_locale_time();

	s = out;
	for (n = node; n->type != NODE_TYPE_END; n++)
	{
		if (n->type != NODE_TYPE_ACTION)
		{
			strcpy(s, n->character);
			s += strlen(s);
			continue;
		}

		switch (n->key->id)
		{
			case DCH_A_M:
			case DCH_P_M:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? P_M_STR : A_M_STR);
				s += strlen(s);
				break;
			case DCH_AM:
			case DCH_PM:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? PM_STR : AM_STR);
				s += strlen(s);
				break;
			case DCH_a_m:
			case DCH_p_m:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? p_m_STR : a_m_STR);
				s += strlen(s);
				break;
			case DCH_am:
			case DCH_pm:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? pm_STR : am_STR);
				s += strlen(s);
				break;
			case DCH_HH:
			case DCH_HH12:

				/*
				 * display time as shown on a 12-hour clock, even for
				 * intervals
				 */
				sprintf(s, "%0*lld", S_FM(n->suffix) ? 0 : (tm->tm_hour >= 0) ? 2 : 3,
						tm->tm_hour % (HOURS_PER_DAY / 2) == 0 ?
						(long long) (HOURS_PER_DAY / 2) :
						(long long) (tm->tm_hour % (HOURS_PER_DAY / 2)));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_HH24:
				sprintf(s, "%0*lld", S_FM(n->suffix) ? 0 : (tm->tm_hour >= 0) ? 2 : 3,
						(long long) tm->tm_hour);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_MI:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (tm->tm_min >= 0) ? 2 : 3,
						tm->tm_min);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_SS:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (tm->tm_sec >= 0) ? 2 : 3,
						tm->tm_sec);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;

#define DCH_to_char_fsec(frac_fmt, frac_val) \
				sprintf(s, frac_fmt, (int) (frac_val)); \
				if (S_THth(n->suffix)) \
					str_numth(s, s, S_TH_TYPE(n->suffix)); \
				s += strlen(s)

			case DCH_FF1:		/* tenth of second */
				DCH_to_char_fsec("%01d", in->fsec / 100000);
				break;
			case DCH_FF2:		/* hundredth of second */
				DCH_to_char_fsec("%02d", in->fsec / 10000);
				break;
			case DCH_FF3:
			case DCH_MS:		/* millisecond */
				DCH_to_char_fsec("%03d", in->fsec / 1000);
				break;
			case DCH_FF4:		/* tenth of a millisecond */
				DCH_to_char_fsec("%04d", in->fsec / 100);
				break;
			case DCH_FF5:		/* hundredth of a millisecond */
				DCH_to_char_fsec("%05d", in->fsec / 10);
				break;
			case DCH_FF6:
			case DCH_US:		/* microsecond */
				DCH_to_char_fsec("%06d", in->fsec);
				break;
#undef DCH_to_char_fsec
			case DCH_SSSS:
				sprintf(s, "%lld",
						(long long) (tm->tm_hour * SECS_PER_HOUR +
									 tm->tm_min * SECS_PER_MINUTE +
									 tm->tm_sec));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_tz:
				INVALID_FOR_INTERVAL;
				if (tmtcTzn(in))
				{
					/* We assume here that timezone names aren't localized */
					char	   *p = asc_tolower_z(tmtcTzn(in));

					strcpy(s, p);
					pfree(p);
					s += strlen(s);
				}
				break;
			case DCH_TZ:
				INVALID_FOR_INTERVAL;
				if (tmtcTzn(in))
				{
					strcpy(s, tmtcTzn(in));
					s += strlen(s);
				}
				break;
			case DCH_TZH:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%c%02d",
						(tm->tm_gmtoff >= 0) ? '+' : '-',
						abs((int) tm->tm_gmtoff) / SECS_PER_HOUR);
				s += strlen(s);
				break;
			case DCH_TZM:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%02d",
						(abs((int) tm->tm_gmtoff) % SECS_PER_HOUR) / SECS_PER_MINUTE);
				s += strlen(s);
				break;
			case DCH_OF:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%c%0*d",
						(tm->tm_gmtoff >= 0) ? '+' : '-',
						S_FM(n->suffix) ? 0 : 2,
						abs((int) tm->tm_gmtoff) / SECS_PER_HOUR);
				s += strlen(s);
				if (abs((int) tm->tm_gmtoff) % SECS_PER_HOUR != 0)
				{
					sprintf(s, ":%02d",
							(abs((int) tm->tm_gmtoff) % SECS_PER_HOUR) / SECS_PER_MINUTE);
					s += strlen(s);
				}
				break;
			case DCH_A_D:
			case DCH_B_C:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? B_C_STR : A_D_STR));
				s += strlen(s);
				break;
			case DCH_AD:
			case DCH_BC:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? BC_STR : AD_STR));
				s += strlen(s);
				break;
			case DCH_a_d:
			case DCH_b_c:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? b_c_STR : a_d_STR));
				s += strlen(s);
				break;
			case DCH_ad:
			case DCH_bc:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? bc_STR : ad_STR));
				s += strlen(s);
				break;
			case DCH_MONTH:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_full_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_toupper_z(months_full[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_Month:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_full_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							months_full[tm->tm_mon - 1]);
				s += strlen(s);
				break;
			case DCH_month:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_full_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_tolower_z(months_full[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_MON:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_abbrev_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_toupper_z(months[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_Mon:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_abbrev_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, months[tm->tm_mon - 1]);
				s += strlen(s);
				break;
			case DCH_mon:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_abbrev_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_tolower_z(months[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_MM:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (tm->tm_mon >= 0) ? 2 : 3,
						tm->tm_mon);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_DAY:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_full_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_toupper_z(days[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_Day:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_full_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							days[tm->tm_wday]);
				s += strlen(s);
				break;
			case DCH_day:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_full_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_tolower_z(days[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_DY:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_abbrev_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_toupper_z(days_short[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_Dy:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_abbrev_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, days_short[tm->tm_wday]);
				s += strlen(s);
				break;
			case DCH_dy:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_abbrev_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_tolower_z(days_short[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_DDD:
			case DCH_IDDD:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 3,
						(n->key->id == DCH_DDD) ?
						tm->tm_yday :
						date2isoyearday(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_DD:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 2, tm->tm_mday);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_D:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%d", tm->tm_wday + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_ID:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%d", (tm->tm_wday == 0) ? 7 : tm->tm_wday);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_WW:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 2,
						(tm->tm_yday - 1) / 7 + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_IW:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 2,
						date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_Q:
				if (!tm->tm_mon)
					break;
				sprintf(s, "%d", (tm->tm_mon - 1) / 3 + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_CC:
				if (is_interval)	/* straight calculation */
					i = tm->tm_year / 100;
				else
				{
					if (tm->tm_year > 0)
						/* Century 20 == 1901 - 2000 */
						i = (tm->tm_year - 1) / 100 + 1;
					else
						/* Century 6BC == 600BC - 501BC */
						i = tm->tm_year / 100 - 1;
				}
				if (i <= 99 && i >= -99)
					sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (i >= 0) ? 2 : 3, i);
				else
					sprintf(s, "%d", i);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_Y_YYY:
				i = ADJUST_YEAR(tm->tm_year, is_interval) / 1000;
				sprintf(s, "%d,%03d", i,
						ADJUST_YEAR(tm->tm_year, is_interval) - (i * 1000));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_YYYY:
			case DCH_IYYY:
				sprintf(s, "%0*d",
						S_FM(n->suffix) ? 0 :
						(ADJUST_YEAR(tm->tm_year, is_interval) >= 0) ? 4 : 5,
						(n->key->id == DCH_YYYY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_YYY:
			case DCH_IYY:
				sprintf(s, "%0*d",
						S_FM(n->suffix) ? 0 :
						(ADJUST_YEAR(tm->tm_year, is_interval) >= 0) ? 3 : 4,
						(n->key->id == DCH_YYY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)) % 1000);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_YY:
			case DCH_IY:
				sprintf(s, "%0*d",
						S_FM(n->suffix) ? 0 :
						(ADJUST_YEAR(tm->tm_year, is_interval) >= 0) ? 2 : 3,
						(n->key->id == DCH_YY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)) % 100);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_Y:
			case DCH_I:
				sprintf(s, "%1d",
						(n->key->id == DCH_Y ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)) % 10);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_RM:
				/* FALLTHROUGH */
			case DCH_rm:

				/*
				 * For intervals, values like '12 month' will be reduced to 0
				 * month and some years.  These should be processed.
				 */
				if (!tm->tm_mon && !tm->tm_year)
					break;
				else
				{
					int			mon = 0;
					const char *const *months;

					if (n->key->id == DCH_RM)
						months = rm_months_upper;
					else
						months = rm_months_lower;

					/*
					 * Compute the position in the roman-numeral array.  Note
					 * that the contents of the array are reversed, December
					 * being first and January last.
					 */
					if (tm->tm_mon == 0)
					{
						/*
						 * This case is special, and tracks the case of full
						 * interval years.
						 */
						mon = tm->tm_year >= 0 ? 0 : MONTHS_PER_YEAR - 1;
					}
					else if (tm->tm_mon < 0)
					{
						/*
						 * Negative case.  In this case, the calculation is
						 * reversed, where -1 means December, -2 November,
						 * etc.
						 */
						mon = -1 * (tm->tm_mon + 1);
					}
					else
					{
						/*
						 * Common case, with a strictly positive value.  The
						 * position in the array matches with the value of
						 * tm_mon.
						 */
						mon = MONTHS_PER_YEAR - tm->tm_mon;
					}

					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -4,
							months[mon]);
					s += strlen(s);
				}
				break;
			case DCH_W:
				sprintf(s, "%d", (tm->tm_mday - 1) / 7 + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_J:
				sprintf(s, "%d", date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
		}
	}

	*s = '\0';
}

/*
 * The invariant for DCH cache entry management is that DCHCounter is equal
 * to the maximum age value among the existing entries, and we increment it
 * whenever an access occurs.  If we approach overflow, deal with that by
 * halving all the age values, so that we retain a fairly accurate idea of
 * which entries are oldest.
 */
static inline void
DCH_prevent_counter_overflow(void)
{
	if (DCHCounter >= (INT_MAX - 1))
	{
		for (int i = 0; i < n_DCHCache; i++)
			DCHCache[i]->age >>= 1;
		DCHCounter >>= 1;
	}
}

/* select a DCHCacheEntry to hold the given format picture */
static DCHCacheEntry *
DCH_cache_getnew(const char *str, bool std)
{
	DCHCacheEntry *ent;

	/* Ensure we can advance DCHCounter below */
	DCH_prevent_counter_overflow();

	/*
	 * If cache is full, remove oldest entry (or recycle first not-valid one)
	 */
	if (n_DCHCache >= DCH_CACHE_ENTRIES)
	{
		DCHCacheEntry *old = DCHCache[0];

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "cache is full (%d)", n_DCHCache);
#endif
		if (old->valid)
		{
			for (int i = 1; i < DCH_CACHE_ENTRIES; i++)
			{
				ent = DCHCache[i];
				if (!ent->valid)
				{
					old = ent;
					break;
				}
				if (ent->age < old->age)
					old = ent;
			}
		}
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "OLD: '%s' AGE: %d", old->str, old->age);
#endif
		old->valid = false;
		strlcpy(old->str, str, DCH_CACHE_SIZE + 1);
		old->age = (++DCHCounter);
		/* caller is expected to fill format, then set valid */
		return old;
	}
	else
	{
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "NEW (%d)", n_DCHCache);
#endif
		Assert(DCHCache[n_DCHCache] == NULL);
		DCHCache[n_DCHCache] = ent = (DCHCacheEntry *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(DCHCacheEntry));
		ent->valid = false;
		strlcpy(ent->str, str, DCH_CACHE_SIZE + 1);
		ent->std = std;
		ent->age = (++DCHCounter);
		/* caller is expected to fill format, then set valid */
		++n_DCHCache;
		return ent;
	}
}

/* look for an existing DCHCacheEntry matching the given format picture */
static DCHCacheEntry *
DCH_cache_search(const char *str, bool std)
{
	/* Ensure we can advance DCHCounter below */
	DCH_prevent_counter_overflow();

	for (int i = 0; i < n_DCHCache; i++)
	{
		DCHCacheEntry *ent = DCHCache[i];

		if (ent->valid && strcmp(ent->str, str) == 0 && ent->std == std)
		{
			ent->age = (++DCHCounter);
			return ent;
		}
	}

	return NULL;
}

/* Find or create a DCHCacheEntry for the given format picture */
static DCHCacheEntry *
DCH_cache_fetch(const char *str, bool std)
{
	DCHCacheEntry *ent;

	if ((ent = DCH_cache_search(str, std)) == NULL)
	{
		/*
		 * Not in the cache, must run parser and save a new format-picture to
		 * the cache.  Do not mark the cache entry valid until parsing
		 * succeeds.
		 */
		ent = DCH_cache_getnew(str, std);

		parse_format(ent->format, str, DCH_keywords, DCH_suff, DCH_index,
					 DCH_FLAG | (std ? STD_FLAG : 0), NULL);

		ent->valid = true;
	}
	return ent;
}

/*
 * Format a date/time or interval into a string according to fmt.
 * We parse fmt into a list of FormatNodes.  This is then passed to DCH_to_char
 * for formatting.
 */
static text *
datetime_to_char_body(TmToChar *tmtc, text *fmt, bool is_interval, Oid collid)
{
	FormatNode *format;
	char	   *fmt_str,
			   *result;
	bool		incache;
	int			fmt_len;
	text	   *res;

	/*
	 * Convert fmt to C string
	 */
	fmt_str = text_to_cstring(fmt);
	fmt_len = strlen(fmt_str);

	/*
	 * Allocate workspace for result as C string
	 */
	result = palloc((fmt_len * DCH_MAX_ITEM_SIZ) + 1);
	*result = '\0';

	if (fmt_len > DCH_CACHE_SIZE)
	{
		/*
		 * Allocate new memory if format picture is bigger than static cache
		 * and do not use cache (call parser always)
		 */
		incache = false;

		format = (FormatNode *) palloc((fmt_len + 1) * sizeof(FormatNode));

		parse_format(format, fmt_str, DCH_keywords,
					 DCH_suff, DCH_index, DCH_FLAG, NULL);
	}
	else
	{
		/*
		 * Use cache buffers
		 */
		DCHCacheEntry *ent = DCH_cache_fetch(fmt_str, false);

		incache = true;
		format = ent->format;
	}

	/* The real work is here */
	DCH_to_char(format, is_interval, tmtc, result, collid);

	if (!incache)
		pfree(format);

	pfree(fmt_str);

	/* convert C-string result to TEXT format */
	res = cstring_to_text(result);

	pfree(result);
	return res;
}

/****************************************************************************
 *				Public routines
 ***************************************************************************/
PG_FUNCTION_INFO_V1(timestampandtz_to_char);
Datum timestampandtz_to_char(PG_FUNCTION_ARGS)
{
	TimestampAndTz *dt = (TimestampAndTz *)PG_GETARG_POINTER(0);
	text *fmt = PG_GETARG_TEXT_P(1), *res;
	TmToChar tmtc;
	int tz;
        struct pg_tm tt;
        struct fmt_tm *tm;
	int thisdate;
	pg_tz * tzp = NULL;
	const char * tzname = NULL;

	if ((VARSIZE(fmt) - VARHDRSZ) <= 0 || TIMESTAMP_NOT_FINITE(dt->time))
		PG_RETURN_NULL();

	/* does the argument have a valid timezone */
	if(dt->tz != 0)
	{
		tzname = tzid_to_tzname(dt->tz);
		tzp = pg_tzset(tzname);
	}
	else
	{
		PG_RETURN_NULL();
	}

	ZERO_tmtc(&tmtc);
        tm = tmtcTm(&tmtc);

	if (timestamp2tm(dt->time, &tz, &tt, &tmtcFsec(&tmtc), &tmtcTzn(&tmtc), tzp) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	thisdate = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
	tm->tm_wday = (thisdate + 1) % 7;
	tm->tm_yday = thisdate - date2j(tm->tm_year, 1, 1) + 1;

	if (!(res = datetime_to_char_body(&tmtc, fmt, false, PG_GET_COLLATION())))
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(res);
}

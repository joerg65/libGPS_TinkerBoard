/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* this implements a GPS hardware library for the Android emulator.
 * the following code should be built as a shared library that will be
 * placed into /system/lib/hw/gps.goldfish.so
 *
 * it will be loaded by the code in hardware/libhardware/hardware.c
 * which is itself called from android_location_GpsLocationProvider.cpp
 */

/*
 * Modified J. Wolff, 2017
 */

#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <math.h>
#include <time.h>

#define  LOG_TAG  "gps_zkw"
#include <cutils/log.h>
#include <cutils/sockets.h>
#include <hardware/gps.h>
#include <cutils/properties.h>
#include <android/log.h>

#define GPS_DEBUG  1
#define NMEA_DEBUG 1
#define GPS_SV_INCLUDE 1

typedef enum {
	GPS_SV = 0,
	BDS_SV = 1,
	GLONASS_SV = 2
}SV_TYPE;


#define PRN_PLUS_BDS 200
#define PRN_PLUS_GLN 64

#define GNSS_TTY "/dev/ttyACM0"
#define GNSS_SPEED B115200

#define  LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)
#define  LOGE(...)  __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,__VA_ARGS__)
#define  LOGW(...)  __android_log_print(ANDROID_LOG_WARN,LOG_TAG,__VA_ARGS__)
#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)
#define  LOGI(...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)

#if GPS_DEBUG
#  define  D(f, ...)   LOGD("%s: line = %d, " f, __func__, __LINE__, ##__VA_ARGS__)
#else
#  define  D(...)   ((void)0)
#endif

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       N M E A   T O K E N I Z E R                     *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

typedef struct {
	const char*  p;
	const char*  end;
} Token;

#define  MAX_NMEA_TOKENS  32

typedef struct {
	int     count;
	Token   tokens[ MAX_NMEA_TOKENS ];
} NmeaTokenizer;

static int
nmea_tokenizer_init( NmeaTokenizer*  t, const char*  p, const char*  end )
{
	int    count = 0;
	char*  q; 
	
	// the initial '$' is optional
	if (p < end && p[0] == '$')
		p += 1;
	
	// remove trailing newline
	if (end > p && end[-1] == '\n') {
		end -= 1;
		if (end > p && end[-1] == '\r')
			end -= 1;
	}
	
	// get rid of checksum at the end of the sentecne
	if (end >= p+3 && end[-3] == '*') {
		end -= 3;
	}
	
	while (p < end) {
		const char*  q = p;
		
		q = memchr(p, ',', end-p);
		if (q == NULL)
			q = end;
		// if (q > p) {
		// q >= p include empty token: ,,
		if (q >= p) {
			if (count < MAX_NMEA_TOKENS) {
				t->tokens[count].p   = p;
				t->tokens[count].end = q;
				count += 1;
			}
		}
		if (q < end)
			q += 1;
		
		p = q;
	}
	
	t->count = count;
	return count;
}

static Token
nmea_tokenizer_get( NmeaTokenizer*  t, int  index )
{
	Token  tok;
	static const char*  dummy = "";
	
	if (index < 0 || index >= t->count) {
		tok.p = tok.end = dummy;
	} else
		tok = t->tokens[index];
	
	return tok;
}


static int
str2int( const char*  p, const char*  end )
{
	int   result = 0;
	int   len    = end - p;
	
	for ( ; len > 0; len--, p++ )
	{
		int  c;
		
		if (p >= end)
			goto Fail;
		
		c = *p - '0';
		if ((unsigned)c >= 10)
			goto Fail;
		
		result = result*10 + c;
	}
	return  result;
	
	Fail:
	return -1;
}

static double
str2float( const char*  p, const char*  end )
{
	int   result = 0;
	int   len    = end - p;
	char  temp[16];
	
	if (len >= (int)sizeof(temp))
		return 0.;
	
	memcpy( temp, p, len );
	temp[len] = 0;
	return strtod( temp, NULL );
}

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       N M E A   P A R S E R                           *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

#define  NMEA_MAX_SIZE  83
#define  MAX_SV_PRN 256

typedef struct {
	int     pos;
	int     overflow;
	int     utc_year;
	int     utc_mon;
	int     utc_day;
	int     utc_diff;
	GpsLocation  fix;
	GpsStatus status;
	#if GPS_SV_INCLUDE
	GpsSvStatus  sv_status; 
	int		sv_num;
	int     sv_status_changed;
	int		sv_used_in_fix[MAX_SV_PRN];
	#endif
	gps_location_callback  callback;
	gps_nmea_callback nmea_callback;
	gps_status_callback status_callback;
	#if GPS_SV_INCLUDE
	gps_sv_status_callback sv_callback;
	#endif
	char    in[ NMEA_MAX_SIZE+1 ];
} NmeaReader;


static void
nmea_reader_update_utc_diff( NmeaReader*  r )
{
	time_t         now = time(NULL);
	struct tm      tm_local;
	struct tm      tm_utc;
	long           time_local, time_utc;
	
	gmtime_r( &now, &tm_utc );
	localtime_r( &now, &tm_local );
	
	time_local = tm_local.tm_sec +
	60*(tm_local.tm_min +
	60*(tm_local.tm_hour +
	24*(tm_local.tm_yday +
	365*tm_local.tm_year)));
	
	time_utc = tm_utc.tm_sec +
	60*(tm_utc.tm_min +
	60*(tm_utc.tm_hour +
	24*(tm_utc.tm_yday +
	365*tm_utc.tm_year)));
	
	r->utc_diff = time_local - time_utc;
}


static void
nmea_reader_init( NmeaReader*  r )
{
	memset( r, 0, sizeof(*r) );
	
	r->pos      = 0;
	r->overflow = 0;
	r->utc_year = -1;
	r->utc_mon  = -1;
	r->utc_day  = -1;
	r->callback = NULL;
	r->nmea_callback = NULL;
	r->status_callback = NULL;
	r->sv_status.num_svs = 0;
    r->fix.bearing  = 0.0f;
	r->fix.size = sizeof(GpsLocation);
	
	nmea_reader_update_utc_diff( r );
}

static void
nmea_reader_set_nmea_callback( NmeaReader* r, gps_nmea_callback cb)
{
	r->nmea_callback = cb;
	if(cb != NULL) {
		D("Sending nmea to new callback");
	}
}

static void
nmea_reader_set_status_callback( NmeaReader* r, gps_status_callback cb)
{
	r->status_callback = cb;
	if(cb != NULL) {
		D("Sending status to new callback");
	}
}

static void
nmea_reader_set_callback( NmeaReader*  r, gps_location_callback  cb )
{
	r->callback = cb;
	if (cb != NULL && r->fix.flags != 0) {
		D("Sending latest fix to new callback");
		r->callback( &r->fix );
		r->fix.flags = 0;
	}
}

#if GPS_SV_INCLUDE
static void
nmea_reader_set_sv_callback( NmeaReader*  r, gps_sv_status_callback  cb )
{
	r->sv_callback = cb;
	if (cb != NULL) {
		D("Sending latest sv info to new callback");
	}
}
#endif

static int
nmea_reader_update_time( NmeaReader*  r, Token  tok )
{
	int        hour, minute;
	double     seconds;
	struct tm  tm;
	time_t     fix_time;
	
	if (tok.p + 6 > tok.end)
		return -1;
	
	if (r->utc_year < 0) {
		// no date yet, get current one
		time_t  now = time(NULL);
		gmtime_r( &now, &tm );
		r->utc_year = tm.tm_year + 1900;
		r->utc_mon  = tm.tm_mon + 1;
		r->utc_day  = tm.tm_mday;
	}
	
	hour    = str2int(tok.p,   tok.p+2);
	minute  = str2int(tok.p+2, tok.p+4);
	seconds = str2float(tok.p+4, tok.end);
	
	tm.tm_hour  = hour;
	tm.tm_min   = minute;
	tm.tm_sec   = (int) seconds;
	tm.tm_year  = r->utc_year - 1900;
	tm.tm_mon   = r->utc_mon - 1;
	tm.tm_mday  = r->utc_day;
	tm.tm_isdst = -1;
	
	fix_time = mktime( &tm ) + r->utc_diff;
	r->fix.timestamp = (long long)fix_time * 1000;
	return 0;
}

static int
nmea_reader_update_date( NmeaReader*  r, Token  date, Token  time )
{
	Token  tok = date;
	int    day, mon, year;
	
	if (tok.p + 6 != tok.end) {
		D("date not properly formatted: '%.*s'", tok.end-tok.p, tok.p);
		return -1;
	}
	day  = str2int(tok.p, tok.p+2);
	mon  = str2int(tok.p+2, tok.p+4);
	year = str2int(tok.p+4, tok.p+6) + 2000;
	
	if ((day|mon|year) < 0) {
		D("date not properly formatted: '%.*s'", tok.end-tok.p, tok.p);
		return -1;
	}
	
	r->utc_year  = year;
	r->utc_mon   = mon;
	r->utc_day   = day;
	
	return nmea_reader_update_time( r, time );
}


static double
convert_from_hhmm( Token  tok )
{
	double  val     = str2float(tok.p, tok.end);
	int     degrees = (int)(floor(val) / 100);
	double  minutes = val - degrees * 100.;
	double  dcoord  = degrees + minutes / 60.0;
	return dcoord;
}


static int
nmea_reader_update_latlong( NmeaReader*  r,
							Token        latitude,
							char         latitudeHemi,
							Token        longitude,
							char         longitudeHemi )
{
	double   lat, lon;
	Token    tok;
	
	tok = latitude;
	if (tok.p + 6 > tok.end) {
		D("latitude is too short: '%.*s'", tok.end-tok.p, tok.p);
		return -1;
	}
	lat = convert_from_hhmm(tok);
	if (latitudeHemi == 'S')
		lat = -lat;
	
	tok = longitude;
	if (tok.p + 6 > tok.end) {
		D("longitude is too short: '%.*s'", tok.end-tok.p, tok.p);
		return -1;
	}
	lon = convert_from_hhmm(tok);
	if (longitudeHemi == 'W')
		lon = -lon;
	
	r->fix.flags    |= GPS_LOCATION_HAS_LAT_LONG;
	r->fix.latitude  = lat;
	r->fix.longitude = lon;
	return 0;
}


static int
nmea_reader_update_altitude( NmeaReader*  r,
							 Token        altitude,
							 Token        units )
{
	double  alt;

    D("nmea_reader_update_altitude()");

	Token   tok = altitude;
	
	if (tok.p >= tok.end) {
        D("nmea_reader_update_altitude() return -1");
		return -1;
	}
	
	r->fix.flags   |= GPS_LOCATION_HAS_ALTITUDE;
	r->fix.altitude = str2float(tok.p, tok.end);
	return 0;
}

static int
nmea_reader_update_accuracy( NmeaReader*  r,
							 Token        accuracy )
{
	double  acc;
    D("nmea_reader_update_accuracy()");
	Token   tok = accuracy;
	
	if (tok.p >= tok.end) {
        D("nmea_reader_update_accuracy() return -1");
		return -1;
	}
	
	r->fix.accuracy = str2float(tok.p, tok.end);
	
	if (r->fix.accuracy == 99.99){
		return 0;
	}
	
	r->fix.flags   |= GPS_LOCATION_HAS_ACCURACY;
	return 0;
}

static int
nmea_reader_update_bearing( NmeaReader*  r,
							Token        bearing )
{
	double  alt;
    D("nmea_reader_update_bearing()");
	Token   tok = bearing;
	
	if (tok.p >= tok.end) {
        D("nmea_reader_update_bearing() return -1");
		//workaround if sometimes the bearing information missing from gps device
        //fake the flag and let the last known bearing:
        r->fix.flags   |= GPS_LOCATION_HAS_BEARING;
		return -1;
	}
	r->fix.flags   |= GPS_LOCATION_HAS_BEARING;
	r->fix.bearing  = str2float(tok.p, tok.end);

	return 0;
}


static int
nmea_reader_update_speed( NmeaReader*  r,
						  Token        speed )
{
	double  alt;
	Token   tok = speed;
	
	if (tok.p >= tok.end)
		return -1;
	
	r->fix.flags   |= GPS_LOCATION_HAS_SPEED;
	r->fix.speed    = str2float(tok.p, tok.end) / 1.85;
	return 0;
}

static int
add_prn_plus(int prn, SV_TYPE sv_type) {    // add prn plus
	if (sv_type == BDS_SV && prn < PRN_PLUS_BDS)    
		prn += PRN_PLUS_BDS;
	else if(sv_type == GLONASS_SV && prn < PRN_PLUS_GLN)
		prn += PRN_PLUS_GLN;    
	return prn;
}


static void
nmea_reader_encode_sv_status(NmeaReader*  r) {    // encode used_in_fix flag
	int i;
	
	// if num_svs is larger than GPS_MAX_SVS, set num_svs to GPS_MAX_SVS
	if ( r->sv_status.num_svs > GPS_MAX_SVS)     
		r->sv_status.num_svs = GPS_MAX_SVS;      // this will prevent overflow crash
        D("r->sv_status.num_svs: %d", r->sv_status.num_svs);
		for ( i = 0; i < r->sv_status.num_svs; ++i)
		{
			GpsSvInfo *info = &(r->sv_status.sv_list[i]);
			int prn = info->prn;
			info->azimuth = (int)info->azimuth;	
			if (r->sv_used_in_fix[prn]) {
				info->azimuth += 720; 
			}
		}
}

static void
nmea_reader_parse( NmeaReader*  r )
{
	/* we received a complete sentence, now parse it to generate
	 * a new GPS fix...
	 */
	NmeaTokenizer  tzer[1];
	Token          tok;
	int			   sv_type;
	
	D("Received: '%.*s'", r->pos, r->in);
	if (r->pos < 9) {
		D("Too short. discarded.");
		return;
	}
	
	nmea_tokenizer_init(tzer, r->in, r->in + r->pos);
	#if NMEA_DEBUG
	{
		int  n;
		D("Found %d tokens", tzer->count);
		for (n = 0; n < tzer->count; n++) {
			Token  tok = nmea_tokenizer_get(tzer,n);
			D("%2d: '%.*s'", n, tok.end-tok.p, tok.p);
		}
	}
	#endif
	
	tok = nmea_tokenizer_get(tzer, 0);
	if (tok.p + 5 > tok.end) {
		D("sentence id '%.*s' too short, ignored.", tok.end-tok.p, tok.p);
		return;
	}    
	
	if (memcmp(tok.p, "BD", 2) == 0) {
		sv_type = BDS_SV;
		D("BDS satellites");
	}
	else if (memcmp(tok.p, "GL", 2) == 0 ) {
		sv_type = GLONASS_SV;
		D("GLONASS satellites");
	}
	else {
		sv_type = GPS_SV;
		D("GPS satellites");
	}	
	// ignore first two characters.
	tok.p += 2;
	if ( !memcmp(tok.p, "GGA", 3) ) {
		// GPS fix
		Token  tok_time          = nmea_tokenizer_get(tzer,1);
		Token  tok_latitude      = nmea_tokenizer_get(tzer,2);
		Token  tok_latitudeHemi  = nmea_tokenizer_get(tzer,3);
		Token  tok_longitude     = nmea_tokenizer_get(tzer,4);
		Token  tok_longitudeHemi = nmea_tokenizer_get(tzer,5);
		Token  tok_isPix         = nmea_tokenizer_get(tzer,6);
		Token  tok_altitude      = nmea_tokenizer_get(tzer,9);
		Token  tok_altitudeUnits = nmea_tokenizer_get(tzer,10);
		
		if (tok_isPix.p[0] > '0') {
			nmea_reader_update_time(r, tok_time);
			nmea_reader_update_latlong(r, tok_latitude,
									   tok_latitudeHemi.p[0],
							  tok_longitude,
							  tok_longitudeHemi.p[0]);
			nmea_reader_update_altitude(r, tok_altitude, tok_altitudeUnits);
		}
		
	} else if ( !memcmp(tok.p, "GSA", 3) ) {
		#if GPS_SV_INCLUDE
		
		Token  tok_fixStatus   = nmea_tokenizer_get(tzer, 2);
		int i;
		
		memset(r->sv_used_in_fix, 0, sizeof(MAX_SV_PRN));	
		
		if (tok_fixStatus.p[0] != '\0' && tok_fixStatus.p[0] != '1') {
			
			Token  tok_accuracy      = nmea_tokenizer_get(tzer, 15);
			
			nmea_reader_update_accuracy(r, tok_accuracy);   // pdop
			
			r->sv_status.used_in_fix_mask = 0ul;
			
			for (i = 3; i <= 14; ++i) {
				
				Token  tok_prn  = nmea_tokenizer_get(tzer, i);
				//int prn = add_prn_plus(str2int(tok_prn.p, tok_prn.end), sv_type);
				int prn = str2int(tok_prn.p, tok_prn.end);
				if (prn > 0 && prn < MAX_SV_PRN) {
					r->sv_used_in_fix[prn] = 1;
					r->sv_status.used_in_fix_mask |= (1ul << (prn - 1));
					D("%s: fix mask is %d", __FUNCTION__, r->sv_status.used_in_fix_mask);
				}
			}
		}
		#endif
		// do something ?
	} else if ( !memcmp(tok.p, "RMC", 3) ) {
		Token  tok_time          = nmea_tokenizer_get(tzer,1);
		Token  tok_fixStatus     = nmea_tokenizer_get(tzer,2);
		Token  tok_latitude      = nmea_tokenizer_get(tzer,3);
		Token  tok_latitudeHemi  = nmea_tokenizer_get(tzer,4);
		Token  tok_longitude     = nmea_tokenizer_get(tzer,5);
		Token  tok_longitudeHemi = nmea_tokenizer_get(tzer,6);
		Token  tok_speed         = nmea_tokenizer_get(tzer,7);
		Token  tok_bearing       = nmea_tokenizer_get(tzer,8);
		Token  tok_date          = nmea_tokenizer_get(tzer,9);
		
		D("in RMC, fixStatus=%c", tok_fixStatus.p[0]);
		if (tok_fixStatus.p[0] == 'A')
		{
			nmea_reader_update_date( r, tok_date, tok_time );
			
			nmea_reader_update_latlong( r, tok_latitude,
										tok_latitudeHemi.p[0],
							   tok_longitude,
							   tok_longitudeHemi.p[0] );
			
			nmea_reader_update_bearing( r, tok_bearing );
			nmea_reader_update_speed  ( r, tok_speed );
		}
		#if GPS_SV_INCLUDE
		//r->sv_status_changed = 1;   // update sv status when receive gps, that's last sv status.
		#endif
		
	} else if ( !memcmp(tok.p, "GSV", 3) ) {
		#if GPS_SV_INCLUDE
		Token  tok_noSatellites  = nmea_tokenizer_get(tzer, 3);
		int    noSatellites = str2int(tok_noSatellites.p, tok_noSatellites.end);
		
		if (noSatellites > 0) {
			
			Token  tok_noSentences   = nmea_tokenizer_get(tzer, 1);
			Token  tok_sentence      = nmea_tokenizer_get(tzer, 2);
			
			int sentence = str2int(tok_sentence.p, tok_sentence.end);
			int totalSentences = str2int(tok_noSentences.p, tok_noSentences.end);
			int curr;
			int i;
			
			
			if (sentence == 1) {
				r->sv_num = 0;
				r->sv_status_changed = 0;
			}
			
			curr = r->sv_status.num_svs;
			
			i = 0;
			// max 4 group sv info in one sentence
			while (i < 4 && r->sv_num < noSatellites){
				
				Token  tok_prn = nmea_tokenizer_get(tzer, i * 4 + 4);
				Token  tok_elevation = nmea_tokenizer_get(tzer, i * 4 + 5);
				Token  tok_azimuth = nmea_tokenizer_get(tzer, i * 4 + 6);
				Token  tok_snr = nmea_tokenizer_get(tzer, i * 4 + 7);
				
				if (curr >= 0 && curr < GPS_MAX_SVS) {  // prevent from overflow
					//r->sv_status.sv_list[curr].prn = add_prn_plus(str2int(tok_prn.p, tok_prn.end), sv_type);
					r->sv_status.sv_list[curr].prn = str2int(tok_prn.p, tok_prn.end);
					r->sv_status.sv_list[curr].elevation = str2float(tok_elevation.p, tok_elevation.end);
					r->sv_status.sv_list[curr].azimuth = str2float(tok_azimuth.p, tok_azimuth.end);
					r->sv_status.sv_list[curr].snr = str2float(tok_snr.p, tok_snr.end);
					
					curr += 1;
				}
				r->sv_status.num_svs += 1;
				r->sv_num += 1;
				
				i += 1;
			}
			
			if (sentence == totalSentences) {
				r->sv_status_changed = 1;
			}     
			D("GSV message with total satellites %d", noSatellites);   
			
		}
		
		#endif
	}else {
		tok.p -= 2;
		D("unknown sentence '%.*s", tok.end-tok.p, tok.p);
	}
    int check = 1;
    D("r->fix.flags: %d", r->fix.flags);
    if ((r->fix.flags & GPS_LOCATION_HAS_LAT_LONG) == 0) check = 0;
    if ((r->fix.flags & GPS_LOCATION_HAS_ACCURACY) == 0) check = 0;
    if ((r->fix.flags & GPS_LOCATION_HAS_SPEED) == 0) check = 0;
    if ((r->fix.flags & GPS_LOCATION_HAS_BEARING) == 0) check = 0;
    if ((r->fix.flags & GPS_LOCATION_HAS_ALTITUDE) == 0) check = 0;
	if (check != 0) {
		#if GPS_DEBUG
		char   temp[256];
		char*  p   = temp;
		char*  end = p + sizeof(temp);
		time_t time;
		struct tm   utc;
		
		p += snprintf( p, end-p, "sending fix" );
		if (r->fix.flags & GPS_LOCATION_HAS_LAT_LONG) {
			p += snprintf(p, end-p, " lat=%g lon=%g", r->fix.latitude, r->fix.longitude);
		}
		if (r->fix.flags & GPS_LOCATION_HAS_ALTITUDE) {
			p += snprintf(p, end-p, " altitude=%g", r->fix.altitude);
		}
		if (r->fix.flags & GPS_LOCATION_HAS_SPEED) {
			p += snprintf(p, end-p, " speed=%g", r->fix.speed);
		}
		if (r->fix.flags & GPS_LOCATION_HAS_BEARING) {
			p += snprintf(p, end-p, " bearing=%g", r->fix.bearing);
		}
		if (r->fix.flags & GPS_LOCATION_HAS_ACCURACY) {
			p += snprintf(p,end-p, " accuracy=%g", r->fix.accuracy);
		}
		
		time = r->fix.timestamp / 1000;	
		gmtime_r( &time, &utc );
		p += snprintf(p, end-p, " time=%s", asctime( &utc ));
		D("%s", temp);
		#endif
		if (r->callback) {
			r->callback( &r->fix );
			r->fix.flags = 0;
		}
		else {
			D("no callback, keeping data until needed !");
		}
	}
	#if GPS_SV_INCLUDE
	if ( r->sv_status_changed == 1 ) {
		r->sv_status_changed = 0;
		if (r->sv_callback) {            
			nmea_reader_encode_sv_status(r);            
			r->sv_callback(&r->sv_status);
			
			r->sv_status.num_svs = 0;
			memset(r->sv_used_in_fix, 0, sizeof( MAX_SV_PRN ));	
		}
		else{
			D("no sv callback, keeping data until needed !");
		}
	}
	#endif
}

/*
 * static GpsUtcTime get_system_timestamp() {
 *	struct timeval tp;
 *	gettimeofday(&tp, NULL);    // get current time
 *	GpsUtcTime t = tp.tv_sec * 1000 + tp.tv_usec / 1000;
 *	return t;
 * }
 */

static void
nmea_reader_addc( NmeaReader*  r, int  c )
{
	if (r->overflow) {
		r->overflow = (c != '\n');
		return;
	}
	
	if (r->pos >= (int) sizeof(r->in)-1 ) {
		r->overflow = 1;
		r->pos      = 0;
		return;
	}
	
	r->in[r->pos] = (char)c;
	r->pos       += 1;
	
	if (c == '\n') {
		nmea_reader_parse( r );
		if (r->nmea_callback) {
			r->nmea_callback( r->fix.timestamp, r->in, r->pos );
		}
		else {
			D("No nmea callback");
		}
		r->pos = 0;
	}
}

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       C O N N E C T I O N   S T A T E                 *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

/* commands sent to the gps thread */
enum {
	CMD_QUIT  = 0,
	CMD_START = 1,
	CMD_STOP  = 2
};

/* this is the state of our connection to the qemu_gpsd daemon */
typedef struct {
	int                     init;
	int                     fd;
	GpsCallbacks            callbacks;
	pthread_t               thread;
	int                     control[2];
	char                    device[32];
	int                     speed;
} GpsState;

static GpsState  _gps_state[1];

#if GPS_SV_INCLUDE
static char * gps_idle_on   = "$PCGDC,IDLEON,1,*1\r\n";
static char * gps_idle_off  = "$PCGDC,IDLEOFF,1,*1\r\n";
#endif

static void
gps_state_done( GpsState*  s )
{
	// tell the thread to quit, and wait for it
	char   cmd = CMD_QUIT;
	void*  dummy;
	write( s->control[0], &cmd, 1 );
	pthread_join(s->thread, &dummy);
	
	// close the control socket pair
	close( s->control[0] ); s->control[0] = -1;
	close( s->control[1] ); s->control[1] = -1;
	
	// close connection to the QEMU GPS daemon
	close( s->fd ); s->fd = -1;
	s->init = 0;
}

static void
gps_state_start( GpsState*  s )
{
	char  cmd = CMD_START;
	int   ret;
	
	do { ret=write( s->control[0], &cmd, 1 ); }
	while (ret < 0 && errno == EINTR);
	
	if (ret != 1)
		D("Could not send CMD_START command: ret=%d: %s",
		  ret, strerror(errno));
		
		#if GPS_SV_INCLUDE
		write(s->fd,gps_idle_off,strlen(gps_idle_off));
	D("%s",gps_idle_off);
	#endif
}

static void
gps_state_stop( GpsState*  s )
{
	char  cmd = CMD_STOP;
	int   ret;
	
	do { ret=write( s->control[0], &cmd, 1 ); }
	while (ret < 0 && errno == EINTR);
	
	if (ret != 1)
		D("Could not send CMD_STOP command: ret=%d: %s",
		  ret, strerror(errno));
		
		#if GPS_SV_INCLUDE
		write(s->fd,gps_idle_on,strlen(gps_idle_on));
	D("%s",gps_idle_on);
	#endif
}


static int
epoll_register( int  epoll_fd, int  fd )
{
	struct epoll_event  ev;
	int                 ret, flags;
	
	/* important: make the fd non-blocking */
	flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	
	ev.events  = EPOLLIN;
	ev.data.fd = fd;
	do {
		ret = epoll_ctl( epoll_fd, EPOLL_CTL_ADD, fd, &ev );
	} while (ret < 0 && errno == EINTR);
	return ret;
}


static int
epoll_deregister( int  epoll_fd, int  fd )
{
	int  ret;
	do {
		ret = epoll_ctl( epoll_fd, EPOLL_CTL_DEL, fd, NULL );
	} while (ret < 0 && errno == EINTR);
	return ret;
}

/* this is the main thread, it waits for commands from gps_state_start/stop and,
 * when started, messages from the QEMU GPS daemon. these are simple NMEA sentences
 * that must be parsed to be converted into GPS fixes sent to the framework
 */
static void
gps_state_thread( void*  arg )
{
	GpsState*   state = (GpsState*) arg;
	NmeaReader  reader[1];
	int         epoll_fd   = epoll_create(2);
	int         started    = 0;
	int         gps_fd     = state->fd;
	int         control_fd = state->control[1];
	int         t_sec = -1;
	
	nmea_reader_init( reader );
	
	// register control file descriptors for polling
	epoll_register( epoll_fd, control_fd );
	epoll_register( epoll_fd, gps_fd );
	
	D("gps thread running");
	
	// now loop
	for (;;) {
		struct epoll_event   events[2];
		int                  ne, nevents;
		
		nevents = epoll_wait( epoll_fd, events, 2, -1 );
		if (nevents < 0) {
			if (errno != EINTR)
				D("epoll_wait() unexpected error: %s", strerror(errno));
			continue;
		}
		D("gps thread received %d events", nevents);
		for (ne = 0; ne < nevents; ne++) {
			if ((events[ne].events & (EPOLLERR|EPOLLHUP)) != 0) {
				D("EPOLLERR or EPOLLHUP after epoll_wait() !?");
				return;
			}
			if ((events[ne].events & EPOLLIN) != 0) {
				int  fd = events[ne].data.fd;
				
				if (fd == control_fd)
				{
					char  cmd = 255;
					int   ret;
					D("gps control fd event");
					do {
						ret = read( fd, &cmd, 1 );
					} while (ret < 0 && errno == EINTR);
					
					if (cmd == CMD_QUIT) {
						D("gps thread quitting on demand");
						return;
					}
					else if (cmd == CMD_START) {
						if (!started) {
							D("gps thread starting  location_cb=%p", state->callbacks.location_cb);
							started = 1;
							nmea_reader_set_nmea_callback( reader, state->callbacks.nmea_cb );
							nmea_reader_set_callback( reader, state->callbacks.location_cb );
							nmea_reader_set_status_callback(reader, state->callbacks.status_cb);
							#if GPS_SV_INCLUDE
							nmea_reader_set_sv_callback( reader, state->callbacks.sv_status_cb );
							#endif
							if (reader->status_callback) {
								reader->status.status = GPS_STATUS_SESSION_BEGIN;
								reader->status_callback(&reader->status);
							}
						}
					}
					else if (cmd == CMD_STOP) {
						if (started) {
							D("gps thread stopping");
							started = 0;
							if (reader->status_callback) {
								reader->status.status = GPS_STATUS_SESSION_END;
								reader->status_callback(&reader->status);
							}
							nmea_reader_set_nmea_callback( reader, NULL );
							nmea_reader_set_callback( reader, NULL );
							nmea_reader_set_status_callback( reader, NULL );
							#if GPS_SV_INCLUDE
							nmea_reader_set_sv_callback( reader, NULL );
							#endif
							
						}
					}
				}
				else if (fd == gps_fd)
				{
					char  buff[128];
					D("gps fd event");
					for (;;) {
						int  nn, ret;
						
						ret = read( fd, buff, sizeof( buff ) );
						if (ret < 0) {
							if (errno == EINTR)
								continue;
							if (errno != EWOULDBLOCK)
								D("error while reading from gps daemon socket: %s:", strerror(errno));
							break;
						}
						
						D("gps fd received: %.*s bytes: %d", ret, buff, ret);
						for (nn = 0; nn < ret; nn++)
							nmea_reader_addc( reader, buff[nn] );
					}
					D("gps fd event end");
				}
				else
				{
					D("epoll_wait() returned unkown fd %d ?", fd);
				}
			}
		}
	}
}

#define MAX_NAME_LEN 32
#define CONFIG_LINE_BUFFER_SIZE 100
#define MAX_LLIST_NAME_LEN 256
#define CONF_FILE "/system/etc/gps.conf"

char gps_dev[MAX_LLIST_NAME_LEN];
int gps_speed;

int read_int_config_line(char* config_line) {    
	char prm_name[MAX_NAME_LEN];
	int val;
	sscanf(config_line, "%s %d\n", prm_name, &val);
	return val;
}

void read_str_config_line(char* config_line, char* val) {    
	char prm_name[MAX_NAME_LEN];
	sscanf(config_line, "%s %s\n", prm_name, val);
}

int read_config_file(char* config_filename) {
	FILE *fp;
	int speed = 0;
	char buf[CONFIG_LINE_BUFFER_SIZE];
	if ((fp=fopen(config_filename, "r")) == NULL) {
		fprintf(stderr, "Failed to open config file %s", gps_dev);
		return EXIT_FAILURE;
	}
	while(! feof(fp)) {
		fgets(buf, CONFIG_LINE_BUFFER_SIZE, fp);
		if (buf[0] == '#' || strlen(buf) < 4) {
			continue;
		}
		if (strstr(buf, "GPSPORT ")) {
			read_str_config_line(buf, gps_dev);
		}
		if (strstr(buf, "GPSSPEED ")) {
			speed = read_int_config_line(buf);
		}
	}
	fclose(fp);
	
	switch (speed) {
		case 9600:
			gps_speed = B9600;
			break;
		case 19200:
			gps_speed = B19200;
			break;
		case 38400:
			gps_speed = B38400;
			break;
		case 57600:
			gps_speed = B57600;
			break;
		case 115200:
			gps_speed = B115200;
			break;
		default:
			gps_speed = B9600;
			break;
	} 
	LOGI("GPSPORT = %s, SPEED = %d\n", gps_dev, speed);
	return EXIT_SUCCESS;
}

// open tty
static void
gps_state_init( GpsState*  state)
{
	struct termios termios;
	
	state->init       = 1;
	state->control[0] = -1;
	state->control[1] = -1;
	state->fd         = -1;
	
	strcpy(gps_dev, GNSS_TTY);
	gps_speed = GNSS_SPEED;
	
	if (EXIT_SUCCESS == read_config_file(CONF_FILE)) {
		D("success reading configuration");
	}
	strcpy(state->device, gps_dev);
	state->speed = gps_speed;
	
	state->fd = open(state->device, O_RDWR | O_NONBLOCK | O_NOCTTY);
	
	if (state->fd < 0) {
		LOGE("no gps detected: %d", state->fd);
		return;
	}
	D("gps uart open %s success!", state->device);
	
	struct termios cfg;    
	tcgetattr(state->fd, &cfg);
	cfmakeraw(&cfg);
	cfsetispeed(&cfg, state->speed);
	cfsetospeed(&cfg, state->speed);
	tcsetattr(state->fd, TCSANOW, &cfg);
	
	D("gps will read from %s", state->device);
	
	if ( socketpair( AF_LOCAL, SOCK_STREAM, 0, state->control ) < 0 ) {
		D("could not create thread control socket pair: %s", strerror(errno));
		goto Fail;
	}
	
	state->thread = state->callbacks.create_thread_cb( "gps_state_thread", gps_state_thread, state );    
	
	if ( !state->thread ) {
		D("could not create gps thread: %s", strerror(errno));
		goto Fail;
	}
	
	// state->callbacks = *callbacks;
	
	D("gps state initialized");
	return;
	
	Fail:
	gps_state_done( state );
}


/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       I N T E R F A C E                               *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/


static int
zkw_gps_init(GpsCallbacks* callbacks)
{
	GpsState*  s = _gps_state;
	s->callbacks = *callbacks;
	if (!s->init)
		gps_state_init(s);
	
	if (s->fd < 0)
		return -1;
	
	return 0;
}

static void
zkw_gps_cleanup(void)
{
	GpsState*  s = _gps_state;
	
	if (s->init)
		gps_state_done(s);
}

static void gps_hardware_power( int state )
{
	int fd = open( "/proc/gps", O_RDWR );
	if ( fd <= 0 ){
		D( "/proc/gps open faild, errno %d\r\n", errno );
		return;
	}
	if ( state ){
		write( fd , "1", 1 );
	}else{
		write( fd , "0", 1 );
	}
	close( fd );
}

static int
zkw_gps_start()
{
	GpsState*  s = _gps_state;
	
	if (!s->init) {
		D("%s: called with uninitialized state !!", __FUNCTION__);
		return -1;
	}
	gps_hardware_power( 1 );
	D("%s: called", __FUNCTION__);
	gps_state_start(s);
	return 0;
}


static int
zkw_gps_stop()
{
	GpsState*  s = _gps_state;
	
	if (!s->init) {
		D("%s: called with uninitialized state !!", __FUNCTION__);
		return -1;
	}
	gps_hardware_power( 0 );
	D("%s: called", __FUNCTION__);
	gps_state_stop(s);
	return 0;
}

static int
zkw_gps_inject_time(GpsUtcTime time, int64_t timeReference, int uncertainty)
{
	return 0;
}

static int
zkw_gps_inject_location(double latitude, double longitude, float accuracy)
{
	return 0;
}

static void
zkw_gps_delete_aiding_data(GpsAidingData flags)
{
	return;
}


static int 
zkw_gps_set_position_mode(GpsPositionMode mode, GpsPositionRecurrence recurrence,
						  uint32_t min_interval, uint32_t preferred_accuracy, uint32_t preferred_time)
{
	return 0;
}

static const void*
zkw_gps_get_extension(const char* name)
{
	// no extensions supported
	return NULL;
}

static const GpsInterface  zkwGpsInterface = {
	.size  = sizeof(GpsInterface),
	.init  = zkw_gps_init,
	.start = zkw_gps_start,
	.stop  = zkw_gps_stop,
	.cleanup = zkw_gps_cleanup,
	.inject_time=zkw_gps_inject_time,
	.inject_location=zkw_gps_inject_location,
	.delete_aiding_data=zkw_gps_delete_aiding_data,
	.set_position_mode=zkw_gps_set_position_mode,
	.get_extension=zkw_gps_get_extension,
};

static const GpsInterface* get_gps_interface()
{
	return &zkwGpsInterface;
}

static int open_gps(const struct hw_module_t* module, char const* name,
					struct hw_device_t** device)
{
	
	struct gps_device_t *dev = malloc(sizeof(struct gps_device_t));
	memset(dev, 0, sizeof(*dev));
	
	D("Zkw hal driver, version=%d.%d", module->version_major, module->version_minor); 
	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t*)module;
	dev->get_gps_interface = get_gps_interface;
	_gps_state->init = 0;
	D("Zkw hal driver opened.");
	*device = (struct hw_device_t*)dev;
	return 0;
}


static struct hw_module_methods_t gps_module_methods = {
	.open = open_gps
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
	.tag = HARDWARE_MODULE_TAG,
	.version_major = 3,
	.version_minor = 13,
	.id            = GPS_HARDWARE_MODULE_ID,
	.name          = "HZZKW GNSS Module",
	.author        = "Jarod Lee",
	.methods       = &gps_module_methods,
};


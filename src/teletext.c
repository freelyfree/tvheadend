/*
 *  Teletext parsing functions
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "tvhead.h"
#include "teletext.h"

/**
 *
 */
typedef struct tt_mag {
  int ttm_curpage;
  uint8_t ttm_page[23*40 + 1];
} tt_mag_t;


/**
 *
 */
typedef struct tt_private {
  tt_mag_t ttp_mags[8];

  int ttp_rundown_valid;
  uint8_t ttp_rundown[23*40 + 1];

} tt_private_t;


static void teletext_rundown_copy(tt_private_t *ttp, tt_mag_t *ttm);

static void teletext_rundown_scan(th_transport_t *t, tt_private_t *ttp);

#define bitreverse(b) \
(((b) * 0x0202020202ULL & 0x010884422010ULL) % 1023)

static const uint8_t hamtable[] = {
  0x01, 0xff, 0x81, 0x01, 0xff, 0x00, 0x01, 0xff, 
  0xff, 0x02, 0x01, 0xff, 0x0a, 0xff, 0xff, 0x07, 
  0xff, 0x00, 0x01, 0xff, 0x00, 0x80, 0xff, 0x00, 
  0x06, 0xff, 0xff, 0x0b, 0xff, 0x00, 0x03, 0xff, 
  0xff, 0x0c, 0x01, 0xff, 0x04, 0xff, 0xff, 0x07, 
  0x06, 0xff, 0xff, 0x07, 0xff, 0x07, 0x07, 0x87, 
  0x06, 0xff, 0xff, 0x05, 0xff, 0x00, 0x0d, 0xff, 
  0x86, 0x06, 0x06, 0xff, 0x06, 0xff, 0xff, 0x07, 
  0xff, 0x02, 0x01, 0xff, 0x04, 0xff, 0xff, 0x09, 
  0x02, 0x82, 0xff, 0x02, 0xff, 0x02, 0x03, 0xff, 
  0x08, 0xff, 0xff, 0x05, 0xff, 0x00, 0x03, 0xff, 
  0xff, 0x02, 0x03, 0xff, 0x03, 0xff, 0x83, 0x03, 
  0x04, 0xff, 0xff, 0x05, 0x84, 0x04, 0x04, 0xff, 
  0xff, 0x02, 0x0f, 0xff, 0x04, 0xff, 0xff, 0x07, 
  0xff, 0x05, 0x05, 0x85, 0x04, 0xff, 0xff, 0x05, 
  0x06, 0xff, 0xff, 0x05, 0xff, 0x0e, 0x03, 0xff, 
  0xff, 0x0c, 0x01, 0xff, 0x0a, 0xff, 0xff, 0x09, 
  0x0a, 0xff, 0xff, 0x0b, 0x8a, 0x0a, 0x0a, 0xff, 
  0x08, 0xff, 0xff, 0x0b, 0xff, 0x00, 0x0d, 0xff, 
  0xff, 0x0b, 0x0b, 0x8b, 0x0a, 0xff, 0xff, 0x0b, 
  0x0c, 0x8c, 0xff, 0x0c, 0xff, 0x0c, 0x0d, 0xff, 
  0xff, 0x0c, 0x0f, 0xff, 0x0a, 0xff, 0xff, 0x07, 
  0xff, 0x0c, 0x0d, 0xff, 0x0d, 0xff, 0x8d, 0x0d, 
  0x06, 0xff, 0xff, 0x0b, 0xff, 0x0e, 0x0d, 0xff, 
  0x08, 0xff, 0xff, 0x09, 0xff, 0x09, 0x09, 0x89, 
  0xff, 0x02, 0x0f, 0xff, 0x0a, 0xff, 0xff, 0x09, 
  0x88, 0x08, 0x08, 0xff, 0x08, 0xff, 0xff, 0x09, 
  0x08, 0xff, 0xff, 0x0b, 0xff, 0x0e, 0x03, 0xff, 
  0xff, 0x0c, 0x0f, 0xff, 0x04, 0xff, 0xff, 0x09, 
  0x0f, 0xff, 0x8f, 0x0f, 0xff, 0x0e, 0x0f, 0xff, 
  0x08, 0xff, 0xff, 0x05, 0xff, 0x0e, 0x0d, 0xff, 
  0xff, 0x0e, 0x0f, 0xff, 0x0e, 0x8e, 0xff, 0x0e, 
};

static uint8_t
ham_decode(uint8_t a, uint8_t b)
{
  a = hamtable[a];
  b = hamtable[b];

  return (b << 4) | (a & 0xf);
}

/**
 *
 */
static time_t
tt_construct_unix_time(uint8_t *buf)
{
  time_t t, r[3], v[3];
  int i;
  struct tm tm;

  t = dispatch_clock;
  localtime_r(&t, &tm);

  tm.tm_hour = atoi((char *)buf);
  tm.tm_min = atoi((char *)buf + 3);
  tm.tm_sec = atoi((char *)buf + 6);

  r[0] = mktime(&tm);
  tm.tm_mday--;
  r[1] = mktime(&tm);
  tm.tm_mday += 2;
  r[2] = mktime(&tm);

  for(i = 0; i < 3; i++)
    v[i] = labs(r[i] - t);
  
  if(v[0] < v[1] && v[0] < v[2])
    return r[0];

  if(v[1] < v[2] && v[1] < v[0])
    return r[1];

  return r[2];
}


/**
 *
 */
static int
is_tt_clock(const uint8_t *str)
{
  return 
    isdigit(str[0]) && isdigit(str[1]) && str[2] == ':' &&
    isdigit(str[3]) && isdigit(str[4]) && str[5] == ':' &&
    isdigit(str[6]) && isdigit(str[7]);
}


/**
 *
 */
static int
update_tt_clock(th_transport_t *t, const uint8_t *buf)
{
  uint8_t str[10];
  int i;
  time_t ti;

  for(i = 0; i < 8; i++)
    str[i] = buf[i] & 0x7f;
  str[8] = 0;

  if(!is_tt_clock(str))
    return 0;

  ti = tt_construct_unix_time(str);
  if(t->tht_tt_clock == ti)
    return 0;

  t->tht_tt_clock = ti;
  //  printf("teletext clock is: %s", ctime(&ti));
  return 1;
}



/**
 *
 */
#if 0
static void
dump_page(tt_mag_t *ttm)
{
  int i, j, v;
  char buf[41];
  
  printf("------------------------------------------------\n");
  printf("------------------------------------------------\n");
  for(i = 0; i < 23; i++) {

    for(j = 0; j < 40; j++) {
      v = ttm->ttm_page[40 * i + j];
      v &= 0x7f;
      if(v < 32)
	v = ' ';
      buf[j] = v;
    }
    buf[j] = 0;
    printf("%s   | %x %x %x\n", buf,
	   ttm->ttm_page[40 * i + 0],
	   ttm->ttm_page[40 * i + 1],
	   ttm->ttm_page[40 * i + 2]);
  }
}
#endif

/**
 *
 */
static void
tt_decode_line(th_transport_t *t, th_stream_t *st, uint8_t *buf)
{
  uint8_t mpag, line, s12, s34, c;
  int page, magidx, i;
  tt_mag_t *ttm;
  tt_private_t *ttp;

  if(st->st_priv == NULL) {
    /* Allocate privdata for reassembly */
    ttp = st->st_priv = calloc(1, sizeof(tt_private_t));
  } else {
    ttp = st->st_priv;
  }


  mpag = ham_decode(buf[0], buf[1]);
  magidx = mpag & 7;
  ttm = &ttp->ttp_mags[magidx];

  line = mpag >> 3;

  if(line >= 30)
    return; /* Network lines, PDC, etc, dont worry about these */

  switch(line) {
  case 0:
    if(ttm->ttm_curpage != 0) {

      if(ttm->ttm_curpage == 192) {
	//	dump_page(ttm);
	teletext_rundown_copy(ttp, ttm);
      }

      memset(ttm->ttm_page, ' ', 23 * 40);
      ttm->ttm_curpage = 0;
    }

    if((page = ham_decode(buf[2], buf[3])) == 0xff)
      return;

    /* The page is BDC encoded, mag 0 is displayed as page 800+ */
    page = (magidx ?: 8) * 100 + (page >> 4) * 10 + (page & 0xf);

    ttm->ttm_curpage = page;

    s12 = ham_decode(buf[4], buf[5]);
    s34 = ham_decode(buf[6], buf[7]);
    c = ham_decode(buf[8], buf[9]);

    //    ttd->magazine_serial = c & 0x10 ? 1 : 0;

    if(s12 & 0x80) {
      /* Erase page */
      memset(ttm->ttm_page, ' ', 23 * 40);
    }

    if(update_tt_clock(t, buf + 34))
      teletext_rundown_scan(t, ttp);

#if 0
    printf("%02x: %s\n", s12, buf[9 - 4] & (1 << 7) ? "Erase" : "Not Erase");
    printf("Page %d: Control = %x\n", mag->page, c);
    printf("%s\n", buf[13 - 4] & (1 << 1) ? "Magser" : "");

    printf("%s\n", buf[12 - 4] & (1 << 7) ? "Inhibit" : "");
    printf("%s\n", buf[12 - 4] & (1 << 5) ? "Intr" : "");
    printf("%s\n", buf[12 - 4] & (1 << 3) ? "Update" : "");
    printf("%s\n", buf[12 - 4] & (1 << 1) ? "Supress" : "");
#endif

    break;

  case 1 ... 23:
    for(i = 0; i < 40; i++) {
      c = buf[i + 2] & 0x7f;
      if(c < 32)
	c += 0x80;
      ttm->ttm_page[i + 40 * (line - 1)] = c;
    }
    break;

  default:
    break;
  }
}


/**
 *
 */
void
teletext_input(th_transport_t *t, th_stream_t *st, uint8_t *tsb)
{
  int i, j;
  uint8_t *x, buf[42];

  x = tsb + 4;
  for(i = 0; i < 4; i++) {
    if(*x == 2) {
      for(j = 0; j < 42; j++)
	buf[j] = bitreverse(x[4 + j]);
      tt_decode_line(t, st, buf);
    }
    x += 46;
  }
}




/**
 * Swedish TV4 rundown dump (page 192)
 *

  Starttid Titel                L{ngd      | 20 83 53
 ,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,   | 94 2c 2c
  23:47:05 Reklam block         00:04:15   | 8d 83 32
                                           | 20 20 20
 ,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,   | 94 2c 2c
  23:47:30 HV3 BENGT OCH PETRA  00:00:03   | 20 86 32
  23:47:34 S-6/6 : Spoons I 6 ( 00:10:23   | 20 87 32
  23:57:57 RV3 FOLK I TRAPPAN   00:00:03   | 20 86 32
  23:59:36 Reklam block         00:02:50   | 20 83 32
  00:00:01 LINEUP13 BENGT OCH P 00:00:13   | 20 86 30
  00:00:14 AABILO6123           00:00:13   | 20 86 30
  00:00:28 S-4/6 : Allo Allo IV 00:10:28   | 20 87 30
  00:10:57 RV3 VITA RENA PRICKA 00:00:03   | 20 86 30
  00:11:00 LOKAL REKLAM         00:01:31   | 20 81 30
  00:16:37 Reklam block         00:04:25   | 20 83 30
  00:16:58 HV3 BYGGLOV 2        00:00:03   | 20 86 30
  00:17:51 Trailer block        00:01:20   | 20 82 30
  00:18:21 S-4/6 : Allo Allo IV 00:14:14   | 20 87 30
  00:32:36 AABILO6123           00:00:13   | 20 86 30
 ,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,   | 94 2c 2c
 se {ven rundown.tv4.se                    | 83 73 65
                                           | 83 20 20
                                   23:43   | 83 20 20

*/


static int
tt_time_to_len(const uint8_t *buf)
{
  int l;
  char str[10];


  memcpy(str, buf, 8);
  str[2] = 0;
  str[5] = 0;
  str[8] = 0;
  
  l = atoi(str + 0) * 3600 + atoi(str + 3) * 60 + atoi(str + 6);
  return l;
}

/*
 * Decode the Swedish TV4 teletext rundown page to figure out if we are 
 * currently in a commercial break
 */
static void
teletext_rundown_copy(tt_private_t *ttp, tt_mag_t *ttm)
{
  /* Sanity check */
  if(memcmp((char *)ttm->ttm_page + 2, "Starttid", strlen("Starttid")) ||
     memcmp((char *)ttm->ttm_page + 11,"Titel", strlen("Titel")) ||
     memcmp((char *)ttm->ttm_page + 20 * 40 + 9,
	    "rundown.tv4.se", strlen("rundown.tv4.se")))
    return;
  
  memcpy(ttp->ttp_rundown, ttm->ttm_page, 23 * 40);
  ttp->ttp_rundown_valid = 1;
}


static void
teletext_rundown_scan(th_transport_t *t, tt_private_t *ttp)
{
  int i;
  uint8_t *l;
  time_t now = t->tht_tt_clock, start, stop, last = 0;
  th_commercial_advice_t ca;

  if(ttp->ttp_rundown_valid == 0)
    return;

  for(i = 0; i < 23; i++) {
    l = ttp->ttp_rundown + 40 * i;
    if((l[1] & 0xf0) != 0x80 || !is_tt_clock(l + 32) || !is_tt_clock(l + 2))
      continue;
    
    if(!memcmp(l + 11, "Nyhetspuff", strlen("Nyhetspuff")))
      ca = COMMERCIAL_YES;
    else
      ca = (l[1] & 0xf) == 7 ? COMMERCIAL_NO : COMMERCIAL_YES;

    start = tt_construct_unix_time(l + 2);
    stop  = start + tt_time_to_len(l + 32);
    
    if(start <= now && stop > now)
      t->tht_tt_commercial_advice = ca;
    
    if(start > now && ca != t->tht_tt_commercial_advice && last == 0)
      last = start;
  }
}
/*
 * Simple MPEG/DVB parser to achieve network/service information without initial tuning data
 *
 * Copyright (C) 2006 - 2014 Winfried Koehler
 * Copyright (C) 2017 - 2018 mighty-p 
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 * Or, point your browser to http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 *
 * The project's page is https://github.com/mighty-p/t2scan
 *
 *  referred standards:
 *    ISO/IEC 13818-1
 *    ETSI EN 300 468 v1.14.1
 *    ETSI TR 101 211
 *    ETSI ETR 211
 *    ITU-T H.222.0 / ISO/IEC 13818-1
 *    http://www.eutelsat.com/satellites/pdf/Diseqc/Reference docs/bus_spec.pdf
 *
 ##############################################################################
 * This is tool is derived from the dvb scan tool,
 * Copyright: Johannes Stezenbach <js@convergence.de> and others, GPLv2 and LGPL
 * (linuxtv-dvb-apps-1.1.0/utils/scan)
 *
 * Differencies:
 * - command line options
 * - detects dvb card automatically
 * - no need for initial tuning data
 * - some adaptions for VDR syntax
 *
 * have phun, wirbel 2006/02/16
 ##############################################################################
 */

#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <assert.h>
#include <getopt.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/version.h>

#include "version.h"
#include "scan.h"
#include "dump-vdr.h"
#include "dump-xine.h"
#include "dump-dvbscan.h"
#include "dump-mplayer.h"
#include "dump-vlc-m3u.h"
#include "dump-xml.h"
#include "dvbscan.h"
#include "parse-dvbscan.h"
#include "countries.h"
#include "atsc_psip_section.h"
#include "descriptors.h"
#include "iconv_codes.h"
#include "char-coding.h"
#include "si_types.h"
#include "tools.h"

#define USE_EMUL
#ifdef USE_EMUL
#define EMUL(fname, fargs...) if (flags.emulate) fname(fargs); else
#define em_static
#else
#define EMUL(fname, fargs...)
#define em_static static
#endif

static char demux_devname[80];

struct t2scan_flags flags = {
  0,                // readback value t2scan version {YYYYMMDD}
  SCAN_TERRESTRIAL, // scan type
  0,                // scan DVB-T and DVB-T2 if type is t
  0,                // default lowest channel to scan
  133,              // default highest channel to scan
  ATSC_VSB,         // default for ATSC scan
  0,                // need 2nd generation frontend
  DE,               // country index or sat index
  1,                // tuning speed {1 = fast, 2 = medium, 3 = slow}
  0,                // filter timeout {0 = default, 1 = long} 
  0,                // default: no deduplicating // NOTE: I may change this after next release
  1,                // dump_provider, dump also provider name
  21,               // VDR version for output; 2.1+ //changed 20180330
  0,                // 0 = qam auto, 1 = search qams
  1,                // scan encrypted channels = yes
  0x0302,           // assuming DVB API version 3.2
  0,                // codepage, 0 = UTF-8
  0,                // print pmt
  0,                // emulate
};
 
static unsigned int delsys_min = 0;             // initialization of delsys loop. 0 = delsys legacy.
static unsigned int delsys_max = 0;             // initialization of delsys loop. 0 = delsys legacy.
static unsigned int modulation_min = 0;         // initialization of modulation loop. QAM64  if FE_QAM
static unsigned int modulation_max = 1;         // initialization of modulation loop. QAM256 if FE_QAM
static unsigned int dvbc_symbolrate_min = 0;    // initialization of symbolrate loop. 6900
static unsigned int dvbc_symbolrate_max = 1;    // initialization of symbolrate loop. 6875
static unsigned int freq_offset_min = 0;        // initialization of freq offset loop. 0 == offset (0), 1 == offset(+), 2 == offset(-), 3 == offset1(+), 4 == offset2(+)
static unsigned int freq_offset_max = 4;        // initialization of freq offset loop.
static int this_channellist = DVBT_EU_UHF800;   // t2scan uses by default DVB-T with UHF channels below 790 MHz.
static unsigned int ATSC_type = ATSC_VSB;       // 20090227: flag type vars shouldnt be signed. 
static unsigned int no_ATSC_PSIP = 0;           // 20090227: initialization was missing, signed -> unsigned                
static unsigned int serv_select = 3;            // 20080106: radio and tv as default (no service/other). 20090227: flag type vars shouldnt be signed. 

struct timespec start_time = { 0, 0 };

static bool bandwidth_auto                              = true;
static enum fe_spectral_inversion caps_inversion        = INVERSION_AUTO;
static enum fe_code_rate caps_fec                       = FEC_AUTO;
static enum fe_modulation caps_qam                      = QAM_AUTO;
static enum fe_modulation this_qam                      = QAM_64;
//static enum fe_modulation this_atsc                     = VSB_8;
static enum fe_transmit_mode caps_transmission_mode     = TRANSMISSION_MODE_AUTO;
static enum fe_guard_interval caps_guard_interval       = GUARD_INTERVAL_AUTO;
static enum fe_hierarchy caps_hierarchy                 = HIERARCHY_AUTO;
static struct dvb_frontend_info fe_info;

enum __output_format {
  OUTPUT_VDR,
  OUTPUT_GSTREAMER,
  OUTPUT_PIDS,
  OUTPUT_XINE,
  OUTPUT_DVBSCAN_TUNING_DATA,
  OUTPUT_MPLAYER,
  OUTPUT_VLC_M3U,
  OUTPUT_XML,
};
static enum __output_format output_format = OUTPUT_VDR;

cList _scanned_transponders, * scanned_transponders = &_scanned_transponders;
cList _output_transponders, * output_transponders = &_output_transponders;
cList _new_transponders, * new_transponders = &_new_transponders;
static struct transponder * current_tp;

static void setup_filter(struct section_buf * s, const char * dmx_devname, int pid, int table_id, int table_id_ext,
                         int run_once, int segmented, uint32_t filter_flags);
static void add_filter(struct section_buf * s);
static void copy_fe_params(struct transponder * dest, struct transponder * source);


// According to the DVB standards, the combination of network_id and  transport_stream_id should be unique,
// but in real life the satellite operators and broadcasters don't care enough to coordinate the numbering.
// Thus we identify TPs by frequency (scan handles only one satellite at a time).
// Further complication: Different NITs on one satellite sometimes list the same TP with slightly different
// frequencies, so we have to search within some bandwidth.
struct transponder * alloc_transponder(uint32_t frequency, unsigned delsys, uint8_t polarization) {
  struct transponder * tn;
  struct transponder * t = calloc(1, sizeof(* t));
  bool   known = false;
  char   name[20];
  struct cell* cell;

  t->source = 0;
  t->frequency = frequency;
  t->locks_with_params = false;
  t->delsys = delsys;
  t->polarization =polarization;

  switch(delsys) {
     case SYS_DVBT:
     case SYS_DVBT2:
        t->type = SCAN_TERRESTRIAL;
        break;
     case SYS_ATSC:
        t->type = SCAN_TERRCABLE_ATSC;
        break;
     case SYS_DVBC_ANNEX_A:
     case SYS_DVBC_ANNEX_C:
        t->type = SCAN_CABLE;
        break;
     default:
        t->type = SCAN_SATELLITE;
     }      

  // save current freq to alternative list of freqs.
  sprintf(name, "cells_%u", frequency);
  t->cells = &(t->_cells);
  NewList(t->cells, name);
  cell = calloc(1, sizeof(struct cell));
  cell->center_frequencies[cell->num_center_frequencies++] = frequency;
  AddItem(t->cells, cell);

  sprintf(name, "services_%u", frequency);
  t->services = &(t->_services);
  NewList(t->services, name);

  t->network_name = NULL;  

  if (frequency > 0) { //dont check, if we dont yet know freq.
     for(tn = new_transponders->first; tn; tn = tn->next) {
        if (tn->delsys != t->delsys)
           continue;
        if (tn->frequency == frequency) {
           if ((t->type == SCAN_SATELLITE) && (polarization != tn->polarization))
              continue;
           known = true;
           break;
           }
        }
     }

  if (known == false) {
     AddItem(new_transponders, t);
     }
  return t;
}

const char * scantype_to_text(scantype_t scantype) {
  switch(scantype) {
     case SCAN_CABLE:          return "CABLE";
     case SCAN_TERRESTRIAL:    return "TERRESTRIAL";
     case SCAN_TERRCABLE_ATSC: return "TERRCABLE_ATSC";
     default: return "UNKNOWN";
     }
}

void print_transponder(char * dest, struct transponder * t) {
  char plp_id[5];
  memset(plp_id, 0, sizeof(plp_id));
 
  switch(t->type) {
     case SCAN_TERRESTRIAL:
        if (t->delsys == SYS_DVBT2)
           snprintf(&plp_id[0], sizeof(plp_id), "P%d", t->plp_id);

        sprintf(dest, "%-8s f = %6d kHz I%sB%sC%sD%sT%sG%sY%s%s (%u:%u:%u)",
                modulation_name(t->modulation),
                freq_scale(t->frequency, 1e-3),
                vdr_inversion_name(t->inversion),
                vdr_bandwidth_name(t->bandwidth),
                vdr_fec_name(t->coderate),
                vdr_fec_name(t->coderate_LP),
                vdr_transmission_mode_name(t->transmission),
                vdr_guard_name(t->guard),
                vdr_hierarchy_name(t->hierarchy),
                &plp_id[0],
                t->original_network_id,
                t->network_id,
                t->transport_stream_id);
        break;
     case SCAN_TERRCABLE_ATSC:
        sprintf(dest, "%-8s f=%d kHz (%u:%u:%u)",
                atsc_mod_to_txt(t->modulation),
                freq_scale(t->frequency, 1e-3),
                t->original_network_id,
                t->network_id,
                t->transport_stream_id);
        break;
     case SCAN_CABLE:
        sprintf(dest, "%-8s f = %d kHz S%dC%s  (%u:%u:%u)",
                modulation_name(t->modulation),
                freq_scale(t->frequency, 1e-3),
                freq_scale(t->symbolrate, 1e-3),
                vdr_fec_name(t->coderate),
                t->original_network_id,
                t->network_id,
                t->transport_stream_id);
        break;
     default:
        warning("unimplemented frontend type %d\n", t->type);
     }
}

bool fe_supports_scan(int fd, scantype_t type, struct dvb_frontend_info info) {
  struct dtv_property p[] = {{.cmd = DTV_ENUM_DELSYS }};
  struct dtv_properties cmdseq = {.num = 1, .props = p};
  bool result = false;

  if (flags.api_version >= 0x0505) {
     EMUL(em_getproperty, &cmdseq)
     if (ioctl(fd, FE_GET_PROPERTY, &cmdseq) < 0)
        return 0;

     verbose("   check %s:\n", info.name);

     for(;p[0].u.buffer.len > 0; p[0].u.buffer.len--) {
        fe_delivery_system_t delsys = p[0].u.buffer.data[p[0].u.buffer.len - 1];
        const char * dname[]  = {
           "UNDEFINED", "DVB-C ann.A", "DVB-C ann.B", "DVB-T", "DSS", "DVB-S", "DVB-S2", "DVB-H", "ISDB-T", "ISDB-S",
           "ISDB-C", "ATSC", "ATSC/MH", "DTMB", "CMMB", "DAB", "DVB-T2", "TURBO-FEC", "DVB-C ann.C" };
        verbose("           %s\n", delsys <= SYS_DVBC_ANNEX_C?dname[delsys]:"???");
        switch(type) {
           case SCAN_TERRESTRIAL:
              if (delsys == SYS_DVBT || delsys == SYS_DVBT2)
                 result = true;
              break;
           case SCAN_CABLE:
              if (delsys == SYS_DVBC_ANNEX_AC || delsys == SYS_DVBC2)
                 result = true;
              break;
           case SCAN_TERRCABLE_ATSC:
              if (delsys == SYS_ATSC)
                 result = true;
              break;
           default:;
           }
         }
     return result; // not found.           
     }
  else {
     warning("YOU ARE USING OUTDATED DVB DRIVERS.\n");
     p[0].cmd = DTV_DELIVERY_SYSTEM;
     switch(type) {
        case SCAN_TERRESTRIAL:    p[0].u.data = SYS_DVBT;          break;
        case SCAN_CABLE:          p[0].u.data = SYS_DVBC_ANNEX_AC; break;
        case SCAN_SATELLITE:      p[0].u.data = SYS_DVBS;          break;
        case SCAN_TERRCABLE_ATSC: p[0].u.data = SYS_ATSC;          break;
        default: return 0;
        }
     return (ioctl(fd, FE_SET_PROPERTY, &cmdseq) == 0);
     }
  return false; // unsupported
}

int cmp_freq_pol(void * a, void * b) {
  struct transponder * t1 = a, * t2 = b;
  if (t1->frequency > t2->frequency) return 1;
  if (t1->frequency < t2->frequency) return -1;
  if (t1->polarization > t2->polarization) return 1;
  if (t1->polarization < t2->polarization) return -1;
  return 0;
}

void bubbleSort(pList list, cmp_func compare) {
  if (compare == NULL) {
     warning("sort function not assigned.\n");
     return;
     }

  unsigned i,j;
  for(i=0; i<list->count; i++) {
     bool swapped = false;
     for(j=0; j<list->count-i-1; j++) {
        void * a = GetItem(list, j), * b = GetItem(list, j + 1);
        if (compare(a,b) > 0) {
           UnlinkItem(list, b, 0);
           InsertItem(list, b, ((pItem) a)->index);
           swapped = true;          
           }
        }
     if (! swapped) return;
   }
}

static void dump_lists(int adapter, int frontend) {
  struct transponder * t;
  struct service * s;
  int n = 0, i, index = 0;
  char sn[20];
  FILE * dest = flags.emulate ? stderr:stdout; // no fprintf output to stdout /w emul. why? :(

  if (verbosity > 4) bubbleSort(output_transponders, cmp_freq_pol);

  for(t = output_transponders->first; t; t = t->next) {
     for(s = (t->services)->first; s; s = s->next) {
        if (s->video_pid && !(serv_select & 1))
           continue;  /* no TV services */
        if (!s->video_pid &&  (s->audio_num || s->ac3_num) && !(serv_select & 2))
           continue;  /* no radio services */
        if (!s->video_pid && !(s->audio_num || s->ac3_num) && !(serv_select & 4))
           continue;  /* no data/other services */
        if (s->scrambled && (flags.ca_select == 0))
           continue; /* FTA only */
        n++;
        }
     }

  info("(time: %s) dumping lists (%d services)\n..\n", run_time(), n);

  switch(output_format) {
     case OUTPUT_VLC_M3U:
        vlc_xspf_prolog(dest, adapter, frontend, &flags);
        break;
     case OUTPUT_XML:
        xml_dump(dest, output_transponders);
        break;
     default:;
     }

  for(t = output_transponders->first; t; t = t->next) {
     if (output_format == OUTPUT_DVBSCAN_TUNING_DATA && ((t->source >> 8) == 64)) {
        dvbscan_dump_tuningdata(dest, t, index++, &flags);
        continue;
        }                        
     for(s = (t->services)->first; s; s = s->next) {
        if (!s->service_name) { // no service name in SDT                                
           snprintf(sn, sizeof(sn), "service_id %d", s->service_id);
           s->service_name = strdup(sn);
           }
        /* ':' is field separator in vdr service lists */
        for(i = 0; s->service_name[i]; i++) {
           if (s->service_name[i] == ':')
              s->service_name[i] = ' ';
           }
        for(i = 0; s->provider_name && s->provider_name[i]; i++) {
           if (s->provider_name[i] == ':')
              s->provider_name[i] = ' ';
           }
        if (s->video_pid && !(serv_select & 1))                                         // vpid, this is tv
           continue; /* no TV services */
        if (!s->video_pid &&  (s->audio_num || s->ac3_num) && !(serv_select & 2))       // no vpid, but apid or ac3pid, this is radio
           continue; /* no radio services */
        if (!s->video_pid && !(s->audio_num || s->ac3_num) && !(serv_select & 4))       // no vpid, no apid, no ac3pid, this is service/other
           continue; /* no data/other services */
        if (s->scrambled && (flags.ca_select == 0))                                     // caid, this is scrambled tv or radio
           continue; /* FTA only */
        switch(output_format) {
           case OUTPUT_VDR:
              vdr_dump_service_parameter_set(dest, s, t, &flags);
              break;
           case OUTPUT_XINE:
              xine_dump_service_parameter_set(dest, s, t, &flags);
              break;
           case OUTPUT_MPLAYER:
              mplayer_dump_service_parameter_set(dest, s, t, &flags);
              break;
           case OUTPUT_VLC_M3U:
              vlc_dump_service_parameter_set_as_xspf(dest, s, t, &flags);
              break;
           default:
              break;
           }
     }
  }
  switch(output_format) {
     case OUTPUT_VLC_M3U:
        vlc_xspf_epilog(dest);
        break;
     default:;
     }
  fflush(stderr);
  fflush(stdout);
  info("Done, scan time: %s\n", run_time());
}

int get_api_version(int frontend_fd, struct t2scan_flags * flags) {
  struct dtv_property p[] = {{.cmd = DTV_API_VERSION }};
  struct dtv_properties cmdseq = {.num = 1, .props = p};

  /* expected to fail with old drivers,
   * therefore no warning to user. 20090324 -wk
   */
  if (ioctl(frontend_fd, FE_GET_PROPERTY, &cmdseq))
     return -1;

  flags->api_version = p[0].u.data;
  return 0;
}



struct service * find_service(struct transponder * t, uint16_t service_id) {
  struct service * s;

  for(s = (t->services)->first; s; s = s->next) {
     if (s->service_id == service_id)
        return s;
     }
  return NULL;
}


static const char * usage = "\n"
  "usage: %s [options...] \n"
  "       ---filter options ---\n"
  "       -c <N>, --channel-min <N>\n"
  "               lowest channel to scan\n"
  "       -C <N>, --channel-max <N>\n"
  "               highest channel to scan\n"
  "       -t <N>, --dvbt_type <N>\n"
  "               specify DVB-T type to scan\n"
  "                 0 = DVB-T and DVB-T2 [default]\n"
  "                 1 = DVB-T only\n"
  "                 2 = DVB-T2 only\n"
  "       ---output options---\n"
  "       -o <format>, --output-format <format>\n"
  "               determine output format\n"
  "                 gstreamer = channels.conf for dvbsrc plugin\n"
  "                 mplayer   = mplayer output\n"
  "                 vdr       = channels.conf for vdr >=2.1 [default]\n"
  "                 vdr20     = channels.conf for vdr 2.0.x\n"
  "                 vlc       = VLC xspf playlist (experimental)\n"
  "                 xine      = tzap/czap/xine output\n"
  "                 xml       = w_scan XML tuning data\n"
  "       -E, --no-encrypted\n"
  "               exclude encrypted services from output\n"
  "       -s <list of services>, --output-services  <list of services>\n"
  "               specify types of services to be included in output\n"
  "                 t = include TV channels in output [default: on]\n"
  "                 r = include radio channels in output [default: on]\n"
  "                 o = include other services in output [default: off]\n"
  "               Example: \"-s tro\" includes everything in output\n"
  "               Example: \"-s t\" includes only TV channels in output\n"
  "       ---other options---\n"
  "       -V      show version of t2scan\n"
  "       -h      show this help\n"
  "       -H      show expert help (additional options)\n";

static const char * ext_opts = "%s expert help\n"
  ".................Filter Options..........\n"
  "       -L <channel-list>, --channel-list <channel-list>\n"
  "               one of the following generic channel lists for Europe:\n"
  "                  0: Europe, UHF channels below 790 MHz [default]\n"
  "                  1: Europe, UHF channels below 700 MHz\n"
  "                  2: Europe, all UHF channels\n"
  "                  3: Europe, all VHF and UHF channels\n"
  "                  4: France, specific list with offsets\n"
  "                  5: GB, specific list with offsets\n"
  "                  6: Australia\n"
  "       -Y <country>, --country <country>\n"  
  "               use settings for a specific country:\n"
  "                 DE, GB, US, AU, .., ? for list [default: auto-detect]\n"
  "       -d\n"
  "               don't scan duplicate transponders (experimental)\n"
  ".................General.................\n"
  "       -I <charset>, --charset <charset>\n"
  "               convert to charset, i.e. 'UTF-8', 'ISO-8859-15'\n"
  "               use 'iconv --list' for full list of charsets.\n"
  "       -v, --verbose\n"
  "               be more verbose (repeat for more)\n"
  "       -q, --quiet\n"
  "               be more quiet   (repeat for less)\n"
  ".................Device..................\n"
  "       -a <N>, --adapter <N>\n"
  "               use device /dev/dvb/adapterN/ [default: auto detect]\n"
  "               (also allowed: -a /dev/dvb/adapterN/frontendM)\n"
  "       -F, --long-demux-timeout\n"
  "               use long filter timeout\n"
  "       -S <N>, --lock-timeout <N>\n"
  "               tuning speed (lock timeout)\n"
  "                 1 = fastest [default]\n"
  "                 2 = medium\n"
  "                 3 = slowest\n"
//  ".................DVB-C...................\n"
//  "       -i N, --inversion N\n"
//  "               spectral inversion setting for cable TV\n"
//  "                       (0: off, 1: on, 2: auto [default])\n"
//  "       -Q N, --dvbc-modulation N\n"
//  "               set DVB-C modulation, see table:\n"
//  "                       0  = QAM64\n"
//  "                       1  = QAM256\n"
//  "                       2  = QAM128\n"
//  "               NOTE: for experienced users only!!\n"
//  "       -e N,--dvbc-extflags N\n"
//  "               extended scan flags (DVB-C only),\n"
//  "               Any combination of these flags:\n"
//  "               1 = use extended symbolrate list\n"
//  "                       enables scan of symbolrates\n"
//  "                       6111, 6250, 6790, 6811, 5900,\n"
//  "                       5000, 3450, 4000, 6950, 7000,\n"
//  "                       6952, 6956, 6956.5, 5217\n"
//  "               2 = extended QAM scan (enable QAM128)\n"
//  "                       recommended for Nethterlands and Finland\n"
//  "               NOTE: extended scan will be *slow*\n"
//  "       -S N, dvbc-symbolrate N\n"
//  "               set DVB-C symbol rate, see table:\n"
//  "                       0  = 6.9000 MSymbol/s\n"
//  "                       1  = 6.8750 MSymbol/s\n"
//  "                       2  = 6.9565 MSymbol/s\n"
//  "                       3  = 6.9560 MSymbol/s\n"
//  "                       4  = 6.9520 MSymbol/s\n"
//  "                       5  = 6.9500 MSymbol/s\n"
//  "                       6  = 6.7900 MSymbol/s\n"
//  "                       7  = 6.8110 MSymbol/s\n"
//  "                       8  = 6.2500 MSymbol/s\n"
//  "                       9  = 6.1110 MSymbol/s\n"
//  "                       10 = 6.0860 MSymbol/s\n"
//  "                       11 = 5.9000 MSymbol/s\n"
//  "                       12 = 5.4830 MSymbol/s\n"
//  "                       13 = 5.2170 MSymbol/s\n"
//  "                       14 = 5.1560 MSymbol/s\n"
//  "                       15 = 5.0000 MSymbol/s\n"
//  "                       16 = 4.0000 MSymbol/s\n"
//  "                       17 = 3.4500 MSymbol/s\n"
//  "               NOTE: for experienced users only!!\n"
  ".................ATSC (untested).........\n"
  "       -m <mode>, --scan-mode <mode>\n"
  "               t2scan supports the following scan modes:\n"
  "               t = DVB-T [default]\n"
  "               a = ATSC (vsb/qam) (UNTESTED)\n"
  "       -A <N>, --atsc_type <N>\n"
  "               specify ATSC type\n"
  "               1 = Terrestrial [default]\n"
  "               2 = Cable\n"
  "               3 = both, Terrestrial and Cable\n"
  "       -P, --use-pat\n"
  "               do not use ATSC PSIP tables for scanning\n"
  "               (but only PAT and PMT) (applies for ATSC only)\n";

/*no_argument, required_argument and optional_argument. */
static struct option long_options[] = {
    {"scan-mode"         , required_argument, NULL, 'm'},
    //---
    {"atsc_type"         , required_argument, NULL, 'A'},
    {"dvbt_type"         , required_argument, NULL, 't'},
    //---
    {"country"           , required_argument, NULL, 'Y'},
    {"channel-list"      , required_argument, NULL, 'L'},
    {"channel-min"       , required_argument, NULL, 'c'},
    {"channel-max"       , required_argument, NULL, 'C'},
    {"no-encrypted"      , no_argument      , NULL, 'E'},
    //---
    {"output-format"     , required_argument, NULL, 'o'},
    {"help"              , no_argument      , NULL, 'h'},
    //---
    {"extended-help"     , no_argument      , NULL, 'H'},
    {"charset"           , required_argument, NULL, 'I'},
    {"verbose"           , no_argument      , NULL, 'v'},
    {"debug"             , no_argument      , NULL, '!'},
    {"quiet"             , no_argument      , NULL, 'q'},
    {"adapter"           , required_argument, NULL, 'a'},
    {"long-demux-timeout", no_argument,       NULL, 'F'},
    {"output-services"   , required_argument, NULL, 's'},
    {"lock-timeout"      , required_argument, NULL, 'S'},
//    {"inversion"         , required_argument, NULL, 'i'},
//    {"dvbc-modulation"   , required_argument, NULL, 'Q'},
//    {"dvbc-extflags"     , required_argument, NULL, 'e'},
//    {"dvbc-symbolrate"   , required_argument, NULL, 'S'},
    {"use-pat"           , required_argument, NULL, 'P'},
    {NULL                , 0                , NULL,  0 },
};

cList _running_filters, * running_filters = &_running_filters;
cList _waiting_filters, * waiting_filters = &_waiting_filters;
static int n_running;
#define MAX_RUNNING 27
static struct pollfd poll_fds[MAX_RUNNING];
static struct section_buf * poll_section_bufs[MAX_RUNNING];

static void handle_sigint(int sig) {
  error("interrupted by SIGINT, dumping partial result...\n");
  dump_lists(-1, -1);
  exit(2);
}

void bad_usage(char * pname) {
  fprintf(stderr, usage, pname);
}

void ext_help(void) {
  fprintf(stderr, ext_opts, "t2scan");
}

int device_is_preferred(int caps, const char * frontend_name, uint16_t scantype) {
  int preferred = 1; // no preferrence
  /* add other good/bad cards here. */
  if (strncmp("VLSI VES1820", frontend_name, 12) == 0)
     /* bad working FF dvb-c card, known to have qam256 probs. */
     preferred = 0; // not preferred
  else if ((strncmp("Sony CXD2820R", frontend_name, 13) == 0) && (scantype != SCAN_TERRESTRIAL))
     /* Pinnacle PCTV 290e, known to have probs on cable. */
     preferred = 0; // not preferred
  else if (caps & FE_CAN_2G_MODULATION)
     /* wt2scan preferres devices which are DVB-{C,T}2 */
     preferred = 2; // preferred
  return preferred;        
}

static int dvbc_modulation(int index) {
  switch(index) {
     case 0:   return QAM_64;
     case 1:   return QAM_256;
     case 2:   return QAM_128;                        
     default:  return QAM_AUTO;
     }
}

static int dvbc_symbolrate(int index) {
  switch(index) { 
     // 8MHz, Rolloff 0.15 -> 8000000 / 1.15 -> symbolrate <= 6956521,74
     case 0:   return 6900000;  // 8MHz, 6.900MSymbol/s is mostly used for 8MHz
     case 1:   return 6875000;  // 8MHz, 6.875MSymbol/s also used quite often for 8MHz
     case 2:   return 6956500;  // 8MHz
     case 3:   return 6956000;  // 8MHz
     case 4:   return 6952000;  // 8MHz
     case 5:   return 6950000;  // 8MHz
     case 6:   return 6790000;  // 8MHz
     case 7:   return 6811000;  // 8MHz
     case 8:   return 6250000;  // 8MHz
     case 9:   return 6111000;  // 8MHz

     // 7MHz, Rolloff 0.15 -> 7000000 / 1.15 -> symbolrate <= 6086956,52
     case 10:  return 6086000;  // 8MHz, 7MHz, sort 7MHz descending by probability
     case 11:  return 5900000;  // 8MHz, 7MHz
     case 12:  return 5483000;  // 8MHz, 7MHz

     // 6MHz, Rolloff 0.15 -> 6000000 / 1.15 -> symbolrate <= 5217391,30
     case 13:  return 5217000;  // 6MHz, 7MHz, 8MHz, sort 6MHz descending by probability
     case 14:  return 5156000;  // 6MHz, 7MHz, 8MHz
     case 15:  return 5000000;  // 6MHz, 7MHz, 8MHz
     case 16:  return 4000000;  // 6MHz, 7MHz, 8MHz
     case 17:  return 3450000;  // 6MHz, 7MHz, 8MHz

     default:  return 0;
     }
}

uint16_t carrier_timeout(uint8_t delsys) {
  switch(delsys) {
     case SYS_DVBT:
     case SYS_DVBT2:
        return 2000;
     case SYS_DVBS:
     case SYS_DVBS2:
        return 2000;
     case SYS_DVBC_ANNEX_A:
     case SYS_DVBC_ANNEX_B:
     #if (SYS_DVBC_ANNEX_A != SYS_DVBC_ANNEX_C)
     case SYS_DVBC_ANNEX_C:
     #endif
        return 1500;
     default:
        return 3000;
     }
}

uint16_t lock_timeout(uint8_t delsys) {
  switch(delsys) {
     case SYS_DVBT:
     case SYS_DVBT2:
        return 4000;
     case SYS_DVBC_ANNEX_A:
     case SYS_DVBC_ANNEX_B:
     #if (SYS_DVBC_ANNEX_A != SYS_DVBC_ANNEX_C)
     case SYS_DVBC_ANNEX_C:
     #endif
        return 3000;
     default:
        return 8000;
     }
}

/* service_ids are guaranteed to be unique within one TP
 * (acc. DVB standards unique within one network, but in real life...)
 */
struct service * alloc_service(struct transponder * t, uint16_t service_id) {
  struct service * s = calloc(1, sizeof(* s));
  s->service_id = service_id;
  s->transponder = t;
  AddItem(t->services, s);
  return s;
}

static int find_descriptor(uint8_t tag, const unsigned char * buf, int descriptors_loop_len, const unsigned char ** desc,
                          int * desc_len) {
  while(descriptors_loop_len > 0) {
     unsigned char descriptor_tag = buf[0];
     unsigned char descriptor_len = buf[1] + 2;
 
     if (!descriptor_len) {
        warning("descriptor_tag == 0x%02x, len is 0\n", descriptor_tag);
        break;
        }

     if (tag == descriptor_tag) {
        if (desc)
           *desc = buf;
        if (desc_len)
           *desc_len = descriptor_len;
        return 1;
        }
 
     buf                  += descriptor_len;
     descriptors_loop_len -= descriptor_len;
  }
  return 0;
}

static void parse_descriptors(enum table_id t, const unsigned char * buf, int descriptors_loop_len, void *data,
                              scantype_t scantype) {
  while(descriptors_loop_len > 0) {
     unsigned char descriptor_tag = buf[0];
     unsigned char descriptor_len = buf[1] + 2;

     if (descriptor_len == 0) {
        debug("descriptor_tag == 0x%02x, len is 0\n", descriptor_tag);
        break;
        }

     switch(descriptor_tag) {
        case MHP_application_descriptor:
        case MHP_application_name_desriptor:
        case MHP_transport_protocol_descriptor:
        case dvb_j_application_descriptor:
        case dvb_j_application_location_descriptor:
                break;
        case ca_descriptor: /* 20080106 */
                if (t == TABLE_PMT)
                   parse_ca_descriptor(buf, data);        
                break;        
        case iso_639_language_descriptor:
                if (t == TABLE_PMT)
                   parse_iso639_language_descriptor(buf, data);
                break;
        case application_icons_descriptor:
        case carousel_identifier_descriptor:
                break;
        case network_name_descriptor:
                if (t == TABLE_NIT_ACT)
                   parse_network_name_descriptor(buf, data);
                break;
        case service_list_descriptor:
        case stuffing_descriptor:
                break;
        case satellite_delivery_system_descriptor:
                if ((scantype == SCAN_SATELLITE) && ((t == TABLE_NIT_ACT) || (t == TABLE_NIT_OTH)))
                   parse_satellite_delivery_system_descriptor(buf, data, caps_inversion);
                break;
        case cable_delivery_system_descriptor:
                if ((scantype == SCAN_CABLE) && ((t == TABLE_NIT_ACT) || (t == TABLE_NIT_OTH)))
                   parse_cable_delivery_system_descriptor(buf, data, caps_inversion);
                break;
        case vbi_data_descriptor:
        case vbi_teletext_descriptor:
        case bouquet_name_descriptor:
                break;
        case service_descriptor:
                if ((t == TABLE_SDT_ACT) || (t == TABLE_SDT_OTH))
                   parse_service_descriptor(buf, data, flags.codepage);
                break;
        case country_availability_descriptor:
        case linkage_descriptor:
        case nvod_reference_descriptor:
        case time_shifted_service_descriptor:
        case short_event_descriptor:
        case extended_event_descriptor:
        case time_shifted_event_descriptor:
        case component_descriptor:
        case mosaic_descriptor: 
        case stream_identifier_descriptor:
                break;
        case ca_identifier_descriptor:
                if ((t == TABLE_SDT_ACT) || (t == TABLE_SDT_OTH))
                   parse_ca_identifier_descriptor(buf, data);
                break;
        case content_descriptor:
        case parental_rating_descriptor:
        case teletext_descriptor:
        case telephone_descriptor:
        case local_time_offset_descriptor:
        case subtitling_descriptor:
                parse_subtitling_descriptor(buf, data);
                break;
        case terrestrial_delivery_system_descriptor:
                if ((scantype == SCAN_TERRESTRIAL) && ((t == TABLE_NIT_ACT) || (t == TABLE_NIT_OTH)))
                   parse_terrestrial_delivery_system_descriptor(buf, data, caps_inversion);
                break;
        case extension_descriptor: // 6.2.16 Extension descriptor
                switch (buf[2]) { // descriptor_tag_extension;
                   // see descriptors.h: _extended_descriptors && 300468v011101p 6.4
                   case C2_delivery_system_descriptor:
                        if ((scantype == SCAN_CABLE) && ((t == TABLE_NIT_ACT) || (t == TABLE_NIT_OTH))) {
                           parse_C2_delivery_system_descriptor(buf, data, caps_inversion);
                           }
                        break;
                   case T2_delivery_system_descriptor:
                        if ((scantype == SCAN_TERRESTRIAL) && ((t == TABLE_NIT_ACT) || (t == TABLE_NIT_OTH))) {
                           parse_T2_delivery_system_descriptor(buf, data, caps_inversion);
                           }
                        break;
                   case SH_delivery_system_descriptor:
                        if (((scantype == SCAN_SATELLITE) || (scantype == SCAN_TERRESTRIAL)) &&
                            ((t == TABLE_NIT_ACT) || (t == TABLE_NIT_OTH))) {
                           parse_SH_delivery_system_descriptor(buf, data, caps_inversion);
                           }
                        break;
                   case network_change_notify_descriptor:
                        parse_network_change_notify_descriptor(buf, &((struct transponder *) data)->network_change);
                        break;
                   // all other extended descriptors here: do nothing so far.
                   case image_icon_descriptor:
                   case cpcm_delivery_signalling_descriptor:
                   case CP_descriptor:
                   case CP_identifier_descriptor:
                   case supplementary_audio_descriptor:
                   case message_descriptor:
                   case target_region_descriptor:
                   case target_region_name_descriptor:
                   case service_relocated_descriptor:
                   case XAIT_PID_descriptor_descriptor:
                   case video_depth_range_descriptor :
                   case T2MI_descriptor:
                   default:;
                   }
                break;
        case multilingual_network_name_descriptor:
        case multilingual_bouquet_name_descriptor:
        case multilingual_service_name_descriptor:
        case multilingual_component_descriptor:
        case private_data_specifier_descriptor:
        case service_move_descriptor:
        case short_smoothing_buffer_descriptor:
                break;
        case frequency_list_descriptor:
                if ((scantype == SCAN_TERRESTRIAL) && ((t == TABLE_NIT_ACT) || (t == TABLE_NIT_OTH)))
                   parse_frequency_list_descriptor(buf, data);
                break;
        case partial_transport_stream_descriptor:
        case data_broadcast_descriptor:
        case scrambling_descriptor:
        case data_broadcast_id_descriptor:
        case transport_stream_descriptor:
        case dsng_descriptor:
        case pdc_descriptor:
        case ac3_descriptor:
        case ancillary_data_descriptor:
        case cell_list_descriptor:
        case cell_frequency_link_descriptor:
        case announcement_support_descriptor:
        case application_signalling_descriptor:
        case service_identifier_descriptor:
        case service_availability_descriptor:
        case default_authority_descriptor:
        case related_content_descriptor:
        case tva_id_descriptor:
        case content_identifier_descriptor:
        case time_slice_fec_identifier_descriptor:
        case ecm_repetition_rate_descriptor:
                break;
        case s2_satellite_delivery_system_descriptor:
                if ((scantype == SCAN_SATELLITE) && ((t == TABLE_NIT_ACT) || (t == TABLE_NIT_OTH)) &&
                   (fe_info.caps & FE_CAN_2G_MODULATION))
                   parse_S2_satellite_delivery_system_descriptor(buf, data);
                break;
        case enhanced_ac3_descriptor:
        case dts_descriptor:
        case aac_descriptor:
                break;                
        case logical_channel_descriptor:
                if ((t == TABLE_NIT_ACT) || (t == TABLE_NIT_OTH))
                   parse_logical_channel_descriptor(buf, data);
                break;
        case 0xF2: // 0xF2 Private DVB Descriptor  Premiere.de, Content Transmission Descriptor
                break;                     
        default:
                verbosedebug("skip descriptor 0x%02x\n", descriptor_tag);
        }

     buf += descriptor_len;
     descriptors_loop_len -= descriptor_len;
     }
}

em_static void parse_pmt(const unsigned char * buf, uint16_t section_length, uint16_t service_id) {
  int program_info_len;
  struct service * s;
  char msg_buf[14 * AUDIO_CHAN_MAX + 1];
  char * tmp;
  int i;

  hexdump(__FUNCTION__, buf, section_length);
  s = find_service(current_tp, service_id);
  if (s == NULL) {
     error("PMT for service_id 0x%04x was not in PAT\n", service_id);
     return;
     }

  s->pcr_pid = ((buf[0] & 0x1f) << 8) | buf[1];
  program_info_len = ((buf[2] & 0x0f) << 8) | buf[3];

  // 20080106, search PMT program info for CA Ids
  buf +=4;
  section_length -= 4;

  while(program_info_len > 0) {
     int descriptor_length = ((int)buf[1]) + 2;
     parse_descriptors(TABLE_PMT, buf, section_length, s, flags.scantype);
     buf += descriptor_length;
     section_length   -= descriptor_length;
     program_info_len -= descriptor_length;
     }

  while(section_length > 0) {
     int ES_info_len = ((buf[3] & 0x0f) << 8) | buf[4];
     int elementary_pid = ((buf[1] & 0x1f) << 8) | buf[2];

     switch(buf[0]) { // stream type
        case iso_iec_11172_video_stream:
        case iso_iec_13818_1_11172_2_video_stream:
           moreverbose("  VIDEO     : PID %d (stream type 0x%x)\n", elementary_pid, buf[0]);
           if (s->video_pid == 0) {
              s->video_pid = elementary_pid;
              s->video_stream_type = buf[0];
              }
           break;
        case iso_iec_11172_audio_stream:
        case iso_iec_13818_3_audio_stream:
           moreverbose("  AUDIO     : PID %d (stream type 0x%x)\n", elementary_pid, buf[0]);
           if (s->audio_num < AUDIO_CHAN_MAX) {
              s->audio_pid[s->audio_num] = elementary_pid;
              s->audio_stream_type[s->audio_num] = buf[0];
              s->audio_num++;
              parse_descriptors(TABLE_PMT, buf + 5, ES_info_len, s, flags.scantype);
              }
           else
              warning("more than %i audio channels, truncating\n", AUDIO_CHAN_MAX);
           break;
        case iso_iec_13818_1_private_sections:
        case iso_iec_13818_1_private_data:
           // ITU-T Rec. H.222.0 | ISO/IEC 13818-1 PES packets containing private data
           if (find_descriptor(teletext_descriptor, buf + 5, ES_info_len, NULL, NULL)) {
              moreverbose("  TELETEXT  : PID %d\n", elementary_pid);
              s->teletext_pid = elementary_pid;
              break;
              }
           else if (find_descriptor(subtitling_descriptor, buf + 5, ES_info_len, NULL, NULL)) {
              // Note: The subtitling descriptor can also signal
              // teletext subtitling, but then the teletext descriptor
              // will also be present; so we can be quite confident
              // that we catch DVB subtitling streams only here, w/o
              // parsing the descriptor.
              moreverbose("  SUBTITLING: PID %d\n", elementary_pid);
              s->subtitling_pid[s->subtitling_num++] = elementary_pid;
              break;
              }
           else if (find_descriptor(ac3_descriptor, buf + 5, ES_info_len, NULL, NULL)) {
              moreverbose("  AC3       : PID %d (stream type 0x%x)\n", elementary_pid, buf[0]);
              if (s->ac3_num < AC3_CHAN_MAX) {
                 s->ac3_pid[s->ac3_num] = elementary_pid;
                 s->ac3_stream_type[s->ac3_num] = buf[0];
                 s->ac3_num++;
                 parse_descriptors(TABLE_PMT, buf + 5, ES_info_len, s, flags.scantype);
                 }
              else
                 warning("more than %i ac3 audio channels, truncating\n", AC3_CHAN_MAX);
              break;
              }
           else if (find_descriptor(enhanced_ac3_descriptor, buf + 5, ES_info_len, NULL, NULL)) {
              moreverbose("  EAC3      : PID %d (stream type 0x%x)\n", elementary_pid, buf[0]);
              if (s->ac3_num < AC3_CHAN_MAX) {
                 s->ac3_pid[s->ac3_num] = elementary_pid;
                 s->ac3_stream_type[s->ac3_num] = buf[0];
                 s->ac3_num++;
                 parse_descriptors(TABLE_PMT, buf + 5, ES_info_len, s, flags.scantype);
                 }
              else
                 warning("more than %i eac3 audio channels, truncating\n", AC3_CHAN_MAX);
              break;
              }
           // we shouldn't reach this one, usually it should be Teletext, Subtitling or AC3 .. 
           moreverbose("  unknown private data: PID 0x%04x\n", elementary_pid);
           break;
        case iso_iec_13522_MHEG:
           //
           //MHEG-5, or ISO/IEC 13522-5, is part of a set of international standards relating to the
           //presentation of multimedia information, standardized by the Multimedia and Hypermedia Experts Group (MHEG).
           //It is most commonly used as a language to describe interactive television services.
           moreverbose("  MHEG      : PID %d\n", elementary_pid);
           break;
        case iso_iec_13818_1_Annex_A_DSM_CC:
           moreverbose("  DSM CC    : PID %d\n", elementary_pid);
           break;
        case iso_iec_13818_1_11172_1_auxiliary:
           moreverbose("  ITU-T Rec. H.222.0 | ISO/IEC 13818-1/11172-1 auxiliary : PID %d\n", elementary_pid);
           break;
        case iso_iec_13818_6_type_a_multiproto_encaps:
           moreverbose("  ISO/IEC 13818-6 Multiprotocol encapsulation    : PID %d\n", elementary_pid);
           break;
        case iso_iec_13818_6_type_b:
           // Digital storage media command and control (DSM-CC) is a toolkit for control channels associated
           // with MPEG-1 and MPEG-2 streams. It is defined in part 6 of the MPEG-2 standard (Extensions for DSM-CC).
           // DSM-CC may be used for controlling the video reception, providing features normally found
           // on VCR (fast-forward, rewind, pause, etc). It may also be used for a wide variety of other purposes
           // including packet data transport. MPEG-2 ISO/IEC 13818-6 (part 6 of the MPEG-2 standard).
           // 
           // DSM-CC defines or extends five distinct protocols:
           //  * User-User 
           //  * User-Network 
           //  * MPEG transport profiles (profiles to the standard MPEG transport protocol ISO/IEC 13818-1 to allow
           //          transmission of event, synchronization, download, and other information in the MPEG transport stream)
           //  * Download 
           //  * Switched Digital Broadcast-Channel Change Protocol (SDB/CCP)
           //         Enables a client to remotely switch from channel to channel in a broadcast environment.
           //         Used to attach a client to a continuous-feed session (CFS) or other broadcast feed. Sometimes used in pay-per-view.
           // 
           moreverbose("  DSM-CC U-N Messages : PID %d\n", elementary_pid);
           break;
        case iso_iec_13818_6_type_c://DSM-CC Stream Descriptors
           moreverbose("  ISO/IEC 13818-6 Stream Descriptors : PID %d\n", elementary_pid);
           break;
        case iso_iec_13818_6_type_d://DSM-CC Sections (any type, including private data)
           moreverbose("  ISO/IEC 13818-6 Sections (any type, including private data) : PID %d\n", elementary_pid);
           break;
        case iso_iec_13818_1_auxiliary:
           moreverbose("  ISO/IEC 13818-1 auxiliary : PID %d\n", elementary_pid);
           break;
        case iso_iec_13818_7_audio_w_ADTS_transp:
           moreverbose("  ADTS Audio Stream (usually AAC) : PID %d (stream type 0x%x)\n", elementary_pid, buf[0]);
           if ((output_format == OUTPUT_VDR) && (flags.vdr_version != 2)) // CHECK!
              break; /* not supported by VDR-1.2..1.7.?? */
           if (s->audio_num < AUDIO_CHAN_MAX) {
              s->audio_pid[s->audio_num] = elementary_pid;
              s->audio_stream_type[s->audio_num] = buf[0];
              s->audio_num++;
              parse_descriptors(TABLE_PMT, buf + 5, ES_info_len, s, flags.scantype);
              }
           else
              warning("more than %i audio channels, truncating\n", AUDIO_CHAN_MAX);
           break;
        case iso_iec_14496_2_visual:
           moreverbose("  ISO/IEC 14496-2 Visual : PID %d\n", elementary_pid);
           break;
        case iso_iec_14496_3_audio_w_LATM_transp:
           moreverbose("  ISO/IEC 14496-3 Audio with LATM transport syntax as def. in ISO/IEC 14496-3/AMD1 : PID %d (stream type 0x%x)\n", elementary_pid, buf[0]);
           if ((output_format == OUTPUT_VDR) && (flags.vdr_version != 2)) // CHECK!
              break; /* not supported by VDR-1.2..1.7.?? */
           if (s->audio_num < AUDIO_CHAN_MAX) {
              s->audio_pid[s->audio_num] = elementary_pid;
              s->audio_stream_type[s->audio_num] = buf[0];
              s->audio_num++;
              parse_descriptors(TABLE_PMT, buf + 5, ES_info_len, s, flags.scantype);
              }
           else
              warning("more than %i audio channels, truncating\n", AUDIO_CHAN_MAX);
           break;
        case iso_iec_14496_1_packet_stream_in_PES:
           moreverbose("  ISO/IEC 14496-1 SL-packetized stream or FlexMux stream carried in PES packets : PID 0x%04x\n", elementary_pid);
           break;
        case iso_iec_14496_1_packet_stream_in_14996:
           moreverbose("  ISO/IEC 14496-1 SL-packetized stream or FlexMux stream carried in ISO/IEC 14496 sections : PID 0x%04x\n", elementary_pid);
           break;
        case iso_iec_13818_6_synced_download_protocol:
           moreverbose("  ISO/IEC 13818-6 DSM-CC synchronized download protocol : PID 0x%04x\n", elementary_pid);
           break;
        case metadata_in_PES:
           moreverbose("  Metadata carried in PES packets using the Metadata Access Unit Wrapper : PID 0x%04x\n", elementary_pid);
           break;
        case metadata_in_metadata_sections:
           moreverbose("  Metadata carried in metadata_sections : PID 0x%04x\n", elementary_pid);
           break;
        case metadata_in_iso_iec_13818_6_data_carous:
           moreverbose("  Metadata carried in ISO/IEC 13818-6 (DSM-CC) Data Carousel : PID 0x%04x\n", elementary_pid);
           break;
        case metadata_in_iso_iec_13818_6_obj_carous:
           moreverbose("  Metadata carried in ISO/IEC 13818-6 (DSM-CC) Object Carousel : PID 0x%04x\n", elementary_pid);
           break;
        case metadata_in_iso_iec_13818_6_synced_dl:
           moreverbose("  Metadata carried in ISO/IEC 13818-6 Synchronized Download Protocol using the Metadata Access Unit Wrapper : PID 0x%04x\n", elementary_pid);
           break;
        case iso_iec_13818_11_IPMP_stream:
           moreverbose("  IPMP stream (defined in ISO/IEC 13818-11, MPEG-2 IPMP) : PID 0x%04x\n", elementary_pid);
           break;
        case iso_iec_14496_10_AVC_video_stream:
           moreverbose("  AVC Video stream, ITU-T Rec. H.264 | ISO/IEC 14496-10 : PID %d (stream type 0x%x)\n", elementary_pid, buf[0]);
           if (s->video_pid == 0) {
              s->video_pid = elementary_pid;
              s->video_stream_type = buf[0];
              }
           break;
        case iso_iec_23008_2_H265_video_hevc_stream:
           moreverbose("  HEVC Video stream, ITU-T Rec. H.265 | ISO/IEC 23008-1 : PID %d (stream type 0x%x)\n", elementary_pid, buf[0]);
           if (s->video_pid == 0) {
              s->video_pid = elementary_pid;
              s->video_stream_type = buf[0];
              }
           break;
        case atsc_a_52b_ac3:
           moreverbose("  AC-3 Audio per ATSC A/52B : PID %d (stream type 0x%x)\n", elementary_pid, buf[0]);
           if (s->ac3_num < AC3_CHAN_MAX) {
              s->ac3_pid[s->ac3_num] = elementary_pid;
              s->ac3_stream_type[s->ac3_num] = buf[0];
              s->ac3_num++;
              parse_descriptors(TABLE_PMT, buf + 5, ES_info_len, s, flags.scantype);
              }
           else
              warning("more than %i ac3 audio channels, truncating\n", AC3_CHAN_MAX);
           break;
        default:
           moreverbose("  OTHER     : PID %d TYPE 0x%02x\n", elementary_pid, buf[0]);
        } //END switch stream type
     buf            += ES_info_len + 5;
     section_length -= ES_info_len + 5;
     }


  tmp = msg_buf;
  tmp += sprintf(tmp, "%d (%.4s)", s->audio_pid[0], s->audio_lang[0]);

  if (s->audio_num >= AUDIO_CHAN_MAX) {
     warning("more than %i audio channels: %i, truncating to %i\n", AUDIO_CHAN_MAX-1, s->audio_num, AUDIO_CHAN_MAX);
     s->audio_num = AUDIO_CHAN_MAX;
     }
  
  for(i=1; i<s->audio_num; i++)
     tmp += sprintf(tmp, ", %d (%.4s)", s->audio_pid[i], s->audio_lang[i]);
  
  debug("tsid=%d sid=%d: %s -- %s, pmt_pid 0x%04x, vpid 0x%04x, apid %s\n",
        s->transport_stream_id,
        s->service_id,
        s->provider_name, s->service_name,
        s->pmt_pid, s->video_pid, msg_buf);
}

em_static void parse_psip_vct(const unsigned char * buf, uint16_t section_length, uint8_t table_id, uint16_t transport_stream_id) {
info("XXX parse_psip\n");
}

em_static void parse_nit(const unsigned char * buf, uint16_t section_length, uint8_t table_id, uint16_t network_id, uint32_t section_flags) {
  char buffer[128];
  int descriptors_loop_len = ((buf[0] & 0x0f) << 8) | buf[1];
  
  verbose("%s: (xxxx:%u:xxxx)\n", table_id == 0x40?"NIT(act)":"NIT(oth)", network_id);
  hexdump(__FUNCTION__, buf, section_length);

  if ((table_id == TABLE_NIT_ACT) && (current_tp->network_id != network_id)) {
     print_transponder(buffer, current_tp);
     verbose("        %s : updating network_id -> (%u:%u:%u)\n",
          buffer, current_tp->original_network_id, network_id, current_tp->transport_stream_id);
     current_tp->network_id = network_id;
     /*check_duplicate_transponders();
     if (verbosity > 1) list_transponders();*/
     }

  if (section_length < descriptors_loop_len + 4) {
     warning("section too short: network_id == 0x%04x, section_length == %i, "
             "descriptors_loop_len == %i\n", network_id, section_length, descriptors_loop_len);
     return;
     }
  // update network_name
  parse_descriptors(table_id, buf + 2, descriptors_loop_len, current_tp, flags.scantype);
  section_length -= descriptors_loop_len + 4;
  buf            += descriptors_loop_len + 4;

  while(section_length > 6) {
     struct transponder tn;  
     uint16_t transport_stream_id = (buf[0] << 8) | buf[1];
     uint16_t original_network_id = (buf[2] << 8) | buf[3];
     descriptors_loop_len =        ((buf[4] << 8) | buf[5]) & 0x0FFF;
     verbose("        ----------------------------------------------------------\n");
     verbose("        %s: (%u:%u:%u)\n",  table_id == 0x40?"NIT(act)":"NIT(oth)",
            original_network_id, network_id, transport_stream_id);
     
     if (section_length < descriptors_loop_len + 4) {
        warning("section too short: transport_stream_id %u, original_network_id %u, "
                "section_length %i, descriptors_loop_len %i\n",
             transport_stream_id, original_network_id, section_length, descriptors_loop_len);
        break;
        }

     // only use the NIT entry for current TSID, NID to get ONID and exact tuning data
     if ((current_tp->type != SCAN_TERRESTRIAL) || ((transport_stream_id == current_tp->transport_stream_id) && (network_id == current_tp->network_id))) {

        memset(&tn, 0, sizeof(tn));
        tn.type                = current_tp->type;
        tn.network_PID         = current_tp->network_PID;
        tn.network_id          = network_id;
        tn.original_network_id = original_network_id;
        tn.transport_stream_id = transport_stream_id;
        tn.network_name = NULL;

        tn.services = &tn._services;
        NewList(tn.services, "tn_services");
        tn.cells = &tn._cells;
        NewList(tn.cells, "tn_cells");


        if ((current_tp->original_network_id == original_network_id) &&
         (current_tp->transport_stream_id == transport_stream_id) &&
         (table_id == TABLE_NIT_ACT)) {
            // if we've found the current tp by onid && ts_id and update it from nit(act), use actual settings as default.      
            copy_fe_params(&tn, current_tp);   //  tn.param = current_tp->param;
        }


        parse_descriptors(table_id, buf + 6, descriptors_loop_len, &tn, flags.scantype);
        tn.source |= table_id << 8;

        current_tp->original_network_id = original_network_id;

        // we ignore the frequency, but set all other things
        //current_tp->bandwidth = tn.bandwidth;

        current_tp->coderate = tn.coderate;
        current_tp->coderate_LP = tn.coderate_LP;
        current_tp->guard = tn.guard;
        current_tp->transmission = tn.transmission;
        current_tp->hierarchy = tn.hierarchy;
        current_tp->modulation = tn.modulation;
        //plpid


     }
     
     section_length -= descriptors_loop_len + 6;
     buf            += descriptors_loop_len + 6;
  }

}

em_static void parse_sdt(const unsigned char * buf, uint16_t section_length, uint16_t transport_stream_id) {
  hexdump(__FUNCTION__, buf, section_length);
  
  buf += 3;              /*  skip original network id + reserved field */
  
  while(section_length > 4) {
      int service_id = (buf[0] << 8) | buf[1];
      int descriptors_loop_len = ((buf[3] & 0x0f) << 8) | buf[4];
      struct service * s;
  
      if (section_length < descriptors_loop_len || !descriptors_loop_len) {
         warning("section too short: service_id == 0x%02x, section_length == %i, "
                 "descriptors_loop_len == %i\n", service_id, section_length, descriptors_loop_len);
         break;
         }
  
      s = find_service(current_tp, service_id);
      if (!s)
         /* maybe PAT has not yet been parsed... */
         s = alloc_service(current_tp, service_id);
  
      s->running   = (buf[3] >> 5) & 0x7;
      s->scrambled = (buf[3] >> 4) & 1;
  
      parse_descriptors(TABLE_SDT_ACT, buf + 5, descriptors_loop_len, s, flags.scantype);
  
      section_length -= descriptors_loop_len + 5;
      buf            += descriptors_loop_len + 5;
      }
}

em_static void parse_pat(const unsigned char * buf, uint16_t section_length, uint16_t transport_stream_id, uint32_t flags) {
   verbose("PAT (xxxx:xxxx:%u)\n", transport_stream_id);  
  hexdump(__FUNCTION__, buf, section_length);

  if (current_tp->transport_stream_id != transport_stream_id) {
     if (current_tp->type == SCAN_TERRESTRIAL) {
        char buffer[128];
        print_transponder(buffer, current_tp);
        verbose("        %s : updating transport_stream_id: -> (%u:%u:%u)\n",
            buffer,
            current_tp->original_network_id,
            current_tp->network_id,
            transport_stream_id);
        current_tp->transport_stream_id = transport_stream_id;
        /*check_duplicate_transponders();
        if (verbosity > 1) list_transponders();*/
        }
     else if (current_tp->transport_stream_id)
        verbose("unexpected transport_stream_id %d, expected %d\n",
                transport_stream_id, current_tp->transport_stream_id);
     }

  // will find out later what this does
 
  while(section_length > 0) {
     struct service * s;
     uint16_t service_id     =  (buf[0] << 8) | buf[1];
     uint16_t program_number = ((buf[2] & 0x1f) << 8) | buf[3];
     buf            += 4;
     section_length -= 4;

     if (service_id == 0) {
        if (program_number != 16)
           info("        %s: network_PID = %d (transport_stream_id %d)\n", __FUNCTION__, program_number, transport_stream_id);
        current_tp->network_PID = program_number;
        continue;
        }
     // SDT might have been parsed first...
     s = find_service(current_tp, service_id);
     if (s == NULL)
        s = alloc_service(current_tp, service_id);
     s->pmt_pid = program_number;

     if (! (flags & SECTION_FLAG_INITIAL)) {
        if (s->priv == NULL) { //  && s->pmt_pid) {  pmt_pid is by spec: 0x0010 .. 0x1FFE . see EN13818-1 p.19 Table 2-3 - PID table
           s->priv = calloc(1, sizeof(struct section_buf));
           setup_filter(s->priv, demux_devname, s->pmt_pid, TABLE_PMT, -1, 1, 0, SECTION_FLAG_FREE);
           add_filter(s->priv);
           }
        }
     }

}

static unsigned int chan_to_freq(int channel, int channellist) {
  //debug("channellist=%d, base_offset=%d, channel=%d, step=%d\n",
  //        channellist, base_offset(channel, channellist),
  //        channel, freq_step(channel, channellist));
    if (base_offset(channel, channellist) != -1) // -1 == invalid
       return base_offset(channel, channellist) +
       channel * freq_step(channel, channellist);
    return 0;
}

static uint16_t check_frontend(int fd, int verbose) {
  fe_status_t status;
  EMUL(em_status, &status)
  ioctl(fd, FE_READ_STATUS, &status);
  if (verbose && !flags.emulate) {
     uint16_t snr, signal;
     uint32_t ber, uncorrected_blocks;

     ioctl(fd, FE_READ_SIGNAL_STRENGTH, &signal);
     ioctl(fd, FE_READ_SNR, &snr);
     ioctl(fd, FE_READ_BER, &ber);
     ioctl(fd, FE_READ_UNCORRECTED_BLOCKS, &uncorrected_blocks);
     info("signal %04x | snr %04x | ber %08x | unc %08x | ", \
                                                  signal, snr, ber, uncorrected_blocks);
     if (status & FE_HAS_LOCK)
        info("FE_HAS_LOCK");
     info("\n");
     }
  return (status & 0x1F);
}

static int set_frontend(int frontend_fd, struct transponder * t) {
  int sequence_len = 0;
  struct dtv_property cmds[13];
  struct dtv_properties cmdseq = {0, cmds};

  switch(t->type) {
     case SCAN_CABLE: // note: fall trough to TERR && ATSC
        if ((t->symbolrate < fe_info.symbol_rate_min) || (t->symbolrate > fe_info.symbol_rate_max)) {
           info("\t skipped: (srate %u unsupported by driver)\n", t->symbolrate);
           return -2;
           }                        
     case SCAN_TERRESTRIAL:
        if (t->delsys == SYS_DVBT2) {
           if (!(fe_info.caps & FE_CAN_2G_MODULATION)) {
              info("\t%d: skipped (no driver support of DVBT2)\n", t->frequency);
              return -2;
              }
           }
        // no break needed here.
     case SCAN_TERRCABLE_ATSC:
        if ((t->frequency < fe_info.frequency_min) || (t->frequency > fe_info.frequency_max)) {
           info("\t skipped: (freq %u unsupported by driver)\n", t->frequency);
           return -2;
           }
        break;
     default:;
     }

  // if (mem_is_zero(&t->param, sizeof(struct tuning_parameters)))
  //    return -1;
  switch(flags.api_version) {
     case 0x0500 ... 0x05FF:
        #ifdef HWDBG
        #define set_cmd_sequence(_cmd, _data)   cmds[sequence_len].cmd = _cmd; \
                                                cmds[sequence_len].u.data = _data; \
                                                cmdseq.num = ++sequence_len; \
                                                info("%s:%d: %-40s = %d\n", __FUNCTION__,__LINE__, \
                                                      property_name(_cmd), _data)
        #else
        #define set_cmd_sequence(_cmd, _data)   cmds[sequence_len].cmd = _cmd; \
                                                cmds[sequence_len].u.data = _data; \
                                                cmdseq.num = ++sequence_len
        #endif
        set_cmd_sequence(DTV_CLEAR, DTV_UNDEFINED);
        switch(t->type) {
           case SCAN_CABLE:
              set_cmd_sequence(DTV_DELIVERY_SYSTEM,   t->delsys);
              set_cmd_sequence(DTV_FREQUENCY,         t->frequency);
              set_cmd_sequence(DTV_INVERSION,         t->inversion);
              set_cmd_sequence(DTV_MODULATION,        t->modulation);
              set_cmd_sequence(DTV_SYMBOL_RATE,       t->symbolrate);
              set_cmd_sequence(DTV_INNER_FEC,         t->coderate);
              break;
           case SCAN_TERRESTRIAL:
              set_cmd_sequence(DTV_DELIVERY_SYSTEM,   t->delsys);
              if (t->delsys == SYS_DVBT2) {
                 set_cmd_sequence(DTV_STREAM_ID, t->plp_id);
                 }
              set_cmd_sequence(DTV_FREQUENCY,         t->frequency);
              set_cmd_sequence(DTV_INVERSION,         t->inversion);
              set_cmd_sequence(DTV_BANDWIDTH_HZ,      t->bandwidth);
              set_cmd_sequence(DTV_CODE_RATE_HP,      t->coderate);
              set_cmd_sequence(DTV_CODE_RATE_LP,      t->coderate_LP);
              set_cmd_sequence(DTV_MODULATION,        t->modulation);
              set_cmd_sequence(DTV_TRANSMISSION_MODE, t->transmission);
              set_cmd_sequence(DTV_GUARD_INTERVAL,    t->guard);
              set_cmd_sequence(DTV_HIERARCHY,         t->hierarchy);
              break;
           case SCAN_TERRCABLE_ATSC:
              set_cmd_sequence(DTV_DELIVERY_SYSTEM,   t->delsys);
              set_cmd_sequence(DTV_FREQUENCY,         t->frequency);
              set_cmd_sequence(DTV_INVERSION,         t->inversion);
              set_cmd_sequence(DTV_MODULATION,        t->modulation);
              break;
           default:
              fatal("Unhandled type %d\n", t->type);
           }
        set_cmd_sequence(DTV_TUNE, DTV_UNDEFINED);
        EMUL(em_setproperty, &cmdseq)                        
        if (ioctl(frontend_fd, FE_SET_PROPERTY, &cmdseq) < 0) {
           errorn("Setting frontend parameters failed\n");
           return -1;
           }
        break;
     default:
        fatal("unsupported DVB API Version %d.%d\n", flags.api_version >> 8, flags.api_version & 0xFF);
     }
  return 0;
}

void init_tp(struct transponder * t) {
  current_tp = t;
  if (current_tp->network_name != NULL) {
     free(current_tp->network_name);
     current_tp->network_name = NULL;
     }
}

uint16_t fe_get_delsys(int frontend_fd, struct transponder * t) {
  struct dtv_property p[] = {{.cmd = DTV_DELIVERY_SYSTEM }};
  struct dtv_properties b = {.num = 1, .props = p};

  EMUL(em_getproperty, &b)
  if (ioctl(frontend_fd, FE_GET_PROPERTY, &b) != 0)
     return 0;

  // verbose("        %s %d: current delsys %u\n", __FUNCTION__, __LINE__, p[0].u.data);

  if (t != NULL) {
     t->delsys = p[0].u.data;
     }
  return p[0].u.data; // success           
}

static void copy_fe_params(struct transponder * dest, struct transponder * source) {
  memcpy(&dest->frequency, &source->frequency,
        (void *) &source->private_from_here - (void *) &source->frequency);

  // NOTE: pointer calc in memcpy is 36 bytes actually.
}


#define MOD_USE_STANDARD  0x0
#define MOD_OVERRIDE_MIN  0x1
#define MOD_OVERRIDE_MAX  0x2

#define DVB_ADAPTER_MAX    32
#define DVB_ADAPTER_SCAN   16
#define DVB_ADAPTER_AUTO  999


#define cl(x)  if (x) { free(x); x=NULL; }  

static void setup_filter(struct section_buf * s, const char * dmx_devname,
                          int pid, int table_id, int table_id_ext,
                          int run_once, int segmented, uint32_t filter_flags) {
  memset(s, 0, sizeof(struct section_buf));

  s->fd = -1;
  s->dmx_devname = dmx_devname;
  s->pid = pid;
  s->table_id = table_id;
  s->flags = filter_flags;

  s->run_once = run_once;
  s->segmented = segmented;
  s->timeout = 1; // add 1sec for safety..
  if (flags.filter_timeout > 0)
     s->timeout += 5 * repetition_rate(flags.scantype, table_id);
  else
     s->timeout += repetition_rate(flags.scantype, table_id);

  s->table_id_ext = table_id_ext;
  s->section_version_number = -1;
  s->next = 0;
  s->prev = 0;
  s->garbage = NULL;
}

static void update_poll_fds(void) {
  struct section_buf * s;
  int i;

  memset(poll_section_bufs, 0, sizeof(poll_section_bufs));
  for(i = 0; i < MAX_RUNNING; i++)
     poll_fds[i].fd = -1;
  i = 0;
  for(s = running_filters->first; s; s = s->next) {
     if (i >= MAX_RUNNING)
        fatal("too many poll_fds\n");
     if (s->fd == -1)
        fatal("s->fd == -1 on running_filters\n");
     verbosedebug("poll fd %d\n", s->fd);
     poll_fds[i].fd = s->fd;
     poll_fds[i].events = POLLIN;
     poll_fds[i].revents = 0;
     poll_section_bufs[i] = s;
     i++;
     }
  if (i != n_running)
     fatal("n_running is hosed\n");
}

static int get_bit(uint8_t *bitfield, int bit) {
  return (bitfield[bit/8] >> (bit % 8)) & 1;
}

static void set_bit(uint8_t *bitfield, int bit) {
  bitfield[bit/8] |= 1 << (bit % 8);
}

/*   returns 0 when more sections are expected
 *           1 when all sections are read on this pid
 *          -1 on invalid table id
 */
static int parse_section(struct section_buf * s) {
  const unsigned char * buf = s->buf;
  uint8_t  table_id;
//uint8_t  section_syntax_indicator;
  uint16_t section_length;                                        // 12bit: 0..4095
  uint16_t table_id_ext;
  uint8_t  section_version_number;
//uint8_t  current_next_indicator;
  uint8_t  section_number;
  uint8_t  last_section_number;
//int pcr_pid;
//int program_info_length;
  int i;

  table_id = buf[0];
  if (s->table_id != table_id)
     return -1;
//section_syntax_indicator = buf[1] & 0x80;
  section_length = (((buf[1] & 0x0f) << 8) | buf[2]) - 9;         // skip 9bytes: 5byte header + 4byte CRC32 

  if (! crc_check(&buf[0],section_length+12)) {
     int verbosity = 5;
     int slow_rep_rate = 30 + repetition_rate(flags.scantype, s->table_id);
     hexdump(__FUNCTION__,&buf[0], section_length+14);
     if (s->timeout < slow_rep_rate) {
        info("increasing filter timeout to %d secs (pid:%d table_id:%d table_id_ext:%d).\n",
             slow_rep_rate,s->pid,s->table_id, s->table_id_ext);
        s->timeout = slow_rep_rate;
        }

     pList list = s->garbage;
     unsigned char * p = (unsigned char *) calloc(1, SECTION_BUF_SIZE + sizeof(cItem));
     if (list == NULL) {
        list = (pList) calloc(1, sizeof(cList));
        NewList(list, "s->garbage");
        s->garbage = list;
        }
     memcpy(&p[sizeof(cItem)-1], buf, SECTION_BUF_SIZE);
     AddItem(s->garbage, p);
     // if ((s->garbage)->count > 3)
     //    return fuzzy_section(s);
     return 0;
     }

  table_id_ext = (buf[3] << 8) | buf[4];                          // p.program_number
  section_version_number = (buf[5] >> 1) & 0x1f;                  // p.version_number = getBits (b, 0, 42, 5); -> 40 + 1 -> 5 bit weit? -> version_number = buf[5] & 0x3e;
//current_next_indicator = buf[5] & 0x01;
  section_number = buf[6];
  last_section_number = buf[7];
//pcr_pid = ((buf[8] & 0x1f) << 8) | buf[9];
//program_info_length = ((buf[10] & 0x0f) << 8) | buf[11];

  if (s->segmented && s->table_id_ext != -1 && s->table_id_ext != table_id_ext) {
     /* find or allocate actual section_buf matching table_id_ext */
     while (s->next_seg) {
        s = s->next_seg;
        if (s->table_id_ext == table_id_ext)
           break;
        }
     if (s->table_id_ext != table_id_ext) {
        assert(s->next_seg == NULL);
        s->next_seg = calloc(1, sizeof(struct section_buf));
        s->next_seg->segmented = s->segmented;
        s->next_seg->run_once = s->run_once;
        s->next_seg->timeout = s->timeout;
        s = s->next_seg;
        s->table_id = table_id;
        s->table_id_ext = table_id_ext;
        s->section_version_number = section_version_number;
        }
     }

  if (s->section_version_number != section_version_number || s->table_id_ext != table_id_ext) {
     struct section_buf *next_seg = s->next_seg;

     if (s->section_version_number != -1 && s->table_id_ext != -1)
        debug("section version_number or table_id_ext changed "
              "%d -> %d / %04x -> %04x\n",
              s->section_version_number, section_version_number,
              s->table_id_ext, table_id_ext);
     s->table_id_ext = table_id_ext;
     s->section_version_number = section_version_number;
     s->sectionfilter_done = 0;
     memset(s->section_done, 0, sizeof(s->section_done));
     s->next_seg = next_seg;
     }

  buf += 8;

  if (!get_bit(s->section_done, section_number)) {
     set_bit(s->section_done, section_number);

     verbosedebug("pid %d (0x%02x), tid %d (0x%02x), table_id_ext %d (0x%04x), "
         "section_number %i, last_section_number %i, version %i\n",
         s->pid, s->pid,
         table_id, table_id,
         table_id_ext, table_id_ext, section_number,
         last_section_number, section_version_number);

     switch(table_id) {
     case TABLE_PAT:
        //verbose("PAT for transport_stream_id %d (0x%04x)\n", table_id_ext, table_id_ext);
        parse_pat(buf, section_length, table_id_ext, s->flags);
        break;
     case TABLE_PMT:
        verbose("PMT %d (0x%04x) for service %d (0x%04x)\n", s->pid, s->pid, table_id_ext, table_id_ext);
        parse_pmt(buf, section_length, table_id_ext);
        break;
     case TABLE_NIT_ACT:
     case TABLE_NIT_OTH:
        //verbose("NIT(%s TS, network_id %d (0x%04x) )\n", table_id == 0x40 ? "actual":"other",
        //       table_id_ext, table_id_ext);
        parse_nit(buf, section_length, table_id, table_id_ext, s->flags);
        break;
     case TABLE_SDT_ACT:
     case TABLE_SDT_OTH:
        verbose("SDT(%s TS, transport_stream_id %d (0x%04x) )\n", table_id == 0x42 ? "actual":"other",
               table_id_ext, table_id_ext);
        parse_sdt(buf, section_length, table_id_ext);
        break;
     case TABLE_VCT_TERR:
     case TABLE_VCT_CABLE:
        verbose("ATSC VCT, table_id %d, table_id_ext %d\n", table_id, table_id_ext);
        parse_psip_vct(buf, section_length, table_id, table_id_ext);
        break;
     default:;
     }

     for(i = 0; i <= last_section_number; i++)
        if (get_bit(s->section_done, i) == 0)
           break;

     if (i > last_section_number)
        s->sectionfilter_done = 1;
  }

  if (s->segmented) {
     /* always wait for timeout; this is because we don't now how
      * many segments there are
      */
     return 0;
     }
  else if (s->sectionfilter_done)
     return 1;

  return 0;
}

static int read_sections(struct section_buf * s) {
  int section_length, count;

  if (s->sectionfilter_done && !s->segmented)
     return 1;

  /* the section filter API guarantess that we get one full section
   * per read(), provided that the buffer is large enough (it is)
   */
  if (((count = read(s->fd, s->buf, sizeof(s->buf))) < 0) && errno == EOVERFLOW)
     count = read(s->fd, s->buf, sizeof(s->buf));
  if (count < 0) {
     errorn("read error: (count < 0)");
     return -1;
     }

  if (count < 4)
     return -1;

  section_length = ((s->buf[1] & 0x0f) << 8) | s->buf[2];

  if (count != section_length + 3)
     return -1;

  if (parse_section(s) == 1)
     return 1;

  return 0;
}

static int start_filter(struct section_buf * s) {
  struct dmx_sct_filter_params f;

  if (n_running >= MAX_RUNNING) {
     verbose("%s: too much filters. skip for now\n", __FUNCTION__); 
     goto err0;
     }
  if ((s->fd = open(s->dmx_devname, O_RDWR)) < 0) {
     warning("%s: could not open demux.\n", __FUNCTION__);
     goto err0;
     }

  verbosedebug("%s pid %d (0x%04x) table_id 0x%02x\n",
               __FUNCTION__, s->pid, s->pid, s->table_id);

  memset(&f, 0, sizeof(f));
  f.pid = (uint16_t) s->pid;

  if (s->table_id < 0x100 && s->table_id > 0) {
     f.filter.filter[0] = (uint8_t) s->table_id;
     f.filter.mask[0]   = 0xff;
     }

  f.timeout = 0;
  f.flags = DMX_IMMEDIATE_START;

  if (ioctl(s->fd, DMX_SET_FILTER, &f) == -1) {
     errorn("ioctl DMX_SET_FILTER failed");
     goto err1;
     }

  s->sectionfilter_done = 0;
  time(&s->start_time);

  AddItem(running_filters, s);

  n_running++;
  update_poll_fds();

  return 0;

  err1:
     ioctl(s->fd, DMX_STOP);
     close(s->fd);
  err0:
     return -1;
}

static void stop_filter(struct section_buf * s) {
  verbosedebug("%s: pid %d (0x%04x)\n", __FUNCTION__,s->pid,s->pid);

  ioctl(s->fd, DMX_STOP);
  close(s->fd);

  s->fd = -1;
  UnlinkItem(running_filters, s, false);
  s->running_time += time(NULL) - s->start_time;

  n_running--;
  update_poll_fds();
  if (s->garbage) {
     ClearList(s->garbage);
     free(s->garbage);
     }
}


static void add_filter(struct section_buf * s) {
  verbosedebug("%s %d: pid=%d (0x%04x), s=%p\n",
     __FUNCTION__,__LINE__,s->pid, s->pid, s);
  EMUL(em_addfilter, s)
  if (start_filter(s)) // could not start filter immediately.
     AddItem(waiting_filters, s);
}

static void remove_filter(struct section_buf * s) {
  verbosedebug("%s: pid %d (0x%04x)\n",__FUNCTION__,s->pid,s->pid);
  stop_filter(s);

  if (s->flags & SECTION_FLAG_FREE) {
     free(s);
     s = NULL;
     }

  if (running_filters->count > (MAX_RUNNING - 1)) // maximum num of filters reached.
     return;

  for(s = waiting_filters->first; s; s = s->next) {
     UnlinkItem(waiting_filters, s, false);
     if (start_filter(s)) {
        // any non-zero is error -> put again to list.
        InsertItem(waiting_filters, s, 0);
        break;
        }
     }
}

/* return value:
 * non-zero on success.
 * zero on timeout.
 */
static int read_filters(void) {
  struct section_buf * s;
  int i, n, done = 0;

  n = poll(poll_fds, n_running, 25);
  if (n == -1)
     errorn("poll");

  for(i = 0; i < n_running; i++) {
     s = poll_section_bufs[i];
     if (!s)
        fatal("poll_section_bufs[%d] is NULL\n", i);
     if (poll_fds[i].revents)
        done = read_sections(s) == 1;
     else
        done = 0; /* timeout */
     if (done || time(NULL) > s->start_time + s->timeout) {
        if (s->run_once) {
           if (done)
              verbosedebug("filter success: pid 0x%04x\n", s->pid);
           else {
              const char * intro = "        Info: no data from ";
              // timeout waiting for data.
              switch(s->table_id) {
                 case TABLE_PAT:       info   ("%sPAT after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_CAT:       info   ("%sCAT after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_PMT:       info   ("%sPMT after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_TSDT:      info   ("%sTSDT after %lld seconds\n",        intro, (long long) s->timeout); break;
                 case TABLE_NIT_ACT:   info   ("%sNIT(actual )after %lld seconds\n", intro, (long long) s->timeout); break;
                 case TABLE_NIT_OTH:   verbose("%sNIT(other) after %lld seconds\n",  intro, (long long) s->timeout); break; // not always available.
                 case TABLE_SDT_ACT:   info   ("%sSDT(actual) after %lld seconds\n", intro, (long long) s->timeout); break;
                 case TABLE_SDT_OTH:   info   ("%sSDT(other) after %lld seconds\n",  intro, (long long) s->timeout); break;
                 case TABLE_BAT:       info   ("%sBAT after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_EIT_ACT:   info   ("%sEIT(actual) after %lld seconds\n", intro, (long long) s->timeout); break;
                 case TABLE_EIT_OTH:   info   ("%sEIT(other) after %lld seconds\n",  intro, (long long) s->timeout); break;
                 case TABLE_TDT:       info   ("%sTDT after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_RST:       info   ("%sRST after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_TOT:       info   ("%sTOT after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_AIT:       info   ("%sAIT after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_CST:       info   ("%sCST after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_RCT:       info   ("%sRCT after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_CIT:       info   ("%sCIT after %lld seconds\n",         intro, (long long) s->timeout); break;
                 case TABLE_VCT_TERR:  info   ("%sVCT(terr) after %lld seconds\n",   intro, (long long) s->timeout); break;
                 case TABLE_VCT_CABLE: info   ("%sVCT(cable) after %lld seconds\n",  intro, (long long) s->timeout); break;
                 default:              info   ("%spid %u after %lld seconds\n",      intro, s->pid, (long long) s->timeout);
                 }
             }
           remove_filter(s);
           }
        }
     }
  return done;
}


static void scan_tp(void) {
  struct section_buf s[4];
  int result = 0;

  // scan for services: start filters for SDT and PAT (slowest filters first)

  setup_filter(&s[2], demux_devname, PID_SDT_BAT_ST, TABLE_SDT_ACT, -1, 1, 0, 0);
  add_filter(&s[2]);

  setup_filter(&s[3], demux_devname, PID_PAT, TABLE_PAT, -1, 1, 0, 0);
  add_filter(&s[3]);

  EMUL(em_readfilters, &result)
  do { read_filters(); }
     while((running_filters->count > 0) || (waiting_filters->count > 0));
}


/* scans a successful tuned new transponder's
 * program association table && network information table for update of its
 * transponder data
 */
static bool initial_table_lookup(int frontend_fd) {
  struct section_buf s;
  int result;
  current_tp->network_PID = PID_NIT_ST;
  memset(&s, 0, sizeof(s));
  verbose("        initial PAT lookup..\n");
  setup_filter(&s, demux_devname, PID_PAT, TABLE_PAT, -1, 1, 0, SECTION_FLAG_INITIAL);
  add_filter(&s);
  EMUL(em_readfilters, &result)
  do { result = read_filters(); }                
     while((running_filters->count > 0) || (waiting_filters->count > 0));
  
  if (result == 0) {
     // doesnt look like valid tp.
     return false;
     }

  // cxd2820r overwrites silently delsys, toggling between SYS_DVBT && SYS_DVBT2.
  // Therefore updating current_tp, kindly asking driver for actual delsys.
  fe_get_delsys(frontend_fd, current_tp);
  memset(&s, 0, sizeof(s));
  verbose("        initial NIT lookup..\n");
  setup_filter(&s, demux_devname, current_tp->network_PID, TABLE_NIT_ACT, -1, 1, 0, SECTION_FLAG_INITIAL);
  add_filter(&s);
  EMUL(em_readfilters, &result)
  do { result = read_filters(); }                
     while((running_filters->count > 0) || (waiting_filters->count > 0));
  return true;
}

static int is_nearly_same_frequency(uint32_t f1, uint32_t f2, scantype_t type) {
  uint32_t diff;
  if (f1 == f2)
     return 1;
  diff = (f1 > f2) ? (f1 - f2) : (f2 - f1);
  //FIXME: use symbolrate etc. to estimate bandwidth
  
   // 750kHz
   if (diff < 750000) {
      debug("f1 = %u is same TP as f2 = %u (diff=%d)\n", f1, f2, diff);
      return 1;
   }
     
  return 0;
}

void print_signal_info(int frontend_fd) {
  struct dtv_property p[] = {{.cmd = DTV_STAT_SIGNAL_STRENGTH }, {.cmd = DTV_STAT_CNR }};
  struct dtv_properties cmdseq = {.num = 2, .props = p};

  /* expected to fail with old drivers,
   * therefore no warning to user. 20090324 -wk
   */
  if (ioctl(frontend_fd, FE_GET_PROPERTY, &cmdseq)) {
     return;

  }

  double sigstr = 0.0;
  if (p[0].u.st.len>0) {
    switch (p[1].u.st.stat[0].scale) {
      case FE_SCALE_RELATIVE:
        sigstr = (p[0].u.st.stat[0].uvalue/65535.0)*100.0;
        info("        Signal strength: %2.1f/100\n",sigstr);
        break;       
      case FE_SCALE_DECIBEL:
        sigstr = p[0].u.st.stat[0].svalue/1000.0;
        info("        Signal strength: %2.1f dBm\n",sigstr);
        break;
      default: break;       
    }
  }


  double quality = 0.0;
  if (p[1].u.st.len>0) {
    switch (p[1].u.st.stat[0].scale) {
      case FE_SCALE_RELATIVE:
        quality = (p[1].u.st.stat[0].uvalue/65535.0)*100.0;
        info("        Signal quality: %2.1f/100\n",quality);
        break;
      case FE_SCALE_DECIBEL:
        quality = p[1].u.st.stat[0].svalue/1000.0;
        info("        Signal quality: %2.1f dB\n",quality);      
        break; 
      default: break;
    }
  }

}

/* identify wether tn is already in list of new transponders */
static int is_already_scanned_transponder(struct transponder * tn) {
  struct transponder * t;

  for(t = scanned_transponders->first; t; t = t->next) {
     switch(tn->type) {
        case SCAN_TERRESTRIAL:
        case SCAN_CABLE:
           if ((t->type == tn->type) && is_nearly_same_frequency(t->frequency, tn->frequency, t->type)) {
              //return (t->source >> 8) == 64;
              return 1;
           }
           break;
        case SCAN_TERRCABLE_ATSC:
           if ((t->type == tn->type) && is_nearly_same_frequency(t->frequency, tn->frequency, t->type) &&
              (t->modulation == tn->modulation))
              return (t->source >> 8) == 64;
           break;
        default:
           fatal("Unhandled type %d\n", tn->type);
        }
     }
  return 0;
}

/* identify whether tn is a duplicate of an already found transponder */
static int is_already_found_transponder(struct transponder * tn) {
  struct transponder * t;
  for(t = output_transponders->first; t; t = t->next) {
      if ((t->type == tn->type) && is_nearly_same_frequency(t->frequency, tn->frequency, t->type)) 
         continue; // ensure we do not compare the transponder with itself
      debug("  (%d %d, %d %d, %d, %d)\n", t->original_network_id, tn->original_network_id, t->network_id, tn->network_id, t->transport_stream_id, tn->transport_stream_id);
      if (t->original_network_id != tn->original_network_id)
         continue;
      if (t->network_id != tn->network_id)
         continue;
      if (t->transport_stream_id != tn->transport_stream_id)
         continue;
      // same ONID, NID, TID = same transponder
      info("        This is a duplicate of what was found on %d.\n",freq_scale(t->frequency, 1e-3));
      return 1;
  }

  return 0;
}


static void network_scan(int frontend_fd, int tuning_data) {

  uint32_t f = 0, channel, mod_parm, sr_parm, offs, this_sr=0;
  uint8_t delsys_parm, delsys = 0, last_delsys = 255;
  uint16_t ret = 0, lastret = 0;
  struct transponder * t = NULL, * ptest;
  struct transponder test;
  char buffer[128];
  ptest=&test;
  memset(&test, 0, sizeof(test));
  struct timespec timeout, meas_start, meas_stop;
  uint16_t time2carrier = 8000, time2lock = 8000;  


    //do last things before starting scan loop
  switch(flags.scantype) {
     case SCAN_TERRCABLE_ATSC:
        switch(ATSC_type) {
           case ATSC_VSB:
              modulation_min=modulation_max=ATSC_VSB;
              break;
           case ATSC_QAM:
              modulation_min=modulation_max=ATSC_QAM;
              break;
           default:
              modulation_min=ATSC_VSB;
              modulation_max=ATSC_QAM;
              break;
           }
        // disable symbolrate loop
        dvbc_symbolrate_min=dvbc_symbolrate_max=0;
        break;
     case SCAN_TERRESTRIAL:
        // disable qam loop, disable symbolrate loop
        modulation_min=modulation_max=0;
        dvbc_symbolrate_min=dvbc_symbolrate_max=0;
        // enable legacy delsys loop.
        delsys_min = delsysloop_min(0, this_channellist);
        // enable T2 loop.
        delsys_max = delsysloop_max(0, this_channellist);
        break;
     case SCAN_CABLE:
        // if choosen srate is too high for channellist's bandwidth,
        // fall back to scan all srates. scan loop will skip unsupported srates later.
        if(dvbc_symbolrate(dvbc_symbolrate_min) > max_dvbc_srate(freq_step(0, this_channellist))) {
           dvbc_symbolrate_min=0;
           dvbc_symbolrate_max=17;
           }
        // enable C2 loop.
        //delsys_max = 1;  // enable it later here.
        break;
     default:warning("unsupported delivery system %d.\n", flags.scantype);
  }

  /* ATSC VSB, ATSC QAM, DVB-T, DVB-C, here,
   * please change freqs inside country.c for ATSC, DVB-T, DVB-C
   */
  for(delsys_parm = delsys_min; delsys_parm <= delsys_max; delsys_parm++) {
     if ((delsys_parm > 0) && ((fe_info.caps & FE_CAN_2G_MODULATION) == 0)) {
        break;
        }
     for(mod_parm = modulation_min; mod_parm <= modulation_max; mod_parm++) {
        for(channel=flags.channel_min; channel <= flags.channel_max; channel++) {
           for(offs = freq_offset_min; offs <= freq_offset_max; offs++) {
              for(sr_parm = dvbc_symbolrate_min; sr_parm <= dvbc_symbolrate_max; sr_parm++) {                
                 test.type = flags.scantype;
                 switch(test.type) {
                    case SCAN_TERRESTRIAL:
                       if (delsys_parm != last_delsys) {
                          delsys = delsys_parm == 0? SYS_DVBT : SYS_DVBT2;
                          if (delsys==SYS_DVBT && flags.dvbt_type==2) continue;
                          if (delsys==SYS_DVBT2 && flags.dvbt_type==1) continue;
                          info("Scanning DVB-%s...\n", delsys == SYS_DVBT?"T":"T2");
                          last_delsys = delsys_parm;
                          }
                       f = chan_to_freq(channel, this_channellist);
                       if (! f) continue; //skip unused channels
                       if (freq_offset(channel, this_channellist, offs) == -1)
                          continue; //skip this one
                       f += freq_offset(channel, this_channellist, offs);                
                       if (test.bandwidth != (__u32) bandwidth(channel, this_channellist))
                          info("Scanning %sMHz frequencies...\n", vdr_bandwidth_name(bandwidth(channel, this_channellist)));
                       test.frequency         = f;
                       test.inversion         = caps_inversion;
                       test.bandwidth         = (__u32) bandwidth(channel, this_channellist);
                       test.coderate          = caps_fec;
                       test.coderate_LP       = caps_fec;
                       test.modulation        = caps_qam;
                       test.transmission      = caps_transmission_mode;
                       test.guard             = caps_guard_interval;
                       test.hierarchy         = caps_hierarchy;
                       test.delsys            = delsys;
                       test.plp_id            = 0;
                       time2carrier = carrier_timeout(test.delsys);
                       time2lock    = lock_timeout   (test.delsys);
                       if (is_already_scanned_transponder(&test)) {
                          info("%d (CH%d): skipped (already scanned transponder)\n", freq_scale(f, 1e-3),channel);
                          continue;
                          }
//if (f==506000000) test.frequency = 522000000; // TEST FOR Dups! Do NOT!!! check-in to GIT!!
                       info("%d (CH%d): ", freq_scale(f, 1e-3),channel);
                       break;
                    case SCAN_CABLE:
                       f = chan_to_freq(channel, this_channellist);
                       if (! f)
                          continue; //skip unused channels
                       if (freq_offset(channel, this_channellist, offs) == -1)
                          continue; //skip this one
                       f += freq_offset(channel, this_channellist, offs);
                       this_sr = dvbc_symbolrate(sr_parm);
                       if (this_sr > (uint32_t) max_dvbc_srate(freq_step(channel, this_channellist)))
                          continue; //skip symbol rates higher than theoretical limit given by bw && roll_off
                       this_qam = caps_qam;
                       if (flags.qam_no_auto > 0) {
                          this_qam = dvbc_modulation(mod_parm);
                          if (test.modulation != this_qam)
                             info ("searching QAM%s...\n", vdr_modulation_name(this_qam));
                          }
                       test.inversion         = caps_inversion;
                       test.delsys            = SYS_DVBC_ANNEX_A;
                       test.modulation        = this_qam;
                       test.symbolrate        = this_sr;
                       test.coderate          = caps_fec;
                       time2carrier = carrier_timeout(test.delsys);
                       time2lock    = lock_timeout   (test.delsys);
                       if (f != test.frequency) {
                          test.frequency = f;
                          if (is_already_scanned_transponder(&test)) {
                             info("%d: skipped (already known transponder)\n", freq_scale(f, 1e-3));
                             continue;
                             }
                          info("%d: sr%d ", freq_scale(f, 1e-3) , freq_scale(this_sr, 1e-3)); 
                          }
                       else {
                          if (is_already_scanned_transponder(&test))
                             continue;
                          info("sr%d ", freq_scale(this_sr, 1e-3));
                          }
                       break;
                    default:;
                 } // END: switch (test.type)
     
                 info("(time: %s) ", run_time());
                 if (set_frontend(frontend_fd, ptest) < 0) {
                    print_transponder(buffer, ptest);
                    dprintf(1,"\n%s:%d: Setting frontend failed %s\n", __FUNCTION__, __LINE__, buffer);
                    continue;
                    }
                 get_time(&meas_start);
                 set_timeout(time2carrier * flags.tuning_timeout, &timeout);  // N msec * {1,2,3}
                 if (!flags.emulate)
                    usleep(100000);
                 ret = 0; lastret = ret;

                 // look for some signal.
                 while((ret & (FE_HAS_SIGNAL | FE_HAS_CARRIER)) == 0) {
                     ret = check_frontend(frontend_fd,0);
                     if (ret != lastret) {
                        get_time(&meas_stop);
                        verbose("\n        (%.3fsec): %s%s%s (0x%X)",
                             elapsed(&meas_start, &meas_stop),
                             ret & FE_HAS_SIGNAL ?"S":"",
                             ret & FE_HAS_CARRIER?"C":"",
                             ret & FE_HAS_LOCK?   "L":"",
                             ret);
                        lastret = ret;
                     }
                     if (timeout_expired(&timeout) || flags.emulate) break;
                     usleep(50000);
                 }
                 if ((ret & (FE_HAS_SIGNAL | FE_HAS_CARRIER)) == 0) {
                    if (sr_parm == dvbc_symbolrate_max)
                       info("\n");
                    continue;
                    }
                 verbose("\n        (%.3fsec) signal", elapsed(&meas_start, &meas_stop));
                 //now, we should get also lock.
                 set_timeout(time2lock * flags.tuning_timeout, &timeout);  // N msec * {1,2,3}

                 while((ret & FE_HAS_LOCK) == 0) {
                     ret = check_frontend(frontend_fd,0);
                     if (ret != lastret) {
                        get_time(&meas_stop);
                        verbose("\n        (%.3fsec): %s%s%s (0x%X)",
                             elapsed(&meas_start, &meas_stop),
                             ret & FE_HAS_SIGNAL ?"S":"",
                             ret & FE_HAS_CARRIER?"C":"",
                             ret & FE_HAS_LOCK?   "L":"",
                             ret);
                        lastret = ret;
                        }
                     if (timeout_expired(&timeout) || flags.emulate) break;
                     usleep(50000);
                     }
                 if ((ret & FE_HAS_LOCK) == 0) {
                    if (sr_parm == dvbc_symbolrate_max)
                       info("\n");
                    continue;
                    }
                 verbose("\n        (%.3fsec) lock\n", elapsed(&meas_start, &meas_stop));

                 if ((test.type == SCAN_TERRESTRIAL) && (delsys != fe_get_delsys(frontend_fd, NULL))) {
                    verbose("wrong delsys: skip over.\n");                    // cxd2820r: T <-> T2
                    continue;
                    }

                 //if (__tune_to_transponder(frontend_fd, ptest,0) < 0)
                 //   continue;
if (f==506000000) test.frequency = 506000000; // TEST FOR Dups! Do NOT!!! check-in to GIT!!
                 t = alloc_transponder(f, test.delsys, test.polarization);
                 t->type = ptest->type;
                 t->source = 0;
                 t->network_name=NULL;
                 init_tp(t);

                 copy_fe_params(t, ptest);
                 print_transponder(buffer, t);
                 info("        signal ok:\t%s\n", buffer);
                 switch(ptest->type) {
                    case SCAN_TERRCABLE_ATSC:
                       //initial_table_lookup(frontend_fd); // would this work here? Don't know, need Info!
                       break;
                    default:
                                             

                       if (initial_table_lookup(frontend_fd)) {
                         print_transponder(buffer,current_tp);
                         if (flags.dedup==2) {
                            info("        %s : scanning for services\n",buffer);
                            scan_tp();
                            print_signal_info(frontend_fd);
                            AddItem(output_transponders, current_tp);
                         }
                         else if (flags.dedup==1) {
                            int isDuplicate = is_already_found_transponder(current_tp);
                            if (isDuplicate) {
                              info("        skipping.\n");                            
                            } else {
                               info("        %s : scanning for services\n",buffer);
                               scan_tp();
                               AddItem(output_transponders, current_tp);
                            }
                         } else {
                            info("        %s : scanning for services\n",buffer);
                            scan_tp();
                            AddItem(output_transponders, current_tp);
                         } 
                         AddItem(scanned_transponders, current_tp);
                       }

                      
                       break;
                    }
                 break;
                 } // END: for sr_parm
              } // END: for offs
           } // END: for channel       
        } // END: for mod_parm
     } // END: for delsys_parm

}

int main(int argc, char ** argv) {
  char frontend_devname [80];
  int adapter = DVB_ADAPTER_AUTO, frontend = 0, demux = 0;
  int opt;
  unsigned int i = 0, j;
  int frontend_fd = -1;
  int fe_open_mode;
  uint16_t scantype = SCAN_TERRESTRIAL;
  int Radio_Services = 1;
  int TV_Services = 1;
  int Other_Services = 0; // 20080106: don't search other services by default.
//  int ext = 0;
  int retVersion = 0;
  int device_preferred = -1;
  int valid_initial_data = 0;
  int modulation_flags = MOD_USE_STANDARD;
  int override_channellist = -1;
  char * country = NULL;
  char * codepage = NULL;
  char * satellite = NULL;
  char * initdata = NULL;
  char * positionfile = NULL;

  // initialize lists.
  NewList(running_filters, "running_filters");
  NewList(waiting_filters, "waiting_filters");
  NewList(scanned_transponders, "scanned_transponders");
  NewList(output_transponders, "output_transponders");
  NewList(new_transponders, "new_transponders");

  #define cleanup() cl(country); cl(satellite); cl(initdata); cl(positionfile); cl(codepage);

  flags.version = version;
  run_time_init();
  
  for (opt=0; opt<argc; opt++) info("%s ", argv[opt]); info("%s", "\n");

  while((opt = getopt_long(argc, argv, "a:c:dhm:o:q:s:t:vA:C:DEFGHI:L:MP:S:VY:Z", long_options, NULL)) != -1) {
     switch(opt) {
     case 'a': //adapter
             if (strstr(optarg, "/dev/dvb")) {
                if (sscanf(optarg, "/dev/dvb/adapter%d/frontend%d", &adapter, &frontend) != 2)
                   adapter = DVB_ADAPTER_AUTO, frontend = 0;
                }
             else {
                adapter = DVB_ADAPTER_AUTO, frontend = 0;
                if (sscanf(optarg, "%d", &adapter) < 1) {
                   adapter = 9999, frontend = 0;
                   flags.emulate = 1;
                   em_init(optarg);
                   }                   
                }
             break;
     case 'A': //ATSC type
             ATSC_type = strtoul(optarg,NULL,0);
             switch(ATSC_type) {
                case 1: ATSC_type = ATSC_VSB; break;
                case 2: ATSC_type = ATSC_QAM; break;
                case 3: ATSC_type = (ATSC_VSB + ATSC_QAM); break;
                default:
                   cleanup();
                   bad_usage(argv[0]);
                   return -1;
                }
             /* if -A is specified, it implies -f a */
             scantype = SCAN_TERRCABLE_ATSC;
             break;
     case 'c': // lowest channel to scan
             flags.channel_min = strtoul(optarg, NULL, 0);
             if ((flags.channel_min > 133)) bad_usage(argv[0]);
             break;
     case 'C': // highest channel to scan
             flags.channel_max = strtoul(optarg, NULL, 0);
             if ((flags.channel_max > 133)) bad_usage(argv[0]);
             break;
     case 'd': // avoiding duplicate multiplexes in output
             flags.dedup = 1;
             break;
     case 'D': // avoiding duplicate multiplexes in output
             flags.dedup = 2;
             break;
     case 'E': //exclude encrypted channels
             flags.ca_select = 0;
             break;
     case 'F': //filter timeout
             flags.filter_timeout = 1;
             break;
     case 'h': // help
             bad_usage("t2scan");
             cleanup();
             return 0;
             break;
     case 'H': //expert help
             ext_help();
             cleanup();
             return 0;
             break;
     case 'I': // iconv to charset (-C in w_scan)
             codepage = strdup(optarg);
             break;
     case 'L': // channel list setting, default channel list for country is automatically set
             override_channellist = strtoul(optarg, NULL, 0);
             break;
     case 'm': // scan mode (t=dvb-t [default], a=atsc)
             if (strcmp(optarg, "t") == 0) scantype = SCAN_TERRESTRIAL;
//             if (strcmp(optarg, "c") == 0) scantype = SCAN_CABLE;
             if (strcmp(optarg, "a") == 0) scantype = SCAN_TERRCABLE_ATSC;
             if (scantype == SCAN_TERRCABLE_ATSC) {
                this_channellist = ATSC_VSB;
                country = strdup("US");
                }
             break;
     case 'o': //output format
             if (strcmp(optarg, "xine") == 0) output_format = OUTPUT_XINE;
             else if (strcmp(optarg, "xml") == 0) output_format = OUTPUT_XML;
             else if (strcmp(optarg, "mplayer") == 0) output_format = OUTPUT_MPLAYER;
             else if (strcmp(optarg, "vlc") == 0) output_format = OUTPUT_VLC_M3U;
             else if (strcmp(optarg, "gstreamer") == 0) output_format = OUTPUT_GSTREAMER;
             else if (strcmp(optarg, "vdr20") == 0) {
                output_format = OUTPUT_VDR;
                flags.vdr_version = 2;
             } else {
                output_format = OUTPUT_VDR;
                flags.vdr_version = 21;
             }
             break;
     case 'P': //ATSC PSIP scan
             no_ATSC_PSIP = 1;
             break;
     case 'q': //quiet
             if (--verbosity < 0)
                verbosity = 0;
             break;
     case 's': // included services in output
             TV_Services = (strstr(optarg, "t"))? 1: 0;
             Radio_Services = (strstr(optarg, "r"))? 1: 0;
             Other_Services = (strstr(optarg, "o"))? 1: 0;
             break;
     case 'S': //tuning speed, in w_scan this option was 't'
             flags.tuning_timeout = strtoul(optarg, NULL, 0);
             if ((flags.tuning_timeout < 1)) bad_usage(argv[0]);
             if ((flags.tuning_timeout > 3)) bad_usage(argv[0]);
             break;
     case 't': // dvb-t modes to scan (0=all, 1=DVB-T, 2=DVB-T2)
             flags.dvbt_type = strtoul(optarg, NULL, 0);
             if ((flags.dvbt_type > 2)) bad_usage(argv[0]);
             break;
     case 'v': //verbose
             verbosity++;
             break;
     case 'V': //Version
             retVersion++;
             break;
     case 'Y': 
             if (0 == strcasecmp(optarg, "?")) {
                print_countries();
                cleanup();
                return(0);
             }
             cl(country);
             country=strdup(optarg);             
             break;
     case '!': //debug
             verbosity=5;
             break;
//     case 'i': //specify inversion (DVB-C)
//             caps_inversion = strtoul(optarg, NULL, 0);
//             break;
//     case 'X': //extended DVB-C scan flags (was 'e' in w_scan)
//             ext = strtoul(optarg, NULL, 0);
//             if (ext & 0x01)
//                dvbc_symbolrate_max = 17;
//             if (ext & 0x02) {
//                modulation_max = 2;
//                modulation_flags |= MOD_OVERRIDE_MAX;
//                }
//             break;
     default: //undefined
             cleanup();
             bad_usage(argv[0]);
             return -1;
     }
  }
  if (retVersion) {
     info("%d\n", version);
     cleanup();
     return 0;
     }
  info("t2scan version %d (compiled for DVB API %d.%d)\n", version, DVB_API_VERSION, DVB_API_VERSION_MINOR);
  if (NULL == initdata) {
      if ((NULL == country) && (scantype != SCAN_SATELLITE)) {
         country = strdup(country_to_short_name(get_user_country()));
         info("guessing country '%s', use -Y <country> to override\n", country);
      }
      if ((NULL == satellite) && (scantype == SCAN_SATELLITE)) {
         cleanup();
         fatal("Missing argument \"-s\" (satellite setting)\n");
      }                
  }
  serv_select = 1 * TV_Services + 2 * Radio_Services + 4 * Other_Services;
  if (caps_inversion > INVERSION_AUTO) {
     info("Inversion out of range!\n");
     bad_usage(argv[0]);
     cleanup();
     return -1;
     }
  if (((adapter >= DVB_ADAPTER_MAX) && (adapter != DVB_ADAPTER_AUTO) && (!flags.emulate)) || (adapter < 0)) {
     info("Invalid adapter: out of range (0..%d)\n", DVB_ADAPTER_MAX - 1);
     bad_usage(argv[0]);
     cleanup();
     return -1;
     }
  switch(scantype) {
     case SCAN_TERRCABLE_ATSC:
     case SCAN_CABLE:
     case SCAN_TERRESTRIAL:
        if (country != NULL) {
           int atsc = ATSC_type;
           int dvb  = scantype;
           flags.atsc_type = ATSC_type;
           choose_country(country, &atsc, &dvb, &scantype, &this_channellist);
           //dvbc: setting qam loop
           if ((modulation_flags & MOD_OVERRIDE_MAX) == MOD_USE_STANDARD)
              modulation_max = dvbc_qam_max(2, this_channellist);
           if ((modulation_flags & MOD_OVERRIDE_MIN) == MOD_USE_STANDARD)
              modulation_min = dvbc_qam_min(2, this_channellist);
           flags.list_id = txt_to_country(country);
           cl(country);
        }
        switch(override_channellist) {             
           case 0: this_channellist = DVBT_EU_UHF800; break;
           case 1: this_channellist = DVBT_EU_UHF700; break;
           case 2: this_channellist = DVBT_EU_UHF; break;
           case 3: this_channellist = DVBT_EU_VHFUHF; break;
           case 4: this_channellist = DVBT_FR; break;
           case 5: this_channellist = DVBT_GB; break;
           case 6: this_channellist = DVBT_AU; break;
           default: break;
        }
        break;
     
     default:
        cleanup();
        fatal("Unknown scan type %d\n", scantype);
     }

  if (initdata != NULL) {
     valid_initial_data = dvbscan_parse_tuningdata(initdata, &flags);
     cl(initdata);
     if (valid_initial_data == 0) {
        cleanup();
        fatal("Could not read initial tuning data. EXITING.\n");
        }
     if (flags.scantype != scantype) {
        warning("\n"
                "========================================================================\n"
                "INITIAL TUNING DATA NEEDS FRONTEND TYPE %s, YOU SELECTED TYPE %s.\n"
                "I WILL OVERRIDE YOUR DEFAULTS TO %s\n"
                "========================================================================\n",
                scantype_to_text(flags.scantype),
                scantype_to_text(scantype),
                scantype_to_text(flags.scantype));
        scantype = flags.scantype;                        
        sleep(10); // enshure that user reads warning.
        }
     }
  info("scan type %s, channellist %d\n", scantype_to_text(scantype), this_channellist);
  switch(output_format) {
     case OUTPUT_VDR:
        switch(flags.vdr_version) {
           case 2:
              info("output format vdr-2.0\n");
              break;
           case 21:
              info("output format vdr-2.1\n");
              break;
           default:
              fatal("UNKNOWN VDR VERSION.");
           }
        break;
     case OUTPUT_GSTREAMER:
        // Gstreamer output: As vdr-1.7+, but pmt_pid added at end of line.
        flags.print_pmt = 1;
        flags.vdr_version = 2;
        output_format = OUTPUT_VDR;
        info("output format gstreamer\n");
        break;
     case OUTPUT_XINE:
        info("output format czap/tzap/szap/xine\n");
        break;
     case OUTPUT_MPLAYER:
        info("output format mplayer\n");
        break;
     case OUTPUT_DVBSCAN_TUNING_DATA:
         info("output format initial tuning data\n");
         break;
     case OUTPUT_PIDS:
        info("output format PIDs only\n");
        break;
     case OUTPUT_VLC_M3U:
        info("output format vlc xspf playlist\n");
        // vlc format will be output always as utf-8.
        if (codepage)
           free(codepage);
        codepage = strdup("ISO-8859-1");
        break;
     case OUTPUT_XML:
        info("output format w_scan XML tuning data\n");
        if (codepage)
           free(codepage);
        codepage = strdup("ISO-8859-1");
        break;
     default:
        cleanup();
        fatal("unhandled output format %d\n", output_format);
     }
  if (codepage) {
     flags.codepage = get_codepage_index(codepage);
     info("output charset '%s'\n", iconv_codes[flags.codepage]);
     }
  else {
     flags.codepage = get_user_codepage();
     info("output charset '%s', use -C <charset> to override\n", iconv_codes[flags.codepage]);
     }        
  if ( adapter == DVB_ADAPTER_AUTO ) {
     info("Info: using DVB adapter auto detection.\n");
     fe_open_mode = O_RDWR | O_NONBLOCK;
     for(i=0; i < DVB_ADAPTER_SCAN; i++) {
        for(j=0; j < 4; j++) {
           snprintf(frontend_devname, sizeof(frontend_devname), "/dev/dvb/adapter%i/frontend%i", i, j);
           if ((frontend_fd = open(frontend_devname, fe_open_mode)) < 0) {
               continue;
               }
           /* determine FE type and caps */
           if (ioctl(frontend_fd, FE_GET_INFO, &fe_info) == -1) {
              info("   ERROR: unable to determine frontend type\n");
              close(frontend_fd);
              continue;
              }
           
           if (flags.api_version < 0x0500)
              get_api_version(frontend_fd, &flags);
           
           if (fe_supports_scan(frontend_fd, scantype, fe_info)) {
              info("\t%s -> %s \"%s\": ", frontend_devname, scantype_to_text(scantype), fe_info.name);
              if (device_is_preferred(fe_info.caps, fe_info.name, scantype) >= device_preferred) {
                 if (device_is_preferred(fe_info.caps, fe_info.name, scantype) > device_preferred) {
                    device_preferred = device_is_preferred(fe_info.caps, fe_info.name, scantype);
                    adapter=i;
                    frontend=j;
                    }
                 switch(device_preferred) {
                    case 0: // device known to have probs. usable anyway..
                       info("usable :-|\n");
                       break;
                    case 1: // device w/o problems
                       info("good :-)\n");
                       break;
                    case 2: // perfect device found. stop scanning
                       info("very good :-))\n\n");
                       i=DVB_ADAPTER_AUTO;
                       break;
                    default:;
                    }
                 }
              else {
                 info("usable, but not preferred\n");
                 }
              close(frontend_fd);
              }
           else {
              info("\t%s -> \"%s\" doesnt support %s -> SEARCH NEXT ONE.\n",
                  frontend_devname, fe_info.name, scantype_to_text(scantype));
              close(frontend_fd);
              }
           } // END: for j
        } // END: for i
     if (adapter < DVB_ADAPTER_AUTO) {
        snprintf(frontend_devname, sizeof(frontend_devname), "/dev/dvb/adapter%i/frontend%i", adapter, frontend);
        info("Using %s frontend (adapter %s)\n", scantype_to_text(scantype), frontend_devname);
        }
     }
  snprintf(frontend_devname, sizeof(frontend_devname), "/dev/dvb/adapter%i/frontend%i", adapter, frontend);
  snprintf(demux_devname, sizeof(demux_devname),       "/dev/dvb/adapter%i/demux%i"   , adapter, demux);

  for(i = 0; i < MAX_RUNNING; i++)
     poll_fds[i].fd = -1;

  fe_open_mode = O_RDWR;
  if (adapter == DVB_ADAPTER_AUTO) {
     cleanup();
     fatal("***** NO USEABLE %s CARD FOUND. *****\n"
             "Please check wether dvb driver is loaded and\n"
             "verify that no dvb application (i.e. vdr) is running.\n",
             scantype_to_text(scantype));
     }
  EMUL(em_open, &frontend_fd)
  if ((frontend_fd = open(frontend_devname, fe_open_mode)) < 0) {
     cleanup();
     fatal("failed to open '%s': %d %s\n", frontend_devname, errno, strerror(errno));
     }
  info("-_-_-_-_ Getting frontend capabilities-_-_-_-_ \n");
  /* determine FE type and caps */
  EMUL(em_info, &fe_info)
  if (ioctl(frontend_fd, FE_GET_INFO, &fe_info) == -1) {
     cleanup();
     fatal("FE_GET_INFO failed: %d %s\n", errno, strerror(errno));
     }
  flags.scantype = scantype;

  EMUL(em_dvbapi, &flags.api_version)
  if (get_api_version(frontend_fd, &flags) < 0)
     fatal("Your DVB driver doesnt support DVB API v5. Please upgrade.\n");

  info("Using DVB API %d.%d\n", flags.api_version >> 8, flags.api_version & 0xFF);

  info("frontend '%s' supports\n", fe_info.name && *fe_info.name?fe_info.name:"<NULL pointer>");

  switch(flags.scantype) {
     case SCAN_TERRESTRIAL:
        if (fe_info.caps & FE_CAN_2G_MODULATION) {
           info("DVB-T2\n");
           }
        if (fe_info.caps & FE_CAN_INVERSION_AUTO) {
           info("INVERSION_AUTO\n");
           caps_inversion=INVERSION_AUTO;
           }
        else {
           info("INVERSION_AUTO not supported, trying INVERSION_OFF.\n");
           caps_inversion=INVERSION_OFF;
           }
        if (fe_info.caps & FE_CAN_QAM_AUTO) {
           info("QAM_AUTO\n");
           caps_qam=QAM_AUTO;
           }
        else {
           info("QAM_AUTO not supported, trying QAM_64.\n");
           caps_qam=QAM_64;
           }
        if (fe_info.caps & FE_CAN_TRANSMISSION_MODE_AUTO) {
           info("TRANSMISSION_MODE_AUTO\n");
           caps_transmission_mode=TRANSMISSION_MODE_AUTO;
           }
        else {
           caps_transmission_mode=dvbt_transmission_mode(5, this_channellist);
           info("TRANSMISSION_MODE not supported, trying %s.\n",
                 transmission_mode_name(caps_transmission_mode));
           }
        if (fe_info.caps & FE_CAN_GUARD_INTERVAL_AUTO) {
           info("GUARD_INTERVAL_AUTO\n");
           caps_guard_interval=GUARD_INTERVAL_AUTO;
           }
        else {
           info("GUARD_INTERVAL_AUTO not supported, trying GUARD_INTERVAL_1_8.\n");
           caps_guard_interval=GUARD_INTERVAL_1_8;
           }
        if (fe_info.caps & FE_CAN_HIERARCHY_AUTO) {
           info("HIERARCHY_AUTO\n");
           caps_hierarchy=HIERARCHY_AUTO;
           }
        else {
           info("HIERARCHY_AUTO not supported, trying HIERARCHY_NONE.\n");
           caps_hierarchy=HIERARCHY_NONE;
           }
        if (fe_info.caps & FE_CAN_FEC_AUTO) {
           info("FEC_AUTO\n");
           caps_fec=FEC_AUTO;
           }
        else {
           info("FEC_AUTO not supported, trying FEC_NONE.\n");
           caps_fec=FEC_NONE;
           }
        if (fe_info.caps & FE_CAN_BANDWIDTH_AUTO) {
           info("BANDWIDTH_AUTO\n");
           bandwidth_auto = true;
           }
        else {
           info("BANDWIDTH_AUTO not supported, trying 6/7/8 MHz.\n");
           bandwidth_auto = false;
           }
        if (fe_info.frequency_min == 0 || fe_info.frequency_max == 0) {
           info("This dvb driver is *buggy*: the frequency limits are undefined - please report to linuxtv.org\n");
           fe_info.frequency_min = 177500000; fe_info.frequency_max = 858000000;
           }
        else {
           info("FREQ (%.2fMHz ... %.2fMHz)\n", fe_info.frequency_min/1e6, fe_info.frequency_max/1e6);
           }
        break;
     case SCAN_CABLE:
        //if (fe_info.caps & FE_CAN_2G_MODULATION) {
        //  info("DVB-C2\n");
        //  }
        if (fe_info.caps & FE_CAN_INVERSION_AUTO) {
           info("INVERSION_AUTO\n");
           caps_inversion=INVERSION_AUTO;
           }
        else {
           info("INVERSION_AUTO not supported, trying INVERSION_OFF.\n");
           caps_inversion=INVERSION_OFF;
           }
        if (fe_info.caps & FE_CAN_QAM_AUTO) {
           info("QAM_AUTO\n");
           caps_qam=QAM_AUTO;
           }
        else {
           info("QAM_AUTO not supported, trying");
           //print out modulations in the sequence they will be scanned.
           for(i = modulation_min; i <= modulation_max; i++)
                 info(" %s", modulation_name(dvbc_modulation(i)));
           info(".\n");
           caps_qam=QAM_64;
           flags.qam_no_auto = 1;
           }
        if (fe_info.caps & FE_CAN_FEC_AUTO) {
           info("FEC_AUTO\n");
           caps_fec=FEC_AUTO;
           }
        else {
           info("FEC_AUTO not supported, trying FEC_NONE.\n");
           caps_fec=FEC_NONE;
           }
        if (fe_info.frequency_min == 0 || fe_info.frequency_max == 0) {
           info("This dvb driver is *buggy*: the frequency limits are undefined - please report to linuxtv.org\n");
           fe_info.frequency_min = 177500000; fe_info.frequency_max = 858000000;
           }
        else {
           info("FREQ (%.2fMHz ... %.2fMHz)\n", fe_info.frequency_min/1e6, fe_info.frequency_max/1e6);
           }
        if (fe_info.symbol_rate_min == 0 || fe_info.symbol_rate_max == 0) {
           info("This dvb driver is *buggy*: the symbol rate limits are undefined - please report to linuxtv.org\n");
           fe_info.symbol_rate_min = 4000000; fe_info.symbol_rate_max = 7000000;
           }
        else {
           info("SRATE (%.3fMSym/s ... %.3fMSym/s)\n", fe_info.symbol_rate_min/1e6, fe_info.symbol_rate_max/1e6);
           }
        break;
     case SCAN_TERRCABLE_ATSC:
        if (fe_info.caps & FE_CAN_INVERSION_AUTO) {
           info("INVERSION_AUTO\n");
           caps_inversion=INVERSION_AUTO;
           }
        else {
           info("INVERSION_AUTO not supported, trying INVERSION_OFF.\n");
           caps_inversion=INVERSION_OFF;
           }
        if (fe_info.caps & FE_CAN_8VSB) {
           info("8VSB\n");
           }
        if (fe_info.caps & FE_CAN_16VSB) {
           info("16VSB\n");
           }
        if (fe_info.caps & FE_CAN_QAM_64) {
           info("QAM_64\n");
           }
        if (fe_info.caps & FE_CAN_QAM_256) {
           info("QAM_256\n");
           }
        if (fe_info.frequency_min == 0 || fe_info.frequency_max == 0) {
           info("This dvb driver is *buggy*: the frequency limits are undefined - please report to linuxtv.org\n");
           fe_info.frequency_min = 177500000; fe_info.frequency_max = 858000000;
           }
        else {
           info("FREQ (%.2fMHz ... %.2fMHz)\n", fe_info.frequency_min/1e6, fe_info.frequency_max/1e6);
           }
        break;
     default:
        cleanup();
        fatal("unsupported frontend type.\n");
     }
  info("-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_ \n");

  if (! fe_supports_scan(frontend_fd, scantype, fe_info) && flags.api_version < 0x0505) {
     cleanup();
     fatal("Frontend '%s' doesnt support your choosen scan type '%s'\n",
           fe_info.name, scantype_to_text(scantype));
     }

  signal(SIGINT, handle_sigint);
  network_scan(frontend_fd, valid_initial_data);
  close(frontend_fd);
  dump_lists(adapter, frontend);
  cleanup();
  return 0;
}

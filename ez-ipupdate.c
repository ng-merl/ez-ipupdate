/* ============================================================================
 * Copyright (C) 1998-2000 Angus Mackay. All rights reserved; 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * ============================================================================
 */

/*
 * ez-ipupdate
 *
 * a very simple dynDNS client for the ez-ip dynamic dns service 
 * (http://www.ez-ip.net).
 *
 * why this program when something like:
 *   curl -u user:pass http://www.ez-ip.net/members/update/?key=val&...
 * would do the trick? because there are nicer clients for other OSes and
 * I don't like to see UNIX get the short end of the stick.
 *
 * tested under Linux and Solaris.
 * 
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#define EZIP_DEFAULT_SERVER "www.EZ-IP.Net"
#define EZIP_DEFAULT_PORT "80"
#define EZIP_REQUEST "/members/update/"

#define PGPOW_DEFAULT_SERVER "www.penguinpowered.com"
#define PGPOW_DEFAULT_PORT "2345"
#define PGPOW_REQUEST "update"
#define PGPOW_VERSION "1.0"

#define DHS_DEFAULT_SERVER "members.dhs.org"
#define DHS_DEFAULT_PORT "80"
#define DHS_REQUEST "/nic/hosts"
#define DHS_SUCKY_TIMEOUT 60

#define DYNDNS_DEFAULT_SERVER "members.dyndns.org"
#define DYNDNS_DEFAULT_PORT "80"
#define DYNDNS_REQUEST "/nic/dyndns/"
#define DYNDNS_STAT_REQUEST "/nic/statdns/"

#define DEFAULT_TIMEOUT 120
#define DEFAULT_UPDATE_PERIOD 600
#if __linux__
#  define DEFAULT_IF "eth0"
#elif __OpenBSD__
#  define DEFAULT_IF "ne0"
#else
#  define DEFAULT_IF "eth0"
#endif

#ifdef DEBUG
#  define BUFFER_SIZE (16*1024)
#else
#  define BUFFER_SIZE (4*1024-1)
#endif

#ifdef HAVE_GETOPT_H
#  include <getopt.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if HAVE_FCNTL_H
#  include <fcntl.h>
#endif
#include <netinet/in.h>
#if HAVE_ARPA_INET_H
#  include <arpa/inet.h>
#endif
#include <netdb.h>
#include <sys/socket.h>
#if HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#if HAVE_SIGNAL_H
#  include <signal.h>
#endif
#if HAVE_SYS_TIME_H
#  include <sys/time.h>
#endif
#if HAVE_SYSLOG_H
#  include <syslog.h>
#endif
#if HAVE_STRERROR
extern int errno;
#  define error_string strerror(errno)
#elif HAVE_SYS_ERRLIST
extern const char *const sys_errlist[];
extern int errno;
#  define error_string (sys_errlist[errno])
#else
#  define error_string "error message not found"
#endif
#if HAVE_PWD_H && HAVE_GRP_H
#  include <pwd.h>
#  include <grp.h>
#endif



#if __linux__ || __OpenBSD__
#  define IF_LOOKUP 1
#  include <sys/ioctl.h>
#  include <net/if.h>
#endif

#include <conf_file.h>

#if !defined(__GNUC__) && !defined(HAVE_SNPRINTF)
#error "get gcc, fix this code, or find yourself a snprintf!"
#else
#  if HAVE_SNPRINTF
#    define  snprint(x, y, z...) snprintf(x, y, ## z)
#  else
#    define  snprint(x, y, z...) sprintf(x, ## z)
#  endif
#endif

#ifndef HAVE_HERROR
#  define herror(x) fprintf(stderr, "%s: error\n", x)
#endif

#ifdef DEBUG
#define dprintf(x) if( options & OPT_DEBUG ) \
{ \
  fprintf(stderr, "%s,%d: ", __FILE__, __LINE__); \
    fprintf x; \
}
#else
#  define dprintf(x)
#endif

#ifndef OS
#  define OS "unknown"
#endif

/**************************************************/

enum {
  SERV_EZIP,
  SERV_PGPOW,
  SERV_DHS,
  SERV_DYNDNS,
  SERV_DYNDNS_STAT
};

struct service_t
{
  int type;
  char *name;
  int (*update_entry)(void);
  int (*check_info)(void);
  char **fields_used;
  char *default_server;
  char *default_port;
  char *default_request;
};

/**************************************************/

static char *program_name = NULL;
static char *config_file = NULL;
static char *server = NULL;
static char *port = NULL;
static char user[256];
static char auth[512];
static char user_name[128];
static char password[128];
static char *address = NULL;
static char *request = NULL;
static int wildcard = 0;
static char *mx = NULL;
static char *url = NULL;
static char *host = NULL;
static char *cloak_title = NULL;
static char *interface = NULL;
static int ntrys = 1;
static int update_period = DEFAULT_UPDATE_PERIOD;
static int resolv_period = DEFAULT_UPDATE_PERIOD/10;
static struct timeval timeout;
static int max_interval = 0;
static int service_set = 0;

static volatile int client_sockfd;
static volatile int last_sig = 0;

/* service objects for various services */
int EZIP_update_entry(void);
int EZIP_check_info(void);
static char *EZIP_fields_used[] = { "server", "user", "address", "wildcard", "mx", "url", "host" };
static struct service_t EZIP_service = {
  SERV_EZIP,
  "ez-ip",
  EZIP_update_entry,
  EZIP_check_info,
  EZIP_fields_used,
  EZIP_DEFAULT_SERVER,
  EZIP_DEFAULT_PORT,
  EZIP_REQUEST
};

int PGPOW_update_entry(void);
int PGPOW_check_info(void);
static char *PGPOW_fields_used[] = { "server", "host" };
static struct service_t PGPOW_service = {
  SERV_PGPOW,
  "justlinux (penguinpowered)",
  PGPOW_update_entry,
  PGPOW_check_info,
  PGPOW_fields_used,
  PGPOW_DEFAULT_SERVER,
  PGPOW_DEFAULT_PORT,
  PGPOW_REQUEST
};

int DHS_update_entry(void);
int DHS_check_info(void);
static char *DHS_fields_used[] = { "server", "user", "address", "wildcard", "mx", "url", "host" };
static struct service_t DHS_service = {
  SERV_DHS,
  "ez-ip",
  DHS_update_entry,
  DHS_check_info,
  DHS_fields_used,
  DHS_DEFAULT_SERVER,
  DHS_DEFAULT_PORT,
  DHS_REQUEST
};

int DYNDNS_update_entry(void);
int DYNDNS_check_info(void);
static char *DYNDNS_fields_used[] = { "server", "user", "address", "wildcard", "mx", "host" };
static struct service_t DYNDNS_service = {
  SERV_DYNDNS,
  "dyndns",
  DYNDNS_update_entry,
  DYNDNS_check_info,
  DYNDNS_fields_used,
  DYNDNS_DEFAULT_SERVER,
  DYNDNS_DEFAULT_PORT,
  DYNDNS_REQUEST
};

int DYNDNS_STAT_update_entry(void);
int DYNDNS_STAT_check_info(void);
static char *DYNDNS_STAT_fields_used[] = { "server", "user", "address", "wildcard", "mx", "host" };
static struct service_t DYNDNS_STAT_service = {
  SERV_DYNDNS_STAT,
  "dyndns-static",
  DYNDNS_update_entry,
  DYNDNS_check_info,
  DYNDNS_fields_used,
  DYNDNS_DEFAULT_SERVER,
  DYNDNS_DEFAULT_PORT,
  DYNDNS_STAT_REQUEST
};

/* default to EZIP for historical reasons */
static struct service_t *service = &DEF_SERVICE;

int options;

#define OPT_DEBUG       0x0001
#define OPT_DAEMON      0x0004
#define OPT_QUIET       0x0008
#define OPT_FOREGROUND  0x0010

enum { 
  CMD__start = 1,
  CMD_service_type,
  CMD_server,
  CMD_user,
  CMD_address,
  CMD_wildcard,
  CMD_mx,
  CMD_max_interval,
  CMD_url,
  CMD_host,
  CMD_cloak_title,
  CMD_interface,
  CMD_retrys,
  CMD_resolv_period,
  CMD_period,
  CMD_daemon,
  CMD_debug,
  CMD_foreground,
  CMD_quiet,
  CMD_timeout,
  CMD_run_as_user,
  CMD__end
};

int conf_handler(struct conf_cmd *cmd, char *arg);
static struct conf_cmd conf_commands[] = {
  { CMD_address,       "address",       CONF_NEED_ARG, 1, conf_handler, "%s=<ip address>" },
  { CMD_cloak_title,   "cloak_title",   CONF_NEED_ARG, 1, conf_handler, "%s=<title>" },
  { CMD_daemon,        "daemon",        CONF_NO_ARG,   1, conf_handler, "%s" },
  { CMD_debug,         "debug",         CONF_NO_ARG,   1, conf_handler, "%s" },
  { CMD_foreground,    "foreground",    CONF_NO_ARG,   1, conf_handler, "%s" },
  { CMD_host,          "host",          CONF_NEED_ARG, 1, conf_handler, "%s=<host>" },
  { CMD_interface,     "interface",     CONF_NEED_ARG, 1, conf_handler, "%s=<interface>" },
  { CMD_mx,            "mx",            CONF_NEED_ARG, 1, conf_handler, "%s=<mail exchanger>" },
  { CMD_max_interval,  "max-interval",  CONF_NEED_ARG, 1, conf_handler, "%s=<number of seconds between updates>" },
  { CMD_retrys,        "retrys",        CONF_NEED_ARG, 1, conf_handler, "%s=<number of trys>" },
  { CMD_server,        "server",        CONF_NEED_ARG, 1, conf_handler, "%s=<server name>" },
  { CMD_service_type,  "service-type",  CONF_NEED_ARG, 1, conf_handler, "%s=<ezip|pgpower>" },
  { CMD_timeout,       "timeout",       CONF_NEED_ARG, 1, conf_handler, "%s=<sec.millisec>" },
  { CMD_resolv_period, "resolv-period", CONF_NEED_ARG, 1, conf_handler, "%s=<time between failed resolve attempts>" },
  { CMD_period,        "period",        CONF_NEED_ARG, 1, conf_handler, "%s=<time between update attempts>" },
  { CMD_url,           "url",           CONF_NEED_ARG, 1, conf_handler, "%s=<url>" },
  { CMD_user,          "user",          CONF_NEED_ARG, 1, conf_handler, "%s=<user name>[:password]" },
  { CMD_run_as_user,   "run-as-user",   CONF_NEED_ARG, 1, conf_handler, "%s=<user>" },
  { CMD_wildcard,      "wildcard",      CONF_NO_ARG,   1, conf_handler, "%s" },
  { CMD_quiet,         "quiet",         CONF_NO_ARG,   1, conf_handler, "%s" },
  { 0, 0, 0, 0, 0 }
};

/**************************************************/

void print_usage( void );
void print_version( void );
void parse_args( int argc, char **argv );
int do_connect(int *sock, char *host, char *port);
void base64Encode(char *intext, char *output);
int main( int argc, char **argv );

/**************************************************/

void print_usage( void )
{
  fprintf(stdout, "usage: ");
  fprintf(stdout, "%s [options] \n\n", program_name);
  fprintf(stdout, " Options are:\n");
  fprintf(stdout, "  -a, --address <ip address>\tstring to send as your ip address\n");
  fprintf(stdout, "  -c, --config-file <file>\tconfiguration file, almost all arguments can be\n");
  fprintf(stdout, "\t\t\t\tgiven with: <name>[=<value>]\n\t\t\t\tto see a list of possible config commands\n");
  fprintf(stdout, "\t\t\t\ttry \"echo help | %s -c -\"\n", program_name);
  fprintf(stdout, "  -d, --daemon\t\t\trun as a daemon periodicly updating if necessary\n");
#ifdef DEBUG
  fprintf(stdout, "  -D, --debug\t\t\tturn on debuggin\n");
#endif
  fprintf(stdout, "  -f, --foreground\t\twhen running as a daemon run in the foreground\n");
  fprintf(stdout, "  -h, --host <host>\t\tstring to send as host parameter\n");
  fprintf(stdout, "  -i, --interface <iface>\twhich interface to use, the default is %s\n\t\t\t\tbut a common one to use would be ppp0\n", DEFAULT_IF);
  fprintf(stdout, "  -L, --cloak_title <host>\tsome stupid thing for DHS only\n");
  fprintf(stdout, "  -m, --mx <mail exchange>\tstring to send as your mail exchange\n");
  fprintf(stdout, "  -M, --max-interval <# of sec>\tmax time in between updates\n");
  fprintf(stdout, "  -p, --resolv-period <sec>\tperiod to check IP if it can't be resolved\n");
  fprintf(stdout, "  -P, --period <# of sec>\tperiod to check IP in daemon \n\t\t\t\tmode (default: 1800 seconds)\n");
  fprintf(stdout, "  -q, --quiet \t\t\tbe quiet\n");
  fprintf(stdout, "  -r, --retrys <num>\t\tnumber of trys (default: 1)\n");
  fprintf(stdout, "  -R, --run-as-user <user>\tchange to <user> for running, be ware\n\t\t\t\tthat this can cause problems with handeling\n\t\t\t\tSIGHUP properly if that user can't read the\n\t\t\t\tconfig file\n");
  fprintf(stdout, "  -s, --server <server[:port]>\tthe server to connect to\n");
  fprintf(stdout, "  -S, --service-type <server>\tthe type of service that you are using\n");
  fprintf(stdout, "  -t, --timeout <sec.millisec>\tthe amount of time to wait on I/O\n");
  fprintf(stdout, "  -U, --url <url>\t\tstring to send as the url parameter\n");
  fprintf(stdout, "  -u, --user <user[:passwd]>\tuser ID and password, if either is left blank \n\t\t\t\tthey will be prompted for\n");
  fprintf(stdout, "  -w, --wildcard\t\tset your domain to have a wildcard alias\n");
  fprintf(stdout, "      --help\t\t\tdisplay this help and exit\n");
  fprintf(stdout, "      --version\t\t\toutput version information and exit\n");
  fprintf(stdout, "      --credits\t\t\tprint the credits and exit\n");
  fprintf(stdout, "\n");
}

void print_version( void )
{
  fprintf(stdout, "%s: - %s - $Id: ez-ipupdate.c,v 1.13 2000/04/20 06:59:07 amackay Exp $\n", program_name, VERSION);
}

void print_credits( void )
{
  fprintf( stdout, "AUTHORS / CONTRIBUTORS\n"
      "  Angus Mackay <amackay@gusnet.cx>\n"
      "\n" );
}

#if HAVE_SIGNAL_H
RETSIGTYPE sigint_handler(int sig)
{
  char message[] = "interupted.\n";
  close(client_sockfd);
  write(2, message, sizeof(message)-1);
  exit(0);
}
RETSIGTYPE generic_sig_handler(int sig)
{
  last_sig = sig;
}
#endif

int option_handler(int id, char *optarg)
{
#if HAVE_PWD_H && HAVE_GRP_H
  struct passwd *pw;
#endif
  char *tmp;
  int i;

  switch(id)
  {
    case CMD_address:
      if(address) { free(address); }
      address = strdup(optarg);
      dprintf((stderr, "address: %s\n", address));
      break;

    case CMD_daemon:
      options |= OPT_DAEMON;
      dprintf((stderr, "daemon mode\n"));
      break;

    case CMD_debug:
#ifdef DEBUG
      options |= OPT_DEBUG;
      dprintf((stderr, "debugging on\n"));
#else
      fprintf(stderr, "debugging was not enabled at compile time\n");
#endif
      break;

    case CMD_foreground:
      options |= OPT_FOREGROUND;
      dprintf((stderr, "fork()ing off\n"));
      break;

    case CMD_host:
      if(host) { free(host); }
      host = strdup(optarg);
      dprintf((stderr, "host: %s\n", host));
      break;

    case CMD_interface:
      if(interface) { free(interface); }
      interface = strdup(optarg);
      dprintf((stderr, "interface: %s\n", interface));
      break;

    case CMD_mx:
      if(mx) { free(mx); }
      mx = strdup(optarg);
      dprintf((stderr, "mx: %s\n", mx));
      break;

    case CMD_max_interval:
      max_interval = atoi(optarg);
      dprintf((stderr, "max_interval: %d\n", max_interval));
      break;

    case CMD_period:
      update_period = atoi(optarg);
      dprintf((stderr, "update_period: %d\n", update_period));
      break;

    case CMD_resolv_period:
      resolv_period = atoi(optarg);
      dprintf((stderr, "resolv_period: %d\n", resolv_period));
      break;

    case CMD_quiet:
      options |= OPT_QUIET;
      dprintf((stderr, "quiet mode\n"));
      break;

    case CMD_retrys:
      ntrys = atoi(optarg);
      dprintf((stderr, "ntrys: %d\n", ntrys));
      break;

    case CMD_server:
      if(server) { free(server); }
      server = strdup(optarg);
      tmp = strchr(server, ':');
      if(tmp)
      {
        *tmp++ = '\0';
        if(port) { free(port); }
        port = strdup(tmp);
      }
      dprintf((stderr, "server: %s\n", server));
      dprintf((stderr, "port: %s\n", port));
      break;

    case CMD_service_type:
      if(strcmp("ezip", optarg) == 0 || strcmp("ez-ip", optarg) == 0)
      {
        service = &EZIP_service;
      }
      else if(strcmp("pgpow", optarg) == 0 || 
          strcmp("penguinpowered", optarg) == 0 ||
          strcmp("justlinix", optarg) == 0)
      {
        service = &PGPOW_service;
      }
      else if(strcmp("dhs", optarg) == 0)
      {
        service = &DHS_service;
      }
      else if(strcmp("dyndns", optarg) == 0)
      {
        service = &DYNDNS_service;
      }
      else if(strcmp("dyndns-stat", optarg) == 0 ||
          strcmp("dyndns-static", optarg) == 0)
      {
        service = &DYNDNS_STAT_service;
      }
      else
      {
        fprintf(stderr, "unknown service type: %s\n", optarg);
        fprintf(stderr, "try one of: ezip, pgpow, dhs, dyndns, dyndns-static\n");
        exit(1);
      }
      service_set = 1;
      dprintf((stderr, "service_type: %s\n", service->name));
      break;

    case CMD_user:
      strncpy(user, optarg, sizeof(user));
      user[sizeof(user)-1] = '\0';
      dprintf((stderr, "user: %s\n", user));
      tmp = strchr(optarg, ':');
      if(tmp)
      {
        tmp++;
        while(*tmp) { *tmp++ = '*'; }
      }
      break;

    case CMD_run_as_user:
#if HAVE_PWD_H && HAVE_GRP_H
      if((pw=getpwnam(optarg)) == NULL)
      {
        i = atoi(optarg);
      }
      else
      {
        if(setgid(pw->pw_gid) != 0)
        {
          fprintf(stderr, "error changing group id\n");
        }
        dprintf((stderr, "GID now %d\n", pw->pw_gid));
        i = pw->pw_uid;
      }
      if(setuid(i) != 0)
      {
        fprintf(stderr, "error changing user id\n");
      }
      dprintf((stderr, "UID now %d\n", i));
#else
      fprintf(stderr, "option \"daemon-user\" not supported on this system\n");
#endif
      break;

    case CMD_url:
      if(url) { free(url); }
      url = strdup(optarg);
      dprintf((stderr, "url: %s\n", url));
      break;

    case CMD_wildcard:
      wildcard = 1;
      dprintf((stderr, "wildcard: %d\n", wildcard));
      break;

    case CMD_cloak_title:
      if(cloak_title) { free(cloak_title); }
      cloak_title = strdup(optarg);
      dprintf((stderr, "cloak_title: %s\n", cloak_title));
      break;

    case CMD_timeout:
      timeout.tv_sec = atoi(optarg);
      timeout.tv_usec = (atof(optarg) - timeout.tv_sec) * 1000000L;
      dprintf((stderr, "timeout: %ld.%06ld\n", timeout.tv_sec, timeout.tv_usec));
      break;

    default:
      dprintf((stderr, "case not handled: %d\n", id));
      break;
  }

  return 0;
}

int conf_handler(struct conf_cmd *cmd, char *arg)
{
  return(option_handler(cmd->id, arg));
}


#ifdef HAVE_GETOPT_LONG
#  define xgetopt( x1, x2, x3, x4, x5 ) getopt_long( x1, x2, x3, x4, x5 )
#else
#  define xgetopt( x1, x2, x3, x4, x5 ) getopt( x1, x2, x3 )
#endif

void parse_args( int argc, char **argv )
{
#ifdef HAVE_GETOPT_LONG
  struct option long_options[] = {
      {"address",       required_argument,      0, 'a'},
      {"config_file",   required_argument,      0, 'c'},
      {"daemon",        no_argument,            0, 'd'},
      {"debug",         no_argument,            0, 'D'},
      {"foreground",    no_argument,            0, 'f'},
      {"host",          required_argument,      0, 'h'},
      {"interface",     required_argument,      0, 'i'},
      {"cloak_title",   required_argument,      0, 'L'},
      {"mx",            required_argument,      0, 'm'},
      {"max-interval",  required_argument,      0, 'M'},
      {"resolv-period", required_argument,      0, 'p'},
      {"period",        required_argument,      0, 'P'},
      {"quiet",         no_argument,            0, 'q'},
      {"retrys",        required_argument,      0, 'r'},
      {"run-as-user",   required_argument,      0, 'R'},
      {"server",        required_argument,      0, 's'},
      {"service-type",  required_argument,      0, 'S'},
      {"timeout",       required_argument,      0, 't'},
      {"url",           required_argument,      0, 'U'},
      {"user",          required_argument,      0, 'u'},
      {"wildcard",      no_argument,            0, 'w'},
      {"help",          no_argument,            0, 'H'},
      {"version",       no_argument,            0, 'V'},
      {"credits",       no_argument,            0, 'C'},
      {0,0,0,0}
  };
#else
#  define long_options NULL
#endif
  int opt;

  while((opt=xgetopt(argc, argv, "a:c:dDfh:i:L:m:M:p:P:qr:R:s:S:t:U:u:wHVC", 
          long_options, NULL)) != -1)
  {
    switch (opt)
    {
      case 'a':
        option_handler(CMD_address, optarg);
        break;

      case 'c':
        if(config_file) { free(config_file); }
        config_file = strdup(optarg);
        dprintf((stderr, "config_file: %s\n", config_file));
        if(config_file)
        {
          if(parse_conf_file(config_file, conf_commands) != 0)
          {
            fprintf(stderr, "error parsing config file \"%s\"\n", config_file);
            exit(1);
          }
        }
        break;

      case 'd':
        option_handler(CMD_daemon, optarg);
        break;

      case 'D':
        option_handler(CMD_debug, optarg);
        break;

      case 'f':
        option_handler(CMD_foreground, optarg);
        break;

      case 'h':
        option_handler(CMD_host, optarg);
        break;

      case 'i':
        option_handler(CMD_interface, optarg);
        break;

      case 'L':
        option_handler(CMD_cloak_title, optarg);
        break;

      case 'm':
        option_handler(CMD_mx, optarg);
        break;

      case 'M':
        option_handler(CMD_max_interval, optarg);
        break;

      case 'p':
        option_handler(CMD_resolv_period, optarg);
        break;

      case 'P':
        option_handler(CMD_period, optarg);
        break;

      case 'q':
        option_handler(CMD_quiet, optarg);
        break;

      case 'r':
        option_handler(CMD_retrys, optarg);
        break;

      case 'R':
        option_handler(CMD_run_as_user, optarg);
        break;

      case 's':
        option_handler(CMD_server, optarg);
        break;

      case 'S':
        option_handler(CMD_service_type, optarg);
        break;

      case 't':
        option_handler(CMD_timeout, optarg);
        break;

      case 'u':
        option_handler(CMD_user, optarg);
        break;

      case 'U':
        option_handler(CMD_url, optarg);
        break;

      case 'w':
        option_handler(CMD_wildcard, optarg);
        break;

      case 'H':
        print_usage();
        exit(0);
        break;

      case 'V':
        print_version();
        exit(0);
        break;

      case 'C':
        print_credits();
        exit(0);
        break;

      default:
#ifdef HAVE_GETOPT_LONG
        fprintf(stderr, "Try `%s --help' for more information\n", argv[0]);
#else
        fprintf(stderr, "Try `%s -H' for more information\n", argv[0]);
        fprintf(stderr, "warning: this program was compilied without getopt_long\n");
        fprintf(stderr, "         as such all long options will not work!\n");
#endif
        exit(1);
        break;
    }
  }
}

/*
 * do_connect
 *
 * connect a socket and return the file descriptor
 *
 */
int do_connect(int *sock, char *host, char *port)
{
  struct sockaddr_in address;
  int len;
  int result;
  struct hostent *hostinfo;
  struct servent *servinfo;

  // set up the socket
  *sock = socket(AF_INET, SOCK_STREAM, 0);
  address.sin_family = AF_INET;

  // get the host address
  hostinfo = gethostbyname(host);
  if(!hostinfo)
  {
    if(!(options & OPT_QUIET))
    {
      herror("gethostbyname");
    }
    return(-1);
  }
  address.sin_addr = *(struct in_addr *)*hostinfo -> h_addr_list;

  // get the host port
  servinfo = getservbyname(port, "tcp");
  if(servinfo)
  {
    address.sin_port = servinfo -> s_port;
  }
  else
  {
    address.sin_port = htons(atoi(port));
  }


  // connect the socket
  len = sizeof(address);
  result = connect(*sock, (struct sockaddr *)&address, len);

  if(result == -1) {
    if(!(options & OPT_QUIET))
    {
      perror("connect");
    }
    return(-1);
  }

  // print out some info
  if(!(options & OPT_QUIET))
  {
    fprintf(stderr,
        "connected to %s (%s) on port %d.\n",
        host,
        inet_ntoa(address.sin_addr),
        ntohs(address.sin_port));
  }

  return 0;
}

static char table64[]=
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
void base64Encode(char *intext, char *output)
{
  unsigned char ibuf[3];
  unsigned char obuf[4];
  int i;
  int inputparts;

  while(*intext) {
    for (i = inputparts = 0; i < 3; i++) { 
      if(*intext) {
        inputparts++;
        ibuf[i] = *intext;
        intext++;
      }
      else
        ibuf[i] = 0;
    }

    obuf [0] = (ibuf [0] & 0xFC) >> 2;
    obuf [1] = ((ibuf [0] & 0x03) << 4) | ((ibuf [1] & 0xF0) >> 4);
    obuf [2] = ((ibuf [1] & 0x0F) << 2) | ((ibuf [2] & 0xC0) >> 6);
    obuf [3] = ibuf [2] & 0x3F;

    switch(inputparts) {
      case 1: /* only one byte read */
        sprintf(output, "%c%c==", 
            table64[obuf[0]],
            table64[obuf[1]]);
        break;
      case 2: /* two bytes read */
        sprintf(output, "%c%c%c=", 
            table64[obuf[0]],
            table64[obuf[1]],
            table64[obuf[2]]);
        break;
      default:
        sprintf(output, "%c%c%c%c", 
            table64[obuf[0]],
            table64[obuf[1]],
            table64[obuf[2]],
            table64[obuf[3]] );
        break;
    }
    output += 4;
  }
  *output=0;
}

void output(void *buf)
{
  fd_set writefds;
  int max_fd;
  struct timeval tv;
  int ret;

  dprintf((stderr, "I say: %s\n", (char *)buf));

  // set up our fdset and timeout
  FD_ZERO(&writefds);
  FD_SET(client_sockfd, &writefds);
  max_fd = client_sockfd;
  memcpy(&tv, &timeout, sizeof(struct timeval));

  ret = select(max_fd + 1, NULL, &writefds, NULL, &tv);
  dprintf((stderr, "ret: %d\n", ret));

  if(ret == -1)
  {
    dprintf((stderr, "select: %s\n", error_string));
  }
  else if(ret == 0)
  {
    fprintf(stderr, "timeout\n");
  }
  else
  {
    /* if we woke up on client_sockfd do the data passing */
    if(FD_ISSET(client_sockfd, &writefds))
    {
      if(send(client_sockfd, buf, strlen(buf), 0) == -1)
      {
        fprintf(stderr, "error send()ing request\n");
      }
    }
    else
    {
      dprintf((stderr, "error: case not handled."));
    }
  }
}

int read_input(void *buf, int len)
{
  fd_set readfds;
  int max_fd;
  struct timeval tv;
  int ret;
  int bread = -1;

  // set up our fdset and timeout
  FD_ZERO(&readfds);
  FD_SET(client_sockfd, &readfds);
  max_fd = client_sockfd;
  memcpy(&tv, &timeout, sizeof(struct timeval));

  ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
  dprintf((stderr, "ret: %d\n", ret));

  if(ret == -1)
  {
    dprintf((stderr, "select: %s\n", error_string));
  }
  else if(ret == 0)
  {
    fprintf(stderr, "timeout\n");
  }
  else
  {
    /* if we woke up on client_sockfd do the data passing */
    if(FD_ISSET(client_sockfd, &readfds))
    {
      if((bread=recv(client_sockfd, buf, len, 0)) == -1)
      {
        fprintf(stderr, "error send()ing request\n");
      }
    }
    else
    {
      dprintf((stderr, "error: case not handled."));
    }
  }

  return(bread);
}

#ifdef IF_LOOKUP
int get_if_addr(int sock, char *name, struct sockaddr_in *sin)
{
  struct ifreq ifr;

  memset(&ifr, 0, sizeof(ifr));
  strcpy(ifr.ifr_name, name);
  /* why does this need to be done twice? */
  if(ioctl(sock, SIOCGIFADDR, &ifr) < 0) 
  { 
    perror("ioctl(SIOCGIFADDR)"); 
    memset(sin, 0, sizeof(struct sockaddr_in));
    dprintf((stderr, "%s: %s\n", name, "unknown interface"));
    return -1;
  }
  if(ioctl(sock, SIOCGIFADDR, &ifr) < 0)
  { 
    perror("ioctl(SIOCGIFADDR)"); 
    memset(sin, 0, sizeof(struct sockaddr_in));
    dprintf((stderr, "%s: %s\n", name, "unknown interface"));
    return -1;
  }

  if(ifr.ifr_addr.sa_family == AF_INET)
  {
    memcpy(sin, &(ifr.ifr_addr), sizeof(struct sockaddr_in));
    dprintf((stderr, "%s: %s\n", name, inet_ntoa(sin->sin_addr)));
    return 0;
  }
  else
  {
    memset(sin, 0, sizeof(struct sockaddr_in));
    dprintf((stderr, "%s: %s\n", name, "could not resolve interface"));
    return -1;
  }
  return -1;
}
#endif

static int read_response(char *buf)
{
  int bytes; 

  bytes = read_input(buf, BUFFER_SIZE);
  if(bytes < 1)
  {
    close(client_sockfd);
    return(-1);
  }
  buf[bytes] = '\0';

  dprintf((stderr, "server says: %s\n", buf));
  
  if(strncmp("OK", buf, 2) != 0)
  {
    return(1);
  }
  else
  {
    return(0);
  }
}

int PGPOW_check_info(void)
{
  char buf[BUFSIZ+1];

  if(host == NULL)
  {
    printf("host: ");
    fgets(buf, BUFSIZ, stdin);
    host = strdup(buf);
  }

  return 0;
}

int EZIP_check_info(void)
{
  return 0;
}

int EZIP_update_entry(void)
{
  char buf[BUFFER_SIZE+1];
  char *bp = buf;
  int bytes;
  int btot;
  int ret;

  buf[BUFFER_SIZE] = '\0';

  if(do_connect((int*)&client_sockfd, server, port) != 0)
  {
    if(!(options & OPT_QUIET))
    {
      fprintf(stderr, "error connecting to %s:%s\n", server, port);
    }
    close(client_sockfd);
    return(-1);
  }

  snprint(buf, BUFFER_SIZE, "GET %s?mode=update&", request);
  output(buf);
  snprint(buf, BUFFER_SIZE, "%s=%s&", "ipaddress", address);
  output(buf);
  snprint(buf, BUFFER_SIZE, "%s=%s&", "wildcard", wildcard ? "yes" : "no");
  output(buf);
  snprint(buf, BUFFER_SIZE, "%s=%s&", "mx", mx);
  output(buf);
  snprint(buf, BUFFER_SIZE, "%s=%s&", "url", url);
  output(buf);
  snprint(buf, BUFFER_SIZE, "%s=%s&", "host", host);
  output(buf);
  snprint(buf, BUFFER_SIZE, " HTTP/1.0\015\012");
  output(buf);
  snprint(buf, BUFFER_SIZE, "Authorization: Basic %s\015\012", auth);
  output(buf);
  snprint(buf, BUFFER_SIZE, "User-Agent: %s-%s %s (%s)\015\012", 
      "ez-update", VERSION, OS, "by Angus Mackay");
  output(buf);
  snprint(buf, BUFFER_SIZE, "Host: %s\015\012", server);
  output(buf);
  snprint(buf, BUFFER_SIZE, "\015\012");
  output(buf);

  bp = buf;
  bytes = 0;
  btot = 0;
  while((bytes=read_input(bp, BUFFER_SIZE-btot)) > 0)
  {
    bp += bytes;
    btot += bytes;
    dprintf((stderr, "btot: %d\n", btot));
  }
  close(client_sockfd);
  buf[btot] = '\0';

  dprintf((stderr, "server output: %s\n", buf));

  if(sscanf(buf, " HTTP/1.%*c %3d", &ret) != 1)
  {
    ret = -1;
  }

  switch(ret)
  {
    case -1:
      if(!(options & OPT_QUIET))
      {
        fprintf(stderr, "strange server response, are you connecting to the right server?\n");
      }
      return(-1);
      break;

    case 200:
      if(!(options & OPT_QUIET))
      {
        printf("request successful\n");
      }
      break;

    case 401:
      if(!(options & OPT_QUIET))
      {
        fprintf(stderr, "authentication failure\n");
      }
      return(-1);
      break;

    default:
      if(!(options & OPT_QUIET))
      {
        // reuse the auth buffer
        *auth = '\0';
        sscanf(buf, " HTTP/1.%*c %*3d %255[^\r\n]", auth);
        fprintf(stderr, "unknown return code: %d\n", ret);
        fprintf(stderr, "server response: %s\n", auth);
      }
      return(-1);
      break;
  }

  return 0;
}

int DYNDNS_check_info(void)
{
  return 0;
}

int DYNDNS_update_entry(void)
{
  char buf[BUFFER_SIZE+1];
  char *bp = buf;
  int bytes;
  int btot;
  int ret;

  buf[BUFFER_SIZE] = '\0';

  // make sure that we can get our own ip address first
  if((options & OPT_DAEMON) || address == NULL || *address == '\0')
  {
#ifdef IF_LOOKUP
    struct sockaddr_in sin;
    int sock;
#endif

    if(address) { free(address); address = NULL; }

#ifdef IF_LOOKUP
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(get_if_addr(sock, interface, &sin) == 0)
    {
      if(address) { free(address); }
      address = strdup(inet_ntoa(sin.sin_addr)),
      close(sock);
    }
    else
    {
      fprintf(stderr, "could not resolve ip address.\n");
      close(sock);
      return(-1);
    }
    close(sock);
#else
    printf("ip address: ");
    fgets(buf, BUFSIZ, stdin);
    address = strdup(buf);
#endif
  }

  if(do_connect((int*)&client_sockfd, server, port) != 0)
  {
    if(!(options & OPT_QUIET))
    {
      fprintf(stderr, "error connecting to %s:%s\n", server, port);
    }
    close(client_sockfd);
    return(-1);
  }

  snprint(buf, BUFFER_SIZE, "GET %s?action=edit&started=1&hostname=YES&", request);
  output(buf);
  snprint(buf, BUFFER_SIZE, "%s=%s&", "myip", address);
  output(buf);
  snprint(buf, BUFFER_SIZE, "%s=%s&", "wildcard", wildcard ? "ON" : "OFF");
  output(buf);
  snprint(buf, BUFFER_SIZE, "%s=%s&", "mx", mx);
  output(buf);
  snprint(buf, BUFFER_SIZE, "%s=%s&", "backmx", "NO");
  output(buf);
  snprint(buf, BUFFER_SIZE, "%s=%s&", "host_id", host);
  output(buf);
  snprint(buf, BUFFER_SIZE, " HTTP/1.0\015\012");
  output(buf);
  snprint(buf, BUFFER_SIZE, "Authorization: Basic %s\015\012", auth);
  output(buf);
  snprint(buf, BUFFER_SIZE, "User-Agent: %s-%s %s (%s)\015\012", 
      "ez-update", VERSION, OS, "by Angus Mackay");
  output(buf);
  snprint(buf, BUFFER_SIZE, "Host: %s\015\012", server);
  output(buf);
  snprint(buf, BUFFER_SIZE, "\015\012");
  output(buf);

  bp = buf;
  bytes = 0;
  btot = 0;
  while((bytes=read_input(bp, BUFFER_SIZE-btot)) > 0)
  {
    bp += bytes;
    btot += bytes;
    dprintf((stderr, "btot: %d\n", btot));
  }
  close(client_sockfd);
  buf[btot] = '\0';

  dprintf((stderr, "server output: %s\n", buf));

  if(sscanf(buf, " HTTP/1.%*c %3d", &ret) != 1)
  {
    ret = -1;
  }

  switch(ret)
  {
    case -1:
      if(!(options & OPT_QUIET))
      {
        fprintf(stderr, "strange server response, are you connecting to the right server?\n");
      }
      return(-1);
      break;

    case 200:
      if(strstr(buf, "NOERROR\n") != NULL)
      {
        if(!(options & OPT_QUIET))
        {
          printf("request successful\n");
        }
      }
      else
      {
        fprintf(stderr, "error processing request\n");
        if(!(options & OPT_QUIET))
        {
          fprintf(stderr, "server output: %s\n", buf);
        }
        return(-1);
      }
      break;

    case 401:
      if(!(options & OPT_QUIET))
      {
        fprintf(stderr, "authentication failure\n");
      }
      return(-1);
      break;

    default:
      if(!(options & OPT_QUIET))
      {
        // reuse the auth buffer
        *auth = '\0';
        sscanf(buf, " HTTP/1.%*c %*3d %255[^\r\n]", auth);
        fprintf(stderr, "unknown return code: %d\n", ret);
        fprintf(stderr, "server response: %s\n", auth);
      }
      return(-1);
      break;
  }

  return 0;
}

int PGPOW_update_entry(void)
{
  char buf[BUFFER_SIZE+1];

  buf[BUFFER_SIZE] = '\0';

  // make sure that we can get our own ip address first
  if(strcmp("update", request) == 0)
  {
    if((options & OPT_DAEMON) || address == NULL || *address == '\0')
    {
#ifdef IF_LOOKUP
      struct sockaddr_in sin;
      int sock;
#endif

      if(address) { free(address); address = NULL; }

#ifdef IF_LOOKUP
      sock = socket(AF_INET, SOCK_STREAM, 0);
      if(get_if_addr(sock, interface, &sin) == 0)
      {
        if(address) { free(address); }
        address = strdup(inet_ntoa(sin.sin_addr)),
        close(sock);
      }
      else
      {
        fprintf(stderr, "could not resolve ip address.\n");
        close(sock);
        return(-1);
      }
      close(sock);
#else
      printf("ip address: ");
      fgets(buf, BUFSIZ, stdin);
      address = strdup(buf);
#endif
    }
  }

  if(do_connect((int*)&client_sockfd, server, port) != 0)
  {
    if(!(options & OPT_QUIET))
    {
      fprintf(stderr, "error connecting to %s:%s\n", server, port);
    }
    close(client_sockfd);
    return(-1);
  }

  /* read server message */
  if(read_response(buf) != 0)
  {
    fprintf(stderr, "strange server response, are you connecting to the right server?\n");
    close(client_sockfd);
    return(-1);
  }

  /* send version command */
  snprint(buf, BUFFER_SIZE, "VER %s [%s-%s %s (%s)]\015\012", PGPOW_VERSION,
      "ez-update", VERSION, OS, "by Angus Mackay");
  output(buf);

  if(read_response(buf) != 0)
  {
    if(strncmp("ERR", buf, 3) == 0)
    {
      fprintf(stderr, "error talking to server: %s\n", &(buf[3]));
    }
    else
    {
      fprintf(stderr, "error talking to server:\n\t%s\n", buf);
    }
    close(client_sockfd);
    return(-1);
  }

  /* send user command */
  snprint(buf, BUFFER_SIZE, "USER %s\015\012", user_name);
  output(buf);

  if(read_response(buf) != 0)
  {
    if(strncmp("ERR", buf, 3) == 0)
    {
      fprintf(stderr, "error talking to server: %s\n", &(buf[3]));
    }
    else
    {
      fprintf(stderr, "error talking to server:\n\t%s\n", buf);
    }
    close(client_sockfd);
    return(-1);
  }

  /* send pass command */
  snprint(buf, BUFFER_SIZE, "PASS %s\015\012", password);
  output(buf);

  if(read_response(buf) != 0)
  {
    if(strncmp("ERR", buf, 3) == 0)
    {
      fprintf(stderr, "error talking to server: %s\n", &(buf[3]));
    }
    else
    {
      fprintf(stderr, "error talking to server:\n\t%s\n", buf);
    }
    close(client_sockfd);
    return(-1);
  }

  /* send host command */
  snprint(buf, BUFFER_SIZE, "HOST %s\015\012", host);
  output(buf);

  if(read_response(buf) != 0)
  {
    if(strncmp("ERR", buf, 3) == 0)
    {
      fprintf(stderr, "error talking to server: %s\n", &(buf[3]));
    }
    else
    {
      fprintf(stderr, "error talking to server:\n\t%s\n", buf);
    }
    close(client_sockfd);
    return(-1);
  }

  /* send oper command */
  snprint(buf, BUFFER_SIZE, "OPER %s\015\012", request);
  output(buf);

  if(read_response(buf) != 0)
  {
    if(strncmp("ERR", buf, 3) == 0)
    {
      fprintf(stderr, "error talking to server: %s\n", &(buf[3]));
    }
    else
    {
      fprintf(stderr, "error talking to server:\n\t%s\n", buf);
    }
    close(client_sockfd);
    return(-1);
  }

  if(strcmp("update", request) == 0)
  {
    /* send ip command */
    snprint(buf, BUFFER_SIZE, "IP %s\015\012", address);
    output(buf);

    if(read_response(buf) != 0)
    {
      if(strncmp("ERR", buf, 3) == 0)
      {
        fprintf(stderr, "error talking to server: %s\n", &(buf[3]));
      }
      else
      {
        fprintf(stderr, "error talking to server:\n\t%s\n", buf);
      }
      close(client_sockfd);
      return(-1);
    }
  }

  /* send done command */
  snprint(buf, BUFFER_SIZE, "DONE\015\012");
  output(buf);

  if(read_response(buf) != 0)
  {
    if(strncmp("ERR", buf, 3) == 0)
    {
      fprintf(stderr, "error talking to server: %s\n", &(buf[3]));
    }
    else
    {
      fprintf(stderr, "error talking to server:\n\t%s\n", buf);
    }
    close(client_sockfd);
    return(-1);
  }

  if(!(options & OPT_QUIET))
  {
    printf("request successful\n");
  }

  close(client_sockfd);
  return 0;
}

int DHS_check_info(void)
{
  return 0;
}

/*
 * grrrrr, it seems that dhs.org requires us to use POST
 * also DHS doesn't update both the mx record and the address at the same
 * time, this service really stinks. go with justlinix.com (penguinpowered)
 * instead, the only advantage is short host names.
 */
int DHS_update_entry(void)
{
  char buf[BUFFER_SIZE+1];
  char putbuf[BUFFER_SIZE+1];
  char *bp = buf;
  int bytes;
  int btot;
  int ret;
  char *domain = NULL;
  char *hostname = NULL;
  char *p;
  int limit;
  int retval = 0;

  buf[BUFFER_SIZE] = '\0';
  putbuf[BUFFER_SIZE] = '\0';

  // make sure that we can get our own ip address first
  if(1)
  {
    if((options & OPT_DAEMON) || address == NULL || *address == '\0')
    {
#ifdef IF_LOOKUP
      struct sockaddr_in sin;
      int sock;
#endif

      if(address) { free(address); address = NULL; }

#ifdef IF_LOOKUP
      sock = socket(AF_INET, SOCK_STREAM, 0);
      if(get_if_addr(sock, interface, &sin) == 0)
      {
        if(address) { free(address); }
        address = strdup(inet_ntoa(sin.sin_addr)),
        close(sock);
      }
      else
      {
        fprintf(stderr, "could not resolve ip address.\n");
        close(sock);
        return(-1);
      }
      close(sock);
#else
      printf("ip address: ");
      fgets(buf, BUFSIZ, stdin);
      address = strdup(buf);
#endif
    }
  }


  /* parse apart the domain and hostname */
  hostname = strdup(host);
  if((p=strchr(hostname, '.')) == NULL)
  {
    if(!(options & OPT_QUIET))
    {
      fprintf(stderr, "error parsing hostname from host %s\n", host);
    }
    return(-1);
  }
  *p = '\0';
  p++;
  if(*p == '\0')
  {
    if(!(options & OPT_QUIET))
    {
      fprintf(stderr, "error parsing domain from host %s\n", host);
    }
    return(-1);
  }
  domain = strdup(p);

  dprintf((stderr, "hostname: %s, domain: %s\n", hostname, domain));

  if(do_connect((int*)&client_sockfd, server, port) != 0)
  {
    if(!(options & OPT_QUIET))
    {
      fprintf(stderr, "error connecting to %s:%s\n", server, port);
    }
    close(client_sockfd);
    return(-1);
  }

  snprint(buf, BUFFER_SIZE, "POST %s HTTP/1.0\015\012", request);
  output(buf);
  snprint(buf, BUFFER_SIZE, "Authorization: Basic %s\015\012", auth);
  output(buf);
  snprint(buf, BUFFER_SIZE, "User-Agent: %s-%s %s (%s)\015\012", 
      "ez-update", VERSION, OS, "by Angus Mackay");
  output(buf);
  snprint(buf, BUFFER_SIZE, "Host: %s\015\012", server);
  output(buf);

  p = putbuf;
  *p = '\0';
  limit = BUFFER_SIZE - 1 - strlen(buf);
  snprint(p, limit, "hostscmd=edit&hostscmdstage=2&type=4&");
  p += strlen(p);
  limit = BUFFER_SIZE - 1 - strlen(buf);
  snprint(p, limit, "%s=%s&", "updatetype", "Online");
  p += strlen(p);
  limit = BUFFER_SIZE - 1 - strlen(buf);
  snprint(p, limit, "%s=%s&", "ip", address);
  p += strlen(p);
  limit = BUFFER_SIZE - 1 - strlen(buf);
  snprint(p, limit, "%s=%s&", "mx", mx);
  p += strlen(p);
  limit = BUFFER_SIZE - 1 - strlen(buf);
  snprint(p, limit, "%s=%s&", "offline_url", url);
  p += strlen(p);
  limit = BUFFER_SIZE - 1 - strlen(buf);
  if(cloak_title)
  {
    snprint(p, limit, "%s=%s&", "cloak", "Y");
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);
    snprint(p, limit, "%s=%s&", "cloak_title", cloak_title);
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);
  }
  else
  {
    snprint(p, limit, "%s=%s&", "cloak_title", "");
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);
  }
  snprint(p, limit, "%s=%s&", "submit", "Update");
  p += strlen(p);
  limit = BUFFER_SIZE - 1 - strlen(buf);
  snprint(p, limit, "%s=%s&", "domain", domain);
  p += strlen(p);
  limit = BUFFER_SIZE - 1 - strlen(buf);
  snprint(p, limit, "%s=%s", "hostname", hostname);
  p += strlen(p);
  limit = BUFFER_SIZE - 1 - strlen(buf);

  snprint(buf, BUFFER_SIZE, "Content-length: %d\015\012", strlen(putbuf));
  output(buf);
  snprint(buf, BUFFER_SIZE, "\015\012");
  output(buf);

  output(putbuf);
  snprint(buf, BUFFER_SIZE, "\015\012");
  output(buf);

  bp = buf;
  bytes = 0;
  btot = 0;
  while((bytes=read_input(bp, BUFFER_SIZE-btot)) > 0)
  {
    bp += bytes;
    btot += bytes;
    dprintf((stderr, "btot: %d\n", btot));
  }
  close(client_sockfd);
  buf[btot] = '\0';

  dprintf((stderr, "server output: %s\n", buf));

  if(sscanf(buf, " HTTP/1.%*c %3d", &ret) != 1)
  {
    ret = -1;
  }

  switch(ret)
  {
    case -1:
      if(!(options & OPT_QUIET))
      {
        fprintf(stderr, "strange server response, are you connecting to the right server?\n");
      }
      retval = -1;
      break;

    case 200:
      if(!(options & OPT_QUIET))
      {
        printf("request successful\n");
      }
      break;

    case 401:
      if(!(options & OPT_QUIET))
      {
        fprintf(stderr, "authentication failure\n");
      }
      retval = -1;
      break;

    default:
      if(!(options & OPT_QUIET))
      {
        // reuse the auth buffer
        *auth = '\0';
        sscanf(buf, " HTTP/1.%*c %*3d %255[^\r\n]", auth);
        fprintf(stderr, "unknown return code: %d\n", ret);
        fprintf(stderr, "server response: %s\n", auth);
      }
      retval = -1;
      break;
  }

  // okay, dhs's service is incredibly stupid and will not work with two
  // requests right after each other. I could care less that this is ugly,
  // I personally will NEVER use dhs, it is laughable.
  sleep(DHS_SUCKY_TIMEOUT < timeout.tv_sec ? DHS_SUCKY_TIMEOUT : timeout.tv_sec);

  // this stupid service requires us to do seperate request if we want to 
  // update the mail exchanger (mx). grrrrrr
  if(*mx != '\0')
  {
    if(do_connect((int*)&client_sockfd, server, port) != 0)
    {
      if(!(options & OPT_QUIET))
      {
        fprintf(stderr, "error connecting to %s:%s\n", server, port);
      }
      close(client_sockfd);
      return(-1);
    }

    snprint(buf, BUFFER_SIZE, "POST %s HTTP/1.0\015\012", request);
    output(buf);
    snprint(buf, BUFFER_SIZE, "Authorization: Basic %s\015\012", auth);
    output(buf);
    snprint(buf, BUFFER_SIZE, "User-Agent: %s-%s %s (%s)\015\012", 
        "ez-update", VERSION, OS, "by Angus Mackay");
    output(buf);
    snprint(buf, BUFFER_SIZE, "Host: %s\015\012", server);
    output(buf);

    p = putbuf;
    *p = '\0';
    limit = BUFFER_SIZE - 1 - strlen(buf);
    snprint(p, limit, "hostscmd=edit&hostscmdstage=2&type=4&");
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);
    snprint(p, limit, "%s=%s&", "updatetype", "Update+Mail+Exchanger");
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);
    snprint(p, limit, "%s=%s&", "ip", address);
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);
    snprint(p, limit, "%s=%s&", "mx", mx);
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);
    snprint(p, limit, "%s=%s&", "offline_url", url);
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);
    if(cloak_title)
    {
      snprint(p, limit, "%s=%s&", "cloak", "Y");
      p += strlen(p);
      limit = BUFFER_SIZE - 1 - strlen(buf);
      snprint(p, limit, "%s=%s&", "cloak_title", cloak_title);
      p += strlen(p);
      limit = BUFFER_SIZE - 1 - strlen(buf);
    }
    else
    {
      snprint(p, limit, "%s=%s&", "cloak_title", "");
      p += strlen(p);
      limit = BUFFER_SIZE - 1 - strlen(buf);
    }
    snprint(p, limit, "%s=%s&", "submit", "Update");
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);
    snprint(p, limit, "%s=%s&", "domain", domain);
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);
    snprint(p, limit, "%s=%s", "hostname", hostname);
    p += strlen(p);
    limit = BUFFER_SIZE - 1 - strlen(buf);

    snprint(buf, BUFFER_SIZE, "Content-length: %d\015\012", strlen(putbuf));
    output(buf);
    snprint(buf, BUFFER_SIZE, "\015\012");
    output(buf);

    output(putbuf);
    snprint(buf, BUFFER_SIZE, "\015\012");
    output(buf);

    bp = buf;
    bytes = 0;
    btot = 0;
    while((bytes=read_input(bp, BUFFER_SIZE-btot)) > 0)
    {
      bp += bytes;
      btot += bytes;
      dprintf((stderr, "btot: %d\n", btot));
    }
    close(client_sockfd);
    buf[btot] = '\0';

    dprintf((stderr, "server output: %s\n", buf));

    if(sscanf(buf, " HTTP/1.%*c %3d", &ret) != 1)
    {
      ret = -1;
    }

    switch(ret)
    {
      case -1:
        if(!(options & OPT_QUIET))
        {
          fprintf(stderr, "strange server response, are you connecting to the right server?\n");
        }
        retval = -1;
        break;

      case 200:
        if(!(options & OPT_QUIET))
        {
          printf("request successful\n");
        }
        break;

      case 401:
        if(!(options & OPT_QUIET))
        {
          fprintf(stderr, "authentication failure\n");
        }
        retval = -1;
        break;

      default:
        if(!(options & OPT_QUIET))
        {
          // reuse the auth buffer
          *auth = '\0';
          sscanf(buf, " HTTP/1.%*c %*3d %255[^\r\n]", auth);
          fprintf(stderr, "unknown return code: %d\n", ret);
          fprintf(stderr, "server response: %s\n", auth);
        }
        retval = -1;
        break;
    }
  }

  return(retval);
}


void handle_sig(int sig)
{
  switch(sig)
  {
    case SIGHUP:
      if(config_file)
      {
#if HAVE_SYSLOG_H
        syslog(LOG_NOTICE, "SIGHUP recieved, re-reading config file\n");
#else
        fprintf(stderr, "SIGHUP recieved, re-reading config file\n");
#endif
        if(parse_conf_file(config_file, conf_commands) != 0)
        {
#if HAVE_SYSLOG_H
          syslog(LOG_NOTICE, "error parsing config file \"%s\"\n", config_file);
#else
          fprintf(stderr, "error parsing config file \"%s\"\n", config_file);
#endif
        }
      }
      break;
    case SIGTERM:
      /* 
       * this is used to wake up the client so that it will perform an update 
       */
      break;
    case SIGQUIT:
#if HAVE_SYSLOG_H
      syslog(LOG_NOTICE, "received SIGQUIT, shutting down\n");
      closelog();
      exit(0);
#else
      fprintf(stderr, "received SIGQUIT, shutting down\n");
      exit(0);
#endif
    default:
      dprintf((stderr, "case not handled: %d\n", sig));
      break;
  }
}

int main(int argc, char **argv)
{
  int ifresolve_warned = 0;
  int i;
  int retval = 1;
  time_t last_update = 0;
#ifdef IF_LOOKUP
  int sock = -1;
  struct sockaddr_in sin;
  struct sockaddr_in sin2;
#endif

#ifdef __GNUC__
  mcheck(NULL);
#endif

  dprintf((stderr, "staring...\n"));

  program_name = argv[0];
  options = 0;
  *user = '\0';
  timeout.tv_sec = DEFAULT_TIMEOUT;
  timeout.tv_usec = 0;

#if HAVE_SIGNAL_H
  // catch user interupts
  signal(SIGINT,  sigint_handler);
  signal(SIGHUP,  generic_sig_handler);
  signal(SIGTERM, generic_sig_handler);
  signal(SIGQUIT, generic_sig_handler);
#endif

  parse_args(argc, argv);

  if(!service_set && !(options & OPT_QUIET))
  {
    fprintf(stderr, "using default service: %s\n", service->name);
  }

  if(!(options & OPT_QUIET) && !(options & OPT_DAEMON))
  {
    fprintf(stderr, "ez-ipupdate Version %s\nCopyright (C) 1999-2000 Angus Mackay.\n", VERSION);
  }

  dprintf((stderr, "options: 0x%04X\n", options));
  dprintf((stderr, "interface: %s\n", interface));
  dprintf((stderr, "ntrys: %d\n", ntrys));
  dprintf((stderr, "server: %s:%s\n", server, port));

  dprintf((stderr, "address: %s\n", address));
  dprintf((stderr, "wildcard: %d\n", wildcard));
  dprintf((stderr, "mx: %s\n", mx));
  dprintf((stderr, "auth: %s\n", auth));

#ifdef IF_LOOKUP
  if(options & OPT_DAEMON)
  {
    sock = socket(AF_INET, SOCK_STREAM, 0);
  }
#endif

  if(server == NULL)
  {
    server = strdup(service->default_server);
  }
  if(port == NULL)
  {
    port = strdup(service->default_port);
  }

  if(host == NULL) 
  { 
    printf("host: ");
    if((host=malloc(128)) == NULL)
    {
      perror("allocating memory");
      exit(1);
    }
    fgets(host, 127, stdin);
    host[strlen(host)-1] = '\0';
  }

  *user_name = '\0';
  *password = '\0';
  if(*user != '\0')
  {
    sscanf(user, "%127[^:]:%127[^\n]", user_name, password);
    dprintf((stderr, "user_name: %s\n", user_name));
    dprintf((stderr, "password: %s\n", password));
  }
  if(*user_name == '\0')
  {
    printf("user name: ");
    fgets(user_name, 127, stdin);
    user_name[strlen(user_name)-1] = '\0';
  }
  if(*password == '\0')
  {
    strncpy(password, getpass("password: "), sizeof(password));
  }
  sprintf(user, "%s:%s", user_name, password);

  base64Encode(user, auth);

  request = strdup(service->default_request);

  if(interface == NULL) { interface = strdup(DEFAULT_IF); }

  if(service->check_info() != 0)
  {
    fprintf(stderr, "invalid data to perform requested action.");
    exit(1);
  }

  if(address == NULL) { address = strdup(""); }
  if(mx == NULL) { mx = strdup(""); }
  if(url == NULL) { url = strdup(""); }

  if(options & OPT_DAEMON)
  {
#if IF_LOOKUP
    /* background our selves */
    if(!(options & OPT_FOREGROUND))
    {
#  if HAVE_SYSLOG_H
      close(0);
      close(1);
      close(2);
#  endif
      if(fork() > 0) { exit(0); } /* parent */
    }

#  if HAVE_SYSLOG_H
    openlog(program_name, LOG_PID, LOG_USER );
    syslog(LOG_NOTICE, "ez-ipupdate Version %s, Copyright (C) 1998-2000 Angus Mackay.\n", 
        VERSION);
    syslog(LOG_NOTICE, "%s started for interface %s using server %s\n",
        program_name, interface, server);
    options |= OPT_QUIET;
#  endif
    memset(&sin, 0, sizeof(sin));

    for(;;)
    {
#if HAVE_SIGNAL_H
      /* check for signals */
      if(last_sig != 0)
      {
        handle_sig(last_sig);
        last_sig = 0;
      }
#endif
      if(get_if_addr(sock, interface, &sin2) == 0)
      {
        ifresolve_warned = 0;
        if(memcmp(&sin, &sin2, sizeof(sin)) != 0 || 
            (max_interval > 0 && time(NULL) - last_update > max_interval))
        {
          memcpy(&sin, &sin2, sizeof(sin));

          if(service->update_entry() == 0)
          {
#  if HAVE_SYSLOG_H
            syslog(LOG_NOTICE, "successful update for %s->%s\n",
                interface, inet_ntoa(sin.sin_addr));
#  else
            fprintf(stderr, "successful update for %s->%s\n",
                interface, inet_ntoa(sin.sin_addr));
#  endif
          }
          else
          {
#  if HAVE_SYSLOG_H
            syslog(LOG_NOTICE, "failure to update %s->%s\n",
                interface, inet_ntoa(sin.sin_addr));
#  else
            fprintf(stderr, "failure to update %s->%s\n",
                interface, inet_ntoa(sin.sin_addr));
#  endif
            memset(&sin, 0, sizeof(sin));
            last_update = time(NULL);
          }
        }
        sleep(update_period);
      }
      else
      {
        if(!ifresolve_warned)
        {
          ifresolve_warned = 1;
#  if HAVE_SYSLOG_H
          syslog(LOG_NOTICE, "unable to resolve interface %s\n",
              interface);
#  else
          fprintf(stderr, "unable to resolve interface %s\n",
              interface);
#  endif
        }
        sleep(resolv_period);
      }
    }
#else
    fprintf(stderr, "sorry, this mode is only available on platforms that the ");
    fprintf(stderr, "IP address \ncan be determined. feel free to hack the code");
    fprintf(stderr, " though.\n");
    exit(1);
#endif
  }
  else
  {
    for(i=0; i<ntrys; i++)
    {
      if(service->update_entry() == 0)
      {
        retval = 0;
        break;
      }
      if(i+1 != ntrys) { sleep(5); }
    }
  }

#ifdef IF_LOOKUP
  if(sock > 0) { close(sock); }
#endif

  if(address) { free(address); }
  if(config_file) { free(config_file); }
  if(host) { free(host); }
  if(interface) { free(interface); }
  if(mx) { free(mx); }
  if(port) { free(port); }
  if(request) { free(request); }
  if(server) { free(server); }
  if(url) { free(url); }

  dprintf((stderr, "done\n"));
  return(retval);
}


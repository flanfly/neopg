/* dirmngr.c - Keyserver and X.509 LDAP access
 * Copyright (C) 2002 Klarälvdalens Datakonsult AB
 * Copyright (C) 2003, 2004, 2006, 2007, 2008, 2010, 2011 g10 Code GmbH
 * Copyright (C) 2014 Werner Koch
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <fcntl.h>
#ifndef HAVE_W32_SYSTEM
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif
#ifdef HAVE_INOTIFY_INIT
# include <sys/inotify.h>
#endif /*HAVE_INOTIFY_INIT*/
#include <npth.h>

#include <gpg-error.h>

# include <gnutls/gnutls.h>


#define GNUPG_COMMON_NEED_AFLOCAL
#include "dirmngr.h"

#include <assuan.h>

#include "certcache.h"
#include "crlcache.h"
#include "crlfetch.h"
#include "misc.h"
#if USE_LDAP
# include "ldapserver.h"
#endif
#include "../common/asshelp.h"
#if USE_LDAP
# include "ldap-wrapper.h"
#endif
#include "../common/init.h"
#include "../common/gc-opt-flags.h"
#include "dns-stuff.h"
#include "http-common.h"

#ifndef ENAMETOOLONG
# define ENAMETOOLONG EINVAL
#endif

struct options dirmngr_opt;

enum cmd_and_opt_values {
  aNull = 0,
  oCsh		  = 'c',
  oQuiet	  = 'q',
  oSh		  = 's',
  oVerbose	  = 'v',
  oNoVerbose = 500,

  aServer,
  aListCRLs,
  aLoadCRL,
  aFetchCRL,
  aShutdown,
  aFlush,
  aGPGConfList,
  aGPGConfTest,

  oOptions,
  oDebug,
  oDebugAll,
  oDebugWait,
  oDebugLevel,
  oGnutlsDebug,
  oNoGreeting,
  oNoOptions,
  oHomedir,
  oNoDetach,
  oLogFile,
  oBatch,
  oDisableHTTP,
  oDisableLDAP,
  oDisableIPv4,
  oDisableIPv6,
  oIgnoreLDAPDP,
  oIgnoreHTTPDP,
  oIgnoreOCSPSvcUrl,
  oHonorHTTPProxy,
  oHTTPProxy,
  oLDAPProxy,
  oOnlyLDAPProxy,
  oLDAPFile,
  oLDAPTimeout,
  oLDAPAddServers,
  oOCSPResponder,
  oOCSPSigner,
  oOCSPMaxClockSkew,
  oOCSPMaxPeriod,
  oOCSPCurrentPeriod,
  oMaxReplies,
  oHkpCaCert,
  oFakedSystemTime,
  oForce,
  oAllowOCSP,
  oAllowVersionCheck,
  oLDAPWrapperProgram,
  oHTTPWrapperProgram,
  oIgnoreCertExtension,
  oUseTor,
  oNoUseTor,
  oKeyServer,
  oNameServer,
  oStandardResolver,
  oRecursiveResolver,
  oResolverTimeout,
  oConnectTimeout,
  oConnectQuickTimeout,
  aTest
};



static ARGPARSE_OPTS opts[] = {

  ARGPARSE_group (300, N_("@Commands:\n ")),

  ARGPARSE_c (aServer,   "server",  N_("run in server mode (foreground)") ),
  ARGPARSE_c (aListCRLs, "list-crls", N_("list the contents of the CRL cache")),
  ARGPARSE_c (aLoadCRL,  "load-crl",  N_("|FILE|load CRL from FILE into cache")),
  ARGPARSE_c (aFetchCRL, "fetch-crl", N_("|URL|fetch a CRL from URL")),
  ARGPARSE_c (aShutdown, "shutdown",  N_("shutdown the dirmngr")),
  ARGPARSE_c (aFlush,    "flush",     N_("flush the cache")),
  ARGPARSE_c (aGPGConfList, "gpgconf-list", "@"),
  ARGPARSE_c (aGPGConfTest, "gpgconf-test", "@"),

  ARGPARSE_group (301, N_("@\nOptions:\n ")),

  ARGPARSE_s_n (oVerbose,  "verbose",   N_("verbose")),
  ARGPARSE_s_n (oQuiet,    "quiet",     N_("be somewhat more quiet")),
  ARGPARSE_s_n (oSh,       "sh",        N_("sh-style command output")),
  ARGPARSE_s_n (oCsh,      "csh",       N_("csh-style command output")),
  ARGPARSE_s_s (oOptions,  "options",   N_("|FILE|read options from FILE")),
  ARGPARSE_s_s (oDebugLevel, "debug-level",
                N_("|LEVEL|set the debugging level to LEVEL")),
  ARGPARSE_s_n (oNoDetach, "no-detach", N_("do not detach from the console")),
  ARGPARSE_s_s (oLogFile,  "log-file",
                N_("|FILE|write server mode logs to FILE")),
  ARGPARSE_s_n (oBatch,    "batch",       N_("run without asking a user")),
  ARGPARSE_s_n (oForce,    "force",       N_("force loading of outdated CRLs")),
  ARGPARSE_s_n (oAllowOCSP, "allow-ocsp", N_("allow sending OCSP requests")),
  ARGPARSE_s_n (oAllowVersionCheck, "allow-version-check",
                N_("allow online software version check")),
  ARGPARSE_s_n (oDisableHTTP, "disable-http", N_("inhibit the use of HTTP")),
  ARGPARSE_s_n (oDisableLDAP, "disable-ldap", N_("inhibit the use of LDAP")),
  ARGPARSE_s_n (oIgnoreHTTPDP,"ignore-http-dp",
                N_("ignore HTTP CRL distribution points")),
  ARGPARSE_s_n (oIgnoreLDAPDP,"ignore-ldap-dp",
                N_("ignore LDAP CRL distribution points")),
  ARGPARSE_s_n (oIgnoreOCSPSvcUrl, "ignore-ocsp-service-url",
                N_("ignore certificate contained OCSP service URLs")),

  ARGPARSE_s_s (oHTTPProxy,  "http-proxy",
                N_("|URL|redirect all HTTP requests to URL")),
  ARGPARSE_s_s (oLDAPProxy,  "ldap-proxy",
                N_("|HOST|use HOST for LDAP queries")),
  ARGPARSE_s_n (oOnlyLDAPProxy, "only-ldap-proxy",
                N_("do not use fallback hosts with --ldap-proxy")),

  ARGPARSE_s_s (oLDAPFile, "ldapserverlist-file",
                N_("|FILE|read LDAP server list from FILE")),
  ARGPARSE_s_n (oLDAPAddServers, "add-servers",
                N_("add new servers discovered in CRL distribution"
                   " points to serverlist")),
  ARGPARSE_s_i (oLDAPTimeout, "ldaptimeout",
                N_("|N|set LDAP timeout to N seconds")),

  ARGPARSE_s_s (oOCSPResponder, "ocsp-responder",
                N_("|URL|use OCSP responder at URL")),
  ARGPARSE_s_s (oOCSPSigner, "ocsp-signer",
                N_("|FPR|OCSP response signed by FPR")),
  ARGPARSE_s_i (oOCSPMaxClockSkew, "ocsp-max-clock-skew", "@"),
  ARGPARSE_s_i (oOCSPMaxPeriod,    "ocsp-max-period", "@"),
  ARGPARSE_s_i (oOCSPCurrentPeriod, "ocsp-current-period", "@"),

  ARGPARSE_s_i (oMaxReplies, "max-replies",
                N_("|N|do not return more than N items in one query")),

  ARGPARSE_s_s (oNameServer, "nameserver", "@"),
  ARGPARSE_s_s (oKeyServer, "keyserver", "@"),
  ARGPARSE_s_s (oHkpCaCert, "hkp-cacert",
                N_("|FILE|use the CA certificates in FILE for HKP over TLS")),

  ARGPARSE_s_n (oUseTor, "use-tor", N_("route all network traffic via Tor")),
  ARGPARSE_s_n (oNoUseTor, "no-use-tor", "@"),

  ARGPARSE_s_n (oDisableIPv4, "disable-ipv4", "@"),
  ARGPARSE_s_n (oDisableIPv6, "disable-ipv6", "@"),

  ARGPARSE_s_u (oFakedSystemTime, "faked-system-time", "@"), /*(epoch time)*/
  ARGPARSE_s_s (oDebug,    "debug", "@"),
  ARGPARSE_s_n (oDebugAll, "debug-all", "@"),
  ARGPARSE_s_i (oGnutlsDebug, "gnutls-debug", "@"),
  ARGPARSE_s_i (oGnutlsDebug, "tls-debug", "@"),
  ARGPARSE_s_i (oDebugWait, "debug-wait", "@"),
  ARGPARSE_s_n (oNoGreeting, "no-greeting", "@"),
  ARGPARSE_s_s (oHomedir, "homedir", "@"),
  ARGPARSE_s_s (oLDAPWrapperProgram, "ldap-wrapper-program", "@"),
  ARGPARSE_s_s (oHTTPWrapperProgram, "http-wrapper-program", "@"),
  ARGPARSE_s_n (oHonorHTTPProxy, "honor-http-proxy", "@"),
  ARGPARSE_s_s (oIgnoreCertExtension,"ignore-cert-extension", "@"),
  ARGPARSE_s_n (oStandardResolver, "standard-resolver", "@"),
  ARGPARSE_s_n (oRecursiveResolver, "recursive-resolver", "@"),
  ARGPARSE_s_i (oResolverTimeout, "resolver-timeout", "@"),
  ARGPARSE_s_i (oConnectTimeout, "connect-timeout", "@"),
  ARGPARSE_s_i (oConnectQuickTimeout, "connect-quick-timeout", "@"),

  ARGPARSE_group (302,N_("@\n(See the \"info\" manual for a complete listing "
                         "of all commands and options)\n")),

  ARGPARSE_end ()
};

/* The list of supported debug flags.  */
static struct debug_flags_s debug_flags [] =
  {
    { DBG_X509_VALUE   , "x509"    },
    { DBG_CRYPTO_VALUE , "crypto"  },
    { DBG_MEMORY_VALUE , "memory"  },
    { DBG_CACHE_VALUE  , "cache"   },
    { DBG_MEMSTAT_VALUE, "memstat" },
    { DBG_HASHING_VALUE, "hashing" },
    { DBG_IPC_VALUE    , "ipc"     },
    { DBG_DNS_VALUE    , "dns"     },
    { DBG_NETWORK_VALUE, "network" },
    { DBG_LOOKUP_VALUE , "lookup"  },
    { DBG_EXTPROG_VALUE, "extprog" },
    { 77, NULL } /* 77 := Do not exit on "help" or "?".  */
  };

#define DEFAULT_MAX_REPLIES 10
#define DEFAULT_LDAP_TIMEOUT 100 /* arbitrary large timeout */

#define DEFAULT_CONNECT_TIMEOUT       (15*1000)  /* 15 seconds */
#define DEFAULT_CONNECT_QUICK_TIMEOUT ( 2*1000)  /*  2 seconds */

/* Keep track of the current log file so that we can avoid updating
   the log file after a SIGHUP if it didn't changed. Malloced. */
static char *current_logfile;

/* Helper to implement --debug-level. */
static const char *debug_level;

/* Helper to set the GNUTLS log level.  */
static int opt_gnutls_debug = -1;

/* Flag to control the Tor mode.  */
static enum
  { TOR_MODE_AUTO = 0,  /* Switch to NO or YES         */
    TOR_MODE_NEVER,     /* Never use Tor.              */
    TOR_MODE_NO,        /* Do not use Tor              */
    TOR_MODE_YES,       /* Use Tor                     */
    TOR_MODE_FORCE      /* Force using Tor             */
  } tor_mode;


/* Counter for the active connections.  */
static int active_connections;

/* This flag is set by any network access and used by the housekeeping
 * thread to run background network tasks.  */
static int network_activity_seen;

/* A list of filenames registred with --hkp-cacert.  */
static strlist_t hkp_cacert_filenames;


/* The timer tick used for housekeeping stuff.  */
#define TIMERTICK_INTERVAL         (60)

/* How oft to run the housekeeping.  */
#define HOUSEKEEPING_INTERVAL      (600)


/* This union is used to avoid compiler warnings in case a pointer is
   64 bit and an int 32 bit.  We store an integer in a pointer and get
   it back later (npth_getspecific et al.).  */
union int_and_ptr_u
{
  int  aint;
  assuan_fd_t afd;
  void *aptr;
};



/* The key used to store the current file descriptor in the thread
   local storage.  We use this in conjunction with the
   log_set_pid_suffix_cb feature.  */
#ifndef HAVE_W32_SYSTEM
static npth_key_t my_tlskey_current_fd;
#endif

/* Prototypes. */
static void cleanup (void);
#if USE_LDAP
static ldap_server_t parse_ldapserver_file (const char* filename);
#endif /*USE_LDAP*/
static fingerprint_list_t parse_ocsp_signer (const char *string);
static void netactivity_action (void);

static const char *
my_strusage( int level )
{
  const char *p = NULL;
  switch ( level )
    {
    case 11: p = "@DIRMNGR@ (@GNUPG@)";
      break;
    case 13: p = VERSION; break;
      /* TRANSLATORS: @EMAIL@ will get replaced by the actual bug
         reporting address.  This is so that we can change the
         reporting address without breaking the translations.  */
    case 19: p = _("Please report bugs to <@EMAIL@>.\n"); break;
    case 49: p = PACKAGE; break;
    case 1:
    case 40: p = _("Usage: @DIRMNGR@ [options] (-h for help)");
      break;
    case 41: p = _("Syntax: @DIRMNGR@ [options] [command [args]]\n"
                   "Keyserver, CRL, and OCSP access for @GNUPG@\n");
      break;

    default: p = NULL;
    }
  return p;
}


/* Callback from libksba to hash a provided buffer.  Our current
   implementation does only allow SHA-1 for hashing. This may be
   extended by mapping the name, testing for algorithm availibility
   and adjust the length checks accordingly. */
static gpg_error_t
my_ksba_hash_buffer (void *arg, const char *oid,
                     const void *buffer, size_t length, size_t resultsize,
                     unsigned char *result, size_t *resultlen)
{
  (void)arg;

  if (oid && strcmp (oid, "1.3.14.3.2.26"))
    return GPG_ERR_NOT_SUPPORTED;
  if (resultsize < 20)
    return GPG_ERR_BUFFER_TOO_SHORT;
  gcry_md_hash_buffer (2, result, buffer, length);
  *resultlen = 20;
  return 0;
}


/* GNUTLS log function callback.  */
static void
my_gnutls_log (int level, const char *text)
{
  int n;

  n = strlen (text);
  while (n && text[n-1] == '\n')
    n--;

  log_debug ("gnutls:L%d: %.*s\n", level, n, text);
}

/* Setup the debugging.  With a LEVEL of NULL only the active debug
   flags are propagated to the subsystems.  With LEVEL set, a specific
   set of debug flags is set; thus overriding all flags already
   set. */
static void
set_debug (void)
{
  int numok = (debug_level && digitp (debug_level));
  int numlvl = numok? atoi (debug_level) : 0;

  if (!debug_level)
    ;
  else if (!strcmp (debug_level, "none") || (numok && numlvl < 1))
    opt.debug = 0;
  else if (!strcmp (debug_level, "basic") || (numok && numlvl <= 2))
    opt.debug = DBG_IPC_VALUE;
  else if (!strcmp (debug_level, "advanced") || (numok && numlvl <= 5))
    opt.debug = (DBG_IPC_VALUE|DBG_X509_VALUE|DBG_LOOKUP_VALUE);
  else if (!strcmp (debug_level, "expert") || (numok && numlvl <= 8))
    opt.debug = (DBG_IPC_VALUE|DBG_X509_VALUE|DBG_LOOKUP_VALUE
                 |DBG_CACHE_VALUE|DBG_CRYPTO_VALUE);
  else if (!strcmp (debug_level, "guru") || numok)
    {
      opt.debug = ~0;
      /* Unless the "guru" string has been used we don't want to allow
         hashing debugging.  The rationale is that people tend to
         select the highest debug value and would then clutter their
         disk with debug files which may reveal confidential data.  */
      if (numok)
        opt.debug &= ~(DBG_HASHING_VALUE);
    }
  else
    {
      log_error (_("invalid debug-level '%s' given\n"), debug_level);
      log_info (_("valid debug levels are: %s\n"),
                "none, basic, advanced, expert, guru");
      opt.debug = 0; /* Reset debugging, so that prior debug
                        statements won't have an undesired effect. */
    }


  if (opt.debug && !opt.verbose)
    {
      opt.verbose = 1;
      gcry_control (GCRYCTL_SET_VERBOSITY, (int)opt.verbose);
    }
  if (opt.debug && opt.quiet)
    opt.quiet = 0;

  if (opt.debug & DBG_CRYPTO_VALUE )
    gcry_control (GCRYCTL_SET_DEBUG_FLAGS, 1);

  if (opt_gnutls_debug >= 0)
    {
      gnutls_global_set_log_function (my_gnutls_log);
      gnutls_global_set_log_level (opt_gnutls_debug);
    }

  if (opt.debug)
    parse_debug_flag (NULL, &opt.debug, debug_flags);
}


static void
set_tor_mode (void)
{
  if (dirmngr_use_tor ())
    {
      /* Enable Tor mode and when called again force a new curcuit
       * (e.g. on SIGHUP).  */
      enable_dns_tormode (1);
      if (assuan_sock_set_flag (ASSUAN_INVALID_FD, "tor-mode", 1))
        {
          log_error ("error enabling Tor mode: %s\n", strerror (errno));
          log_info ("(is your Libassuan recent enough?)\n");
        }
    }
  else
    disable_dns_tormode ();
}


/* Return true if Tor shall be used.  */
int
dirmngr_use_tor (void)
{
  if (tor_mode == TOR_MODE_AUTO)
    {
      /* FIXME: Figure out whether Tor is running.  */
    }

  if (tor_mode == TOR_MODE_FORCE)
    return 2; /* Use Tor (using 2 to indicate force mode) */
  else if (tor_mode == TOR_MODE_YES)
    return 1; /* Use Tor */
  else
    return 0; /* Do not use Tor.  */
}


static void
wrong_args (const char *text)
{
  es_fprintf (es_stderr, _("usage: %s [options] "), DIRMNGR_NAME);
  es_fputs (text, es_stderr);
  es_putc ('\n', es_stderr);
  dirmngr_exit (2);
}


/* Helper to stop the reaper thread for the ldap wrapper.  */
static void
shutdown_reaper (void)
{
#if USE_LDAP
  ldap_wrapper_wait_connections ();
#endif
}


/* Handle options which are allowed to be reset after program start.
   Return true if the current option in PARGS could be handled and
   false if not.  As a special feature, passing a value of NULL for
   PARGS, resets the options to the default.  REREAD should be set
   true if it is not the initial option parsing. */
static int
parse_rereadable_options (ARGPARSE_ARGS *pargs, int reread)
{
  if (!pargs)
    { /* Reset mode. */
      opt.quiet = 0;
      opt.verbose = 0;
      opt.debug = 0;
      opt.ldap_wrapper_program = NULL;
      opt.disable_http = 0;
      opt.disable_ldap = 0;
      opt.honor_http_proxy = 0;
      opt.http_proxy = NULL;
      opt.ldap_proxy = NULL;
      opt.only_ldap_proxy = 0;
      opt.ignore_http_dp = 0;
      opt.ignore_ldap_dp = 0;
      opt.ignore_ocsp_service_url = 0;
      opt.allow_ocsp = 0;
      opt.allow_version_check = 0;
      opt.ocsp_responder = NULL;
      opt.ocsp_max_clock_skew = 10 * 60;      /* 10 minutes.  */
      opt.ocsp_max_period = 90 * 86400;       /* 90 days.  */
      opt.ocsp_current_period = 3 * 60 * 60;  /* 3 hours. */
      opt.max_replies = DEFAULT_MAX_REPLIES;
      while (opt.ocsp_signer)
        {
          fingerprint_list_t tmp = opt.ocsp_signer->next;
          xfree (opt.ocsp_signer);
          opt.ocsp_signer = tmp;
        }
      FREE_STRLIST (opt.ignored_cert_extensions);
      http_register_tls_ca (NULL);
      FREE_STRLIST (hkp_cacert_filenames);
      FREE_STRLIST (opt.keyserver);
      /* Note: We do not allow resetting of TOR_MODE_FORCE at runtime.  */
      if (tor_mode != TOR_MODE_FORCE)
        tor_mode = TOR_MODE_AUTO;
      enable_standard_resolver (0);
      set_dns_timeout (0);
      opt.connect_timeout = 0;
      opt.connect_quick_timeout = 0;
      return 1;
    }

  switch (pargs->r_opt)
    {
    case oQuiet:   opt.quiet = 1; break;
    case oVerbose: opt.verbose++; break;
    case oDebug:
      parse_debug_flag (pargs->r.ret_str, &opt.debug, debug_flags);
      break;
    case oDebugAll: opt.debug = ~0; break;
    case oDebugLevel: debug_level = pargs->r.ret_str; break;
    case oGnutlsDebug: opt_gnutls_debug = pargs->r.ret_int; break;

    case oLogFile:
      if (!reread)
        return 0; /* Not handled. */
      if (!current_logfile || !pargs->r.ret_str
          || strcmp (current_logfile, pargs->r.ret_str))
        {
          log_set_file (pargs->r.ret_str);
          xfree (current_logfile);
          current_logfile = xtrystrdup (pargs->r.ret_str);
        }
      break;

    case oLDAPWrapperProgram:
      opt.ldap_wrapper_program = pargs->r.ret_str;
      break;
    case oHTTPWrapperProgram:
      opt.http_wrapper_program = pargs->r.ret_str;
      break;

    case oDisableHTTP: opt.disable_http = 1; break;
    case oDisableLDAP: opt.disable_ldap = 1; break;
    case oDisableIPv4: opt.disable_ipv4 = 1; break;
    case oDisableIPv6: opt.disable_ipv6 = 1; break;
    case oHonorHTTPProxy: opt.honor_http_proxy = 1; break;
    case oHTTPProxy: opt.http_proxy = pargs->r.ret_str; break;
    case oLDAPProxy: opt.ldap_proxy = pargs->r.ret_str; break;
    case oOnlyLDAPProxy: opt.only_ldap_proxy = 1; break;
    case oIgnoreHTTPDP: opt.ignore_http_dp = 1; break;
    case oIgnoreLDAPDP: opt.ignore_ldap_dp = 1; break;
    case oIgnoreOCSPSvcUrl: opt.ignore_ocsp_service_url = 1; break;

    case oAllowOCSP: opt.allow_ocsp = 1; break;
    case oAllowVersionCheck: opt.allow_version_check = 1; break;
    case oOCSPResponder: opt.ocsp_responder = pargs->r.ret_str; break;
    case oOCSPSigner:
      opt.ocsp_signer = parse_ocsp_signer (pargs->r.ret_str);
      break;
    case oOCSPMaxClockSkew: opt.ocsp_max_clock_skew = pargs->r.ret_int; break;
    case oOCSPMaxPeriod: opt.ocsp_max_period = pargs->r.ret_int; break;
    case oOCSPCurrentPeriod: opt.ocsp_current_period = pargs->r.ret_int; break;

    case oMaxReplies: opt.max_replies = pargs->r.ret_int; break;

    case oHkpCaCert:
      {
        /* We need to register the filenames with gnutls (http.c) and
         * also for our own cert cache.  */
        char *tmpname;

        /* Do tilde expansion and make path absolute.  */
        tmpname = make_absfilename (pargs->r.ret_str, NULL);
        http_register_tls_ca (tmpname);
        add_to_strlist (&hkp_cacert_filenames, pargs->r.ret_str);
        xfree (tmpname);
      }
      break;

    case oIgnoreCertExtension:
      add_to_strlist (&opt.ignored_cert_extensions, pargs->r.ret_str);
      break;

    case oUseTor:
      tor_mode = TOR_MODE_FORCE;
      break;
    case oNoUseTor:
      if (tor_mode != TOR_MODE_FORCE)
        tor_mode = TOR_MODE_NEVER;
      break;

    case oStandardResolver: enable_standard_resolver (1); break;
    case oRecursiveResolver: enable_recursive_resolver (1); break;

    case oKeyServer:
      if (*pargs->r.ret_str)
        add_to_strlist (&opt.keyserver, pargs->r.ret_str);
      break;

    case oNameServer:
      set_dns_nameserver (pargs->r.ret_str);
      break;

    case oResolverTimeout:
      set_dns_timeout (pargs->r.ret_int);
      break;

    case oConnectTimeout:
      opt.connect_timeout = pargs->r.ret_ulong * 1000;
      break;

    case oConnectQuickTimeout:
      opt.connect_quick_timeout = pargs->r.ret_ulong * 1000;
      break;

    default:
      return 0; /* Not handled. */
    }

  set_dns_verbose (opt.verbose, !!DBG_DNS);
  http_set_verbose (opt.verbose, !!DBG_NETWORK);
  set_dns_disable_ipv4 (opt.disable_ipv4);
  set_dns_disable_ipv6 (opt.disable_ipv6);

  return 1; /* Handled. */
}


/* This fucntion is called after option parsing to adjust some values
 * and call option setup functions.  */
static void
post_option_parsing (void)
{
  /* It would be too surpirsing if the quick timeout is larger than
   * the standard value.  */
  if (opt.connect_quick_timeout > opt.connect_timeout)
    opt.connect_quick_timeout = opt.connect_timeout;

  set_debug ();
  set_tor_mode ();
}


#ifndef HAVE_W32_SYSTEM
static int
pid_suffix_callback (unsigned long *r_suffix)
{
  union int_and_ptr_u value;

  memset (&value, 0, sizeof value);
  value.aptr = npth_getspecific (my_tlskey_current_fd);
  *r_suffix = value.aint;
  return (*r_suffix != -1);  /* Use decimal representation.  */
}
#endif /*!HAVE_W32_SYSTEM*/


static void
thread_init (void)
{
  npth_init ();
  gpgrt_set_syscall_clamp (npth_unprotect, npth_protect);

  /* Now with NPth running we can set the logging callback.  Our
     windows implementation does not yet feature the NPth TLS
     functions.  */
#ifndef HAVE_W32_SYSTEM
  if (npth_key_create (&my_tlskey_current_fd, NULL) == 0)
    if (npth_setspecific (my_tlskey_current_fd, NULL) == 0)
      log_set_pid_suffix_cb (pid_suffix_callback);
#endif /*!HAVE_W32_SYSTEM*/
}


int
dirmngr_main (int argc, char **argv)
{
  enum cmd_and_opt_values cmd = 0;
  ARGPARSE_ARGS pargs;
  int orig_argc;
  char **orig_argv;
  FILE *configfp = NULL;
  char *configname = NULL;
  const char *shell;
  unsigned configlineno;
  int parse_debug = 0;
  int default_config =1;
  int greeting = 0;
  int nogreeting = 0;
  int nodetach = 0;
  int csh_style = 0;
  char *logfile = NULL;
#if USE_LDAP
  char *ldapfile = NULL;
#endif /*USE_LDAP*/
  int debug_wait = 0;
  int rc;
  struct assuan_malloc_hooks malloc_hooks;

  early_system_init ();
  set_strusage (my_strusage);
  log_set_prefix (DIRMNGR_NAME, GPGRT_LOG_WITH_PREFIX | GPGRT_LOG_WITH_PID);

  /* Make sure that our subsystems are ready.  */
  i18n_init ();
  init_common_subsystems (&argc, &argv);

  gcry_control (GCRYCTL_DISABLE_SECMEM, 0);

  ksba_set_malloc_hooks (gcry_malloc, gcry_realloc, gcry_free );
  ksba_set_hash_buffer_function (my_ksba_hash_buffer, NULL);

  /* Init TLS library.  */
  rc = gnutls_global_init ();
  if (rc)
    log_fatal ("gnutls_global_init failed: %s\n", gnutls_strerror (rc));

  /* Init Assuan. */
  malloc_hooks.malloc = gcry_malloc;
  malloc_hooks.realloc = gcry_realloc;
  malloc_hooks.free = gcry_free;
  assuan_set_malloc_hooks (&malloc_hooks);
  assuan_set_assuan_log_prefix (log_get_prefix (NULL));
  assuan_set_system_hooks (ASSUAN_SYSTEM_NPTH);
  assuan_sock_init ();
  setup_libassuan_logging (&opt.debug, dirmngr_assuan_log_monitor);

  setup_libgcrypt_logging ();

  /* Setup defaults. */
  shell = getenv ("SHELL");
  if (shell && strlen (shell) >= 3 && !strcmp (shell+strlen (shell)-3, "csh") )
    csh_style = 1;

  /* Reset rereadable options to default values. */
  parse_rereadable_options (NULL, 0);

  /* Default TCP timeouts.  */
  opt.connect_timeout = DEFAULT_CONNECT_TIMEOUT;
  opt.connect_quick_timeout = DEFAULT_CONNECT_QUICK_TIMEOUT;

  /* LDAP defaults.  */
  opt.add_new_ldapservers = 0;
  opt.ldaptimeout = DEFAULT_LDAP_TIMEOUT;

  /* Other defaults.  */

  /* Check whether we have a config file given on the commandline */
  orig_argc = argc;
  orig_argv = argv;
  pargs.argc = &argc;
  pargs.argv = &argv;
  pargs.flags= 1|(1<<6);  /* do not remove the args, ignore version */
  while (arg_parse( &pargs, opts))
    {
      if (pargs.r_opt == oDebug || pargs.r_opt == oDebugAll)
        parse_debug++;
      else if (pargs.r_opt == oOptions)
        { /* Yes there is one, so we do not try the default one, but
	     read the option file when it is encountered at the
	     commandline */
          default_config = 0;
	}
      else if (pargs.r_opt == oNoOptions)
        default_config = 0; /* --no-options */
      else if (pargs.r_opt == oHomedir)
        {
          gnupg_set_homedir (pargs.r.ret_str);
        }
    }

  if (default_config)
    configname = make_filename (gnupg_homedir (), DIRMNGR_NAME".conf", NULL );

  argc = orig_argc;
  argv = orig_argv;
  pargs.argc = &argc;
  pargs.argv = &argv;
  pargs.flags= 1;  /* do not remove the args */
 next_pass:
  if (configname)
    {
      configlineno = 0;
      configfp = fopen (configname, "r");
      if (!configfp)
        {
          if (default_config)
            {
              if( parse_debug )
                log_info (_("Note: no default option file '%s'\n"),
                          configname );
	    }
          else
            {
              log_error (_("option file '%s': %s\n"),
                         configname, strerror(errno) );
              exit(2);
	    }
          xfree (configname);
          configname = NULL;
	}
      if (parse_debug && configname )
        log_info (_("reading options from '%s'\n"), configname );
      default_config = 0;
    }

  while (optfile_parse( configfp, configname, &configlineno, &pargs, opts) )
    {
      if (parse_rereadable_options (&pargs, 0))
        continue; /* Already handled */
      switch (pargs.r_opt)
        {
        case aServer:
        case aShutdown:
        case aFlush:
	case aListCRLs:
	case aLoadCRL:
        case aFetchCRL:
	case aGPGConfList:
	case aGPGConfTest:
          cmd = pargs.r_opt;
          break;

        case oQuiet: opt.quiet = 1; break;
        case oVerbose: opt.verbose++; break;
        case oBatch: opt.batch=1; break;

        case oDebugWait: debug_wait = pargs.r.ret_int; break;

        case oOptions:
          /* Config files may not be nested (silently ignore them) */
          if (!configfp)
            {
		xfree(configname);
		configname = xstrdup(pargs.r.ret_str);
		goto next_pass;
	    }
          break;
        case oNoGreeting: nogreeting = 1; break;
        case oNoVerbose: opt.verbose = 0; break;
        case oNoOptions: break; /* no-options */
        case oHomedir: /* Ignore this option here. */; break;
        case oNoDetach: nodetach = 1; break;
        case oLogFile: logfile = pargs.r.ret_str; break;
        case oCsh: csh_style = 1; break;
        case oSh: csh_style = 0; break;
	case oLDAPFile:
#        if USE_LDAP
          ldapfile = pargs.r.ret_str;
#        endif /*USE_LDAP*/
          break;
	case oLDAPAddServers: opt.add_new_ldapservers = 1; break;
	case oLDAPTimeout:
	  opt.ldaptimeout = pargs.r.ret_int;
	  break;

        case oFakedSystemTime:
          gnupg_set_time ((time_t)pargs.r.ret_ulong, 0);
          break;

        case oForce: opt.force = 1; break;

        default : pargs.err = configfp? 1:2; break;
	}
    }
  if (configfp)
    {
      fclose (configfp);
      configfp = NULL;
      /* Keep a copy of the name so that it can be read on SIGHUP. */
      opt.config_filename = configname;
      configname = NULL;
      goto next_pass;
    }
  xfree (configname);
  configname = NULL;
  if (log_get_errorcount(0))
    exit(2);
  if (nogreeting )
    greeting = 0;

  if (!opt.homedir_cache)
    opt.homedir_cache = xstrdup (gnupg_homedir ());

  if (greeting)
    {
      es_fprintf (es_stderr, "%s %s; %s\n",
                  strusage(11), strusage(13), strusage(14) );
      es_fprintf (es_stderr, "%s\n", strusage(15) );
    }

  /* Print a warning if an argument looks like an option.  */
  if (!opt.quiet && !(pargs.flags & ARGPARSE_FLAG_STOP_SEEN))
    {
      int i;

      for (i=0; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] == '-')
          log_info (_("Note: '%s' is not considered an option\n"), argv[i]);
    }

  if (!access ("/etc/"DIRMNGR_NAME, F_OK)
      && !strncmp (gnupg_homedir (), "/etc/", 5))
    log_info
      ("NOTE: DirMngr is now a proper part of %s.  The configuration and"
       " other directory names changed.  Please check that no other version"
       " of dirmngr is still installed.  To disable this warning, remove the"
       " directory '/etc/dirmngr'.\n", GNUPG_NAME);

  if (gnupg_faked_time_p ())
    {
      gnupg_isotime_t tbuf;

      log_info (_("WARNING: running with faked system time: "));
      gnupg_get_isotime (tbuf);
      dump_isotime (tbuf);
      log_printf ("\n");
    }

  post_option_parsing ();

  /* Get LDAP server list from file. */
#if USE_LDAP
  if (!ldapfile)
    {
      ldapfile = make_filename (gnupg_homedir (),
                                "dirmngr_ldapservers.conf",
                                NULL);
      opt.ldapservers = parse_ldapserver_file (ldapfile);
      xfree (ldapfile);
    }
  else
      opt.ldapservers = parse_ldapserver_file (ldapfile);
#endif /*USE_LDAP*/

#ifndef HAVE_W32_SYSTEM
  /* We need to ignore the PIPE signal because the we might log to a
     socket and that code handles EPIPE properly.  The ldap wrapper
     also requires us to ignore this silly signal. Assuan would set
     this signal to ignore anyway.*/
  signal (SIGPIPE, SIG_IGN);
#endif

  /* Ready.  Now to our duties. */
  if (!cmd)
    cmd = aServer;
  rc = 0;

  if (cmd == aServer)
    {
      /* Note that this server mode is mainly useful for debugging.  */
      if (argc)
        wrong_args ("--server");

      if (logfile)
        {
          log_set_file (logfile);
          log_set_prefix (NULL, GPGRT_LOG_WITH_TIME | GPGRT_LOG_WITH_PID);
        }

      if (debug_wait)
        {
          log_debug ("waiting for debugger - my pid is %u .....\n",
                     (unsigned int)getpid());
          gnupg_sleep (debug_wait);
          log_debug ("... okay\n");
        }


      thread_init ();
      cert_cache_init (hkp_cacert_filenames);
      crl_cache_init ();
      http_register_netactivity_cb (netactivity_action);
      start_command_handler ();
      shutdown_reaper ();
    }
  else if (cmd == aListCRLs)
    {
      /* Just list the CRL cache and exit. */
      if (argc)
        wrong_args ("--list-crls");
      crl_cache_init ();
      crl_cache_list (es_stdout);
    }
  else if (cmd == aLoadCRL)
    {
      struct server_control_s ctrlbuf;

      memset (&ctrlbuf, 0, sizeof ctrlbuf);
      dirmngr_init_default_ctrl (&ctrlbuf);

      thread_init ();
      cert_cache_init (hkp_cacert_filenames);
      crl_cache_init ();
      if (!argc)
        rc = crl_cache_load (&ctrlbuf, NULL);
      else
        {
          for (; !rc && argc; argc--, argv++)
            rc = crl_cache_load (&ctrlbuf, *argv);
        }
      dirmngr_deinit_default_ctrl (&ctrlbuf);
    }
  else if (cmd == aFetchCRL)
    {
      ksba_reader_t reader;
      struct server_control_s ctrlbuf;

      if (argc != 1)
        wrong_args ("--fetch-crl URL");

      memset (&ctrlbuf, 0, sizeof ctrlbuf);
      dirmngr_init_default_ctrl (&ctrlbuf);

      thread_init ();
      cert_cache_init (hkp_cacert_filenames);
      crl_cache_init ();
      rc = crl_fetch (&ctrlbuf, argv[0], &reader);
      if (rc)
        log_error (_("fetching CRL from '%s' failed: %s\n"),
                     argv[0], gpg_strerror (rc));
      else
        {
          rc = crl_cache_insert (&ctrlbuf, argv[0], reader);
          if (rc)
            log_error (_("processing CRL from '%s' failed: %s\n"),
                       argv[0], gpg_strerror (rc));
          crl_close_reader (reader);
        }
      dirmngr_deinit_default_ctrl (&ctrlbuf);
    }
  else if (cmd == aFlush)
    {
      /* Delete cache and exit. */
      if (argc)
        wrong_args ("--flush");
      rc = crl_cache_flush();
    }
  else if (cmd == aGPGConfTest)
    dirmngr_exit (0);
  else if (cmd == aGPGConfList)
    {
      unsigned long flags = 0;
      char *filename;
      char *filename_esc;

      /* First the configuration file.  This is not an option, but it
	 is vital information for GPG Conf.  */
      if (!opt.config_filename)
        opt.config_filename = make_filename (gnupg_homedir (),
                                             "dirmngr.conf", NULL );

      filename = percent_escape (opt.config_filename, NULL);
      es_printf ("gpgconf-dirmngr.conf:%lu:\"%s\n",
              GC_OPT_FLAG_DEFAULT, filename);
      xfree (filename);

      es_printf ("verbose:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("quiet:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("debug-level:%lu:\"none\n", flags | GC_OPT_FLAG_DEFAULT);
      es_printf ("log-file:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("force:%lu:\n", flags | GC_OPT_FLAG_NONE);

      /* --csh and --sh are mutually exclusive, something we can not
         express in GPG Conf.  --options is only usable from the
         command line, really.  --debug-all interacts with --debug,
         and having both of them is thus problematic.  --no-detach is
         also only usable on the command line.  --batch is unused.  */

      filename = make_filename (gnupg_homedir (),
                                "dirmngr_ldapservers.conf",
                                NULL);
      filename_esc = percent_escape (filename, NULL);
      es_printf ("ldapserverlist-file:%lu:\"%s\n", flags | GC_OPT_FLAG_DEFAULT,
	      filename_esc);
      xfree (filename_esc);
      xfree (filename);

      es_printf ("ldaptimeout:%lu:%u\n",
              flags | GC_OPT_FLAG_DEFAULT, DEFAULT_LDAP_TIMEOUT);
      es_printf ("max-replies:%lu:%u\n",
              flags | GC_OPT_FLAG_DEFAULT, DEFAULT_MAX_REPLIES);
      es_printf ("allow-ocsp:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("allow-version-check:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("ocsp-responder:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("ocsp-signer:%lu:\n", flags | GC_OPT_FLAG_NONE);

      es_printf ("faked-system-time:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("no-greeting:%lu:\n", flags | GC_OPT_FLAG_NONE);

      es_printf ("disable-http:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("disable-ldap:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("honor-http-proxy:%lu\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("http-proxy:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("ldap-proxy:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("only-ldap-proxy:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("ignore-ldap-dp:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("ignore-http-dp:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("ignore-ocsp-service-url:%lu:\n", flags | GC_OPT_FLAG_NONE);
      /* Note: The next one is to fix a typo in gpgconf - should be
         removed eventually. */
      es_printf ("ignore-ocsp-servic-url:%lu:\n", flags | GC_OPT_FLAG_NONE);

      es_printf ("use-tor:%lu:\n", flags | GC_OPT_FLAG_NONE);

      filename_esc = percent_escape (get_default_keyserver (0), NULL);
      es_printf ("keyserver:%lu:\"%s:\n", flags | GC_OPT_FLAG_DEFAULT,
                 filename_esc);
      xfree (filename_esc);


      es_printf ("nameserver:%lu:\n", flags | GC_OPT_FLAG_NONE);
      es_printf ("resolver-timeout:%lu:%u\n",
                 flags | GC_OPT_FLAG_DEFAULT, 0);
    }
  cleanup ();
  return !!rc;
}


static void
cleanup (void)
{
  crl_cache_deinit ();
  cert_cache_deinit (1);
  reload_dns_stuff (1);

#if USE_LDAP
  ldapserver_list_free (opt.ldapservers);
#endif /*USE_LDAP*/
  opt.ldapservers = NULL;

}


void
dirmngr_exit (int rc)
{
  cleanup ();
  exit (rc);
}


void
dirmngr_init_default_ctrl (ctrl_t ctrl)
{
  ctrl->magic = SERVER_CONTROL_MAGIC;
  if (opt.http_proxy)
    ctrl->http_proxy = xstrdup (opt.http_proxy);
  ctrl->http_no_crl = 1;
  ctrl->timeout = opt.connect_timeout;
}


void
dirmngr_deinit_default_ctrl (ctrl_t ctrl)
{
  if (!ctrl)
    return;
  ctrl->magic = 0xdeadbeef;

  xfree (ctrl->http_proxy);
  ctrl->http_proxy = NULL;
}


/* Create a list of LDAP servers from the file FILENAME. Returns the
   list or NULL in case of errors.

   The format fo such a file is line oriented where empty lines and
   lines starting with a hash mark are ignored.  All other lines are
   assumed to be colon seprated with these fields:

   1. field: Hostname
   2. field: Portnumber
   3. field: Username
   4. field: Password
   5. field: Base DN

*/
#if USE_LDAP
static ldap_server_t
parse_ldapserver_file (const char* filename)
{
  char buffer[1024];
  char *p;
  ldap_server_t server, serverstart, *serverend;
  int c;
  unsigned int lineno = 0;
  estream_t fp;

  fp = es_fopen (filename, "r");
  if (!fp)
    {
      log_error (_("error opening '%s': %s\n"), filename, strerror (errno));
      return NULL;
    }

  serverstart = NULL;
  serverend = &serverstart;
  while (es_fgets (buffer, sizeof buffer, fp))
    {
      lineno++;
      if (!*buffer || buffer[strlen(buffer)-1] != '\n')
        {
          if (*buffer && es_feof (fp))
            ; /* Last line not terminated - continue. */
          else
            {
              log_error (_("%s:%u: line too long - skipped\n"),
                         filename, lineno);
              while ( (c=es_fgetc (fp)) != EOF && c != '\n')
                ; /* Skip until end of line. */
              continue;
            }
        }
      /* Skip empty and comment lines.*/
      for (p=buffer; spacep (p); p++)
        ;
      if (!*p || *p == '\n' || *p == '#')
        continue;

      /* Parse the colon separated fields. */
      server = ldapserver_parse_one (buffer, filename, lineno);
      if (server)
        {
          *serverend = server;
          serverend = &server->next;
        }
    }

  if (es_ferror (fp))
    log_error (_("error reading '%s': %s\n"), filename, strerror (errno));
  es_fclose (fp);

  return serverstart;
}
#endif /*USE_LDAP*/

static fingerprint_list_t
parse_ocsp_signer (const char *string)
{
  gpg_error_t err;
  char *fname;
  estream_t fp;
  char line[256];
  char *p;
  fingerprint_list_t list, *list_tail, item;
  unsigned int lnr = 0;
  int c, i, j;
  int errflag = 0;


  /* Check whether this is not a filename and treat it as a direct
     fingerprint specification.  */
  if (!strpbrk (string, "/.~\\"))
    {
      item = xcalloc (1, sizeof *item);
      for (i=j=0; (string[i] == ':' || hexdigitp (string+i)) && j < 40; i++)
        if ( string[i] != ':' )
          item->hexfpr[j++] = string[i] >= 'a'? (string[i] & 0xdf): string[i];
      item->hexfpr[j] = 0;
      if (j != 40 || !(spacep (string+i) || !string[i]))
        {
          log_error (_("%s:%u: invalid fingerprint detected\n"),
                     "--ocsp-signer", 0);
          xfree (item);
          return NULL;
        }
      return item;
    }

  /* Well, it is a filename.  */
  if (*string == '/' || (*string == '~' && string[1] == '/'))
    fname = make_filename (string, NULL);
  else
    {
      if (string[0] == '.' && string[1] == '/' )
        string += 2;
      fname = make_filename (gnupg_homedir (), string, NULL);
    }

  fp = es_fopen (fname, "r");
  if (!fp)
    {
      err = gpg_error_from_syserror ();
      log_error (_("can't open '%s': %s\n"), fname, gpg_strerror (err));
      xfree (fname);
      return NULL;
    }

  list = NULL;
  list_tail = &list;
  for (;;)
    {
      if (!es_fgets (line, DIM(line)-1, fp) )
        {
          if (!es_feof (fp))
            {
              err = gpg_error_from_syserror ();
              log_error (_("%s:%u: read error: %s\n"),
                         fname, lnr, gpg_strerror (err));
              errflag = 1;
            }
          es_fclose (fp);
          if (errflag)
            {
              while (list)
                {
                  fingerprint_list_t tmp = list->next;
                  xfree (list);
                  list = tmp;
                }
            }
          xfree (fname);
          return list; /* Ready.  */
        }

      lnr++;
      if (!*line || line[strlen(line)-1] != '\n')
        {
          /* Eat until end of line. */
          while ( (c=es_getc (fp)) != EOF && c != '\n')
            ;
          err = *line? GPG_ERR_LINE_TOO_LONG
                           /* */: GPG_ERR_INCOMPLETE_LINE;
          log_error (_("%s:%u: read error: %s\n"),
                     fname, lnr, gpg_strerror (err));
          errflag = 1;
          continue;
        }

      /* Allow for empty lines and spaces */
      for (p=line; spacep (p); p++)
        ;
      if (!*p || *p == '\n' || *p == '#')
        continue;

      item = xcalloc (1, sizeof *item);
      *list_tail = item;
      list_tail = &item->next;

      for (i=j=0; (p[i] == ':' || hexdigitp (p+i)) && j < 40; i++)
        if ( p[i] != ':' )
          item->hexfpr[j++] = p[i] >= 'a'? (p[i] & 0xdf): p[i];
      item->hexfpr[j] = 0;
      if (j != 40 || !(spacep (p+i) || p[i] == '\n'))
        {
          log_error (_("%s:%u: invalid fingerprint detected\n"), fname, lnr);
          errflag = 1;
        }
      i++;
      while (spacep (p+i))
        i++;
      if (p[i] && p[i] != '\n')
        log_info (_("%s:%u: garbage at end of line ignored\n"), fname, lnr);
    }
  /*NOTREACHED*/
}




/*
   Stuff used in daemon mode.
 */



/* A global function which allows us to trigger the reload stuff from
   other places.  */
void
dirmngr_sighup_action (void)
{
  log_info (_("SIGHUP received - "
              "re-reading configuration and flushing caches\n"));
  cert_cache_deinit (0);
  crl_cache_deinit ();
  cert_cache_init (hkp_cacert_filenames);
  crl_cache_init ();
  reload_dns_stuff (0);
  ks_hkp_reload ();
}


/* This function is called if some network activity was done.  At this
 * point we know the we have a network and we can decide whether to
 * run scheduled background tasks soon.  The function should return
 * quickly and only trigger actions for another thread. */
static void
netactivity_action (void)
{
  network_activity_seen = 1;
}


#ifdef HAVE_INOTIFY_INIT
/* Read an inotify event and return true if it matches NAME.  */
static int
my_inotify_is_name (int fd, const char *name)
{
  union {
    struct inotify_event ev;
    char _buf[sizeof (struct inotify_event) + 100 + 1];
  } buf;
  int n;
  const char *s;

  s = strrchr (name, '/');
  if (s && s[1])
    name = s + 1;

  n = npth_read (fd, &buf, sizeof buf);
  if (n < sizeof (struct inotify_event))
    return 0;
  if (buf.ev.len < strlen (name)+1)
    return 0;
  if (strcmp (buf.ev.name, name))
    return 0; /* Not the desired file.  */

  return 1; /* Found.  */
}
#endif /*HAVE_INOTIFY_INIT*/


/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* SRS - Sender rewriting scheme support
  (C)2004 Miles Wilton <miles@mirtol.com>
  Copyright (c) The Exim Maintainers 2016 - 2021

  SRS Support Version: 1.0a

  License: GPL */

#include "exim.h"
#ifdef EXPERIMENTAL_SRS_ALT

#include <srs_alt.h>
#include "srs.h"

srs_t    *srs                   = NULL;
uschar   *srs_db_forward        = NULL;
uschar   *srs_db_reverse        = NULL;


/* srs_init just initialises libsrs and creates (if necessary)
   an srs object to use for all srs calls in this instance */

int
eximsrs_init()
{
const uschar *list = srs_config;
uschar secret_buf[SRS_MAX_SECRET_LENGTH];
uschar *secret = NULL;
uschar sbuf[4];
uschar *sbufp;

/* Check if this instance of Exim has not initialized SRS */
if (srs == NULL)
  {
  int co = 0;
  int hashlen, maxage;
  BOOL usetimestamp, usehash;

  /* Copy config vars */
  hashlen = srs_hashlength;
  maxage = srs_maxage;
  usetimestamp = srs_usetimestamp;
  usehash = srs_usehash;

  /* Pass srs_config var (overrides new config vars) */
  co = 0;
  if (srs_config != NULL)
    {
    /* looks like list not expanded, so cannot be tainted */
    secret = string_nextinlist(&list, &co, secret_buf, SRS_MAX_SECRET_LENGTH);

    if ((sbufp = string_nextinlist(&list, &co, sbuf, sizeof(sbuf))))
      maxage = atoi(sbuf);

    if ((sbufp = string_nextinlist(&list, &co, sbuf, sizeof(sbuf))))
      hashlen = atoi(sbuf);

    if ((sbufp = string_nextinlist(&list, &co, sbuf, sizeof(sbuf))))
      usetimestamp = atoi(sbuf);

    if ((sbufp = string_nextinlist(&list, &co, sbuf, sizeof(sbuf))))
      usehash = atoi(sbuf);
    }

  if (srs_hashmin == -1)
    srs_hashmin = hashlen;

  /* First secret specified in secrets? */
  co = 0;
  list = srs_secrets;
  if (secret == NULL || *secret == '\0')
    /* looks like list not expanded so cannot be tainted */
    if (!(secret = string_nextinlist(&list, &co, secret_buf, SRS_MAX_SECRET_LENGTH)))
      {
      log_write(0, LOG_MAIN | LOG_PANIC,
	  "SRS Configuration Error: No secret specified");
      return DEFER;
      }

  /* Check config */
  if (maxage < 0 || maxage > 365)
    {
    log_write(0, LOG_MAIN | LOG_PANIC,
	"SRS Configuration Error: Invalid maximum timestamp age");
    return DEFER;
    }
  if (hashlen < 1 || hashlen > 20 || srs_hashmin < 1 || srs_hashmin > 20)
    {
    log_write(0, LOG_MAIN | LOG_PANIC,
	"SRS Configuration Error: Invalid hash length");
    return DEFER;
    }

  if (!(srs = srs_open(secret, Ustrlen(secret), maxage, hashlen, srs_hashmin)))
    {
    log_write(0, LOG_MAIN | LOG_PANIC,
	"Failed to allocate SRS memory");
    return DEFER;
    }

  srs_set_option(srs, SRS_OPTION_USETIMESTAMP, usetimestamp);
  srs_set_option(srs, SRS_OPTION_USEHASH, usehash);

  /* Extra secrets? */
  /* looks like list not expanded so cannot be tainted */
  while((secret = string_nextinlist(&list, &co, secret_buf, SRS_MAX_SECRET_LENGTH)))
      srs_add_secret(srs, secret,
       	Ustrlen(secret) > SRS_MAX_SECRET_LENGTH ? SRS_MAX_SECRET_LENGTH :  Ustrlen(secret));

  DEBUG(D_any)
    debug_printf("SRS initialized\n");
  }

return OK;
}


int
eximsrs_done()
{
if (srs) srs_close(srs);
srs = NULL;
return OK;
}


int
eximsrs_forward(uschar **result, uschar *orig_sender, uschar *domain)
{
char res[512];
int n;

if ((n = srs_forward(srs, orig_sender, domain, res, sizeof(res))) & SRS_RESULT_FAIL)
  {
  DEBUG(D_any)
    debug_printf("srs_forward failed (%s, %s): %s\n", orig_sender, domain, srs_geterrormsg(n));
  return DEFER;
  }

*result = string_copy(res);
return OK;
}


int
eximsrs_reverse(uschar **result, uschar *address)
{
char res[512];
int n;

if ((n = srs_reverse(srs, address, res, sizeof(res))) & SRS_RESULT_FAIL)
  {
  DEBUG(D_any)
    debug_printf("srs_reverse failed (%s): %s\n", address, srs_geterrormsg(n));
  if (n == SRS_RESULT_NOTSRS || n == SRS_RESULT_BADSRS)
    return DECLINE;
  if (n == SRS_RESULT_BADHASH || n == SRS_RESULT_BADTIMESTAMP || n == SRS_RESULT_TIMESTAMPEXPIRED)
    return FAIL;
  return DEFER;
  }

*result = string_copy(res);
return OK;
}


int
eximsrs_db_set(BOOL reverse, uschar *srs_db)
{
if (reverse)
  srs_db_reverse = (srs_db == NULL ? NULL : string_copy(srs_db));
else
  srs_db_forward = (srs_db == NULL ? NULL : string_copy(srs_db));

if (srs_set_db_functions(srs, (srs_db_forward ? eximsrs_db_insert : NULL),
			     (srs_db_reverse ? eximsrs_db_lookup : NULL)) & SRS_RESULT_FAIL)
  return DEFER;

return OK;
}


srs_result
eximsrs_db_insert(srs_t *srs, char *data, uint data_len, char *result, uint result_len)
{
uschar *res;
uschar buf[64];

if (srs_db_forward == NULL)
  return SRS_RESULT_DBERROR;

srs_db_address = string_copyn(data, data_len);
if (srs_generate_unique_id(srs, srs_db_address, buf, 64) & SRS_RESULT_FAIL)
  return SRS_RESULT_DBERROR;

srs_db_key = string_copyn(buf, 16);

if ((res = expand_string(srs_db_forward)) == NULL)
  return SRS_RESULT_DBERROR;

if (result_len < 17)
  return SRS_RESULT_DBERROR;

Ustrncpy(result, srs_db_key, result_len);

return SRS_RESULT_OK;
}


srs_result
eximsrs_db_lookup(srs_t *srs, char *data, uint data_len, char *result, uint result_len)
{
uschar *res;

if (srs_db_reverse == NULL)
  return SRS_RESULT_DBERROR;

srs_db_key = string_copyn(data, data_len);
if ((res = expand_string(srs_db_reverse)) == NULL)
  return SRS_RESULT_DBERROR;

if (Ustrlen(res) >= result_len)
  return SRS_RESULT_ADDRESSTOOLONG;

strncpy(result, res, result_len);

return SRS_RESULT_OK;
}


#endif


/* gpgme-json.c - JSON based interface to gpgme (server)
 * Copyright (C) 2018 g10 Code GmbH
 *
 * This file is part of GPGME.
 *
 * GPGME is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * GPGME is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, see <http://www.gnu.org/licenses/>.
 * SPDX-License-Identifier: LGPL-2.1+
 */

/* This is tool implements the Native Messaging protocol of web
 * browsers and provides the server part of it.  A Javascript based
 * client can be found in lang/javascript.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif
#include <stdint.h>
#include <sys/stat.h>

#define GPGRT_ENABLE_ES_MACROS 1
#define GPGRT_ENABLE_LOG_MACROS 1
#define GPGRT_ENABLE_ARGPARSE_MACROS 1
#include "gpgme.h"
#include "cJSON.h"


#if GPGRT_VERSION_NUMBER < 0x011c00 /* 1.28 */
int main (void){fputs ("Build with Libgpg-error >= 1.28!\n", stderr);return 1;}
#else /* libgpg-error >= 1.28 */

/* We don't allow a request with more than 64 MiB.  */
#define MAX_REQUEST_SIZE (64 * 1024 * 1024)

/* Minimal, default and maximum chunk size for returned data. The
 * first chunk is returned directly.  If the "more" flag is also
 * returned, a "getmore" command needs to be used to get the next
 * chunk.  Right now this value covers just the value of the "data"
 * element; so to cover for the other returned objects this values
 * needs to be lower than the maximum allowed size of the browser. */
#define MIN_REPLY_CHUNK_SIZE  512
#define DEF_REPLY_CHUNK_SIZE (512 * 1024)
#define MAX_REPLY_CHUNK_SIZE (10 * 1024 * 1024)


static void xoutofcore (const char *type) GPGRT_ATTR_NORETURN;
static cjson_t error_object_v (cjson_t json, const char *message,
                               va_list arg_ptr, gpg_error_t err)
                               GPGRT_ATTR_PRINTF(2,0);
static cjson_t error_object (cjson_t json, const char *message,
                            ...) GPGRT_ATTR_PRINTF(2,3);
static char *error_object_string (const char *message,
                                  ...) GPGRT_ATTR_PRINTF(1,2);


/* True if interactive mode is active.  */
static int opt_interactive;
/* True is debug mode is active.  */
static int opt_debug;

/* Pending data to be returned by a getmore command.  */
static struct
{
  char  *buffer;   /* Malloced data or NULL if not used.  */
  size_t length;   /* Length of that data.  */
  size_t written;  /* # of already written bytes from BUFFER.  */
  const char *type;/* The "type" of the data.  */
  int base64;      /* The "base64" flag of the data.  */
} pending_data;


/*
 * Helper functions and macros
 */

#define xtrymalloc(a)  gpgrt_malloc ((a))
#define xtrystrdup(a)  gpgrt_strdup ((a))
#define xmalloc(a) ({                           \
      void *_r = gpgrt_malloc ((a));            \
      if (!_r)                                  \
        xoutofcore ("malloc");                  \
      _r; })
#define xcalloc(a,b) ({                         \
      void *_r = gpgrt_calloc ((a), (b));       \
      if (!_r)                                  \
        xoutofcore ("calloc");                  \
      _r; })
#define xstrdup(a) ({                           \
      char *_r = gpgrt_strdup ((a));            \
      if (!_r)                                  \
        xoutofcore ("strdup");                  \
      _r; })
#define xstrconcat(a, ...) ({                           \
      char *_r = gpgrt_strconcat ((a), __VA_ARGS__);    \
      if (!_r)                                          \
        xoutofcore ("strconcat");                       \
      _r; })
#define xfree(a) gpgrt_free ((a))

#define spacep(p)   (*(p) == ' ' || *(p) == '\t')

#ifndef HAVE_STPCPY
static GPGRT_INLINE char *
_my_stpcpy (char *a, const char *b)
{
  while (*b)
    *a++ = *b++;
  *a = 0;
  return a;
}
#define stpcpy(a,b) _my_stpcpy ((a), (b))
#endif /*!HAVE_STPCPY*/



static void
xoutofcore (const char *type)
{
  gpg_error_t err = gpg_error_from_syserror ();
  log_error ("%s failed: %s\n", type, gpg_strerror (err));
  exit (2);
}


/* Call cJSON_CreateObject but terminate in case of an error.  */
static cjson_t
xjson_CreateObject (void)
{
  cjson_t json = cJSON_CreateObject ();
  if (!json)
    xoutofcore ("cJSON_CreateObject");
  return json;
}

/* Call cJSON_CreateArray but terminate in case of an error.  */
static cjson_t
xjson_CreateArray (void)
{
  cjson_t json = cJSON_CreateArray ();
  if (!json)
    xoutofcore ("cJSON_CreateArray");
  return json;
}


/* Wrapper around cJSON_AddStringToObject which returns an gpg-error
 * code instead of the NULL or the new object.  */
static gpg_error_t
cjson_AddStringToObject (cjson_t object, const char *name, const char *string)
{
  if (!cJSON_AddStringToObject (object, name, string))
    return gpg_error_from_syserror ();
  return 0;
}


/* Same as cjson_AddStringToObject but prints an error message and
 * terminates the process.  */
static void
xjson_AddStringToObject (cjson_t object, const char *name, const char *string)
{
  if (!cJSON_AddStringToObject (object, name, string))
    xoutofcore ("cJSON_AddStringToObject");
}


/* Same as xjson_AddStringToObject but ignores NULL strings */
static void
xjson_AddStringToObject0 (cjson_t object, const char *name, const char *string)
{
  if (!string)
    return;
  xjson_AddStringToObject (object, name, string);
}

/* Wrapper around cJSON_AddBoolToObject which terminates the process
 * in case of an error.  */
static void
xjson_AddBoolToObject (cjson_t object, const char *name, int abool)
{
  if (!cJSON_AddBoolToObject (object, name, abool))
    xoutofcore ("cJSON_AddStringToObject");
  return ;
}

/* Wrapper around cJSON_AddNumberToObject which terminates the process
 * in case of an error.  */
static void
xjson_AddNumberToObject (cjson_t object, const char *name, double dbl)
{
  if (!cJSON_AddNumberToObject (object, name, dbl))
    xoutofcore ("cJSON_AddNumberToObject");
  return ;
}

/* Wrapper around cJSON_AddItemToObject which terminates the process
 * in case of an error.  */
static void
xjson_AddItemToObject (cjson_t object, const char *name, cjson_t item)
{
  if (!cJSON_AddItemToObject (object, name, item))
    xoutofcore ("cJSON_AddItemToObject");
  return ;
}

/* This is similar to cJSON_AddStringToObject but takes (DATA,
 * DATALEN) and adds it under NAME as a base 64 encoded string to
 * OBJECT.  */
static gpg_error_t
add_base64_to_object (cjson_t object, const char *name,
                      const void *data, size_t datalen)
{
#if GPGRT_VERSION_NUMBER < 0x011d00 /* 1.29 */
  return gpg_error (GPG_ERR_NOT_SUPPORTED);
#else
  gpg_err_code_t err;
  estream_t fp = NULL;
  gpgrt_b64state_t state = NULL;
  cjson_t j_str = NULL;
  void *buffer = NULL;

  fp = es_fopenmem (0, "rwb");
  if (!fp)
    {
      err = gpg_err_code_from_syserror ();
      goto leave;
    }
  state = gpgrt_b64enc_start (fp, "");
  if (!state)
    {
      err = gpg_err_code_from_syserror ();
      goto leave;
    }

  err = gpgrt_b64enc_write (state, data, datalen);
  if (err)
    goto leave;

  err = gpgrt_b64enc_finish (state);
  state = NULL;
  if (err)
    return err;

  es_fputc (0, fp);
  if (es_fclose_snatch (fp, &buffer, NULL))
    {
      fp = NULL;
      err = gpg_error_from_syserror ();
      goto leave;
    }
  fp = NULL;

  j_str = cJSON_CreateStringConvey (buffer);
  if (!j_str)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }
  buffer = NULL;

  if (!cJSON_AddItemToObject (object, name, j_str))
    {
      err = gpg_error_from_syserror ();
      cJSON_Delete (j_str);
      j_str = NULL;
      goto leave;
    }
  j_str = NULL;

 leave:
  xfree (buffer);
  cJSON_Delete (j_str);
  gpgrt_b64enc_finish (state);
  es_fclose (fp);
  return err;
#endif
}


/* Create a JSON error object.  If JSON is not NULL the error message
 * is appended to that object.  An existing "type" item will be replaced. */
static cjson_t
error_object_v (cjson_t json, const char *message, va_list arg_ptr,
                gpg_error_t err)
{
  cjson_t response, j_tmp;
  char *msg;

  msg = gpgrt_vbsprintf (message, arg_ptr);
  if (!msg)
    xoutofcore ("error_object");

  response = json? json : xjson_CreateObject ();

  if (!(j_tmp = cJSON_GetObjectItem (response, "type")))
    xjson_AddStringToObject (response, "type", "error");
  else /* Replace existing "type".  */
    {
      j_tmp = cJSON_CreateString ("error");
      if (!j_tmp)
        xoutofcore ("cJSON_CreateString");
      cJSON_ReplaceItemInObject (response, "type", j_tmp);
     }
  xjson_AddStringToObject (response, "msg", msg);
  xfree (msg);

  xjson_AddNumberToObject (response, "code", err);

  return response;
}


/* Call cJSON_Print but terminate in case of an error.  */
static char *
xjson_Print (cjson_t object)
{
  char *buf;
  buf = cJSON_Print (object);
  if (!buf)
    xoutofcore ("cJSON_Print");
  return buf;
}


static cjson_t
error_object (cjson_t json, const char *message, ...)
{
  cjson_t response;
  va_list arg_ptr;

  va_start (arg_ptr, message);
  response = error_object_v (json, message, arg_ptr, 0);
  va_end (arg_ptr);
  return response;
}


static cjson_t
gpg_error_object (cjson_t json, gpg_error_t err, const char *message, ...)
{
  cjson_t response;
  va_list arg_ptr;

  va_start (arg_ptr, message);
  response = error_object_v (json, message, arg_ptr, err);
  va_end (arg_ptr);
  return response;
}


static char *
error_object_string (const char *message, ...)
{
  cjson_t response;
  va_list arg_ptr;
  char *msg;

  va_start (arg_ptr, message);
  response = error_object_v (NULL, message, arg_ptr, 0);
  va_end (arg_ptr);

  msg = xjson_Print (response);
  cJSON_Delete (response);
  return msg;
}


/* Get the boolean property NAME from the JSON object and store true
 * or valse at R_VALUE.  If the name is unknown the value of DEF_VALUE
 * is returned.  If the type of the value is not boolean,
 * GPG_ERR_INV_VALUE is returned and R_VALUE set to DEF_VALUE.  */
static gpg_error_t
get_boolean_flag (cjson_t json, const char *name, int def_value, int *r_value)
{
  cjson_t j_item;

  j_item = cJSON_GetObjectItem (json, name);
  if (!j_item)
    *r_value = def_value;
  else if (cjson_is_true (j_item))
    *r_value = 1;
  else if (cjson_is_false (j_item))
    *r_value = 0;
  else
    {
      *r_value = def_value;
      return gpg_error (GPG_ERR_INV_VALUE);
    }

  return 0;
}


/* Get the boolean property PROTOCOL from the JSON object and store
 * its value at R_PROTOCOL.  The default is OpenPGP.  */
static gpg_error_t
get_protocol (cjson_t json, gpgme_protocol_t *r_protocol)
{
  cjson_t j_item;

  *r_protocol = GPGME_PROTOCOL_OpenPGP;
  j_item = cJSON_GetObjectItem (json, "protocol");
  if (!j_item)
    ;
  else if (!cjson_is_string (j_item))
    return gpg_error (GPG_ERR_INV_VALUE);
  else if (!strcmp(j_item->valuestring, "openpgp"))
    ;
  else if (!strcmp(j_item->valuestring, "cms"))
    *r_protocol = GPGME_PROTOCOL_CMS;
  else
    return gpg_error (GPG_ERR_UNSUPPORTED_PROTOCOL);

  return 0;
}


/* Get the chunksize from JSON and store it at R_CHUNKSIZE.  */
static gpg_error_t
get_chunksize (cjson_t json, size_t *r_chunksize)
{
  cjson_t j_item;

  *r_chunksize = DEF_REPLY_CHUNK_SIZE;
  j_item = cJSON_GetObjectItem (json, "chunksize");
  if (!j_item)
    ;
  else if (!cjson_is_number (j_item))
    return gpg_error (GPG_ERR_INV_VALUE);
  else if ((size_t)j_item->valueint < MIN_REPLY_CHUNK_SIZE)
    *r_chunksize = MIN_REPLY_CHUNK_SIZE;
  else if ((size_t)j_item->valueint > MAX_REPLY_CHUNK_SIZE)
    *r_chunksize = MAX_REPLY_CHUNK_SIZE;
  else
    *r_chunksize = (size_t)j_item->valueint;

  return 0;
}


/* Extract the keys from the "keys" array in the JSON object.  On
 * success a string with the keys identifiers is stored at R_KEYS.
 * The keys in that string are LF delimited.  On failure an error code
 * is returned.  */
static gpg_error_t
get_keys (cjson_t json, char **r_keystring)
{
  cjson_t j_keys, j_item;
  int i, nkeys;
  char *p;
  size_t length;

  *r_keystring = NULL;

  j_keys = cJSON_GetObjectItem (json, "keys");
  if (!j_keys)
    return gpg_error (GPG_ERR_NO_KEY);
  if (!cjson_is_array (j_keys) && !cjson_is_string (j_keys))
    return gpg_error (GPG_ERR_INV_VALUE);

  /* Fixme: We should better use a membuf like thing.  */
  length = 1; /* For the EOS.  */
  if (cjson_is_string (j_keys))
    {
      nkeys = 1;
      length += strlen (j_keys->valuestring);
      if (strchr (j_keys->valuestring, '\n'))
        return gpg_error (GPG_ERR_INV_USER_ID);
    }
  else
    {
      nkeys = cJSON_GetArraySize (j_keys);
      if (!nkeys)
        return gpg_error (GPG_ERR_NO_KEY);
      for (i=0; i < nkeys; i++)
        {
          j_item = cJSON_GetArrayItem (j_keys, i);
          if (!j_item || !cjson_is_string (j_item))
            return gpg_error (GPG_ERR_INV_VALUE);
          if (i)
            length++; /* Space for delimiter. */
          length += strlen (j_item->valuestring);
          if (strchr (j_item->valuestring, '\n'))
            return gpg_error (GPG_ERR_INV_USER_ID);
        }
    }

  p = *r_keystring = xtrymalloc (length);
  if (!p)
    return gpg_error_from_syserror ();

  if (cjson_is_string (j_keys))
    {
      strcpy (p, j_keys->valuestring);
    }
  else
    {
      for (i=0; i < nkeys; i++)
        {
          j_item = cJSON_GetArrayItem (j_keys, i);
          if (i)
            *p++ = '\n'; /* Add delimiter.  */
          p = stpcpy (p, j_item->valuestring);
        }
    }
  return 0;
}




/*
 *  GPGME support functions.
 */

/* Helper for get_context.  */
static gpgme_ctx_t
_create_new_context (gpgme_protocol_t proto)
{
  gpg_error_t err;
  gpgme_ctx_t ctx;

  err = gpgme_new (&ctx);
  if (err)
    log_fatal ("error creating GPGME context: %s\n", gpg_strerror (err));
  gpgme_set_protocol (ctx, proto);
  gpgme_set_ctx_flag (ctx, "request-origin", "browser");
  return ctx;
}


/* Return a context object for protocol PROTO.  This is currently a
 * statically allocated context initialized for PROTO.  Terminates
 * process on failure.  */
static gpgme_ctx_t
get_context (gpgme_protocol_t proto)
{
  static gpgme_ctx_t ctx_openpgp, ctx_cms;

  if (proto == GPGME_PROTOCOL_OpenPGP)
    {
      if (!ctx_openpgp)
        ctx_openpgp = _create_new_context (proto);
      return ctx_openpgp;
    }
  else if (proto == GPGME_PROTOCOL_CMS)
    {
      if (!ctx_cms)
        ctx_cms = _create_new_context (proto);
      return ctx_cms;
    }
  else
    log_bug ("invalid protocol %d requested\n", proto);
}



/* Free context object retrieved by get_context.  */
static void
release_context (gpgme_ctx_t ctx)
{
  /* Nothing to do right now.  */
  (void)ctx;
}



/* Given a Base-64 encoded string object in JSON return a gpgme data
 * object at R_DATA.  */
static gpg_error_t
data_from_base64_string (gpgme_data_t *r_data, cjson_t json)
{
#if GPGRT_VERSION_NUMBER < 0x011d00 /* 1.29 */
  *r_data = NULL;
  return gpg_error (GPG_ERR_NOT_SUPPORTED);
#else
  gpg_error_t err;
  size_t len;
  char *buf = NULL;
  gpgrt_b64state_t state = NULL;
  gpgme_data_t data = NULL;

  *r_data = NULL;

  /* A quick check on the JSON.  */
  if (!cjson_is_string (json))
    {
      err = gpg_error (GPG_ERR_INV_VALUE);
      goto leave;
    }

  state = gpgrt_b64dec_start (NULL);
  if (!state)
    {
      err = gpg_err_code_from_syserror ();
      goto leave;
    }

  /* Fixme: Data duplication - we should see how to snatch the memory
   * from the json object.  */
  len = strlen (json->valuestring);
  buf = xtrystrdup (json->valuestring);
  if (!buf)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }

  err = gpgrt_b64dec_proc (state, buf, len, &len);
  if (err)
    goto leave;

  err = gpgrt_b64dec_finish (state);
  state = NULL;
  if (err)
    goto leave;

  err = gpgme_data_new_from_mem (&data, buf, len, 1);
  if (err)
    goto leave;
  *r_data = data;
  data = NULL;

 leave:
  xfree (data);
  xfree (buf);
  gpgrt_b64dec_finish (state);
  return err;
#endif
}


/* Create sigsum json array */
static cjson_t
sigsum_to_json (gpgme_sigsum_t summary)
{
  cjson_t result = xjson_CreateArray ();

  if ( (summary & GPGME_SIGSUM_VALID      ))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("valid"));
  if ( (summary & GPGME_SIGSUM_GREEN      ))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("green"));
  if ( (summary & GPGME_SIGSUM_RED        ))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("red"));
  if ( (summary & GPGME_SIGSUM_KEY_REVOKED))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("revoked"));
  if ( (summary & GPGME_SIGSUM_KEY_EXPIRED))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("key-expired"));
  if ( (summary & GPGME_SIGSUM_SIG_EXPIRED))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("sig-expired"));
  if ( (summary & GPGME_SIGSUM_KEY_MISSING))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("key-missing"));
  if ( (summary & GPGME_SIGSUM_CRL_MISSING))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("crl-missing"));
  if ( (summary & GPGME_SIGSUM_CRL_TOO_OLD))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("crl-too-old"));
  if ( (summary & GPGME_SIGSUM_BAD_POLICY ))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("bad-policy"));
  if ( (summary & GPGME_SIGSUM_SYS_ERROR  ))
    cJSON_AddItemToArray (result,
        cJSON_CreateString ("sys-error"));

  return result;
}


/* Helper for summary formatting */
static const char *
validity_to_string (gpgme_validity_t val)
{
  switch (val)
    {
    case GPGME_VALIDITY_UNDEFINED:return "undefined";
    case GPGME_VALIDITY_NEVER:    return "never";
    case GPGME_VALIDITY_MARGINAL: return "marginal";
    case GPGME_VALIDITY_FULL:     return "full";
    case GPGME_VALIDITY_ULTIMATE: return "ultimate";
    case GPGME_VALIDITY_UNKNOWN:
    default:                      return "unknown";
    }
}

static const char *
protocol_to_string (gpgme_protocol_t proto)
{
  switch (proto)
    {
    case GPGME_PROTOCOL_OpenPGP: return "OpenPGP";
    case GPGME_PROTOCOL_CMS:     return "CMS";
    case GPGME_PROTOCOL_GPGCONF: return "gpgconf";
    case GPGME_PROTOCOL_ASSUAN:  return "assuan";
    case GPGME_PROTOCOL_G13:     return "g13";
    case GPGME_PROTOCOL_UISERVER:return "uiserver";
    case GPGME_PROTOCOL_SPAWN:   return "spawn";
    default:
                                 return "unknown";
    }
}

/* Create a sig_notation json object */
static cjson_t
sig_notation_to_json (gpgme_sig_notation_t not)
{
  cjson_t result = xjson_CreateObject ();
  xjson_AddBoolToObject (result, "human_readable", not->human_readable);
  xjson_AddBoolToObject (result, "critical", not->critical);

  xjson_AddStringToObject0 (result, "name", not->name);
  xjson_AddStringToObject0 (result, "value", not->value);

  xjson_AddNumberToObject (result, "flags", not->flags);

  return result;
}

/* Create a key_sig json object */
static cjson_t
key_sig_to_json (gpgme_key_sig_t sig)
{
  cjson_t result = xjson_CreateObject ();

  xjson_AddBoolToObject (result, "revoked", sig->revoked);
  xjson_AddBoolToObject (result, "expired", sig->expired);
  xjson_AddBoolToObject (result, "invalid", sig->invalid);
  xjson_AddBoolToObject (result, "exportable", sig->exportable);

  xjson_AddStringToObject0 (result, "pubkey_algo_name",
                            gpgme_pubkey_algo_name (sig->pubkey_algo));
  xjson_AddStringToObject0 (result, "keyid", sig->keyid);
  xjson_AddStringToObject0 (result, "status", gpgme_strerror (sig->status));
  xjson_AddStringToObject0 (result, "name", sig->name);
  xjson_AddStringToObject0 (result, "email", sig->email);
  xjson_AddStringToObject0 (result, "comment", sig->comment);

  xjson_AddNumberToObject (result, "pubkey_algo", sig->pubkey_algo);
  xjson_AddNumberToObject (result, "timestamp", sig->timestamp);
  xjson_AddNumberToObject (result, "expires", sig->expires);
  xjson_AddNumberToObject (result, "status_code", sig->status);
  xjson_AddNumberToObject (result, "sig_class", sig->sig_class);

  if (sig->notations)
    {
      gpgme_sig_notation_t not;
      cjson_t array = xjson_CreateArray ();
      for (not = sig->notations; not; not = not->next)
        cJSON_AddItemToArray (array, sig_notation_to_json (not));
      xjson_AddItemToObject (result, "notations", array);
    }

  return result;
}

/* Create a tofu info object */
static cjson_t
tofu_to_json (gpgme_tofu_info_t tofu)
{
  cjson_t result = xjson_CreateObject ();

  xjson_AddStringToObject0 (result, "description", tofu->description);

  xjson_AddNumberToObject (result, "validity", tofu->validity);
  xjson_AddNumberToObject (result, "policy", tofu->policy);
  xjson_AddNumberToObject (result, "signcount", tofu->signcount);
  xjson_AddNumberToObject (result, "encrcount", tofu->encrcount);
  xjson_AddNumberToObject (result, "signfirst", tofu->signfirst);
  xjson_AddNumberToObject (result, "signlast", tofu->signlast);
  xjson_AddNumberToObject (result, "encrfirst", tofu->encrfirst);
  xjson_AddNumberToObject (result, "encrlast", tofu->encrlast);

  return result;
}

/* Create a userid json object */
static cjson_t
uid_to_json (gpgme_user_id_t uid)
{
  cjson_t result = xjson_CreateObject ();

  xjson_AddBoolToObject (result, "revoked", uid->revoked);
  xjson_AddBoolToObject (result, "invalid", uid->invalid);

  xjson_AddStringToObject0 (result, "validity",
                            validity_to_string (uid->validity));
  xjson_AddStringToObject0 (result, "uid", uid->uid);
  xjson_AddStringToObject0 (result, "name", uid->name);
  xjson_AddStringToObject0 (result, "email", uid->email);
  xjson_AddStringToObject0 (result, "comment", uid->comment);
  xjson_AddStringToObject0 (result, "address", uid->address);

  xjson_AddNumberToObject (result, "origin", uid->origin);
  xjson_AddNumberToObject (result, "last_update", uid->last_update);

  /* Key sigs */
  if (uid->signatures)
    {
      cjson_t sig_array = xjson_CreateArray ();
      gpgme_key_sig_t sig;

      for (sig = uid->signatures; sig; sig = sig->next)
        cJSON_AddItemToArray (sig_array, key_sig_to_json (sig));

      xjson_AddItemToObject (result, "signatures", sig_array);
    }

  /* TOFU info */
  if (uid->tofu)
    {
      gpgme_tofu_info_t tofu;
      cjson_t array = xjson_CreateArray ();
      for (tofu = uid->tofu; tofu; tofu = tofu->next)
        cJSON_AddItemToArray (array, tofu_to_json (tofu));
      xjson_AddItemToObject (result, "tofu", array);
    }

  return result;
}

/* Create a subkey json object */
static cjson_t
subkey_to_json (gpgme_subkey_t sub)
{
  cjson_t result = xjson_CreateObject ();

  xjson_AddBoolToObject (result, "revoked", sub->revoked);
  xjson_AddBoolToObject (result, "expired", sub->expired);
  xjson_AddBoolToObject (result, "disabled", sub->disabled);
  xjson_AddBoolToObject (result, "invalid", sub->invalid);
  xjson_AddBoolToObject (result, "can_encrypt", sub->can_encrypt);
  xjson_AddBoolToObject (result, "can_sign", sub->can_sign);
  xjson_AddBoolToObject (result, "can_certify", sub->can_certify);
  xjson_AddBoolToObject (result, "can_authenticate", sub->can_authenticate);
  xjson_AddBoolToObject (result, "secret", sub->secret);
  xjson_AddBoolToObject (result, "is_qualified", sub->is_qualified);
  xjson_AddBoolToObject (result, "is_cardkey", sub->is_cardkey);
  xjson_AddBoolToObject (result, "is_de_vs", sub->is_de_vs);

  xjson_AddStringToObject0 (result, "pubkey_algo_name",
                            gpgme_pubkey_algo_name (sub->pubkey_algo));
  xjson_AddStringToObject0 (result, "pubkey_algo_string",
                            gpgme_pubkey_algo_string (sub));
  xjson_AddStringToObject0 (result, "keyid", sub->keyid);
  xjson_AddStringToObject0 (result, "card_number", sub->card_number);
  xjson_AddStringToObject0 (result, "curve", sub->curve);
  xjson_AddStringToObject0 (result, "keygrip", sub->keygrip);

  xjson_AddNumberToObject (result, "pubkey_algo", sub->pubkey_algo);
  xjson_AddNumberToObject (result, "length", sub->length);
  xjson_AddNumberToObject (result, "timestamp", sub->timestamp);
  xjson_AddNumberToObject (result, "expires", sub->expires);

  return result;
}

/* Create a key json object */
static cjson_t
key_to_json (gpgme_key_t key)
{
  cjson_t result = xjson_CreateObject ();

  xjson_AddBoolToObject (result, "revoked", key->revoked);
  xjson_AddBoolToObject (result, "expired", key->expired);
  xjson_AddBoolToObject (result, "disabled", key->disabled);
  xjson_AddBoolToObject (result, "invalid", key->invalid);
  xjson_AddBoolToObject (result, "can_encrypt", key->can_encrypt);
  xjson_AddBoolToObject (result, "can_sign", key->can_sign);
  xjson_AddBoolToObject (result, "can_certify", key->can_certify);
  xjson_AddBoolToObject (result, "can_authenticate", key->can_authenticate);
  xjson_AddBoolToObject (result, "secret", key->secret);
  xjson_AddBoolToObject (result, "is_qualified", key->is_qualified);

  xjson_AddStringToObject0 (result, "protocol",
                            protocol_to_string (key->protocol));
  xjson_AddStringToObject0 (result, "issuer_serial", key->issuer_serial);
  xjson_AddStringToObject0 (result, "issuer_name", key->issuer_name);
  xjson_AddStringToObject0 (result, "fingerprint", key->fpr);
  xjson_AddStringToObject0 (result, "chain_id", key->chain_id);
  xjson_AddStringToObject0 (result, "owner_trust",
                            validity_to_string (key->owner_trust));

  xjson_AddNumberToObject (result, "origin", key->origin);
  xjson_AddNumberToObject (result, "last_update", key->last_update);

  /* Add subkeys */
  if (key->subkeys)
    {
      cjson_t subkey_array = xjson_CreateArray ();
      gpgme_subkey_t sub;
      for (sub = key->subkeys; sub; sub = sub->next)
        cJSON_AddItemToArray (subkey_array, subkey_to_json (sub));

      xjson_AddItemToObject (result, "subkeys", subkey_array);
    }

  /* User Ids */
  if (key->uids)
    {
      cjson_t uid_array = xjson_CreateArray ();
      gpgme_user_id_t uid;
      for (uid = key->uids; uid; uid = uid->next)
        cJSON_AddItemToArray (uid_array, uid_to_json (uid));

      xjson_AddItemToObject (result, "userids", uid_array);
    }

  return result;
}

/* Create a signature json object */
static cjson_t
signature_to_json (gpgme_signature_t sig)
{
  cjson_t result = xjson_CreateObject ();

  xjson_AddStringToObject0 (result, "status",
                            gpgme_strerror (sig->status));

  xjson_AddStringToObject0 (result, "validity",
                            validity_to_string (sig->validity));
  xjson_AddStringToObject0 (result, "fingerprint", sig->fpr);

  xjson_AddItemToObject (result, "summary", sigsum_to_json (sig->summary));

  xjson_AddNumberToObject (result, "created", sig->timestamp);
  xjson_AddNumberToObject (result, "expired", sig->exp_timestamp);
  xjson_AddNumberToObject (result, "code", sig->status);

  return result;
}

/* Create a JSON object from a gpgme_verify result */
static cjson_t
verify_result_to_json (gpgme_verify_result_t verify_result)
{
  cjson_t response = xjson_CreateObject ();

  if (verify_result->signatures)
    {
      cjson_t array = xjson_CreateArray ();
      gpgme_signature_t sig;

      for (sig = verify_result->signatures; sig; sig = sig->next)
        cJSON_AddItemToArray (array, signature_to_json (sig));
      xjson_AddItemToObject (response, "signatures", array);
    }

  return response;
}

/* Create a JSON object from an engine_info */
static cjson_t
engine_info_to_json (gpgme_engine_info_t info)
{
  cjson_t result = xjson_CreateObject ();

  xjson_AddStringToObject0 (result, "protocol",
                            protocol_to_string (info->protocol));
  xjson_AddStringToObject0 (result, "fname", info->file_name);
  xjson_AddStringToObject0 (result, "version", info->version);
  xjson_AddStringToObject0 (result, "req_version", info->req_version);
  xjson_AddStringToObject0 (result, "homedir", info->home_dir ?
                                                info->home_dir :
                                                "default");
  return result;
}

/* Create a gpgme_data from json string data named "name"
 * in the request. Takes the base64 option into account.
 *
 * Adds an error to the "result" on error. */
static gpg_error_t
get_string_data (cjson_t request, cjson_t result, const char *name,
                 gpgme_data_t *r_data)
{
  gpgme_error_t err;
  int opt_base64;
  cjson_t j_data;

  if ((err = get_boolean_flag (request, "base64", 0, &opt_base64)))
    return err;

  /* Get the data.  Note that INPUT is a shallow data object with the
   * storage hold in REQUEST.  */
  j_data = cJSON_GetObjectItem (request, name);
  if (!j_data)
    {
      return gpg_error (GPG_ERR_NO_DATA);
    }
  if (!cjson_is_string (j_data))
    {
      return gpg_error (GPG_ERR_INV_VALUE);
    }
  if (opt_base64)
    {
      err = data_from_base64_string (r_data, j_data);
      if (err)
        {
          gpg_error_object (result, err,
                            "Error decoding Base-64 encoded '%s': %s",
                            name, gpg_strerror (err));
          return err;
        }
    }
  else
    {
      err = gpgme_data_new_from_mem (r_data, j_data->valuestring,
                                     strlen (j_data->valuestring), 0);
      if (err)
        {
          gpg_error_object (result, err, "Error getting '%s': %s",
                            name, gpg_strerror (err));
          return err;
        }
    }
  return 0;
}


/*
 * Implementation of the commands.
 */


/* Create a "data" object and the "type", "base64" and "more" flags
 * from DATA and append them to RESULT.  Ownership of DATA is
 * transferred to this function.  TYPE must be a fixed string.
 * CHUNKSIZE is the chunksize requested from the caller.  If BASE64 is
 * -1 the need for base64 encoding is determined by the content of
 * DATA, all other values are taken as true or false.  Note that
 * op_getmore has similar code but works on PENDING_DATA which is set
 * here.  */
static gpg_error_t
make_data_object (cjson_t result, gpgme_data_t data, size_t chunksize,
                  const char *type, int base64)
{
  gpg_error_t err;
  char *buffer;
  const char *s;
  size_t buflen, n;
  int c;

  if (!base64 || base64 == -1) /* Make sure that we really have a string.  */
    gpgme_data_write (data, "", 1);

  buffer = gpgme_data_release_and_get_mem (data, &buflen);
  data = NULL;
  if (!buffer)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }

  if (base64 == -1)
    {
      base64 = 0;
      if (!buflen)
        log_fatal ("Appended Nul byte got lost\n");
      /* Figure out if there is any Nul octet in the buffer.  In that
       * case we need to Base-64 the buffer.  Due to problems with the
       * browser's Javascript we use Base-64 also in case an UTF-8
       * character is in the buffer.  This is because the chunking may
       * split an UTF-8 characters and JS can't handle this.  */
      for (s=buffer, n=0; n < buflen -1; s++, n++)
        if (!*s || (*s & 0x80))
          {
            buflen--; /* Adjust for the extra nul byte.  */
            base64 = 1;
            break;
          }
    }

  /* Adjust the chunksize if we need to do base64 conversion.  */
  if (base64)
    chunksize = (chunksize / 4) * 3;

  xjson_AddStringToObject (result, "type", type);
  xjson_AddBoolToObject (result, "base64", base64);

  if (buflen > chunksize)
    {
      xjson_AddBoolToObject (result, "more", 1);

      c = buffer[chunksize];
      buffer[chunksize] = 0;
      if (base64)
        err = add_base64_to_object (result, "data", buffer, chunksize);
      else
        err = cjson_AddStringToObject (result, "data", buffer);
      buffer[chunksize] = c;
      if (err)
        goto leave;

      pending_data.buffer = buffer;
      buffer = NULL;
      pending_data.length = buflen;
      pending_data.written = chunksize;
      pending_data.type = type;
      pending_data.base64 = base64;
    }
  else
    {
      if (base64)
        err = add_base64_to_object (result, "data", buffer, buflen);
      else
        err = cjson_AddStringToObject (result, "data", buffer);
    }

 leave:
  gpgme_free (buffer);
  return err;
}



static const char hlp_encrypt[] =
  "op:     \"encrypt\"\n"
  "keys:   Array of strings with the fingerprints or user-ids\n"
  "        of the keys to encrypt the data.  For a single key\n"
  "        a String may be used instead of an array.\n"
  "data:   Input data. \n"
  "\n"
  "Optional parameters:\n"
  "protocol:      Either \"openpgp\" (default) or \"cms\".\n"
  "chunksize:     Max number of bytes in the resulting \"data\".\n"
  "\n"
  "Optional boolean flags (default is false):\n"
  "base64:        Input data is base64 encoded.\n"
  "mime:          Indicate that data is a MIME object.\n"
  "armor:         Request output in armored format.\n"
  "always-trust:  Request --always-trust option.\n"
  "no-encrypt-to: Do not use a default recipient.\n"
  "no-compress:   Do not compress the plaintext first.\n"
  "throw-keyids:  Request the --throw-keyids option.\n"
  "want-address:  Require that the keys include a mail address.\n"
  "wrap:          Assume the input is an OpenPGP message.\n"
  "\n"
  "Response on success:\n"
  "type:   \"ciphertext\"\n"
  "data:   Unless armor mode is used a Base64 encoded binary\n"
  "        ciphertext.  In armor mode a string with an armored\n"
  "        OpenPGP or a PEM message.\n"
  "base64: Boolean indicating whether data is base64 encoded.\n"
  "more:   Optional boolean indicating that \"getmore\" is required.";
static gpg_error_t
op_encrypt (cjson_t request, cjson_t result)
{
  gpg_error_t err;
  gpgme_ctx_t ctx = NULL;
  gpgme_protocol_t protocol;
  size_t chunksize;
  int opt_mime;
  char *keystring = NULL;
  gpgme_data_t input = NULL;
  gpgme_data_t output = NULL;
  int abool;
  gpgme_encrypt_flags_t encrypt_flags = 0;

  if ((err = get_protocol (request, &protocol)))
    goto leave;
  ctx = get_context (protocol);
  if ((err = get_chunksize (request, &chunksize)))
    goto leave;

  if ((err = get_boolean_flag (request, "mime", 0, &opt_mime)))
    goto leave;

  if ((err = get_boolean_flag (request, "armor", 0, &abool)))
    goto leave;
  gpgme_set_armor (ctx, abool);
  if ((err = get_boolean_flag (request, "always-trust", 0, &abool)))
    goto leave;
  if (abool)
    encrypt_flags |= GPGME_ENCRYPT_ALWAYS_TRUST;
  if ((err = get_boolean_flag (request, "no-encrypt-to", 0,&abool)))
    goto leave;
  if (abool)
    encrypt_flags |= GPGME_ENCRYPT_NO_ENCRYPT_TO;
  if ((err = get_boolean_flag (request, "no-compress", 0, &abool)))
    goto leave;
  if (abool)
    encrypt_flags |= GPGME_ENCRYPT_NO_COMPRESS;
  if ((err = get_boolean_flag (request, "throw-keyids", 0, &abool)))
    goto leave;
  if (abool)
    encrypt_flags |= GPGME_ENCRYPT_THROW_KEYIDS;
  if ((err = get_boolean_flag (request, "wrap", 0, &abool)))
    goto leave;
  if (abool)
    encrypt_flags |= GPGME_ENCRYPT_WRAP;
  if ((err = get_boolean_flag (request, "want-address", 0, &abool)))
    goto leave;
  if (abool)
    encrypt_flags |= GPGME_ENCRYPT_WANT_ADDRESS;


  /* Get the keys.  */
  err = get_keys (request, &keystring);
  if (err)
    {
      /* Provide a custom error response.  */
      gpg_error_object (result, err, "Error getting keys: %s",
                        gpg_strerror (err));
      goto leave;
    }

  if ((err = get_string_data (request, result, "data", &input)))
      goto leave;

  if (opt_mime)
    gpgme_data_set_encoding (input, GPGME_DATA_ENCODING_MIME);


  /* Create an output data object.  */
  err = gpgme_data_new (&output);
  if (err)
    {
      gpg_error_object (result, err, "Error creating output data object: %s",
                        gpg_strerror (err));
      goto leave;
    }

  /* Encrypt.  */
  err = gpgme_op_encrypt_ext (ctx, NULL, keystring, encrypt_flags,
                              input, output);
  /* encrypt_result = gpgme_op_encrypt_result (ctx); */
  if (err)
    {
      gpg_error_object (result, err, "Encryption failed: %s",
                        gpg_strerror (err));
      goto leave;
    }
  gpgme_data_release (input);
  input = NULL;

  /* We need to base64 if armoring has not been requested.  */
  err = make_data_object (result, output, chunksize,
                          "ciphertext", !gpgme_get_armor (ctx));
  output = NULL;

 leave:
  xfree (keystring);
  release_context (ctx);
  gpgme_data_release (input);
  gpgme_data_release (output);
  return err;
}



static const char hlp_decrypt[] =
  "op:     \"decrypt\"\n"
  "data:   The encrypted data.\n"
  "\n"
  "Optional parameters:\n"
  "protocol:      Either \"openpgp\" (default) or \"cms\".\n"
  "chunksize:     Max number of bytes in the resulting \"data\".\n"
  "\n"
  "Optional boolean flags (default is false):\n"
  "base64:        Input data is base64 encoded.\n"
  "\n"
  "Response on success:\n"
  "type:   \"plaintext\"\n"
  "data:   The decrypted data.  This may be base64 encoded.\n"
  "base64: Boolean indicating whether data is base64 encoded.\n"
  "mime:   A Boolean indicating whether the data is a MIME object.\n"
  "info:   An optional object with extra information.\n"
  "info:   An object with optional signature information.\n"
  "  Array values:\n"
  "   signatures\n"
  "    String values:\n"
  "     status: The status of the signature.\n"
  "     fingerprint: The fingerprint of the signing key.\n"
  "     validity: The validity as string.\n"
  "    Number values:\n"
  "     code: The status as a number.\n"
  "    Array values:\n"
  "     summary: A string array of the sig summary.\n"
  "more:   Optional boolean indicating that \"getmore\" is required.";
static gpg_error_t
op_decrypt (cjson_t request, cjson_t result)
{
  gpg_error_t err;
  gpgme_ctx_t ctx = NULL;
  gpgme_protocol_t protocol;
  size_t chunksize;
  gpgme_data_t input = NULL;
  gpgme_data_t output = NULL;
  gpgme_decrypt_result_t decrypt_result;
  gpgme_verify_result_t verify_result;

  if ((err = get_protocol (request, &protocol)))
    goto leave;
  ctx = get_context (protocol);
  if ((err = get_chunksize (request, &chunksize)))
    goto leave;

  if ((err = get_string_data (request, result, "data", &input)))
      goto leave;

  /* Create an output data object.  */
  err = gpgme_data_new (&output);
  if (err)
    {
      gpg_error_object (result, err,
                        "Error creating output data object: %s",
                        gpg_strerror (err));
      goto leave;
    }

  /* Decrypt.  */
  err = gpgme_op_decrypt_ext (ctx, GPGME_DECRYPT_VERIFY,
                              input, output);
  decrypt_result = gpgme_op_decrypt_result (ctx);
  if (err)
    {
      gpg_error_object (result, err, "Decryption failed: %s",
                        gpg_strerror (err));
      goto leave;
    }
  gpgme_data_release (input);
  input = NULL;

  if (decrypt_result->is_mime)
    xjson_AddBoolToObject (result, "mime", 1);

  verify_result = gpgme_op_verify_result (ctx);
  if (verify_result && verify_result->signatures)
    {
      xjson_AddItemToObject (result, "info",
                             verify_result_to_json (verify_result));
    }

  err = make_data_object (result, output, chunksize, "plaintext", -1);
  output = NULL;

  if (err)
    {
      gpg_error_object (result, err, "Plaintext output failed: %s",
                        gpg_strerror (err));
      goto leave;
    }

 leave:
  release_context (ctx);
  gpgme_data_release (input);
  gpgme_data_release (output);
  return err;
}



static const char hlp_sign[] =
  "op:     \"sign\"\n"
  "keys:   Array of strings with the fingerprints of the signing key.\n"
  "        For a single key a String may be used instead of an array.\n"
  "data:   Input data. \n"
  "\n"
  "Optional parameters:\n"
  "protocol:      Either \"openpgp\" (default) or \"cms\".\n"
  "chunksize:     Max number of bytes in the resulting \"data\".\n"
  "sender:        The mail address of the sender.\n"
  "mode:          A string with the signing mode can be:\n"
  "               detached (default)\n"
  "               opaque\n"
  "               clearsign\n"
  "\n"
  "Optional boolean flags (default is false):\n"
  "base64:        Input data is base64 encoded.\n"
  "armor:         Request output in armored format.\n"
  "\n"
  "Response on success:\n"
  "type:   \"signature\"\n"
  "data:   Unless armor mode is used a Base64 encoded binary\n"
  "        signature.  In armor mode a string with an armored\n"
  "        OpenPGP or a PEM message.\n"
  "base64: Boolean indicating whether data is base64 encoded.\n"
  "more:   Optional boolean indicating that \"getmore\" is required.";
static gpg_error_t
op_sign (cjson_t request, cjson_t result)
{
  gpg_error_t err;
  gpgme_ctx_t ctx = NULL;
  gpgme_protocol_t protocol;
  size_t chunksize;
  char *keystring = NULL;
  gpgme_data_t input = NULL;
  gpgme_data_t output = NULL;
  int abool;
  cjson_t j_tmp;
  gpgme_sig_mode_t mode = GPGME_SIG_MODE_DETACH;
  gpgme_ctx_t keylist_ctx = NULL;
  gpgme_key_t key = NULL;

  if ((err = get_protocol (request, &protocol)))
    goto leave;
  ctx = get_context (protocol);
  if ((err = get_chunksize (request, &chunksize)))
    goto leave;

  if ((err = get_boolean_flag (request, "armor", 0, &abool)))
    goto leave;
  gpgme_set_armor (ctx, abool);

  j_tmp = cJSON_GetObjectItem (request, "mode");
  if (j_tmp && cjson_is_string (j_tmp))
    {
      if (!strcmp (j_tmp->valuestring, "opaque"))
        {
          mode = GPGME_SIG_MODE_NORMAL;
        }
      else if (!strcmp (j_tmp->valuestring, "clearsign"))
        {
          mode = GPGME_SIG_MODE_CLEAR;
        }
    }

  j_tmp = cJSON_GetObjectItem (request, "sender");
  if (j_tmp && cjson_is_string (j_tmp))
    {
      gpgme_set_sender (ctx, j_tmp->valuestring);
    }

  /* Get the keys.  */
  err = get_keys (request, &keystring);
  if (err)
    {
      /* Provide a custom error response.  */
      gpg_error_object (result, err, "Error getting keys: %s",
                        gpg_strerror (err));
      goto leave;
    }

  /* Do a keylisting and add the keys */
  if ((err = gpgme_new (&keylist_ctx)))
    goto leave;
  gpgme_set_protocol (keylist_ctx, protocol);
  gpgme_set_keylist_mode (keylist_ctx, GPGME_KEYLIST_MODE_LOCAL);

  err = gpgme_op_keylist_start (ctx, keystring, 1);
  if (err)
    {
      gpg_error_object (result, err, "Error listing keys: %s",
                        gpg_strerror (err));
      goto leave;
    }
  while (!(err = gpgme_op_keylist_next (ctx, &key)))
    {
      if ((err = gpgme_signers_add (ctx, key)))
        {
          gpg_error_object (result, err, "Error adding signer: %s",
                            gpg_strerror (err));
          goto leave;
        }
      gpgme_key_unref (key);
    }

  if ((err = get_string_data (request, result, "data", &input)))
      goto leave;

  /* Create an output data object.  */
  err = gpgme_data_new (&output);
  if (err)
    {
      gpg_error_object (result, err, "Error creating output data object: %s",
                        gpg_strerror (err));
      goto leave;
    }

  /* Sign. */
  err = gpgme_op_sign (ctx, input, output, mode);
  if (err)
    {
      gpg_error_object (result, err, "Signing failed: %s",
                        gpg_strerror (err));
      goto leave;
    }

  gpgme_data_release (input);
  input = NULL;

  /* We need to base64 if armoring has not been requested.  */
  err = make_data_object (result, output, chunksize,
                          "signature", !gpgme_get_armor (ctx));
  output = NULL;

 leave:
  xfree (keystring);
  release_context (ctx);
  release_context (keylist_ctx);
  gpgme_data_release (input);
  gpgme_data_release (output);
  return err;
}

static const char hlp_verify[] =
  "op:     \"verify\"\n"
  "data:   The data to verify.\n"
  "\n"
  "Optional parameters:\n"
  "protocol:      Either \"openpgp\" (default) or \"cms\".\n"
  "chunksize:     Max number of bytes in the resulting \"data\".\n"
  "signature:     A detached signature. If missing opaque is assumed.\n"
  "\n"
  "Optional boolean flags (default is false):\n"
  "base64:        Input data is base64 encoded.\n"
  "\n"
  "Response on success:\n"
  "type:   \"plaintext\"\n"
  "data:   The verified data.  This may be base64 encoded.\n"
  "base64: Boolean indicating whether data is base64 encoded.\n"
  "info:   An object with signature information.\n"
  "  Array values:\n"
  "   signatures\n"
  "    String values:\n"
  "     status: The status of the signature.\n"
  "     fingerprint: The fingerprint of the signing key.\n"
  "     validity: The validity as string.\n"
  "    Number values:\n"
  "     code: The status as a number.\n"
  "    Array values:\n"
  "     summary: A string array of the sig summary.\n"
  "more:   Optional boolean indicating that \"getmore\" is required.";
static gpg_error_t
op_verify (cjson_t request, cjson_t result)
{
  gpg_error_t err;
  gpgme_ctx_t ctx = NULL;
  gpgme_protocol_t protocol;
  size_t chunksize;
  gpgme_data_t input = NULL;
  gpgme_data_t signature = NULL;
  gpgme_data_t output = NULL;
  gpgme_verify_result_t verify_result;

  if ((err = get_protocol (request, &protocol)))
    goto leave;
  ctx = get_context (protocol);
  if ((err = get_chunksize (request, &chunksize)))
    goto leave;

  if ((err = get_string_data (request, result, "data", &input)))
    goto leave;

  err = get_string_data (request, result, "signature", &signature);
  /* Signature data is optional otherwise we expect opaque or clearsigned. */
  if (err && err != gpg_error (GPG_ERR_NO_DATA))
    goto leave;

  /* Create an output data object.  */
  err = gpgme_data_new (&output);
  if (err)
    {
      gpg_error_object (result, err, "Error creating output data object: %s",
                        gpg_strerror (err));
      goto leave;
    }

  /* Verify.  */
  if (signature)
    {
      err = gpgme_op_verify (ctx, signature, input, output);
    }
  else
    {
      err = gpgme_op_verify (ctx, input, 0, output);
    }
  if (err)
    {
      gpg_error_object (result, err, "Verify failed: %s", gpg_strerror (err));
      goto leave;
    }
  gpgme_data_release (input);
  input = NULL;
  gpgme_data_release (signature);
  signature = NULL;

  verify_result = gpgme_op_verify_result (ctx);
  if (verify_result && verify_result->signatures)
    {
      xjson_AddItemToObject (result, "info",
                             verify_result_to_json (verify_result));
    }

  err = make_data_object (result, output, chunksize, "plaintext", -1);
  output = NULL;

  if (err)
    {
      gpg_error_object (result, err, "Plaintext output failed: %s",
                        gpg_strerror (err));
      goto leave;
    }

 leave:
  release_context (ctx);
  gpgme_data_release (input);
  gpgme_data_release (output);
  gpgme_data_release (signature);
  return err;
}

static const char hlp_version[] =
  "op:     \"version\"\n"
  "\n"
  "Response on success:\n"
  "gpgme:  The GPGME Version.\n"
  "info:   dump of engine info. containing:\n"
  "        protocol: The protocol.\n"
  "        fname:    The file name.\n"
  "        version:  The version.\n"
  "        req_ver:  The required version.\n"
  "        homedir:  The homedir of the engine or \"default\".\n";
static gpg_error_t
op_version (cjson_t request, cjson_t result)
{
  gpg_error_t err = 0;
  gpgme_engine_info_t ei = NULL;
  cjson_t infos = xjson_CreateArray ();

  if (!cJSON_AddStringToObject (result, "gpgme", gpgme_check_version (NULL)))
    {
      cJSON_Delete (infos);
      return gpg_error_from_syserror ();
    }

  if ((err = gpgme_get_engine_info (&ei)))
    {
      cJSON_Delete (infos);
      return err;
    }

  for (; ei; ei = ei->next)
    cJSON_AddItemToArray (infos, engine_info_to_json (ei));

  if (!cJSON_AddItemToObject (result, "info", infos))
    {
      err = gpg_error_from_syserror ();
      cJSON_Delete (infos);
      return err;
    }

  return 0;
}

static const char hlp_keylist[] =
  "op:     \"keylist\"\n"
  "keys:   Array of strings or fingerprints to lookup\n"
  "        For a single key a String may be used instead of an array.\n"
  "\n"
  "Optional parameters:\n"
  "protocol:      Either \"openpgp\" (default) or \"cms\".\n"
  "chunksize:     Max number of bytes in the resulting \"data\".\n"
  "\n"
  "Optional boolean flags (default is false):\n"
  "secret:        List secret keys.\n"
  "extern:        Add KEYLIST_MODE_EXTERN.\n"
  "local:         Add KEYLIST_MODE_LOCAL. (default mode).\n"
  "sigs:          Add KEYLIST_MODE_SIGS.\n"
  "notations:     Add KEYLIST_MODE_SIG_NOTATIONS.\n"
  "tofu:          Add KEYLIST_MODE_WITH_TOFU.\n"
  "ephemeral:     Add KEYLIST_MODE_EPHEMERAL.\n"
  "validate:      Add KEYLIST_MODE_VALIDATE.\n"
  "\n"
  "Response on success:\n"
  "keys:   Array of keys.\n"
  "  Boolean values:\n"
  "   revoked\n"
  "   expired\n"
  "   disabled\n"
  "   invalid\n"
  "   can_encrypt\n"
  "   can_sign\n"
  "   can_certify\n"
  "   can_authenticate\n"
  "   secret\n"
  "   is_qualified\n"
  "  String values:\n"
  "   protocol\n"
  "   issuer_serial (CMS Only)\n"
  "   issuer_name (CMS Only)\n"
  "   chain_id (CMS Only)\n"
  "   owner_trust (OpenPGP only)\n"
  "   fingerprint\n"
  "  Number values:\n"
  "   last_update\n"
  "   origin\n"
  "  Array values:\n"
  "   subkeys\n"
  "    Boolean values:\n"
  "     revoked\n"
  "     expired\n"
  "     disabled\n"
  "     invalid\n"
  "     can_encrypt\n"
  "     can_sign\n"
  "     can_certify\n"
  "     can_authenticate\n"
  "     secret\n"
  "     is_qualified\n"
  "     is_cardkey\n"
  "     is_de_vs\n"
  "    String values:\n"
  "     pubkey_algo_name\n"
  "     pubkey_algo_string\n"
  "     keyid\n"
  "     card_number\n"
  "     curve\n"
  "     keygrip\n"
  "    Number values:\n"
  "     pubkey_algo\n"
  "     length\n"
  "     timestamp\n"
  "     expires\n"
  "   userids\n"
  "    Boolean values:\n"
  "     revoked\n"
  "     invalid\n"
  "    String values:\n"
  "     validity\n"
  "     uid\n"
  "     name\n"
  "     email\n"
  "     comment\n"
  "     address\n"
  "    Number values:\n"
  "     origin\n"
  "     last_update\n"
  "    Array values:\n"
  "     signatures\n"
  "      Boolean values:\n"
  "       revoked\n"
  "       expired\n"
  "       invalid\n"
  "       exportable\n"
  "      String values:\n"
  "       pubkey_algo_name\n"
  "       keyid\n"
  "       status\n"
  "       uid\n"
  "       name\n"
  "       email\n"
  "       comment\n"
  "      Number values:\n"
  "       pubkey_algo\n"
  "       timestamp\n"
  "       expires\n"
  "       status_code\n"
  "       sig_class\n"
  "      Array values:\n"
  "       notations\n"
  "        Boolean values:\n"
  "         human_readable\n"
  "         critical\n"
  "        String values:\n"
  "         name\n"
  "         value\n"
  "        Number values:\n"
  "         flags\n"
  "     tofu\n"
  "      String values:\n"
  "       description\n"
  "      Number values:\n"
  "       validity\n"
  "       policy\n"
  "       signcount\n"
  "       encrcount\n"
  "       signfirst\n"
  "       signlast\n"
  "       encrfirst\n"
  "       encrlast\n"
  "more:   Optional boolean indicating that \"getmore\" is required.\n"
  "        (not implemented)";
static gpg_error_t
op_keylist (cjson_t request, cjson_t result)
{
  gpg_error_t err;
  gpgme_ctx_t ctx = NULL;
  gpgme_protocol_t protocol;
  size_t chunksize;
  char *keystring = NULL;
  int abool;
  gpgme_keylist_mode_t mode = 0;
  gpgme_key_t key = NULL;
  cjson_t keyarray = xjson_CreateArray ();

  if ((err = get_protocol (request, &protocol)))
    goto leave;
  ctx = get_context (protocol);
  if ((err = get_chunksize (request, &chunksize)))
    goto leave;

  /* Handle the various keylist mode bools. */
  if ((err = get_boolean_flag (request, "secret", 0, &abool)))
    goto leave;
  if (abool)
    mode |= GPGME_KEYLIST_MODE_WITH_SECRET;

  if ((err = get_boolean_flag (request, "extern", 0, &abool)))
    goto leave;
  if (abool)
    mode |= GPGME_KEYLIST_MODE_EXTERN;

  if ((err = get_boolean_flag (request, "local", 0, &abool)))
    goto leave;
  if (abool)
    mode |= GPGME_KEYLIST_MODE_LOCAL;

  if ((err = get_boolean_flag (request, "sigs", 0, &abool)))
    goto leave;
  if (abool)
    mode |= GPGME_KEYLIST_MODE_SIGS;

  if ((err = get_boolean_flag (request, "notations", 0, &abool)))
    goto leave;
  if (abool)
    mode |= GPGME_KEYLIST_MODE_SIG_NOTATIONS;

  if ((err = get_boolean_flag (request, "tofu", 0, &abool)))
    goto leave;
  if (abool)
    mode |= GPGME_KEYLIST_MODE_WITH_TOFU;

  if ((err = get_boolean_flag (request, "ephemeral", 0, &abool)))
    goto leave;
  if (abool)
    mode |= GPGME_KEYLIST_MODE_EPHEMERAL;

  if ((err = get_boolean_flag (request, "validate", 0, &abool)))
    goto leave;
  if (abool)
    mode |= GPGME_KEYLIST_MODE_VALIDATE;

  if (!mode)
    {
      /* default to local */
      mode = GPGME_KEYLIST_MODE_LOCAL;
    }

  /* Get the keys.  */
  err = get_keys (request, &keystring);
  if (err)
    {
      /* Provide a custom error response.  */
      gpg_error_object (result, err, "Error getting keys: %s",
                        gpg_strerror (err));
      goto leave;
    }

  /* Do a keylisting and add the keys */
  if ((err = gpgme_new (&ctx)))
    goto leave;
  gpgme_set_protocol (ctx, protocol);
  gpgme_set_keylist_mode (ctx, mode);

  err = gpgme_op_keylist_start (ctx, keystring,
                                (mode & GPGME_KEYLIST_MODE_WITH_SECRET));
  if (err)
    {
      gpg_error_object (result, err, "Error listing keys: %s",
                        gpg_strerror (err));
      goto leave;
    }

  while (!(err = gpgme_op_keylist_next (ctx, &key)))
    {
      cJSON_AddItemToArray (keyarray, key_to_json (key));
      gpgme_key_unref (key);
    }
  err = 0;

  if (!cJSON_AddItemToObject (result, "keys", keyarray))
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }

 leave:
  xfree (keystring);
  if (err)
    {
      cJSON_Delete (keyarray);
    }
  return err;
}

static const char hlp_getmore[] =
  "op:     \"getmore\"\n"
  "\n"
  "Optional parameters:\n"
  "chunksize:  Max number of bytes in the \"data\" object.\n"
  "\n"
  "Response on success:\n"
  "type:       Type of the pending data\n"
  "data:       The next chunk of data\n"
  "base64:     Boolean indicating whether data is base64 encoded\n"
  "more:       Optional boolean requesting another \"getmore\".";
static gpg_error_t
op_getmore (cjson_t request, cjson_t result)
{
  gpg_error_t err;
  int c;
  size_t n;
  size_t chunksize;

  if ((err = get_chunksize (request, &chunksize)))
    goto leave;

  /* Adjust the chunksize if we need to do base64 conversion.  */
  if (pending_data.base64)
    chunksize = (chunksize / 4) * 3;

  /* Do we have anything pending?  */
  if (!pending_data.buffer)
    {
      err = gpg_error (GPG_ERR_NO_DATA);
      gpg_error_object (result, err, "Operation not possible: %s",
                        gpg_strerror (err));
      goto leave;
    }

  xjson_AddStringToObject (result, "type", pending_data.type);
  xjson_AddBoolToObject (result, "base64", pending_data.base64);

  if (pending_data.written >= pending_data.length)
    {
      /* EOF reached.  This should not happen but we return an empty
       * string once in case of client errors.  */
      gpgme_free (pending_data.buffer);
      pending_data.buffer = NULL;
      xjson_AddBoolToObject (result, "more", 0);
      err = cjson_AddStringToObject (result, "data", "");
    }
  else
    {
      n = pending_data.length - pending_data.written;
      if (n > chunksize)
        {
          n = chunksize;
          xjson_AddBoolToObject (result, "more", 1);
        }
      else
        xjson_AddBoolToObject (result, "more", 0);

      c = pending_data.buffer[pending_data.written + n];
      pending_data.buffer[pending_data.written + n] = 0;
      if (pending_data.base64)
        err = add_base64_to_object (result, "data",
                                    (pending_data.buffer
                                     + pending_data.written), n);
      else
        err = cjson_AddStringToObject (result, "data",
                                       (pending_data.buffer
                                        + pending_data.written));
      pending_data.buffer[pending_data.written + n] = c;
      if (!err)
        {
          pending_data.written += n;
          if (pending_data.written >= pending_data.length)
            {
              gpgme_free (pending_data.buffer);
              pending_data.buffer = NULL;
            }
        }
    }

 leave:
  return err;
}



static const char hlp_help[] =
  "The tool expects a JSON object with the request and responds with\n"
  "another JSON object.  Even on error a JSON object is returned.  The\n"
  "property \"op\" is mandatory and its string value selects the\n"
  "operation; if the property \"help\" with the value \"true\" exists, the\n"
  "operation is not performned but a string with the documentation\n"
  "returned.  To list all operations it is allowed to leave out \"op\" in\n"
  "help mode.  Supported values for \"op\" are:\n\n"
  "  encrypt     Encrypt data.\n"
  "  decrypt     Decrypt data.\n"
  "  keylist     List keys.\n"
  "  sign        Sign data.\n"
  "  verify      Verify data.\n"
  "  version     Get engine information.\n"
  "  getmore     Retrieve remaining data.\n"
  "  help        Help overview.";
static gpg_error_t
op_help (cjson_t request, cjson_t result)
{
  cjson_t j_tmp;
  char *buffer = NULL;
  const char *msg;

  j_tmp = cJSON_GetObjectItem (request, "interactive_help");
  if (opt_interactive && j_tmp && cjson_is_string (j_tmp))
    msg = buffer = xstrconcat (hlp_help, "\n", j_tmp->valuestring, NULL);
  else
    msg = hlp_help;

  xjson_AddStringToObject (result, "type", "help");
  xjson_AddStringToObject (result, "msg", msg);

  xfree (buffer);
  return 0;
}



/*
 * Dispatcher
 */

/* Process a request and return the response.  The response is a newly
 * allocated string or NULL in case of an error.  */
static char *
process_request (const char *request)
{
  static struct {
    const char *op;
    gpg_error_t (*handler)(cjson_t request, cjson_t result);
    const char * const helpstr;
  } optbl[] = {
    { "encrypt", op_encrypt, hlp_encrypt },
    { "decrypt", op_decrypt, hlp_decrypt },
    { "keylist", op_keylist, hlp_keylist },
    { "sign",    op_sign,    hlp_sign },
    { "verify",  op_verify,  hlp_verify },
    { "version", op_version, hlp_version },
    { "getmore", op_getmore, hlp_getmore },
    { "help",    op_help,    hlp_help },
    { NULL }
  };
  size_t erroff;
  cjson_t json;
  cjson_t j_tmp, j_op;
  cjson_t response;
  int helpmode;
  const char *op;
  char *res;
  int idx;

  response = xjson_CreateObject ();

  json = cJSON_Parse (request, &erroff);
  if (!json)
    {
      log_string (GPGRT_LOGLVL_INFO, request);
      log_info ("invalid JSON object at offset %zu\n", erroff);
      error_object (response, "invalid JSON object at offset %zu\n", erroff);
      goto leave;
    }

  j_tmp = cJSON_GetObjectItem (json, "help");
  helpmode = (j_tmp && cjson_is_true (j_tmp));

  j_op = cJSON_GetObjectItem (json, "op");
  if (!j_op || !cjson_is_string (j_op))
    {
      if (!helpmode)
        {
          error_object (response, "Property \"op\" missing");
          goto leave;
        }
      op = "help";  /* Help summary.  */
    }
  else
    op = j_op->valuestring;

  for (idx=0; optbl[idx].op; idx++)
    if (!strcmp (op, optbl[idx].op))
      break;
  if (optbl[idx].op)
    {
      if (helpmode && strcmp (op, "help"))
        {
          xjson_AddStringToObject (response, "type", "help");
          xjson_AddStringToObject (response, "op", op);
          xjson_AddStringToObject (response, "msg", optbl[idx].helpstr);
        }
      else
        {
          gpg_error_t err;

          /* If this is not the "getmore" command and we have any
           * pending data release that data.  */
          if (pending_data.buffer && optbl[idx].handler != op_getmore)
            {
              gpgme_free (pending_data.buffer);
              pending_data.buffer = NULL;
            }

          err = optbl[idx].handler (json, response);
          if (err)
            {
              if (!(j_tmp = cJSON_GetObjectItem (response, "type"))
                  || !cjson_is_string (j_tmp)
                  || strcmp (j_tmp->valuestring, "error"))
                {
                  /* No error type response - provide a generic one.  */
                  gpg_error_object (response, err, "Operation failed: %s",
                                    gpg_strerror (err));
                }

              xjson_AddStringToObject (response, "op", op);
            }
        }
    }
  else  /* Operation not supported.  */
    {
      error_object (response, "Unknown operation '%s'", op);
      xjson_AddStringToObject (response, "op", op);
    }

 leave:
  cJSON_Delete (json);
  if (opt_interactive)
    res = cJSON_Print (response);
  else
    res = cJSON_PrintUnformatted (response);
  if (!res)
    log_error ("Printing JSON data failed\n");
  cJSON_Delete (response);
  return res;
}



/*
 *  Driver code
 */

static char *
get_file (const char *fname)
{
  gpg_error_t err;
  estream_t fp;
  struct stat st;
  char *buf;
  size_t buflen;

  fp = es_fopen (fname, "r");
  if (!fp)
    {
      err = gpg_error_from_syserror ();
      log_error ("can't open '%s': %s\n", fname, gpg_strerror (err));
      return NULL;
    }

  if (fstat (es_fileno(fp), &st))
    {
      err = gpg_error_from_syserror ();
      log_error ("can't stat '%s': %s\n", fname, gpg_strerror (err));
      es_fclose (fp);
      return NULL;
    }

  buflen = st.st_size;
  buf = xmalloc (buflen+1);
  if (es_fread (buf, buflen, 1, fp) != 1)
    {
      err = gpg_error_from_syserror ();
      log_error ("error reading '%s': %s\n", fname, gpg_strerror (err));
      es_fclose (fp);
      xfree (buf);
      return NULL;
    }
  buf[buflen] = 0;
  es_fclose (fp);

  return buf;
}


/* Return a malloced line or NULL on EOF.  Terminate on read
 * error.  */
static char *
get_line (void)
{
  char *line = NULL;
  size_t linesize = 0;
  gpg_error_t err;
  size_t maxlength = 2048;
  int n;
  const char *s;
  char *p;

 again:
  n = es_read_line (es_stdin, &line, &linesize, &maxlength);
  if (n < 0)
    {
      err = gpg_error_from_syserror ();
      log_error ("error reading line: %s\n", gpg_strerror (err));
      exit (1);
    }
  if (!n)
    {
      xfree (line);
      line = NULL;
      return NULL;  /* EOF */
    }
  if (!maxlength)
    {
      log_info ("line too long - skipped\n");
      goto again;
    }
  if (memchr (line, 0, n))
    log_info ("warning: line shortened due to embedded Nul character\n");

  if (line[n-1] == '\n')
    line[n-1] = 0;

  /* Trim leading spaces.  */
  for (s=line; spacep (s); s++)
    ;
  if (s != line)
    {
      for (p=line; *s;)
        *p++ = *s++;
      *p = 0;
      n = p - line;
    }

  return line;
}


/* Process meta commands used with the standard REPL.  */
static char *
process_meta_commands (const char *request)
{
  char *result = NULL;

  while (spacep (request))
    request++;

  if (!strncmp (request, "help", 4) && (spacep (request+4) || !request[4]))
    {
      if (request[4])
        {
          char *buf = xstrconcat ("{ \"help\":true, \"op\":\"", request+5,
                                  "\" }", NULL);
          result = process_request (buf);
          xfree (buf);
        }
      else
        result = process_request ("{ \"op\": \"help\","
                                  " \"interactive_help\": "
                                  "\"\\nMeta commands:\\n"
                                  "  ,read FNAME Process data from FILE\\n"
                                  "  ,help CMD   Print help for a command\\n"
                                  "  ,quit       Terminate process\""
                                  "}");
    }
  else if (!strncmp (request, "quit", 4) && (spacep (request+4) || !request[4]))
    exit (0);
  else if (!strncmp (request, "read", 4) && (spacep (request+4) || !request[4]))
    {
      if (!request[4])
        log_info ("usage: ,read FILENAME\n");
      else
        {
          char *buffer = get_file (request + 5);
          if (buffer)
            {
              result = process_request (buffer);
              xfree (buffer);
            }
        }
    }
  else
    log_info ("invalid meta command\n");

  return result;
}


/* If STRING has a help response, return the MSG property in a human
 * readable format.  */
static char *
get_help_msg (const char *string)
{
  cjson_t json, j_type, j_msg;
  const char *msg;
  char *buffer = NULL;
  char *p;

  json = cJSON_Parse (string, NULL);
  if (json)
    {
      j_type = cJSON_GetObjectItem (json, "type");
      if (j_type && cjson_is_string (j_type)
          && !strcmp (j_type->valuestring, "help"))
        {
          j_msg = cJSON_GetObjectItem (json, "msg");
          if (j_msg || cjson_is_string (j_msg))
            {
              msg = j_msg->valuestring;
              buffer = malloc (strlen (msg)+1);
              if (buffer)
                {
                  for (p=buffer; *msg; msg++)
                    {
                      if (*msg == '\\' && msg[1] == '\n')
                        *p++ = '\n';
                      else
                        *p++ = *msg;
                    }
                  *p = 0;
                }
            }
        }
      cJSON_Delete (json);
    }
  return buffer;
}


/* An interactive standard REPL.  */
static void
interactive_repl (void)
{
  char *line = NULL;
  char *request = NULL;
  char *response = NULL;
  char *p;
  int first;

  es_setvbuf (es_stdin, NULL, _IONBF, 0);
#if GPGRT_VERSION_NUMBER >= 0x011d00 /* 1.29 */
  es_fprintf (es_stderr, "%s %s ready (enter \",help\" for help)\n",
              gpgrt_strusage (11), gpgrt_strusage (13));
#endif
  do
    {
      es_fputs ("> ", es_stderr);
      es_fflush (es_stderr);
      es_fflush (es_stdout);
      xfree (line);
      line = get_line ();
      es_fflush (es_stderr);
      es_fflush (es_stdout);

      first = !request;
      if (line && *line)
        {
          if (!request)
            request = xstrdup (line);
          else
            request = xstrconcat (request, "\n", line, NULL);
        }

      if (!line)
        es_fputs ("\n", es_stderr);

      if (!line || !*line || (first && *request == ','))
        {
          /* Process the input.  */
          xfree (response);
          response = NULL;
          if (request && *request == ',')
            {
              response = process_meta_commands (request+1);
            }
          else if (request)
            {
              response = process_request (request);
            }
          xfree (request);
          request = NULL;

          if (response)
            {
              if (opt_interactive)
                {
                  char *msg = get_help_msg (response);
                  if (msg)
                    {
                      xfree (response);
                      response = msg;
                    }
                }

              es_fputs ("===> ", es_stderr);
              es_fflush (es_stderr);
              for (p=response; *p; p++)
                {
                  if (*p == '\n')
                    {
                      es_fflush (es_stdout);
                      es_fputs ("\n===> ", es_stderr);
                      es_fflush (es_stderr);
                    }
                  else
                    es_putc (*p, es_stdout);
                }
              es_fflush (es_stdout);
              es_fputs ("\n", es_stderr);
            }
        }
    }
  while (line);

  xfree (request);
  xfree (response);
  xfree (line);
}


/* Read and process a single request.  */
static void
read_and_process_single_request (void)
{
  char *line = NULL;
  char *request = NULL;
  char *response = NULL;
  size_t n;

  for (;;)
    {
      xfree (line);
      line = get_line ();
      if (line && *line)
        request = (request? xstrconcat (request, "\n", line, NULL)
                   /**/   : xstrdup (line));
      if (!line)
        {
          if (request)
            {
              xfree (response);
              response = process_request (request);
              if (response)
                {
                  es_fputs (response, es_stdout);
                  if ((n = strlen (response)) && response[n-1] != '\n')
                    es_fputc ('\n', es_stdout);
                }
              es_fflush (es_stdout);
            }
          break;
        }
    }

  xfree (response);
  xfree (request);
  xfree (line);
}


/* The Native Messaging processing loop.  */
static void
native_messaging_repl (void)
{
  gpg_error_t err;
  uint32_t nrequest, nresponse;
  char *request = NULL;
  char *response = NULL;
  size_t n;

  /* Due to the length octets we need to switch the I/O stream into
   * binary mode.  */
  es_set_binary (es_stdin);
  es_set_binary (es_stdout);
  es_setbuf (es_stdin, NULL);  /* stdin needs to be unbuffered! */

  for (;;)
    {
      /* Read length.  Note that the protocol uses native endianess.
       * Is it allowed to call such a thing a well thought out
       * protocol?  */
      if (es_read (es_stdin, &nrequest, sizeof nrequest, &n))
        {
          err = gpg_error_from_syserror ();
          log_error ("error reading request header: %s\n", gpg_strerror (err));
          break;
        }
      if (!n)
        break;  /* EOF */
      if (n != sizeof nrequest)
        {
          log_error ("error reading request header: short read\n");
          break;
        }
      if (nrequest > MAX_REQUEST_SIZE)
        {
          log_error ("error reading request: request too long (%zu MiB)\n",
                     (size_t)nrequest / (1024*1024));
          /* Fixme: Shall we read the request to the bit bucket and
           * return an error reponse or just return an error reponse
           * and terminate?  Needs some testing.  */
          break;
        }

      /* Read request.  */
      request = xtrymalloc (nrequest);
      if (!request)
        {
          err = gpg_error_from_syserror ();
          log_error ("error reading request: Not enough memory for %zu MiB)\n",
                     (size_t)nrequest / (1024*1024));
          /* FIXME: See comment above.  */
          break;
        }
      if (es_read (es_stdin, request, nrequest, &n))
        {
          err = gpg_error_from_syserror ();
          log_error ("error reading request: %s\n", gpg_strerror (err));
          break;
        }
      if (n != nrequest)
        {
          /* That is a protocol violation.  */
          xfree (response);
          response = error_object_string ("Invalid request:"
                                          " short read (%zu of %zu bytes)\n",
                                          n, (size_t)nrequest);
        }
      else /* Process request  */
        {
          if (opt_debug)
            log_debug ("request='%s'\n", request);
          xfree (response);
          response = process_request (request);
          if (opt_debug)
            log_debug ("response='%s'\n", response);
        }
      nresponse = strlen (response);

      /* Write response */
      if (es_write (es_stdout, &nresponse, sizeof nresponse, &n))
        {
          err = gpg_error_from_syserror ();
          log_error ("error writing request header: %s\n", gpg_strerror (err));
          break;
        }
      if (n != sizeof nrequest)
        {
          log_error ("error writing request header: short write\n");
          break;
        }
      if (es_write (es_stdout, response, nresponse, &n))
        {
          err = gpg_error_from_syserror ();
          log_error ("error writing request: %s\n", gpg_strerror (err));
          break;
        }
      if (n != nresponse)
        {
          log_error ("error writing request: short write\n");
          break;
        }
      if (es_fflush (es_stdout) || es_ferror (es_stdout))
        {
          err = gpg_error_from_syserror ();
          log_error ("error writing request: %s\n", gpg_strerror (err));
          break;
        }
    }

  xfree (response);
  xfree (request);
}



static const char *
my_strusage( int level )
{
  const char *p;

  switch (level)
    {
    case  9: p = "LGPL-2.1-or-later"; break;
    case 11: p = "gpgme-json"; break;
    case 13: p = PACKAGE_VERSION; break;
    case 14: p = "Copyright (C) 2018 g10 Code GmbH"; break;
    case 19: p = "Please report bugs to <" PACKAGE_BUGREPORT ">.\n"; break;
    case 1:
    case 40:
      p = "Usage: gpgme-json [OPTIONS]";
      break;
    case 41:
      p = "Native messaging based GPGME operations.\n";
      break;
    case 42:
      p = "1"; /* Flag print 40 as part of 41. */
      break;
    default: p = NULL; break;
    }
  return p;
}

int
main (int argc, char *argv[])
{
#if GPGRT_VERSION_NUMBER < 0x011d00 /* 1.29 */

  fprintf (stderr, "WARNING: Old libgpg-error - using limited mode\n");
  native_messaging_repl ();

#else /* This is a modern libgp-error.  */

  enum { CMD_DEFAULT     = 0,
         CMD_INTERACTIVE = 'i',
         CMD_SINGLE      = 's',
         CMD_LIBVERSION  = 501,
  } cmd = CMD_DEFAULT;
  enum {
    OPT_DEBUG = 600
  };

  static gpgrt_opt_t opts[] = {
    ARGPARSE_c  (CMD_INTERACTIVE, "interactive", "Interactive REPL"),
    ARGPARSE_c  (CMD_SINGLE,      "single",      "Single request mode"),
    ARGPARSE_c  (CMD_LIBVERSION,  "lib-version", "Show library version"),
    ARGPARSE_s_n(OPT_DEBUG,       "debug",       "Flyswatter"),

    ARGPARSE_end()
  };
  gpgrt_argparse_t pargs = { &argc, &argv};

  gpgrt_set_strusage (my_strusage);

#ifdef HAVE_SETLOCALE
  setlocale (LC_ALL, "");
#endif
  gpgme_check_version (NULL);
#ifdef LC_CTYPE
  gpgme_set_locale (NULL, LC_CTYPE, setlocale (LC_CTYPE, NULL));
#endif
#ifdef LC_MESSAGES
  gpgme_set_locale (NULL, LC_MESSAGES, setlocale (LC_MESSAGES, NULL));
#endif

  while (gpgrt_argparse (NULL, &pargs, opts))
    {
      switch (pargs.r_opt)
        {
        case CMD_INTERACTIVE:
          opt_interactive = 1;
          /* Fall trough.  */
        case CMD_SINGLE:
        case CMD_LIBVERSION:
          cmd = pargs.r_opt;
          break;

        case OPT_DEBUG: opt_debug = 1; break;

        default:
          pargs.err = ARGPARSE_PRINT_WARNING;
	  break;
        }
    }
  gpgrt_argparse (NULL, &pargs, NULL);

  if (!opt_debug)
    {
      const char *s = getenv ("GPGME_JSON_DEBUG");
      if (s && atoi (s) > 0)
        opt_debug = 1;
    }

  if (opt_debug)
    {
      const char *home = getenv ("HOME");
      char *file = xstrconcat ("socket://",
                               home? home:"/tmp",
                               "/.gnupg/S.gpgme-json.log", NULL);
      log_set_file (file);
      xfree (file);
    }

  if (opt_debug)
    { int i;
      for (i=0; argv[i]; i++)
        log_debug ("argv[%d]='%s'\n", i, argv[i]);
    }

  switch (cmd)
    {
    case CMD_DEFAULT:
      native_messaging_repl ();
      break;

    case CMD_SINGLE:
      read_and_process_single_request ();
      break;

    case CMD_INTERACTIVE:
      interactive_repl ();
      break;

    case CMD_LIBVERSION:
      printf ("Version from header: %s (0x%06x)\n",
              GPGME_VERSION, GPGME_VERSION_NUMBER);
      printf ("Version from binary: %s\n", gpgme_check_version (NULL));
      printf ("Copyright blurb ...:%s\n", gpgme_check_version ("\x01\x01"));
      break;
    }

  if (opt_debug)
    log_debug ("ready");

#endif /* This is a modern libgp-error.  */
  return 0;
}
#endif /* libgpg-error >= 1.28 */

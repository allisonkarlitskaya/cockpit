/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013-2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpitwebserver.h"

#include "cockpithash.h"
#include "cockpitmemfdread.h"
#include "cockpitmemory.h"
#include "cockpitsocket.h"
#include "cockpitwebresponse.h"

#include "websocket/websocket.h"

#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct _CockpitWebRequest {
  int state;
  GIOStream *io;
  GByteArray *buffer;
  gint delayed_reply;
  CockpitWebServer *web_server;
  gboolean eof_okay;
  GSource *source;
  GSource *timeout;

  GHashTable *headers;
  const gchar *protocol;
  const gchar *host;

  gchar *original_path;
  const gchar *path;
  const gchar *query;

  gchar *method;
  gchar *origin;
};

G_DEFINE_BOXED_TYPE(CockpitWebRequest, cockpit_web_request, NULL, NULL);


/* Used during testing */
gboolean cockpit_webserver_want_certificate = FALSE;

guint cockpit_webserver_request_timeout = 30;
const gsize cockpit_webserver_request_maximum = 8192;

struct _CockpitWebServer {
  GObject parent_instance;

  GString *url_root;
  gchar *forwarded_host_header;
  gchar *forwarded_protocol_header;
  gchar *forwarded_for_header;
  gint request_timeout;
  gint request_max;

  GSocketService *socket_service;
  GMainContext *main_context;
  GHashTable *requests;
};

static gint sig_handle_stream = 0;
static gint sig_handle_resource = 0;

static void cockpit_web_request_free (gpointer data);

static void cockpit_web_request_start (CockpitWebServer *self,
                                   GIOStream *stream);

static gboolean on_incoming (GSocketService *service,
                             GSocketConnection *connection,
                             GObject *source_object,
                             gpointer user_data);

G_DEFINE_TYPE (CockpitWebServer, cockpit_web_server, G_TYPE_OBJECT)

/* ---------------------------------------------------------------------------------------------------- */

static void
cockpit_web_server_init (CockpitWebServer *server)
{
  server->requests = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                            cockpit_web_request_free, NULL);
  server->main_context = g_main_context_ref_thread_default ();
  server->url_root = g_string_new ("");

  server->socket_service = g_socket_service_new ();

  /* The web server has to be explicitly started */
  g_socket_service_stop (server->socket_service);

  g_signal_connect (server->socket_service, "incoming",
                    G_CALLBACK (on_incoming), server);
}

static void
cockpit_web_server_dispose (GObject *object)
{
  CockpitWebServer *self = COCKPIT_WEB_SERVER (object);

  g_hash_table_remove_all (self->requests);

  G_OBJECT_CLASS (cockpit_web_server_parent_class)->dispose (object);
}

static void
cockpit_web_server_finalize (GObject *object)
{
  CockpitWebServer *server = COCKPIT_WEB_SERVER (object);

  g_hash_table_destroy (server->requests);
  if (server->main_context)
    g_main_context_unref (server->main_context);
  g_string_free (server->url_root, TRUE);
  g_free (server->forwarded_host_header);
  g_free (server->forwarded_protocol_header);
  g_free (server->forwarded_for_header);
  g_clear_object (&server->socket_service);

  G_OBJECT_CLASS (cockpit_web_server_parent_class)->finalize (object);
}

static void
on_io_closed (GObject *stream,
              GAsyncResult *result,
              gpointer user_data)
{
  GError *error = NULL;

  if (!g_io_stream_close_finish (G_IO_STREAM (stream), result, &error))
    {
      if (!cockpit_web_should_suppress_output_error ("http", error))
        g_message ("http close error: %s", error->message);
      g_error_free (error);
    }
}

static void
close_io_stream (GIOStream *io)
{
  g_io_stream_close_async (io, G_PRIORITY_DEFAULT, NULL, on_io_closed, NULL);
}

static void
on_web_response_done (CockpitWebResponse *response,
                      gboolean reusable,
                      gpointer user_data)
{
  CockpitWebServer *self = user_data;
  GIOStream *io;

  io = cockpit_web_response_get_stream (response);
  if (reusable)
    cockpit_web_request_start (self, io);
  else
    close_io_stream (io);
}

static gboolean
cockpit_web_server_default_handle_resource (CockpitWebServer *self,
                                            const gchar *path,
                                            GHashTable *headers,
                                            CockpitWebResponse *response)
{
  cockpit_web_response_error (response, 404, NULL, NULL);
  return TRUE;
}

static gboolean
cockpit_web_server_default_handle_stream (CockpitWebServer *self,
                                          CockpitWebRequest *request)
{
  CockpitWebResponse *response;
  gboolean claimed = FALSE;
  GQuark detail;
  gchar *pos;
  gchar *orig_pos;
  gchar bak;

  /* Yes, we happen to know that we can modify this string safely. */
  pos = strchr (request->path, '?');
  if (pos != NULL)
    {
      *pos = '\0';
      pos++;
    }

  /* We also have to strip original_path so that CockpitWebResponse
     can rediscover url_root. */
  orig_pos = strchr (request->original_path, '?');
  if (orig_pos != NULL)
    *orig_pos = '\0';

  request->query = pos;

  /* TODO: Correct HTTP version for response */
  response = cockpit_web_request_respond (request);
  g_signal_connect_data (response, "done", G_CALLBACK (on_web_response_done),
                         g_object_ref (self), (GClosureNotify)g_object_unref, 0);

  /*
   * If the path has more than one component, then we search
   * for handlers registered under the detail like this:
   *
   *   /component/
   *
   * Otherwise we search for handlers registered under detail
   * of the entire path:
   *
   *  /component
   */

  /* Temporarily null terminate string after first component */
  pos = NULL;
  if (request->path[0] != '\0')
    {
      pos = strchr (request->path + 1, '/');
      if (pos != NULL)
        {
          pos++;
          bak = *pos;
          *pos = '\0';
        }
    }
  detail = g_quark_try_string (request->path);
  if (pos != NULL)
    *pos = bak;

  /* See if we have any takers... */
  g_signal_emit (self,
                 sig_handle_resource, detail,
                 request->path,
                 request->headers,
                 response,
                 &claimed);

  if (!claimed)
    claimed = cockpit_web_server_default_handle_resource (self, request->path, request->headers, response);

  /* TODO: Here is where we would plug keep-alive into response */
  g_object_unref (response);

  return claimed;
}

static void
cockpit_web_server_class_init (CockpitWebServerClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = cockpit_web_server_dispose;
  gobject_class->finalize = cockpit_web_server_finalize;

  sig_handle_stream = g_signal_new ("handle-stream",
                                    G_OBJECT_CLASS_TYPE (klass),
                                    G_SIGNAL_RUN_LAST,
                                    0, /* class offset */
                                    g_signal_accumulator_true_handled,
                                    NULL, /* accu_data */
                                    g_cclosure_marshal_generic,
                                    G_TYPE_BOOLEAN,
                                    1,
                                    G_TYPE_POINTER);
                                    // XXX COCKPIT_TYPE_WEB_REQUEST | G_SIGNAL_TYPE_STATIC_SCOPE);

  sig_handle_resource = g_signal_new ("handle-resource",
                                      G_OBJECT_CLASS_TYPE (klass),
                                      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                                      0, /* class offset */
                                      g_signal_accumulator_true_handled,
                                      NULL, /* accu_data */
                                      g_cclosure_marshal_generic,
                                      G_TYPE_BOOLEAN,
                                      3,
                                      G_TYPE_STRING,
                                      G_TYPE_HASH_TABLE,
                                      COCKPIT_TYPE_WEB_RESPONSE);
}

CockpitWebServer *
cockpit_web_server_new (void)
{
  return g_object_new (COCKPIT_TYPE_WEB_SERVER, NULL);
}

/* ---------------------------------------------------------------------------------------------------- */

GHashTable *
cockpit_web_server_new_table (void)
{
  return g_hash_table_new_full (cockpit_str_case_hash, cockpit_str_case_equal, g_free, g_free);
}

gchar *
cockpit_web_server_parse_cookie (GHashTable *headers,
                                 const gchar *name)
{
  const gchar *header;
  const gchar *pos;
  const gchar *value;
  const gchar *end;
  gboolean at_start = TRUE;
  gchar *decoded;
  gint diff;
  gint offset;

  header = g_hash_table_lookup (headers, "Cookie");
  if (!header)
    return NULL;

  for (;;)
    {
      pos = strstr (header, name);
      if (!pos)
        return NULL;

      if (pos != header)
        {
          diff = strlen (header) - strlen (pos);
          offset = 1;
          at_start = FALSE;
          while (offset < diff)
            {
              if (!g_ascii_isspace (*(pos - offset)))
                {
                  at_start = *(pos - offset) == ';';
                  break;
                }
              offset++;
            }
        }

      pos += strlen (name);
      if (*pos == '=' && at_start)
        {
          value = pos + 1;
          end = strchr (value, ';');
          if (end == NULL)
            end = value + strlen (value);

          decoded = g_uri_unescape_segment (value, end, NULL);
          if (!decoded)
            g_debug ("invalid cookie encoding");

          return decoded;
        }
      else
        {
          at_start = FALSE;
        }
      header = pos;
    }
}

typedef struct {
  double qvalue;
  const gchar *value;
} Language;

static gint
sort_qvalue (gconstpointer a,
             gconstpointer b)
{
  const Language *la = *((Language **)a);
  const Language *lb = *((Language **)b);
  if (lb->qvalue == la->qvalue)
    return 0;
  return lb->qvalue < la->qvalue ? -1 : 1;
}

gchar **
cockpit_web_server_parse_accept_list (const gchar *accept,
                                      const gchar *defawlt)
{
  Language *lang;
  GPtrArray *langs;
  GPtrArray *ret;
  gchar *copy;
  gchar *value;
  gchar *next;
  gchar *pos;
  guint i;

  langs = g_ptr_array_new_with_free_func (g_free);

  if (defawlt)
    {
      lang = g_new0 (Language, 1);
      lang->qvalue = 0.1;
      lang->value = defawlt;
      g_ptr_array_add (langs, lang);
    }

  /* First build up an array we can sort */
  accept = copy = g_strdup (accept);

  while (accept)
    {
      next = strchr (accept, ',');
      if (next)
        {
          *next = '\0';
          next++;
        }

      lang = g_new0 (Language, 1);
      lang->qvalue = 1;

      pos = strchr (accept, ';');
      if (pos)
        {
          *pos = '\0';
          if (strncmp (pos + 1, "q=", 2) == 0)
            {
              lang->qvalue = g_ascii_strtod (pos + 3, NULL);
              if (lang->qvalue < 0)
                lang->qvalue = 0;
            }
        }

      lang->value = accept;
      g_ptr_array_add (langs, lang);
      accept = next;
    }

  g_ptr_array_sort (langs, sort_qvalue);

  /* Now in the right order add all the prefs */
  ret = g_ptr_array_new ();
  for (i = 0; i < langs->len; i++)
    {
      lang = langs->pdata[i];
      if (lang->qvalue > 0)
        {
          value = g_strstrip (g_ascii_strdown (lang->value, -1));
          g_ptr_array_add (ret, value);
        }
    }

  /* Add base languages after that */
  for (i = 0; i < langs->len; i++)
    {
      lang = langs->pdata[i];
      if (lang->qvalue > 0)
        {
          pos = strchr (lang->value, '-');
          if (pos)
            {
              value = g_strstrip (g_ascii_strdown (lang->value, pos - lang->value));
              g_ptr_array_add (ret, value);
            }
        }
    }

  g_free (copy);
  g_ptr_array_add (ret, NULL);
  g_ptr_array_free (langs, TRUE);
  return (gchar **)g_ptr_array_free (ret, FALSE);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
cockpit_web_request_free (gpointer data)
{
  CockpitWebRequest *request = data;
  if (request->timeout)
    {
      g_source_destroy (request->timeout);
      g_source_unref (request->timeout);
    }
  if (request->source)
    {
      g_source_destroy (request->source);
      g_source_unref (request->source);
    }

  if (request->headers)
    g_hash_table_unref (request->headers);

  g_free (request->origin);
  g_free (request->original_path);
  g_free (request->method);

  /*
   * Request memory is either cleared or used elsewhere, by
   * handle-stream handlers (eg: the default handler. Don't
   * clear it here. The buffer may still be in use.
   */
  g_byte_array_unref (request->buffer);
  g_object_unref (request->io);
  g_free (request);
}

static void
cockpit_web_request_finish (CockpitWebRequest *request)
{
  g_hash_table_remove (request->web_server->requests, request);
}

static void
process_delayed_reply (CockpitWebRequest *request)
{
  CockpitWebResponse *response;

  g_assert (request->delayed_reply > 299);

  response = cockpit_web_request_respond (request);
  g_signal_connect_data (response, "done", G_CALLBACK (on_web_response_done),
                         g_object_ref (request->web_server), (GClosureNotify)g_object_unref, 0);

  cockpit_web_response_error (response, request->delayed_reply, NULL, NULL);
  g_object_unref (response);
}

static gboolean
path_has_prefix (const gchar *path,
                 GString *prefix)
{
  return prefix->len > 0 &&
         strncmp (path, prefix->str, prefix->len) == 0 &&
         (path[prefix->len] == '\0' || path[prefix->len] == '/');
}

static void
process_request (CockpitWebRequest *request)
{
  gboolean claimed = FALSE;

  if (request->web_server->url_root->len &&
      !path_has_prefix (request->original_path, request->web_server->url_root))
    {
      request->delayed_reply = 404;
    }

  if (request->delayed_reply)
    {
      process_delayed_reply (request);
      return;
    }

  request->path = request->original_path + request->web_server->url_root->len;

  /* See if we have any takers... */
  g_signal_emit (request->web_server, sig_handle_stream, 0, request, &claimed);

  if (!claimed)
    claimed = cockpit_web_server_default_handle_stream (request->web_server, request);

  if (!claimed)
    g_critical ("no handler responded to request: %s", request->path);
}

static gboolean
parse_and_process_request (CockpitWebRequest *request)
{
  gboolean again = FALSE;
  const gchar *str;
  gchar *end = NULL;
  gssize off1;
  gssize off2;
  guint64 length;

  /* The hard input limit, we just terminate the connection */
  if (request->buffer->len > cockpit_webserver_request_maximum * 2)
    {
      g_message ("received HTTP request that was too large");
      goto out;
    }

  off1 = web_socket_util_parse_req_line ((const gchar *)request->buffer->data,
                                         request->buffer->len,
                                         &request->method,
                                         &request->original_path);
  if (off1 == 0)
    {
      again = TRUE;
      goto out;
    }
  if (off1 < 0)
    {
      g_message ("received invalid HTTP request line");
      request->delayed_reply = 400;
      goto out;
    }
  if (!request->original_path || request->original_path[0] != '/')
    {
      g_message ("received invalid HTTP path");
      request->delayed_reply = 400;
      goto out;
    }

  off2 = web_socket_util_parse_headers ((const gchar *)request->buffer->data + off1,
                                        request->buffer->len - off1,
                                        &request->headers);
  if (off2 == 0)
    {
      again = TRUE;
      goto out;
    }
  if (off2 < 0)
    {
      g_message ("received invalid HTTP request headers");
      request->delayed_reply = 400;
      goto out;
    }

  /* If we get a Content-Length then verify it is zero */
  length = 0;
  str = g_hash_table_lookup (request->headers, "Content-Length");
  if (str != NULL)
    {
      end = NULL;
      length = g_ascii_strtoull (str, &end, 10);
      if (!end || end[0])
        {
          g_message ("received invalid Content-Length");
          request->delayed_reply = 400;
          goto out;
        }

      /* The soft limit, we return 413 */
      if (length != 0)
        {
          g_debug ("received non-zero Content-Length");
          request->delayed_reply = 413;
        }
    }

  /* Not enough data yet */
  if (request->buffer->len < off1 + off2 + length)
    {
      again = TRUE;
      goto out;
    }

  if (!g_str_equal (request->method, "GET") && !g_str_equal (request->method, "HEAD"))
    {
      g_message ("received unsupported HTTP method");
      request->delayed_reply = 405;
    }

  request->host = g_hash_table_lookup (request->headers, "Host");
  if (!request->host || g_str_equal (request->host, ""))
    {
      g_message ("received HTTP request without Host header");
      request->delayed_reply = 400;
    }

  g_byte_array_remove_range (request->buffer, 0, off1 + off2);

#if 0
  if (request->metadata)
    {
      /* cockpit-tls always sends these */
      g_object_set_data (G_OBJECT (io), "protocol",
                         json_object_get_boolean_member (request->metdata, "tls") ? "https" : "http");

      g_object_set_data_full (G_OBJECT (io), "origin-ip",
                              g_strdup (json_object_get_string_member (request->metadata, "origin-ip")),
                              g_free);

      /* ...and sometimes this */
      const gchar *value;
      cockpit_json_get_string (request->metadata, "client-certificate", NULL, &value);
      if (value)
        g_object_set_data_full (G_OBJECT (io), "client-certificate", g_strdup (value), g_free);
    }
  else
    {
      const gchar *value = "http";
      if (self->forwarded_protocol_header)
        {
          const *sent_value = g_hash_table_lookup (header, self->forwarded_protocol_header);
          if (sent_value && g_str_equal (sent_value, "https"))
            value = "https"; /* use our own static string */
        }
      g_object_set_data (G_OBJECT (io), "protocol", value);

      if (self->forwarded_for_header)
        {

        }
    }
#endif

  request->protocol = "http";
  request->origin = g_strdup_printf ("%s://%s", request->protocol, request->host);

  process_request (request);

out:
  if (!again)
    cockpit_web_request_finish (request);
  return again;
}

#if !GLIB_CHECK_VERSION(2,43,2)
#define G_IO_ERROR_CONNECTION_CLOSED G_IO_ERROR_BROKEN_PIPE
#endif

static gboolean
should_suppress_request_error (GError *error,
                               gsize received)
{
  /* If no bytes received, then don't worry about ECONNRESET and friends */
  if (received > 0)
    return FALSE;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
    {
      g_debug ("request error: %s", error->message);
      return TRUE;
    }

#if !GLIB_CHECK_VERSION(2,43,2)
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED) &&
      strstr (error->message, g_strerror (ECONNRESET)))
    {
      g_debug ("request error: %s", error->message);
      return TRUE;
    }
#endif

  return FALSE;
}

static gboolean
on_socket_input (GSocket *socket,
                 GIOCondition condition,
                 gpointer user_data)
{
  CockpitWebRequest *request = user_data;
  g_auto(CockpitControlMessages) ccm;
  GError *error = NULL;
  gsize length;
  gssize num_read;

  length = request->buffer->len;

  g_byte_array_set_size (request->buffer, length + cockpit_webserver_request_maximum + 1);

  num_read = g_socket_receive_message (socket,
                                       NULL, /* out GSocketAddress */
                                       (GInputVector[]) { {
                                         request->buffer->data + length,
                                         cockpit_webserver_request_maximum + 1
                                       } },
                                       1,
                                       &ccm.messages,
                                       &ccm.n_messages,
                                       NULL,
                                       NULL, /* GCancellable* */
                                       &error);

  if (num_read < 0)
    {
      /* Just wait and try again */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
        {
          g_error_free (error);
          return TRUE;
        }

      if (!should_suppress_request_error (error, 0))
        g_message ("couldn't read from socket: %s", error->message);

      cockpit_web_request_finish (request);
      g_error_free (error);
      return FALSE;
    }

  JsonObject *metadata = cockpit_memfd_read_json_from_control_messages (&ccm, &error);
  if (metadata != NULL)
    {
      g_object_set_qdata_full (G_OBJECT (request->io),
                               g_quark_from_static_string ("metadata"),
                               metadata, (GDestroyNotify) json_object_unref);
    }
  else if (error != NULL)
    {
      g_warning ("Failed while reading metadata from new connection: %s", error->message);
      g_clear_error (&error);
    }

  g_byte_array_set_size (request->buffer, length + num_read);

  if (num_read == 0)
    {
      if (request->eof_okay)
        close_io_stream (request->io);
      else
        g_debug ("caller closed connection early");
      cockpit_web_request_finish (request);
      return FALSE;
    }

  /* Once we receive data EOF is unexpected (until possible next request) */
  request->eof_okay = FALSE;

  return parse_and_process_request (request);
}

static gboolean
on_request_timeout (gpointer data)
{
  CockpitWebRequest *request = data;
  if (request->eof_okay)
    g_debug ("request timed out, closing");
  else
    g_message ("request timed out, closing");
  cockpit_web_request_finish (request);
  return FALSE;
}

static void
cockpit_web_request_start (CockpitWebServer *self,
                       GIOStream *io)
{
  GSocketConnection *connection;
  CockpitWebRequest *request;
  GSocket *socket;

  request = g_new0 (CockpitWebRequest, 1);
  request->web_server = self;
  request->io = g_object_ref (io);
  request->buffer = g_byte_array_new ();

  /* Right before a request, EOF is not unexpected */
  request->eof_okay = TRUE;

  request->timeout = g_timeout_source_new_seconds (cockpit_webserver_request_timeout);
  g_source_set_callback (request->timeout, on_request_timeout, request, NULL);
  g_source_attach (request->timeout, self->main_context);

  connection = G_SOCKET_CONNECTION (io);
  socket = g_socket_connection_get_socket (connection);
  g_socket_set_blocking (socket, FALSE);

  request->source = g_socket_create_source (g_socket_connection_get_socket (connection),
                                            G_IO_IN, NULL);
  g_source_set_callback (request->source, (GSourceFunc) on_socket_input, request, NULL);
  g_source_attach (request->source, self->main_context);

  /* Owns the request */
  g_hash_table_add (self->requests, request);
}

static gboolean
on_incoming (GSocketService *service,
             GSocketConnection *connection,
             GObject *source_object,
             gpointer user_data)
{
  CockpitWebServer *self = COCKPIT_WEB_SERVER (user_data);
  cockpit_web_request_start (self, G_IO_STREAM (connection));

  /* handled */
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

const gchar *
cockpit_web_request_lookup_header (CockpitWebRequest *self,
                                   const gchar *header)
{
  return g_hash_table_lookup (self->headers, header);
}

gchar *
cockpit_web_request_parse_cookie (CockpitWebRequest *self,
                                  const gchar *name)
{
  return cockpit_web_server_parse_cookie (self->headers, name);
}

const gchar *
cockpit_web_request_get_path (CockpitWebRequest *self)
{
  return self->path;
}

const gchar *
cockpit_web_request_get_method (CockpitWebRequest *self)
{
  return self->method;
}

const gchar *
cockpit_web_request_get_origin (CockpitWebRequest *self)
{
  return self->origin;
}

GByteArray *
cockpit_web_request_get_buffer (CockpitWebRequest *self)
{
  return self->buffer;
}

GHashTable *
cockpit_web_request_get_headers (CockpitWebRequest *self)
{
  return self->headers;
}

GIOStream *
cockpit_web_request_get_io_stream (CockpitWebRequest *self)
{
  return self->io;
}

CockpitWebResponse *
cockpit_web_request_respond (CockpitWebRequest *self)
{
  CockpitWebResponse *response;

  response = cockpit_web_response_new (self->io, self->original_path, self->path, self->query, self->headers);
  cockpit_web_response_set_method (response, self->method);
  cockpit_web_response_set_origin (response, self->origin);

  return response;
}

void
cockpit_web_server_set_url_root (CockpitWebServer *self,
                                 const gchar *url_root)
{
  GString *str = str = g_string_new (url_root);

  while (str->str[0] == '/')
    g_string_erase (str, 0, 1);

  if (str->len)
    {
      while (str->str[str->len - 1] == '/')
        g_string_truncate (str, str->len - 1);
    }

  if (str->len)
    g_string_printf (self->url_root, "/%s", str->str);
  else
    g_string_assign (self->url_root, str->str);

  g_string_free (str, TRUE);
}

const gchar *
cockpit_web_server_get_url_root (CockpitWebServer *self)
{
  if (self->url_root->len)
    return self->url_root->str;
  else
    return NULL;
}

void
cockpit_web_server_set_forwarded_host_header (CockpitWebServer *self,
                                              const gchar *forwarded_host_header)
{
  g_free (self->forwarded_host_header);
  self->forwarded_host_header = g_strdup (forwarded_host_header);
}

void
cockpit_web_server_set_forwarded_protocol_header (CockpitWebServer *self,
                                                  const gchar *forwarded_protocol_header)
{
  g_free (self->forwarded_protocol_header);
  self->forwarded_protocol_header = g_strdup (forwarded_protocol_header);
}

void
cockpit_web_server_set_forwarded_for_header (CockpitWebServer *self,
                                             const gchar *forwarded_for_header)
{
  g_free (self->forwarded_for_header);
  self->forwarded_for_header = g_strdup (forwarded_for_header);
}

guint16
cockpit_web_server_add_inet_listener (CockpitWebServer *self,
                                      const gchar *address,
                                      guint16 port,
                                      GError **error)
{
  if (address != NULL)
    {
      g_autoptr(GSocketAddress) socket_address = g_inet_socket_address_new_from_string (address, port);
      if (socket_address == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Couldn't parse IP address from `%s`", address);
          return 0;
        }

      g_autoptr(GSocketAddress) result_address = NULL;
      if (!g_socket_listener_add_address (G_SOCKET_LISTENER (self->socket_service), socket_address,
                                          G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
                                          NULL, &result_address, error))
        return 0;

      guint16 port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (result_address));
      g_assert (port != 0);

      return port;
    }

  else if (port > 0)
    {
      if (g_socket_listener_add_inet_port (G_SOCKET_LISTENER (self->socket_service), port, NULL, error))
        return port;
      else
        return 0;
    }
  else
    return g_socket_listener_add_any_inet_port (G_SOCKET_LISTENER (self->socket_service), NULL, error);
}

gboolean
cockpit_web_server_add_fd_listener (CockpitWebServer *self,
                                    int fd,
                                    GError **error)
{
  g_autoptr(GSocket) socket = g_socket_new_from_fd (fd, error);
  if (socket == NULL)
    {
      g_prefix_error (error, "Failed to acquire passed socket %i: ", fd);
      return FALSE;
    }

  if (!g_socket_listener_add_socket (G_SOCKET_LISTENER (self->socket_service), socket, NULL, error))
    {
      g_prefix_error (error, "Failed to add listener for socket %i: ", fd);
      return FALSE;
    }

  return TRUE;
}

void
cockpit_web_server_start (CockpitWebServer *self)
{
  g_return_if_fail (COCKPIT_IS_WEB_SERVER (self));
  g_socket_service_start (self->socket_service);
}

GIOStream *
cockpit_web_server_connect (CockpitWebServer *self)
{
  g_return_val_if_fail (COCKPIT_IS_WEB_SERVER (self), NULL);

  g_autoptr(GIOStream) server = NULL;
  g_autoptr(GIOStream) client = NULL;

  cockpit_socket_streampair (&client, &server);

  cockpit_web_request_start (self, server);

  return g_steal_pointer (&client);
}

/* ---------------------------------------------------------------------------------------------------- */

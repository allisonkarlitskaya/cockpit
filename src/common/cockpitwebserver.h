/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
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

#ifndef __COCKPIT_WEB_SERVER_H__
#define __COCKPIT_WEB_SERVER_H__

#include <gio/gio.h>

#include "cockpitwebresponse.h"

G_BEGIN_DECLS

#define COCKPIT_TYPE_WEB_SERVER  (cockpit_web_server_get_type ())
G_DECLARE_FINAL_TYPE(CockpitWebServer, cockpit_web_server, COCKPIT, WEB_SERVER, GObject)

extern guint cockpit_webserver_request_timeout;

GHashTable *       cockpit_web_server_new_table     (void);

gchar *            cockpit_web_server_parse_cookie    (GHashTable *headers,
                                                       const gchar *name);

gchar **           cockpit_web_server_parse_accept_list   (const gchar *accept,
                                                           const gchar *first);

typedef struct _CockpitWebRequest CockpitWebRequest;
GType
cockpit_web_request_get_type (void);
#define COCKPIT_TYPE_WEB_REQUEST (cockpit_web_request_get_type ())

const gchar *
cockpit_web_request_get_path (CockpitWebRequest *self);

GHashTable *
cockpit_web_request_get_headers (CockpitWebRequest *self);

const gchar *
cockpit_web_request_get_method (CockpitWebRequest *self);

const gchar *
cockpit_web_request_lookup_header (CockpitWebRequest *self,
                                   const gchar *header);

const gchar *
cockpit_web_request_get_origin (CockpitWebRequest *self);

const gchar *
cockpit_web_request_get_host (CockpitWebRequest *self);

const gchar *
cockpit_web_request_get_protocol (CockpitWebRequest *self);

const gchar *
cockpit_web_request_get_origin_ip (CockpitWebRequest *self);

const gchar *
cockpit_web_request_get_client_certificate (CockpitWebRequest *self);

GIOStream *
cockpit_web_request_get_io_stream (CockpitWebRequest *self);

GByteArray *
cockpit_web_request_get_buffer (CockpitWebRequest *self);

gchar *
cockpit_web_request_parse_cookie (CockpitWebRequest *self,
                                  const gchar *cookie);

CockpitWebResponse *
cockpit_web_request_respond (CockpitWebRequest *self);

CockpitWebServer *
cockpit_web_server_new (void);

void
cockpit_web_server_start (CockpitWebServer *self);

void
cockpit_web_server_set_url_root (CockpitWebServer *self,
                                 const gchar      *url_root);

const gchar *
cockpit_web_server_get_url_root (CockpitWebServer *self);

void
cockpit_web_server_set_forwarded_host_header (CockpitWebServer *self,
                                              const gchar *forwarded_host_header);

void
cockpit_web_server_set_forwarded_protocol_header (CockpitWebServer *self,
                                                  const gchar *forwarded_protocol_header);

void
cockpit_web_server_set_forwarded_for_header (CockpitWebServer *self,
                                             const gchar *forwarded_for_header);

guint16
cockpit_web_server_add_inet_listener (CockpitWebServer *self,
                                      const gchar *address,
                                      guint16 port,
                                      GError **error);

gboolean
cockpit_web_server_add_fd_listener (CockpitWebServer *self,
                                    int fd,
                                    GError **error);

GIOStream *
cockpit_web_server_connect (CockpitWebServer *self);

G_END_DECLS

#endif /* __COCKPIT_WEB_SERVER_H__ */

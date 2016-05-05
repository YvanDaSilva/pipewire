/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __PINOS_NODE_H__
#define __PINOS_NODE_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosNode PinosNode;
typedef struct _PinosNodeClass PinosNodeClass;
typedef struct _PinosNodePrivate PinosNodePrivate;

#include <pinos/client/introspect.h>
#include <pinos/server/daemon.h>
#include <pinos/server/source.h>
#include <pinos/server/sink.h>

#define PINOS_TYPE_NODE                 (pinos_node_get_type ())
#define PINOS_IS_NODE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_NODE))
#define PINOS_IS_NODE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_NODE))
#define PINOS_NODE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_NODE, PinosNodeClass))
#define PINOS_NODE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_NODE, PinosNode))
#define PINOS_NODE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_NODE, PinosNodeClass))
#define PINOS_NODE_CAST(obj)            ((PinosNode*)(obj))
#define PINOS_NODE_CLASS_CAST(klass)    ((PinosNodeClass*)(klass))

/**
 * PinosNode:
 *
 * Pinos node class.
 */
struct _PinosNode {
  GObject object;

  PinosNodePrivate *priv;
};

/**
 * PinosNodeClass:
 *
 * Pinos node class.
 */
struct _PinosNodeClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType               pinos_node_get_type                (void);

PinosNode *         pinos_node_new                     (PinosDaemon *daemon);

PinosDaemon *       pinos_node_get_daemon              (PinosNode *node);
const gchar *       pinos_node_get_object_path         (PinosNode *node);

void                pinos_node_set_source              (PinosNode   *node,
                                                        PinosSource *source,
                                                        GObject     *iface);
PinosSource *       pinos_node_get_source              (PinosNode   *node);

void                pinos_node_set_sink                (PinosNode   *node,
                                                        PinosSink   *sink,
                                                        GObject     *iface);
PinosSink *         pinos_node_get_sink                (PinosNode   *node);

G_END_DECLS

#endif /* __PINOS_NODE_H__ */
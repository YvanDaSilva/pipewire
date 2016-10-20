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

#include "pinos.h"
#include "thread-mainloop.h"

struct _PinosThreadMainLoopPrivate
{
  GMainContext *maincontext;
  GMainLoop *mainloop;

  gchar *name;

  GPollFunc poll_func;

  GMutex lock;
  GCond cond;
  GCond accept_cond;
  GThread *thread;

  gint n_waiting;
  gint n_waiting_for_accept;
};

#define PINOS_THREAD_MAIN_LOOP_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_THREAD_MAIN_LOOP, PinosThreadMainLoopPrivate))

G_DEFINE_TYPE (PinosThreadMainLoop, pinos_thread_main_loop, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_MAIN_CONTEXT,
  PROP_NAME,
  PROP_MAIN_LOOP,
};

static void
pinos_thread_main_loop_get_property (GObject    *_object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  PinosThreadMainLoop *loop = PINOS_THREAD_MAIN_LOOP (_object);
  PinosThreadMainLoopPrivate *priv = loop->priv;

  switch (prop_id) {
    case PROP_MAIN_CONTEXT:
      g_value_set_boxed (value, priv->maincontext);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_MAIN_LOOP:
      g_value_set_boxed (value, priv->mainloop);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (loop, prop_id, pspec);
      break;
    }
}

static void
pinos_thread_main_loop_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosThreadMainLoop *loop = PINOS_THREAD_MAIN_LOOP (_object);
  PinosThreadMainLoopPrivate *priv = loop->priv;

  switch (prop_id) {
    case PROP_MAIN_CONTEXT:
      priv->maincontext = g_value_dup_boxed (value);
      break;

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (loop, prop_id, pspec);
      break;
    }
}

static void
pinos_thread_main_loop_constructed (GObject * object)
{
  PinosThreadMainLoop *loop = PINOS_THREAD_MAIN_LOOP (object);
  PinosThreadMainLoopPrivate *priv = loop->priv;

  priv->mainloop = g_main_loop_new (priv->maincontext, FALSE);
  g_debug ("thread-mainloop %p: contructed %p %p", loop, priv->maincontext, priv->mainloop);

  G_OBJECT_CLASS (pinos_thread_main_loop_parent_class)->constructed (object);
}

static void
pinos_thread_main_loop_finalize (GObject * object)
{
  PinosThreadMainLoop *loop = PINOS_THREAD_MAIN_LOOP (object);
  PinosThreadMainLoopPrivate *priv = loop->priv;

  if (priv->maincontext)
    g_main_context_unref (priv->maincontext);
  g_main_loop_unref (priv->mainloop);

  g_free (priv->name);
  g_mutex_clear (&priv->lock);
  g_cond_clear (&priv->cond);
  g_cond_clear (&priv->accept_cond);

  G_OBJECT_CLASS (pinos_thread_main_loop_parent_class)->finalize (object);
}

static void
pinos_thread_main_loop_class_init (PinosThreadMainLoopClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosThreadMainLoopPrivate));

  gobject_class->constructed = pinos_thread_main_loop_constructed;
  gobject_class->finalize = pinos_thread_main_loop_finalize;
  gobject_class->set_property = pinos_thread_main_loop_set_property;
  gobject_class->get_property = pinos_thread_main_loop_get_property;

  /**
   * PinosThreadMainLoop:main-context
   *
   * The GMainContext of the loop.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_MAIN_CONTEXT,
                                   g_param_spec_boxed ("main-context",
                                                       "Main Context",
                                                       "The GMainContext of the loop",
                                                       G_TYPE_MAIN_CONTEXT,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));
  /**
   * PinosThreadMainLoop:name
   *
   * The name of the loop as specified at construction time.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The name of the loop thread",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * PinosThreadMainLoop:main-loop
   *
   * The GMainLoop of the loop.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_MAIN_LOOP,
                                   g_param_spec_boxed ("main-loop",
                                                       "Main Loop",
                                                       "The GMainLoop",
                                                       G_TYPE_MAIN_LOOP,
                                                       G_PARAM_READABLE |
                                                       G_PARAM_STATIC_STRINGS));
}

static void
pinos_thread_main_loop_init (PinosThreadMainLoop * loop)
{
  PinosThreadMainLoopPrivate *priv = loop->priv = PINOS_THREAD_MAIN_LOOP_GET_PRIVATE (loop);

  g_mutex_init (&priv->lock);
  g_cond_init (&priv->cond);
  g_cond_init (&priv->accept_cond);
}

/**
 * pinos_thread_main_loop_new:
 * @context: a #GMainContext
 * @name: a thread name
 *
 * Make a new #PinosThreadMainLoop that will run a mainloop on @context in
 * a thread with @name.
 *
 * Returns: a #PinosThreadMainLoop
 */
PinosThreadMainLoop *
pinos_thread_main_loop_new (GMainContext * context, const gchar *name)
{
  PinosThreadMainLoop *loop;

  loop = g_object_new (PINOS_TYPE_THREAD_MAIN_LOOP,
                       "main-context", context,
                       "name", name,
                       NULL);
  return loop;
}

/**
 * pinos_thread_main_loop_get_impl:
 * @loop: a #PinosThreadMainLoop
 *
 * Get the #GMainLoop used by @loop.
 *
 * Returns: the #GMainLoop used by @loop. It remains valid as long as
 *          @loop is valid.
 */
GMainLoop *
pinos_thread_main_loop_get_impl (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopPrivate *priv;

  g_return_val_if_fail (PINOS_IS_THREAD_MAIN_LOOP (loop), NULL);

  priv = loop->priv;

  return priv->mainloop;
}

static GPrivate loop_key;

static gint
do_poll (GPollFD *ufds, guint nfsd, gint timeout_)
{
  gint res;
  PinosThreadMainLoop *loop = g_private_get (&loop_key);
  PinosThreadMainLoopPrivate *priv = loop->priv;

  g_mutex_unlock (&priv->lock);
  res = priv->poll_func (ufds, nfsd, timeout_);
  g_mutex_lock (&priv->lock);

  return res;
}

static gpointer
handle_mainloop (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopPrivate *priv = loop->priv;

  g_mutex_lock (&priv->lock);
  g_private_set (&loop_key, loop);

  priv->poll_func = g_main_context_get_poll_func (priv->maincontext);
  g_main_context_set_poll_func (priv->maincontext, do_poll);

  g_main_context_push_thread_default (priv->maincontext);
  g_debug ("thread-mainloop %p: run mainloop %p context %p", loop, priv->mainloop, priv->maincontext);
  g_main_loop_run (priv->mainloop);
  g_debug ("thread-mainloop %p: done", loop);
  g_main_context_pop_thread_default (priv->maincontext);

  g_main_context_set_poll_func (priv->maincontext, priv->poll_func);

  g_mutex_unlock (&priv->lock);

  return NULL;
}


/**
 * pinos_thread_main_loop_start:
 * @loop: a #PinosThreadMainLoop
 * @error: am optional #GError
 *
 * Start the thread to handle @loop.
 *
 * Returns: %TRUE on success. %FALSE will be returned when an error occured
 *          and @error will contain more information.
 */
gboolean
pinos_thread_main_loop_start (PinosThreadMainLoop *loop, GError **error)
{
  PinosThreadMainLoopPrivate *priv;

  g_return_val_if_fail (PINOS_IS_THREAD_MAIN_LOOP (loop), FALSE);
  priv = loop->priv;
  g_return_val_if_fail (priv->thread == NULL, FALSE);

  priv->thread = g_thread_try_new (priv->name, (GThreadFunc) handle_mainloop, loop, error);

  return priv->thread != NULL;
}

/**
 * pinos_thread_main_loop_stop:
 * @loop: a #PinosThreadMainLoop
 *
 * Quit the main loop and stop its thread.
 */
void
pinos_thread_main_loop_stop (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopPrivate *priv;

  g_return_if_fail (PINOS_IS_THREAD_MAIN_LOOP (loop));
  priv = loop->priv;

  g_return_if_fail (priv->thread != NULL);
  g_return_if_fail (!pinos_thread_main_loop_in_thread (loop));

  g_mutex_lock (&priv->lock);
  g_main_loop_quit (priv->mainloop);
  g_mutex_unlock (&priv->lock);

  g_thread_join (priv->thread);
  priv->thread = NULL;
}

/**
 * pinos_thread_main_loop_lock:
 * @loop: a #PinosThreadMainLoop
 *
 * Lock the mutex associated with @loop.
 */
void
pinos_thread_main_loop_lock (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopPrivate *priv;

  g_return_if_fail (PINOS_IS_THREAD_MAIN_LOOP (loop));
  priv = loop->priv;
  g_return_if_fail (!pinos_thread_main_loop_in_thread (loop));

  g_mutex_lock (&priv->lock);
}

/**
 * pinos_thread_main_loop_unlock:
 * @loop: a #PinosThreadMainLoop
 *
 * Unlock the mutex associated with @loop.
 */
void
pinos_thread_main_loop_unlock (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopPrivate *priv;

  g_return_if_fail (PINOS_IS_THREAD_MAIN_LOOP (loop));
  priv = loop->priv;
  g_return_if_fail (!pinos_thread_main_loop_in_thread (loop));

  g_mutex_unlock (&priv->lock);
}

/**
 * pinos_thread_main_loop_signal:
 * @loop: a #PinosThreadMainLoop
 *
 * Signal the main thread of @loop. If @wait_for_accept is %TRUE,
 * this function waits until pinos_thread_main_loop_accept() is called.
 */
void
pinos_thread_main_loop_signal (PinosThreadMainLoop *loop, gboolean wait_for_accept)
{
  PinosThreadMainLoopPrivate *priv;

  g_return_if_fail (PINOS_IS_THREAD_MAIN_LOOP (loop));
  priv = loop->priv;

  if (priv->n_waiting > 0)
    g_cond_broadcast (&priv->cond);

  if (wait_for_accept) {
     priv->n_waiting_for_accept++;

     while (priv->n_waiting_for_accept > 0)
       g_cond_wait (&priv->accept_cond, &priv->lock);
  }
}

/**
 * pinos_thread_main_loop_wait:
 * @loop: a #PinosThreadMainLoop
 *
 * Wait for the loop thread to call pinos_thread_main_loop_signal().
 */
void
pinos_thread_main_loop_wait (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopPrivate *priv;

  g_return_if_fail (PINOS_IS_THREAD_MAIN_LOOP (loop));
  priv = loop->priv;
  g_return_if_fail (!pinos_thread_main_loop_in_thread (loop));

  priv->n_waiting ++;

  g_cond_wait (&priv->cond, &priv->lock);

  g_assert (priv->n_waiting > 0);
  priv->n_waiting --;
}

/**
 * pinos_thread_main_loop_accept:
 * @loop: a #PinosThreadMainLoop
 *
 * Signal the loop thread waiting for accept with pinos_thread_main_loop_signal().
 */
void
pinos_thread_main_loop_accept (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopPrivate *priv;

  g_return_if_fail (PINOS_IS_THREAD_MAIN_LOOP (loop));
  priv = loop->priv;
  g_return_if_fail (!pinos_thread_main_loop_in_thread (loop));

  g_assert (priv->n_waiting_for_accept > 0);
  priv->n_waiting_for_accept--;

  g_cond_signal (&priv->accept_cond);
}

/**
 * pinos_thread_main_loop_in_thread:
 * @loop: a #PinosThreadMainLoop
 *
 * Check if we are inside the thread of @loop.
 *
 * Returns: %TRUE when called inside the thread of @loop.
 */
gboolean
pinos_thread_main_loop_in_thread (PinosThreadMainLoop *loop)
{
  g_return_val_if_fail (PINOS_IS_THREAD_MAIN_LOOP (loop), FALSE);

  return g_thread_self() == loop->priv->thread;
}
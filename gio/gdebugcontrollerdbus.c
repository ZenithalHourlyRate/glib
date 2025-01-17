/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright © 2021 Endless OS Foundation, LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <gio/gio.h>
#include "gdebugcontroller.h"
#include "gdebugcontrollerdbus.h"
#include "giomodule-priv.h"
#include "gi18n.h"
#include "gio/gdbusprivate.h"
#include "gio/gmarshal-internal.h"

/**
 * SECTION:gdebugcontrollerdbus
 * @title: GDebugControllerDBus
 * @short_description: Debugging controller D-Bus implementation
 * @include: gio/gio.h
 *
 * #GDebugControllerDBus is an implementation of #GDebugController which exposes
 * debug settings as a D-Bus object.
 *
 * It is a #GInitable object, and will register an object at
 * `/org/gtk/Debugging` on the bus given as
 * #GDebugControllerDBus:connection once it’s initialized. The object will be
 * unregistered when the last reference to the #GDebugControllerDBus is dropped.
 *
 * Since: 2.72
 */

static const gchar org_gtk_Debugging_xml[] =
  "<node>"
    "<interface name='org.gtk.Debugging'>"
      "<property name='DebugEnabled' type='b' access='read'/>"
      "<method name='SetDebugEnabled'>"
        "<arg type='b' name='debug-enabled' direction='in'/>"
      "</method>"
    "</interface>"
  "</node>";

static GDBusInterfaceInfo *org_gtk_Debugging;

#define G_DEBUG_CONTROLLER_DBUS_GET_INITABLE_IFACE(o) (G_TYPE_INSTANCE_GET_INTERFACE ((o), G_TYPE_INITABLE, GInitable))

static void g_debug_controller_dbus_iface_init (GDebugControllerInterface *iface);
static void g_debug_controller_dbus_initable_iface_init (GInitableIface *iface);
static gboolean g_debug_controller_dbus_authorize_default (GDebugControllerDBus  *self,
                                                           GDBusMethodInvocation *invocation);

typedef enum
{
  PROP_CONNECTION = 1,
  /* Overrides: */
  PROP_DEBUG_ENABLED,
} GDebugControllerDBusProperty;

static GParamSpec *props[PROP_CONNECTION + 1] = { NULL, };

typedef enum
{
  SIGNAL_AUTHORIZE,
} GDebugControllerDBusSignal;

static guint signals[SIGNAL_AUTHORIZE + 1] = {0};

typedef struct
{
  GObject parent_instance;

  GDBusConnection *connection;  /* (owned) */
  guint object_id;

  gboolean debug_enabled;
} GDebugControllerDBusPrivate;

G_DEFINE_TYPE_WITH_CODE (GDebugControllerDBus, g_debug_controller_dbus, G_TYPE_OBJECT,
                         G_ADD_PRIVATE (GDebugControllerDBus)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                g_debug_controller_dbus_initable_iface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_DEBUG_CONTROLLER,
                                                g_debug_controller_dbus_iface_init)
                         _g_io_modules_ensure_extension_points_registered ();
                         g_io_extension_point_implement (G_DEBUG_CONTROLLER_EXTENSION_POINT_NAME,
                                                         g_define_type_id,
                                                         "dbus",
                                                         30))

static void
g_debug_controller_dbus_init (GDebugControllerDBus *dbus)
{
}

static void
set_debug_enabled (GDebugControllerDBus *self,
                   gboolean              debug_enabled)
{
  GDebugControllerDBusPrivate *priv = g_debug_controller_dbus_get_instance_private (self);

  if (debug_enabled != priv->debug_enabled)
    {
      GVariantBuilder builder;

      priv->debug_enabled = debug_enabled;

      /* Change the default log writer’s behaviour in GLib. */
      g_log_set_debug_enabled (debug_enabled);

      /* Notify internally and externally of the property change. */
      g_object_notify (G_OBJECT (self), "debug-enabled");

      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&builder, "{sv}", "DebugEnabled", g_variant_new_boolean (priv->debug_enabled));

      g_dbus_connection_emit_signal (priv->connection,
                                     NULL,
                                     "/org/gtk/Debugging",
                                     "org.gtk.DBus.Properties",
                                     "PropertiesChanged",
                                     g_variant_new ("(sa{sv}as)",
                                                    "org.gtk.Debugging",
                                                    &builder,
                                                    NULL),
                                     NULL);

      g_debug ("Debug output %s", debug_enabled ? "enabled" : "disabled");
    }
}

/* Called in the #GMainContext which was default when the #GDebugControllerDBus
 * was initialised. */
static GVariant *
dbus_get_property (GDBusConnection  *connection,
                   const gchar      *sender,
                   const gchar      *object_path,
                   const gchar      *interface_name,
                   const gchar      *property_name,
                   GError          **error,
                   gpointer          user_data)
{
  GDebugControllerDBus *self = user_data;
  GDebugControllerDBusPrivate *priv = g_debug_controller_dbus_get_instance_private (self);

  if (g_str_equal (property_name, "DebugEnabled"))
    return g_variant_new_boolean (priv->debug_enabled);

  g_assert_not_reached ();

  return NULL;
}

/* Called in a worker thread. */
static void
authorize_task_cb (GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data,
                   GCancellable *cancellable)
{
  GDebugControllerDBus *self = G_DEBUG_CONTROLLER_DBUS (source_object);
  GDBusMethodInvocation *invocation = G_DBUS_METHOD_INVOCATION (task_data);
  gboolean authorized = TRUE;

  g_signal_emit (self, signals[SIGNAL_AUTHORIZE], 0, invocation, &authorized);

  g_task_return_boolean (task, authorized);
}

/* Called in the #GMainContext which was default when the #GDebugControllerDBus
 * was initialised. */
static void
authorize_cb (GObject      *object,
              GAsyncResult *result,
              gpointer      user_data)
{
  GDebugControllerDBus *self = G_DEBUG_CONTROLLER_DBUS (object);
  GTask *task = G_TASK (result);
  GDBusMethodInvocation *invocation = g_task_get_task_data (task);
  GVariant *parameters = g_dbus_method_invocation_get_parameters (invocation);
  gboolean enabled = FALSE;
  gboolean authorized;

  authorized = g_task_propagate_boolean (task, NULL);

  if (!authorized)
    {
      GError *local_error = g_error_new (G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                         _("Not authorized to change debug settings"));
      g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&local_error));
      return;
    }

  /* Update the property value. */
  g_variant_get (parameters, "(b)", &enabled);
  set_debug_enabled (self, enabled);

  g_dbus_method_invocation_return_value (invocation, NULL);
}

/* Called in the #GMainContext which was default when the #GDebugControllerDBus
 * was initialised. */
static void
dbus_method_call (GDBusConnection       *connection,
                  const gchar           *sender,
                  const gchar           *object_path,
                  const gchar           *interface_name,
                  const gchar           *method_name,
                  GVariant              *parameters,
                  GDBusMethodInvocation *invocation,
                  gpointer               user_data)
{
  GDebugControllerDBus *self = user_data;
  GDebugControllerDBusClass *klass = G_DEBUG_CONTROLLER_DBUS_GET_CLASS (self);

  /* Only on the org.gtk.Debugging interface */
  if (g_str_equal (method_name, "SetDebugEnabled"))
    {
      GTask *task = NULL;

      task = g_task_new (self, NULL, authorize_cb, NULL);
      g_task_set_source_tag (task, dbus_method_call);
      g_task_set_task_data (task, g_object_ref (invocation), (GDestroyNotify) g_object_unref);

      /* Check the calling peer is authorised to change the debug mode. So that
       * the signal handler can block on checking polkit authorisation (which
       * definitely involves D-Bus calls, and might involve user interaction),
       * emit the #GDebugControllerDBus::authorize signal in a worker thread, so
       * that handlers can synchronously block it. This is similar to how
       * #GDBusInterfaceSkeleton::g-authorize-method works.
       *
       * If no signal handlers are connected, don’t bother running the worker
       * thread, and just return a default value of %FALSE. Fail closed. */
      if (g_signal_has_handler_pending (self, signals[SIGNAL_AUTHORIZE], 0, FALSE) ||
          klass->authorize != g_debug_controller_dbus_authorize_default)
        g_task_run_in_thread (task, authorize_task_cb);
      else
        g_task_return_boolean (task, FALSE);

      g_clear_object (&task);
    }
  else
    g_assert_not_reached ();
}

static gboolean
g_debug_controller_dbus_initable_init (GInitable     *initable,
                                       GCancellable  *cancellable,
                                       GError       **error)
{
  GDebugControllerDBus *self = G_DEBUG_CONTROLLER_DBUS (initable);
  GDebugControllerDBusPrivate *priv = g_debug_controller_dbus_get_instance_private (self);
  static const GDBusInterfaceVTable vtable = {
    dbus_method_call,
    dbus_get_property,
    NULL /* set_property */,
    { 0 }
  };

  if (org_gtk_Debugging == NULL)
    {
      GError *local_error = NULL;
      GDBusNodeInfo *info;

      info = g_dbus_node_info_new_for_xml (org_gtk_Debugging_xml, &local_error);
      if G_UNLIKELY (info == NULL)
        g_error ("%s", local_error->message);
      org_gtk_Debugging = g_dbus_node_info_lookup_interface (info, "org.gtk.Debugging");
      g_assert (org_gtk_Debugging != NULL);
      g_dbus_interface_info_ref (org_gtk_Debugging);
      g_dbus_node_info_unref (info);
    }

  priv->object_id = g_dbus_connection_register_object (priv->connection,
                                                       "/org/gtk/Debugging",
                                                       org_gtk_Debugging,
                                                       &vtable, self, NULL, error);
  if (priv->object_id == 0)
    return FALSE;

  return TRUE;
}

static void
g_debug_controller_dbus_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  GDebugControllerDBus *self = G_DEBUG_CONTROLLER_DBUS (object);
  GDebugControllerDBusPrivate *priv = g_debug_controller_dbus_get_instance_private (self);

  switch ((GDebugControllerDBusProperty) prop_id)
    {
    case PROP_CONNECTION:
      g_value_set_object (value, priv->connection);
      break;
    case PROP_DEBUG_ENABLED:
      g_value_set_boolean (value, priv->debug_enabled);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_debug_controller_dbus_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  GDebugControllerDBus *self = G_DEBUG_CONTROLLER_DBUS (object);
  GDebugControllerDBusPrivate *priv = g_debug_controller_dbus_get_instance_private (self);

  switch ((GDebugControllerDBusProperty) prop_id)
    {
    case PROP_CONNECTION:
      /* Construct only */
      g_assert (priv->connection == NULL);
      priv->connection = g_value_dup_object (value);
      break;
    case PROP_DEBUG_ENABLED:
      set_debug_enabled (self, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_debug_controller_dbus_dispose (GObject *object)
{
  GDebugControllerDBus *self = G_DEBUG_CONTROLLER_DBUS (object);
  GDebugControllerDBusPrivate *priv = g_debug_controller_dbus_get_instance_private (self);

  if (priv->object_id != 0)
    {
      g_dbus_connection_unregister_object (priv->connection, priv->object_id);
      priv->object_id = 0;
    }

  g_clear_object (&priv->connection);

  G_OBJECT_CLASS (g_debug_controller_dbus_parent_class)->dispose (object);
}

static gboolean
g_debug_controller_dbus_authorize_default (GDebugControllerDBus  *self,
                                           GDBusMethodInvocation *invocation)
{
  return TRUE;
}

static void
g_debug_controller_dbus_class_init (GDebugControllerDBusClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = g_debug_controller_dbus_get_property;
  gobject_class->set_property = g_debug_controller_dbus_set_property;
  gobject_class->dispose = g_debug_controller_dbus_dispose;

  klass->authorize = g_debug_controller_dbus_authorize_default;

  /**
   * GDebugControllerDBus:connection:
   *
   * The D-Bus connection to expose the debugging interface on.
   *
   * Typically this will be the same connection (to the system or session bus)
   * which the rest of the application or service’s D-Bus objects are registered
   * on.
   *
   * Since: 2.72
   */
  props[PROP_CONNECTION] =
      g_param_spec_object ("connection", "D-Bus Connection",
                           "The D-Bus connection to expose the debugging interface on.",
                           G_TYPE_DBUS_CONNECTION,
                           G_PARAM_READWRITE |
                           G_PARAM_CONSTRUCT_ONLY |
                           G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, G_N_ELEMENTS (props), props);

  g_object_class_override_property (gobject_class, PROP_DEBUG_ENABLED, "debug-enabled");

  /**
   * GDebugControllerDBus::authorize:
   * @controller: The #GDebugControllerDBus emitting the signal.
   * @invocation: A #GDBusMethodInvocation.
   *
   * Emitted when a D-Bus peer is trying to change the debug settings and used
   * to determine if that is authorized.
   *
   * This signal is emitted in a dedicated worker thread, so handlers are
   * allowed to perform blocking I/O. This means that, for example, it is
   * appropriate to call `polkit_authority_check_authorization_sync()` to check
   * authorization using polkit.
   *
   * If %FALSE is returned then no further handlers are run and the request to
   * change the debug settings is rejected.
   *
   * Otherwise, if %TRUE is returned, signal emission continues. If no handlers
   * return %FALSE, then the debug settings are allowed to be changed.
   *
   * Signal handlers must not modify @invocation, or cause it to return a value.
   *
   * The default class handler just returns %TRUE.
   *
   * Returns: %TRUE if the call is authorized, %FALSE otherwise.
   *
   * Since: 2.72
   */
  signals[SIGNAL_AUTHORIZE] =
    g_signal_new ("authorize",
                  G_TYPE_DEBUG_CONTROLLER_DBUS,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GDebugControllerDBusClass, authorize),
                  _g_signal_accumulator_false_handled,
                  NULL,
                  _g_cclosure_marshal_BOOLEAN__OBJECT,
                  G_TYPE_BOOLEAN,
                  1,
                  G_TYPE_DBUS_METHOD_INVOCATION);
  g_signal_set_va_marshaller (signals[SIGNAL_AUTHORIZE],
                              G_TYPE_FROM_CLASS (klass),
                              _g_cclosure_marshal_BOOLEAN__OBJECTv);
}

static void
g_debug_controller_dbus_iface_init (GDebugControllerInterface *iface)
{
}

static void
g_debug_controller_dbus_initable_iface_init (GInitableIface *iface)
{
  iface->init = g_debug_controller_dbus_initable_init;
}

/**
 * g_debug_controller_dbus_new:
 * @connection: a #GDBusConnection to register the debug object on
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * Create a new #GDebugControllerDBus and synchronously initialize it.
 *
 * Initializing the object will export the debug object on @connection. The
 * object will remain registered until the last reference to the
 * #GDebugControllerDBus is dropped.
 *
 * Initialization may fail if registering the object on @connection fails.
 *
 * Returns: (nullable) (transfer full): a new #GDebugControllerDBus, or %NULL
 *   on failure
 * Since: 2.72
 */
GDebugControllerDBus *
g_debug_controller_dbus_new (GDBusConnection  *connection,
                             GCancellable     *cancellable,
                             GError          **error)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return g_initable_new (G_TYPE_DEBUG_CONTROLLER_DBUS,
                         cancellable,
                         error,
                         "connection", connection,
                         NULL);
}

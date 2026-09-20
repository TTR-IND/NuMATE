#include "panel.h"

/*
 * mate-session starts required components one phase at a time:
 *   WindowManager → Panel → Desktop → Application
 * Every app launched in a phase is held in pending_apps until it
 * calls org.gnome.SessionManager.RegisterClient, exits, or the
 * 30-second GSM_MANAGER_PHASE_TIMEOUT fires.
 *
 * axiom-panel is the Panel-phase required component. If it never
 * registers, the Desktop phase does not start and nemo-desktop
 * sits behind that timeout. This file is that registration.
 *
 * DESKTOP_AUTOSTART_ID is the token mate-session put in our
 * environment when it spawned us. Unset after reading so children
 * do not inherit it and look like a second copy of this client.
 */

static GDBusConnection *sm_bus;
static char            *client_path;

static void
end_session_respond(void)
{
	if (!sm_bus || !client_path)
		return;
	g_dbus_connection_call(sm_bus,
	                       "org.gnome.SessionManager",
	                       client_path,
	                       "org.gnome.SessionManager.ClientPrivate",
	                       "EndSessionResponse",
	                       g_variant_new("(bs)", TRUE, ""),
	                       NULL,
	                       G_DBUS_CALL_FLAGS_NONE,
	                       -1, NULL, NULL, NULL);
}

static void
on_sm_signal(GDBusConnection *c,
             const gchar     *sender,
             const gchar     *object_path,
             const gchar     *iface,
             const gchar     *signal,
             GVariant        *params,
             gpointer         unused)
{
	(void)c;
	(void)sender;
	(void)object_path;
	(void)iface;
	(void)params;
	(void)unused;

	if (g_strcmp0(signal, "Stop") == 0) {
		gtk_main_quit();
		return;
	}
	if (g_strcmp0(signal, "QueryEndSession") == 0 ||
	    g_strcmp0(signal, "EndSession") == 0)
		end_session_respond();
}

static void
on_registered(GObject *src, GAsyncResult *res, gpointer unused)
{
	GError *err = NULL;
	GVariant *ret;
	const char *path;

	(void)unused;
	ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
	if (!ret) {
		g_clear_error(&err);
		return;
	}
	g_variant_get(ret, "(o)", &path);
	client_path = g_strdup(path);
	g_variant_unref(ret);

	g_dbus_connection_signal_subscribe(sm_bus,
	                                   "org.gnome.SessionManager",
	                                   "org.gnome.SessionManager.ClientPrivate",
	                                   NULL,
	                                   client_path,
	                                   NULL,
	                                   G_DBUS_SIGNAL_FLAGS_NONE,
	                                   on_sm_signal,
	                                   NULL, NULL);
}

void
panel_session_register(void)
{
	GError *err = NULL;
	const char *startup_id;

	if (client_path)
		return;

	startup_id = g_getenv("DESKTOP_AUTOSTART_ID");
	if (!startup_id)
		startup_id = "";

	sm_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!sm_bus) {
		g_clear_error(&err);
		return;
	}

	g_dbus_connection_call(sm_bus,
	                       "org.gnome.SessionManager",
	                       "/org/gnome/SessionManager",
	                       "org.gnome.SessionManager",
	                       "RegisterClient",
	                       g_variant_new("(ss)", "mate-panel.desktop", startup_id),
	                       G_VARIANT_TYPE("(o)"),
	                       G_DBUS_CALL_FLAGS_NONE,
	                       -1, NULL,
	                       on_registered, NULL);

	g_unsetenv("DESKTOP_AUTOSTART_ID");
}

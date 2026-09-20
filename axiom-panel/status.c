#include "panel.h"

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	guint32 id;
	gchar *app_name;
	gchar *summary;
	gchar *body;
	gchar *sender;
	gint64 timestamp;
} Notification;

typedef struct {
	gchar *app_id;
	gchar *app_name;
	gchar *app_icon;
	GdkPixbuf *icon_pixbuf;
	GList *notifications;
	gint unread_count;
	GtkWidget *stack_widget;
	gboolean has_tray_item;
	gchar *sender_bus_name;
	gchar *object_path;
	gchar *menu_path;
} AppGroup;

typedef struct {
	GtkWidget *panel;
	GtkWidget *pill;
	GtkWidget *clock_label;
	GtkWidget *wifi_icon;
	GtkWidget *battery_icon;
	GtkWidget *panel_battery_icon;
	GtkWidget *battery_label;
	GtkWidget *battery_box;
	GtkWidget *panel_window;
	GtkWidget *notif_window;
	GtkWidget *notif_inner;
	GtkWidget *notif_title;
	GtkWidget *username_label;
	GtkWidget *brightness_slider;
	GtkWidget *volume_slider;
	GtkWidget *volume_icon;
	GtkWidget *wifi_tile;
	GtkWidget *bt_tile;
	GtkWidget *airplane_tile;
	GList *groups;
	GDBusConnection *notif_conn;
	GDBusProxy *upower_proxy;
	GDBusProxy *backlight_proxy;
	guint32 next_id;
	gboolean wifi_on;
	gboolean bt_on;
} Status;

static Status *g_st;
static void on_brightness(GtkRange *r, gpointer u);
static void refresh_battery_ui(void);

static gchar *
run_cmd(const gchar *cmd)
{
	gchar *out = NULL;

	if (!g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL))
		return NULL;
	if (out)
		g_strstrip(out);
	if (out && !*out) {
		g_free(out);
		return NULL;
	}
	return out;
}

/* ── battery / brightness / volume ─────────────────────────────────── */

static const char *
battery_icon_for(int percent, gboolean charging)
{
	if (charging)
		return "battery-good-charging-symbolic";
	if (percent >= 95)
		return "battery-full-symbolic";
	if (percent >= 60)
		return "battery-good-symbolic";
	if (percent >= 30)
		return "battery-low-symbolic";
	if (percent >= 10)
		return "battery-caution-symbolic";
	return "battery-empty-symbolic";
}

static void
refresh_battery_ui(void)
{
	GVariant *v;
	gboolean present = FALSE;
	int percent = 0;
	guint32 state = 0;
	gboolean charging;
	const gchar *icon;
	gchar *text;

	if (!g_st->battery_icon && !g_st->panel_battery_icon)
		return;
	if (!g_st->upower_proxy) {
		if (g_st->battery_icon)
			gtk_widget_set_visible(g_st->battery_icon, FALSE);
		if (g_st->battery_box)
			gtk_widget_set_visible(g_st->battery_box, FALSE);
		return;
	}
	v = g_dbus_proxy_get_cached_property(g_st->upower_proxy, "IsPresent");
	present = v ? g_variant_get_boolean(v) : FALSE;
	if (v)
		g_variant_unref(v);
	if (g_st->battery_icon)
		gtk_widget_set_visible(g_st->battery_icon, present);
	if (g_st->battery_box)
		gtk_widget_set_visible(g_st->battery_box, present);
	if (!present)
		return;

	v = g_dbus_proxy_get_cached_property(g_st->upower_proxy, "Percentage");
	percent = v ? (int)(g_variant_get_double(v) + 0.5) : 0;
	if (v)
		g_variant_unref(v);
	v = g_dbus_proxy_get_cached_property(g_st->upower_proxy, "State");
	state = v ? g_variant_get_uint32(v) : 0;
	if (v)
		g_variant_unref(v);
	charging = (state == 1 || state == 5);
	icon = battery_icon_for(CLAMP(percent, 0, 100), charging);
	v = g_dbus_proxy_get_cached_property(g_st->upower_proxy, "IconName");
	if (v) {
		const gchar *name = g_variant_get_string(v, NULL);

		if (name && *name && gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), name))
			icon = name;
	}
	if (g_st->battery_icon) {
		gtk_image_set_from_icon_name(GTK_IMAGE(g_st->battery_icon), icon, GTK_ICON_SIZE_MENU);
		gtk_image_set_pixel_size(GTK_IMAGE(g_st->battery_icon), 15);
	}
	if (g_st->panel_battery_icon) {
		gtk_image_set_from_icon_name(GTK_IMAGE(g_st->panel_battery_icon), icon, GTK_ICON_SIZE_MENU);
		gtk_image_set_pixel_size(GTK_IMAGE(g_st->panel_battery_icon), 18);
	}
	if (g_st->battery_label) {
		text = g_strdup_printf("%d%%", CLAMP(percent, 0, 100));
		gtk_label_set_text(GTK_LABEL(g_st->battery_label), text);
		g_free(text);
	}
	if (v)
		g_variant_unref(v);
}

static void
on_upower_changed(GDBusProxy *p, GVariant *c, GStrv inv, gpointer u)
{
	(void)p;
	(void)c;
	(void)inv;
	(void)u;
	refresh_battery_ui();
}

static void
battery_init(void)
{
	gchar *owner;

	g_st->upower_proxy = g_dbus_proxy_new_for_bus_sync(
		G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
		"org.freedesktop.UPower",
		"/org/freedesktop/UPower/devices/DisplayDevice",
		"org.freedesktop.UPower.Device", NULL, NULL);
	if (!g_st->upower_proxy)
		return;
	owner = g_dbus_proxy_get_name_owner(g_st->upower_proxy);
	if (!owner) {
		g_object_unref(g_st->upower_proxy);
		g_st->upower_proxy = NULL;
		return;
	}
	g_free(owner);
	g_signal_connect(g_st->upower_proxy, "g-properties-changed",
	                 G_CALLBACK(on_upower_changed), NULL);
}

static void
set_brightness_percent(int percent)
{
	if (!g_st->backlight_proxy)
		return;
	g_dbus_proxy_call(g_st->backlight_proxy, "SetBrightness",
	                  g_variant_new("(u)", (guint32)CLAMP(percent, 0, 100)),
	                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
backlight_got(GObject *src, GAsyncResult *res, gpointer u)
{
	GVariant *ret;
	guint32 value = 0;

	(void)u;
	ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, NULL);
	if (!ret)
		return;
	g_variant_get(ret, "(u)", &value);
	g_variant_unref(ret);
	if (!g_st->brightness_slider)
		return;
	g_signal_handlers_block_by_func(g_st->brightness_slider, G_CALLBACK(on_brightness), NULL);
	gtk_range_set_value(GTK_RANGE(g_st->brightness_slider), CLAMP(value, 0, 100));
	g_signal_handlers_unblock_by_func(g_st->brightness_slider, G_CALLBACK(on_brightness), NULL);
}

static double
brightness_percent(void)
{
	return 50.0;
}

static void
refresh_brightness_slider(void)
{
	if (!g_st->backlight_proxy || !g_st->brightness_slider)
		return;
	g_dbus_proxy_call(g_st->backlight_proxy, "GetBrightness",
	                  NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, backlight_got, NULL);
}

static void
on_backlight_signal(GDBusProxy *p, const gchar *sender, const gchar *sig,
                    GVariant *params, gpointer u)
{
	guint32 value = 0;

	(void)p;
	(void)sender;
	(void)u;
	if (g_strcmp0(sig, "BrightnessChanged") != 0)
		return;
	g_variant_get(params, "(u)", &value);
	if (!g_st->brightness_slider)
		return;
	g_signal_handlers_block_by_func(g_st->brightness_slider, G_CALLBACK(on_brightness), NULL);
	gtk_range_set_value(GTK_RANGE(g_st->brightness_slider), CLAMP(value, 0, 100));
	g_signal_handlers_unblock_by_func(g_st->brightness_slider, G_CALLBACK(on_brightness), NULL);
}

static void
backlight_init(void)
{
	g_st->backlight_proxy = g_dbus_proxy_new_for_bus_sync(
		G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
		"org.mate.PowerManager",
		"/org/mate/PowerManager/Backlight",
		"org.mate.PowerManager.Backlight", NULL, NULL);
	if (!g_st->backlight_proxy)
		return;
	g_signal_connect(g_st->backlight_proxy, "g-signal", G_CALLBACK(on_backlight_signal), NULL);
}

static int
volume_percent(void)
{
	gchar *out = run_cmd("sh -c \"amixer sget Master | grep -oP '\\[\\d+%\\]' | head -1 | tr -d '[]%'\"");
	int p = out ? atoi(out) : 50;

	g_free(out);
	return CLAMP(p, 0, 100);
}

static gboolean
volume_muted(void)
{
	gchar *out = run_cmd("sh -c \"amixer sget Master | grep -oP '\\[(on|off)\\]' | head -1 | tr -d '[]'\"");
	gboolean m = (out && g_strcmp0(out, "off") == 0);

	g_free(out);
	return m;
}

static void
set_volume_percent(int percent)
{
	gchar *cmd = g_strdup_printf("amixer sset Master %d%% >/dev/null 2>&1", CLAMP(percent, 0, 100));

	g_spawn_command_line_async(cmd, NULL);
	g_free(cmd);
}

static gboolean
wifi_enabled(void)
{
	gchar *out = run_cmd("nmcli radio wifi");
	gboolean on = (out && strstr(out, "enabled"));

	g_free(out);
	return on;
}

static gboolean
bt_enabled(void)
{
	gchar *out = run_cmd("sh -c \"bluetoothctl show 2>/dev/null | grep -oP 'Powered: \\K\\w+'\"");
	gboolean on = (out && g_strcmp0(out, "yes") == 0);

	g_free(out);
	return on;
}

/* ── notifications ─────────────────────────────────────────────────── */

static void
free_notification(Notification *n)
{
	if (!n)
		return;
	g_free(n->app_name);
	g_free(n->summary);
	g_free(n->body);
	g_free(n->sender);
	g_free(n);
}

static AppGroup *
find_or_create_group(const gchar *app_id, const gchar *app_name, const gchar *app_icon)
{
	GList *l;
	AppGroup *g;

	for (l = g_st->groups; l; l = l->next) {
		g = l->data;
		if (g_strcmp0(g->app_id, app_id) == 0)
			return g;
	}
	g = g_new0(AppGroup, 1);
	g->app_id = g_strdup(app_id ? app_id : "unknown");
	g->app_name = g_strdup(app_name ? app_name : "Unknown");
	g->app_icon = g_strdup(app_icon ? app_icon : "application-x-executable");
	g_st->groups = g_list_prepend(g_st->groups, g);
	return g;
}

static GDesktopAppInfo *
desktop_for_token(const gchar *token)
{
	gchar *lower, *dashed, *id;
	GDesktopAppInfo *app = NULL;
	const gchar *stems[3];
	guint i;

	if (!token || !*token)
		return NULL;
	lower = g_ascii_strdown(token, -1);
	dashed = g_strdup(lower);
	for (id = dashed; *id; id++)
		if (*id == '_' || *id == ' ')
			*id = '-';
	stems[0] = token;
	stems[1] = lower;
	stems[2] = dashed;
	for (i = 0; i < 3 && !app; i++) {
		id = g_str_has_suffix(stems[i], ".desktop")
		     ? g_strdup(stems[i])
		     : g_strdup_printf("%s.desktop", stems[i]);
		app = g_desktop_app_info_new(id);
		g_free(id);
	}
	g_free(dashed);
	g_free(lower);
	return app;
}

/*
 * Yandex Browser (and a few other Chromium skins) leave SNI Title empty
 * or set it to the process basename. Resolve the .desktop display name
 * from the sender's /proc/<pid>/exe instead of trusting Title.
 */
static gchar *
name_from_exe(const gchar *exe_path)
{
	GList *all, *l;
	gchar *found = NULL, *exe_base;

	if (!exe_path || !*exe_path)
		return NULL;
	exe_base = g_path_get_basename(exe_path);
	all = g_app_info_get_all();
	for (l = all; l && !found; l = l->next) {
		const gchar *exec;
		gchar *exec_base, *resolved;

		if (!G_IS_DESKTOP_APP_INFO(l->data))
			continue;
		exec = g_app_info_get_executable(G_APP_INFO(l->data));
		if (!exec)
			continue;
		if (exec[0] == '/') {
			resolved = realpath(exec, NULL);
			if (resolved && g_strcmp0(resolved, exe_path) == 0)
				found = g_strdup(g_app_info_get_display_name(G_APP_INFO(l->data)));
			g_free(resolved);
		}
		if (found)
			break;
		exec_base = g_path_get_basename(exec);
		if (g_strcmp0(exec_base, exe_base) == 0)
			found = g_strdup(g_app_info_get_display_name(G_APP_INFO(l->data)));
		g_free(exec_base);
	}
	g_list_free_full(all, g_object_unref);
	if (!found) {
		gchar *dashed = g_strdup(exe_base);
		GDesktopAppInfo *app;
		gchar *p;

		for (p = dashed; *p; p++)
			if (*p == '_')
				*p = '-';
		app = desktop_for_token(dashed);
		if (app) {
			found = g_strdup(g_app_info_get_display_name(G_APP_INFO(app)));
			g_object_unref(app);
		}
		g_free(dashed);
	}
	g_free(exe_base);
	return found;
}

static gchar *
name_from_bus(const gchar *bus_name)
{
	GDBusConnection *bus;
	GVariant *pid_v;
	gchar *found = NULL;
	guint32 pid = 0;

	if (!bus_name)
		return NULL;
	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (!bus)
		return NULL;
	pid_v = g_dbus_connection_call_sync(bus,
		"org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "GetConnectionUnixProcessID",
		g_variant_new("(s)", bus_name),
		G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
	if (pid_v) {
		g_variant_get(pid_v, "(u)", &pid);
		g_variant_unref(pid_v);
		if (pid > 0) {
			gchar *link = g_strdup_printf("/proc/%u/exe", pid);
			gchar real[4096];
			ssize_t n;

			n = readlink(link, real, sizeof(real) - 1);
			if (n > 0) {
				real[n] = '\0';
				found = name_from_exe(real);
			}
			g_free(link);
		}
	}
	g_object_unref(bus);
	return found;
}

static void
resolve_notify_identity(const gchar *app_name, const gchar *desktop_hint,
                        const gchar *app_icon, gchar **out_name, gchar **out_icon)
{
	GDesktopAppInfo *app;

	*out_name = NULL;
	*out_icon = NULL;
	app = desktop_for_token(desktop_hint);
	if (!app)
		app = desktop_for_token(app_name);
	if (app) {
		const gchar *name = g_app_info_get_display_name(G_APP_INFO(app));
		GIcon *icon = g_app_info_get_icon(G_APP_INFO(app));

		if (name && *name)
			*out_name = g_strdup(name);
		if (icon)
			*out_icon = g_icon_to_string(icon);
		g_object_unref(app);
	}
	if (!*out_name)
		*out_name = g_strdup((app_name && *app_name) ? app_name : "Unknown App");
	if (!*out_icon && app_icon && *app_icon)
		*out_icon = g_strdup(app_icon);
	if (!*out_icon)
		*out_icon = g_strdup("application-x-executable");
}

static gchar *
format_age(gint64 ts)
{
	gint64 diff = time(NULL) - ts;

	if (diff < 60)
		return g_strdup("Just now");
	if (diff < 3600)
		return g_strdup_printf("%lld min ago", (long long)(diff / 60));
	if (diff < 86400)
		return g_strdup_printf("%lld hours ago", (long long)(diff / 3600));
	return g_strdup_printf("%lld days ago", (long long)(diff / 86400));
}

static GtkWidget *
make_bubble(Notification *n)
{
	GtkWidget *bubble, *header, *sender, *when, *body;
	gchar *age;

	bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_name(bubble, "NotifBubble");
	header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	sender = gtk_label_new(n->sender);
	gtk_widget_set_name(sender, "NotifSender");
	gtk_widget_set_halign(sender, GTK_ALIGN_START);
	gtk_label_set_ellipsize(GTK_LABEL(sender), PANGO_ELLIPSIZE_END);
	age = format_age(n->timestamp);
	when = gtk_label_new(age);
	gtk_widget_set_name(when, "NotifTime");
	g_free(age);
	gtk_box_pack_start(GTK_BOX(header), sender, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(header), when, FALSE, FALSE, 0);
	body = gtk_label_new(n->body && n->body[0] ? n->body : n->summary);
	gtk_widget_set_name(body, "NotifContent");
	gtk_widget_set_halign(body, GTK_ALIGN_START);
	gtk_label_set_line_wrap(GTK_LABEL(body), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_ellipsize(GTK_LABEL(body), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(body), 28);
	gtk_box_pack_start(GTK_BOX(bubble), header, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bubble), body, FALSE, FALSE, 0);
	return bubble;
}

static void
on_sni_context(GtkWidget *button, AppGroup *g)
{
	if (!g->has_tray_item || !g->sender_bus_name || !g->object_path)
		return;
	{
		GtkWidget *top = gtk_widget_get_toplevel(button);
		gint lx = 0, ly = 0, ox = 0, oy = 0;
		GtkAllocation alloc;
		GdkWindow *gw;
		GDBusConnection *bus;

		gtk_widget_translate_coordinates(button, top, 0, 0, &lx, &ly);
		gtk_widget_get_allocation(button, &alloc);
		ly += alloc.height;
		gw = gtk_widget_get_window(top);
		if (gw)
			gdk_window_get_origin(gw, &ox, &oy);
		bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
		if (bus) {
			g_dbus_connection_call(bus, g->sender_bus_name, g->object_path,
			                       "org.kde.StatusNotifierItem", "ContextMenu",
			                       g_variant_new("(ii)", ox + lx, oy + ly),
			                       NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
			g_object_unref(bus);
		}
	}
}

static GtkWidget *
make_card(AppGroup *g)
{
	GtkWidget *card, *bar, *ibox, *icon, *info, *name, *status, *opts;
	gchar *stxt, *btxt;

	card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_name(card, "NotifCard");

	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_container_set_border_width(GTK_CONTAINER(bar), 12);

	if (g->icon_pixbuf) {
		GdkPixbuf *scaled = gdk_pixbuf_scale_simple(g->icon_pixbuf, 36, 36, GDK_INTERP_BILINEAR);
		icon = gtk_image_new_from_pixbuf(scaled);
		g_object_unref(scaled);
	} else {
		icon = gtk_image_new_from_icon_name(g->app_icon, GTK_ICON_SIZE_DND);
		gtk_image_set_pixel_size(GTK_IMAGE(icon), 36);
	}

	info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	name = gtk_label_new(g->app_name);
	gtk_widget_set_name(name, "NotifAppName");
	gtk_widget_set_halign(name, GTK_ALIGN_START);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(name), 22);
	if (g->unread_count > 0)
		stxt = g_strdup_printf("%d new", g->unread_count);
	else if (g->has_tray_item)
		stxt = g_strdup("Running");
	else
		stxt = g_strdup("No new messages");
	status = gtk_label_new(stxt);
	g_free(stxt);
	gtk_widget_set_name(status, "NotifAppStatus");
	gtk_widget_set_halign(status, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(info), name, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(info), status, FALSE, FALSE, 0);

	opts = gtk_button_new_from_icon_name("view-more-symbolic", GTK_ICON_SIZE_MENU);
	gtk_button_set_relief(GTK_BUTTON(opts), GTK_RELIEF_NONE);
	g_signal_connect(opts, "clicked", G_CALLBACK(on_sni_context), g);

	gtk_box_pack_start(GTK_BOX(bar), icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), info, TRUE, TRUE, 0);
	if (g->unread_count > 0) {
		btxt = g_strdup_printf("%d", g->unread_count);
		ibox = gtk_label_new(btxt);
		g_free(btxt);
		gtk_widget_set_name(ibox, "NotifBadge");
		gtk_box_pack_end(GTK_BOX(bar), ibox, FALSE, FALSE, 0);
	}
	gtk_box_pack_end(GTK_BOX(bar), opts, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(card), bar, FALSE, FALSE, 0);

	if (g->notifications) {
		GList *l;

		g->stack_widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
		gtk_container_set_border_width(GTK_CONTAINER(g->stack_widget), 12);
		for (l = g->notifications; l; l = l->next)
			gtk_box_pack_start(GTK_BOX(g->stack_widget), make_bubble(l->data), FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(card), g->stack_widget, FALSE, FALSE, 0);
	}
	return card;
}

static void
rebuild_notif(void)
{
	GList *kids, *l;
	gboolean any = FALSE;

	if (!g_st->notif_inner)
		return;
	kids = gtk_container_get_children(GTK_CONTAINER(g_st->notif_inner));
	for (l = kids; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(kids);

	for (l = g_st->groups; l; l = l->next) {
		AppGroup *g = l->data;

		if (!g->notifications && !g->has_tray_item)
			continue;
		any = TRUE;
		gtk_box_pack_start(GTK_BOX(g_st->notif_inner), make_card(g), FALSE, FALSE, 0);
	}
	if (g_st->notif_title)
		gtk_label_set_text(GTK_LABEL(g_st->notif_title), any ? "Notifications" : "No Notifications");
	gtk_widget_show_all(g_st->notif_inner);
}

static void
clear_all(GtkWidget *b, gpointer u)
{
	GList *l;

	(void)b;
	(void)u;
	for (l = g_st->groups; l; l = l->next) {
		AppGroup *g = l->data;

		g_list_free_full(g->notifications, (GDestroyNotify)free_notification);
		g->notifications = NULL;
		g->unread_count = 0;
	}
	rebuild_notif();
}

static GtkWidget *
create_notif_window(void)
{
	GtkWidget *win, *outer, *header, *scroll, *clear;

	win = gtk_window_new(GTK_WINDOW_POPUP);
	panel_ensure_rgba(win);
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
	gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);

	outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_name(outer, "NotifOuter");
	gtk_widget_set_hexpand(outer, FALSE);
	gtk_container_add(GTK_CONTAINER(win), outer);

	header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_name(header, "NotifHeader");
	gtk_container_set_border_width(GTK_CONTAINER(header), 16);
	g_st->notif_title = gtk_label_new("Notifications");
	gtk_widget_set_name(g_st->notif_title, "NotifTitle");
	gtk_widget_set_halign(g_st->notif_title, GTK_ALIGN_START);
	clear = gtk_button_new_with_label("Clear all");
	gtk_widget_set_name(clear, "NotifClearBtn");
	g_signal_connect(clear, "clicked", G_CALLBACK(clear_all), NULL);
	gtk_box_pack_start(GTK_BOX(header), g_st->notif_title, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(header), clear, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);

	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	g_st->notif_inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(g_st->notif_inner), 8);
	gtk_container_add(GTK_CONTAINER(scroll), g_st->notif_inner);
	gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
	gtk_widget_show_all(outer);
	return win;
}

/* ── quick settings ────────────────────────────────────────────────── */

static void
launch_cmd(GtkWidget *w, gpointer cmd)
{
	(void)w;
	if (cmd)
		g_spawn_command_line_async((const char *)cmd, NULL);
}

static void
on_brightness(GtkRange *r, gpointer u)
{
	(void)u;
	set_brightness_percent((int)round(gtk_range_get_value(r)));
}

static void
on_volume(GtkRange *r, gpointer u)
{
	(void)u;
	set_volume_percent((int)round(gtk_range_get_value(r)));
}

static void
toggle_wifi(GtkWidget *tile, gpointer u)
{
	gboolean on = gtk_style_context_has_class(gtk_widget_get_style_context(tile), "active");

	(void)u;
	g_spawn_command_line_async(on ? "nmcli radio wifi off" : "nmcli radio wifi on", NULL);
}

static void
toggle_bt(GtkWidget *tile, gpointer u)
{
	gboolean on = gtk_style_context_has_class(gtk_widget_get_style_context(tile), "active");

	(void)u;
	g_spawn_command_line_async(on ? "bluetoothctl power off" : "bluetoothctl power on", NULL);
}

static void
toggle_air(GtkWidget *tile, gpointer u)
{
	gboolean on = gtk_style_context_has_class(gtk_widget_get_style_context(tile), "active");

	(void)u;
	g_spawn_command_line_async(on ? "nmcli radio all on" : "nmcli radio all off", NULL);
}

static GtkWidget *
make_tile(const char *icon, const char *label, GCallback cb)
{
	GtkWidget *tile, *box, *img, *lab;

	tile = gtk_button_new();
	gtk_widget_set_size_request(tile, 100, 100);
	gtk_style_context_add_class(gtk_widget_get_style_context(tile), "tile");
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND);
	gtk_image_set_pixel_size(GTK_IMAGE(img), 28);
	gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
	lab = gtk_label_new(label);
	gtk_widget_set_halign(lab, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), lab, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(tile), box);
	g_signal_connect(tile, "clicked", cb, NULL);
	return tile;
}

static void
set_tile_active(GtkWidget *tile, gboolean on)
{
	GtkStyleContext *ctx = gtk_widget_get_style_context(tile);

	if (on)
		gtk_style_context_add_class(ctx, "active");
	else
		gtk_style_context_remove_class(ctx, "active");
}

static void
update_volume_icon(void)
{
	const char *name;
	int vol;
	gboolean muted;

	if (!g_st->volume_icon)
		return;
	muted = volume_muted();
	vol = volume_percent();
	if (muted)
		name = "audio-volume-muted-symbolic";
	else if (vol > 70)
		name = "audio-volume-high-symbolic";
	else if (vol > 30)
		name = "audio-volume-medium-symbolic";
	else
		name = "audio-volume-low-symbolic";
	gtk_image_set_from_icon_name(GTK_IMAGE(g_st->volume_icon), name, GTK_ICON_SIZE_DND);
	gtk_image_set_pixel_size(GTK_IMAGE(g_st->volume_icon), PANEL_ICON_PX);
}

static void
refresh_pill(void)
{
	char buf[16];
	time_t now;
	struct tm tm;

	if (!g_st)
		return;
	now = time(NULL);
	localtime_r(&now, &tm);
	strftime(buf, sizeof buf, "%H:%M", &tm);
	if (g_st->clock_label)
		gtk_label_set_text(GTK_LABEL(g_st->clock_label), buf);

	g_st->wifi_on = wifi_enabled();
	if (g_st->wifi_icon)
		gtk_image_set_from_icon_name(GTK_IMAGE(g_st->wifi_icon),
			g_st->wifi_on ? "network-wireless-signal-excellent-symbolic"
			              : "network-wireless-offline-symbolic",
			GTK_ICON_SIZE_MENU);
	refresh_battery_ui();
}

static gboolean
periodic(gpointer u)
{
	struct utsname uts;
	gchar *host;

	(void)u;
	refresh_pill();
	if (!g_st->wifi_tile)
		return G_SOURCE_CONTINUE;

	g_st->wifi_on = wifi_enabled();
	g_st->bt_on = bt_enabled();
	set_tile_active(g_st->wifi_tile, g_st->wifi_on);
	set_tile_active(g_st->bt_tile, g_st->bt_on);
	set_tile_active(g_st->airplane_tile, !g_st->wifi_on && !g_st->bt_on);

	if (g_st->username_label) {
		uname(&uts);
		host = g_strdup_printf("%s@%s", g_get_user_name(), uts.nodename);
		gtk_label_set_text(GTK_LABEL(g_st->username_label), host);
		g_free(host);
	}
	refresh_brightness_slider();
	if (g_st->volume_slider) {
		g_signal_handlers_block_by_func(g_st->volume_slider, G_CALLBACK(on_volume), NULL);
		gtk_range_set_value(GTK_RANGE(g_st->volume_slider), volume_percent());
		g_signal_handlers_unblock_by_func(g_st->volume_slider, G_CALLBACK(on_volume), NULL);
	}
	update_volume_icon();
	return G_SOURCE_CONTINUE;
}

static GtkWidget *
create_settings_window(void)
{
	GtkWidget *win, *card, *status, *grid, *brow, *vrow, *bottom;
	GtkWidget *settings_btn, *power_btn;
	struct utsname uts;
	gchar *host;

	win = gtk_window_new(GTK_WINDOW_POPUP);
	panel_ensure_rgba(win);
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
	gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);

	card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_name(card, "PanelCard");
	gtk_container_add(GTK_CONTAINER(win), card);

	status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_margin_start(status, 16);
	gtk_widget_set_margin_end(status, 16);
	gtk_widget_set_margin_top(status, 12);
	gtk_widget_set_margin_bottom(status, 18);
	g_st->username_label = gtk_label_new("");
	gtk_widget_set_halign(g_st->username_label, GTK_ALIGN_START);
	g_st->battery_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_no_show_all(g_st->battery_box, TRUE);
	g_st->battery_label = gtk_label_new("");
	g_st->panel_battery_icon = gtk_image_new_from_icon_name("battery-full-symbolic", GTK_ICON_SIZE_MENU);
	gtk_image_set_pixel_size(GTK_IMAGE(g_st->panel_battery_icon), 18);
	gtk_box_pack_start(GTK_BOX(g_st->battery_box), g_st->battery_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(g_st->battery_box), g_st->panel_battery_icon, FALSE, FALSE, 0);
	gtk_widget_show(g_st->battery_label);
	gtk_widget_show(g_st->panel_battery_icon);
	gtk_box_pack_start(GTK_BOX(status), g_st->username_label, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(status), g_st->battery_box, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(card), status, FALSE, FALSE, 0);

	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_widget_set_margin_start(grid, 16);
	gtk_widget_set_margin_end(grid, 16);
	g_st->wifi_tile = make_tile("network-wireless-signal-excellent-symbolic", "Wi-Fi", G_CALLBACK(toggle_wifi));
	g_st->bt_tile = make_tile("bluetooth-active-symbolic", "Bluetooth", G_CALLBACK(toggle_bt));
	g_st->airplane_tile = make_tile("airplane-mode-symbolic", "Airplane", G_CALLBACK(toggle_air));
	gtk_grid_attach(GTK_GRID(grid), g_st->wifi_tile, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), g_st->bt_tile, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), g_st->airplane_tile, 2, 0, 1, 1);
	gtk_box_pack_start(GTK_BOX(card), grid, FALSE, FALSE, 0);

	brow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_ROW_GAP);
	gtk_widget_set_margin_start(brow, PANEL_ROW_SIDE);
	gtk_widget_set_margin_end(brow, PANEL_ROW_SIDE);
	gtk_widget_set_margin_top(brow, PANEL_ROW_TB);
	gtk_widget_set_margin_bottom(brow, PANEL_ROW_TB);
	{
		GtkWidget *bicon = gtk_image_new_from_icon_name("display-brightness-symbolic", GTK_ICON_SIZE_DND);

		gtk_image_set_pixel_size(GTK_IMAGE(bicon), PANEL_ICON_PX);
		gtk_widget_set_size_request(bicon, PANEL_ICON_PX, PANEL_ICON_PX);
		gtk_widget_set_halign(bicon, GTK_ALIGN_CENTER);
		gtk_widget_set_valign(bicon, GTK_ALIGN_CENTER);
		gtk_box_pack_start(GTK_BOX(brow), bicon, FALSE, FALSE, 0);
	}
	g_st->brightness_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_scale_set_draw_value(GTK_SCALE(g_st->brightness_slider), FALSE);
	gtk_widget_set_hexpand(g_st->brightness_slider, TRUE);
	gtk_widget_set_valign(g_st->brightness_slider, GTK_ALIGN_CENTER);
	g_signal_connect(g_st->brightness_slider, "value-changed", G_CALLBACK(on_brightness), NULL);
	gtk_box_pack_start(GTK_BOX(brow), g_st->brightness_slider, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(card), brow, FALSE, FALSE, 0);

	vrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_ROW_GAP);
	gtk_widget_set_margin_start(vrow, PANEL_ROW_SIDE);
	gtk_widget_set_margin_end(vrow, PANEL_ROW_SIDE);
	gtk_widget_set_margin_top(vrow, PANEL_ROW_TB);
	gtk_widget_set_margin_bottom(vrow, PANEL_ROW_TB);
	g_st->volume_icon = gtk_image_new_from_icon_name("audio-volume-high-symbolic", GTK_ICON_SIZE_DND);
	gtk_image_set_pixel_size(GTK_IMAGE(g_st->volume_icon), PANEL_ICON_PX);
	gtk_widget_set_size_request(g_st->volume_icon, PANEL_ICON_PX, PANEL_ICON_PX);
	gtk_widget_set_halign(g_st->volume_icon, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(g_st->volume_icon, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(vrow), g_st->volume_icon, FALSE, FALSE, 0);
	g_st->volume_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_scale_set_draw_value(GTK_SCALE(g_st->volume_slider), FALSE);
	gtk_widget_set_hexpand(g_st->volume_slider, TRUE);
	gtk_widget_set_valign(g_st->volume_slider, GTK_ALIGN_CENTER);
	g_signal_connect(g_st->volume_slider, "value-changed", G_CALLBACK(on_volume), NULL);
	gtk_box_pack_start(GTK_BOX(vrow), g_st->volume_slider, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(card), vrow, FALSE, FALSE, 0);

	bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_margin_start(bottom, 16);
	gtk_widget_set_margin_end(bottom, 16);
	gtk_widget_set_margin_bottom(bottom, 12);
	settings_btn = gtk_button_new_with_label("Settings");
	gtk_style_context_add_class(gtk_widget_get_style_context(settings_btn), "action-btn");
	g_signal_connect(settings_btn, "clicked", G_CALLBACK(launch_cmd), "numate-settings");
	power_btn = gtk_button_new_with_label("Power");
	gtk_style_context_add_class(gtk_widget_get_style_context(power_btn), "power-btn");
	g_signal_connect(power_btn, "clicked", G_CALLBACK(launch_cmd), "mate-session-save --shutdown-dialog");
	gtk_box_pack_start(GTK_BOX(bottom), settings_btn, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(bottom), power_btn, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(card), bottom, FALSE, FALSE, 0);

	uname(&uts);
	host = g_strdup_printf("%s@%s", g_get_user_name(), uts.nodename);
	gtk_label_set_text(GTK_LABEL(g_st->username_label), host);
	g_free(host);
	gtk_widget_show_all(card);
	refresh_battery_ui();
	return win;
}

static void
toggle_panel(GtkWidget *btn, gpointer u)
{
	GdkRectangle geo;
	int pw, ph, px, py, nh, avail;

	(void)btn;
	(void)u;
	if (gtk_widget_get_visible(g_st->panel_window)) {
		int cx, cy, nx, ny, floor;

		if (!panel_primary_geo(&geo)) {
			gtk_widget_hide(g_st->panel_window);
			gtk_widget_hide(g_st->notif_window);
			return;
		}
		floor = geo.y + geo.height;
		gtk_window_get_position(GTK_WINDOW(g_st->panel_window), &cx, &cy);
		anim_window_slide(g_st->panel_window, g_st->panel_window,
		                  cx, cy, floor, ANIM_SLIDE_MS, TRUE);
		if (gtk_widget_get_visible(g_st->notif_window)) {
			gtk_window_get_position(GTK_WINDOW(g_st->notif_window), &nx, &ny);
			anim_window_slide(g_st->notif_window, g_st->notif_window,
			                  nx, ny, floor, ANIM_SLIDE_MS, TRUE);
		}
		return;
	}
	if (!panel_primary_geo(&geo))
		return;

	gtk_window_move(GTK_WINDOW(g_st->panel_window), -10000, -10000);
	gtk_widget_show_all(g_st->panel_window);
	while (gtk_events_pending())
		gtk_main_iteration();
	{
		GtkWidget *card = gtk_bin_get_child(GTK_BIN(g_st->panel_window));

		pw = gtk_widget_get_allocated_width(g_st->panel_window);
		ph = card ? gtk_widget_get_allocated_height(card)
		          : gtk_widget_get_allocated_height(g_st->panel_window);
	}
	px = geo.x + geo.width - pw - PANEL_MARGIN;
	py = geo.y + geo.height - PANEL_HEIGHT - MENU_GAP - ph;
	anim_window_slide(g_st->panel_window, g_st->panel_window,
	                  px, geo.y + geo.height, py, ANIM_SLIDE_MS, FALSE);

	rebuild_notif();
	gtk_widget_set_size_request(g_st->notif_window, pw, -1);
	gtk_window_move(GTK_WINDOW(g_st->notif_window), -10000, -10000);
	gtk_widget_show_all(g_st->notif_window);
	while (gtk_events_pending())
		gtk_main_iteration();
	{
		GtkRequisition nat;
		int ny;

		gtk_widget_get_preferred_size(g_st->notif_window, NULL, &nat);
		avail = py - geo.y - NOTIF_PANEL_GAP;
		nh = MIN(nat.height, avail);
		if (nh < 1)
			nh = 1;
		gtk_widget_set_size_request(g_st->notif_window, pw, nh);
		gtk_window_resize(GTK_WINDOW(g_st->notif_window), pw, nh);
		ny = py - NOTIF_PANEL_GAP - nh;
		anim_window_slide(g_st->notif_window, g_st->notif_window,
		                  px, geo.y + geo.height, ny, ANIM_SLIDE_MS, FALSE);
	}
}

/* ── D-Bus: notifications + SNI watcher ────────────────────────────── */

static GdkPixbuf *
parse_icon_pixmap(GVariant *pixmap_array)
{
	gint best_w = 0, best_h = 0;
	GVariant *best = NULL, *bytes_v;
	GVariantIter iter;
	gint32 w, h;
	const guint8 *raw;
	gsize raw_len, expect, i;
	guint8 *pixels;
	GdkPixbuf *pb;

	if (!pixmap_array)
		return NULL;
	g_variant_iter_init(&iter, pixmap_array);
	while (g_variant_iter_loop(&iter, "(ii@ay)", &w, &h, &bytes_v)) {
		if (w > best_w) {
			if (best)
				g_variant_unref(best);
			best_w = w;
			best_h = h;
			best = g_variant_ref(bytes_v);
		}
	}
	if (!best || best_w <= 0 || best_h <= 0) {
		if (best)
			g_variant_unref(best);
		return NULL;
	}
	raw = g_variant_get_fixed_array(best, &raw_len, sizeof(guint8));
	expect = (gsize)best_w * (gsize)best_h * 4;
	if (raw_len < expect) {
		g_variant_unref(best);
		return NULL;
	}
	pixels = g_malloc(expect);
	for (i = 0; i < expect; i += 4) {
		pixels[i + 0] = raw[i + 1];
		pixels[i + 1] = raw[i + 2];
		pixels[i + 2] = raw[i + 3];
		pixels[i + 3] = raw[i + 0];
	}
	g_variant_unref(best);
	pb = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, TRUE, 8,
	                              best_w, best_h, best_w * 4,
	                              (GdkPixbufDestroyNotify)g_free, NULL);
	return pb;
}

typedef struct {
	gchar *bus;
	gchar *path;
} PendingSNI;

static void
on_sni_props(GObject *src, GAsyncResult *res, gpointer user)
{
	PendingSNI *p = user;
	GError *err = NULL;
	GVariant *ret, *dict, *v;
	gchar *item_id = NULL, *icon_name = NULL, *menu_path = NULL, *title = NULL;
	gchar *desktop_entry = NULL, *resolved = NULL;
	GdkPixbuf *pix = NULL;
	AppGroup *g;

	ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
	if (!ret) {
		if (err)
			g_error_free(err);
		g_free(p->bus);
		g_free(p->path);
		g_free(p);
		return;
	}
	g_variant_get(ret, "(@a{sv})", &dict);
	if ((v = g_variant_lookup_value(dict, "Id", G_VARIANT_TYPE_STRING))) {
		item_id = g_variant_dup_string(v, NULL);
		g_variant_unref(v);
	}
	if ((v = g_variant_lookup_value(dict, "Title", G_VARIANT_TYPE_STRING))) {
		title = g_variant_dup_string(v, NULL);
		g_variant_unref(v);
	}
	if ((v = g_variant_lookup_value(dict, "IconName", G_VARIANT_TYPE_STRING))) {
		icon_name = g_variant_dup_string(v, NULL);
		g_variant_unref(v);
	}
	if ((v = g_variant_lookup_value(dict, "Menu", G_VARIANT_TYPE_OBJECT_PATH))) {
		menu_path = g_variant_dup_string(v, NULL);
		g_variant_unref(v);
	}
	if ((v = g_variant_lookup_value(dict, "DesktopEntry", G_VARIANT_TYPE_STRING))) {
		desktop_entry = g_variant_dup_string(v, NULL);
		g_variant_unref(v);
	}
	if ((!icon_name || !*icon_name) &&
	    (v = g_variant_lookup_value(dict, "IconPixmap", G_VARIANT_TYPE("a(iiay)")))) {
		pix = parse_icon_pixmap(v);
		g_variant_unref(v);
	}

	resolved = name_from_bus(p->bus);
	if (!resolved && desktop_entry) {
		GDesktopAppInfo *dai = desktop_for_token(desktop_entry);

		if (dai) {
			resolved = g_strdup(g_app_info_get_display_name(G_APP_INFO(dai)));
			g_object_unref(dai);
		}
	}
	if (!resolved && item_id) {
		GDesktopAppInfo *dai = desktop_for_token(item_id);

		if (dai) {
			resolved = g_strdup(g_app_info_get_display_name(G_APP_INFO(dai)));
			g_object_unref(dai);
		}
	}
	if (!resolved)
		resolved = g_strdup(title && *title ? title
		                    : (item_id && *item_id ? item_id : "Tray"));

	g = find_or_create_group(item_id ? item_id : p->bus, resolved,
	                         (icon_name && *icon_name) ? icon_name : "application-x-executable");
	g_free(g->app_name);
	g->app_name = g_strdup(resolved);
	g->has_tray_item = TRUE;
	if (pix) {
		if (g->icon_pixbuf)
			g_object_unref(g->icon_pixbuf);
		g->icon_pixbuf = pix;
	}
	g_free(g->sender_bus_name);
	g_free(g->object_path);
	g_free(g->menu_path);
	g->sender_bus_name = g_strdup(p->bus);
	g->object_path = g_strdup(p->path);
	g->menu_path = menu_path;
	rebuild_notif();

	g_free(item_id);
	g_free(icon_name);
	g_free(title);
	g_free(desktop_entry);
	g_free(resolved);
	g_variant_unref(dict);
	g_variant_unref(ret);
	g_free(p->bus);
	g_free(p->path);
	g_free(p);
}

static void
watcher_method(GDBusConnection *c, const gchar *sender, const gchar *path,
               const gchar *iface, const gchar *method, GVariant *params,
               GDBusMethodInvocation *inv, gpointer u)
{
	(void)path;
	(void)iface;
	(void)u;
	if (g_strcmp0(method, "RegisterStatusNotifierItem") == 0) {
		const gchar *arg = NULL;
		PendingSNI *p = g_new0(PendingSNI, 1);

		g_variant_get(params, "(&s)", &arg);
		if (arg && arg[0] == '/') {
			p->bus = g_strdup(sender);
			p->path = g_strdup(arg);
		} else {
			p->bus = g_strdup(arg ? arg : sender);
			p->path = g_strdup("/StatusNotifierItem");
		}
		g_dbus_connection_call(c, p->bus, p->path,
		                       "org.freedesktop.DBus.Properties", "GetAll",
		                       g_variant_new("(s)", "org.kde.StatusNotifierItem"),
		                       G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1,
		                       NULL, on_sni_props, p);
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	if (g_strcmp0(method, "RegisterStatusNotifierHost") == 0)
		g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *
watcher_get_prop(GDBusConnection *c, const gchar *s, const gchar *p,
                 const gchar *i, const gchar *name, GError **e, gpointer u)
{
	(void)c;
	(void)s;
	(void)p;
	(void)i;
	(void)e;
	(void)u;
	if (g_strcmp0(name, "IsStatusNotifierHostRegistered") == 0)
		return g_variant_new_boolean(TRUE);
	if (g_strcmp0(name, "ProtocolVersion") == 0)
		return g_variant_new_int32(0);
	if (g_strcmp0(name, "RegisteredStatusNotifierItems") == 0) {
		GVariantBuilder b;
		GList *l;

		g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
		for (l = g_st->groups; l; l = l->next) {
			AppGroup *g = l->data;

			if (g->has_tray_item && g->sender_bus_name)
				g_variant_builder_add(&b, "s", g->sender_bus_name);
		}
		return g_variant_builder_end(&b);
	}
	return NULL;
}

static const GDBusInterfaceVTable watcher_vt = {
	.method_call = watcher_method,
	.get_property = watcher_get_prop,
	.set_property = NULL
};

static void
on_watcher_acquired(GDBusConnection *c, const gchar *name, gpointer u)
{
	const gchar *xml =
		"<node><interface name='org.kde.StatusNotifierWatcher'>"
		"<method name='RegisterStatusNotifierItem'><arg type='s' name='service' direction='in'/></method>"
		"<method name='RegisterStatusNotifierHost'><arg type='s' name='service' direction='in'/></method>"
		"<property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
		"<property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
		"<property name='ProtocolVersion' type='i' access='read'/>"
		"</interface></node>";
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, NULL);

	(void)name;
	(void)u;
	g_dbus_connection_register_object(c, "/StatusNotifierWatcher",
		info->interfaces[0], &watcher_vt, NULL, NULL, NULL);
	g_dbus_node_info_unref(info);
}

static void
notif_method(GDBusConnection *c, const gchar *sender, const gchar *path,
             const gchar *iface, const gchar *method, GVariant *params,
             GDBusMethodInvocation *inv, gpointer u)
{
	(void)c;
	(void)sender;
	(void)path;
	(void)iface;
	(void)u;
	if (g_strcmp0(method, "Notify") == 0) {
		const gchar *app_name = NULL, *app_icon = NULL, *summary = NULL, *body = NULL;
		guint32 replaces = 0;
		GVariant *actions = NULL, *hints = NULL;
		gint32 expire = -1;
		guint32 id;
		AppGroup *g;
		Notification *n;
		gchar *desktop_hint = NULL, *disp_name = NULL, *disp_icon = NULL;
		GList *l;

		g_variant_get(params, "(&su&s&s&s@as@a{sv}i)",
		              &app_name, &replaces, &app_icon, &summary, &body,
		              &actions, &hints, &expire);
		if (hints) {
			GVariant *de = g_variant_lookup_value(hints, "desktop-entry", G_VARIANT_TYPE_STRING);

			if (de) {
				desktop_hint = g_variant_dup_string(de, NULL);
				g_variant_unref(de);
			}
		}
		resolve_notify_identity(app_name, desktop_hint, app_icon, &disp_name, &disp_icon);
		g = find_or_create_group(desktop_hint ? desktop_hint : app_name, disp_name, disp_icon);
		g_free(g->app_name);
		g->app_name = g_strdup(disp_name);
		id = 0;
		if (replaces) {
			for (l = g->notifications; l; l = l->next) {
				Notification *old = l->data;

				if (old->id == replaces) {
					g_free(old->summary);
					g_free(old->body);
					old->summary = g_strdup(summary);
					old->body = g_strdup(body);
					old->timestamp = time(NULL);
					id = replaces;
					break;
				}
			}
		}
		if (!id) {
			id = g_st->next_id++;
			n = g_new0(Notification, 1);
			n->id = id;
			n->app_name = g_strdup(disp_name);
			n->summary = g_strdup(summary);
			n->body = g_strdup(body);
			n->sender = g_strdup(disp_name);
			n->timestamp = time(NULL);
			g->notifications = g_list_append(g->notifications, n);
			g->unread_count++;
		}
		rebuild_notif();
		g_free(desktop_hint);
		g_free(disp_name);
		g_free(disp_icon);
		if (actions)
			g_variant_unref(actions);
		if (hints)
			g_variant_unref(hints);
		g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", id));
		return;
	}
	if (g_strcmp0(method, "CloseNotification") == 0) {
		guint32 id = 0;
		GList *gl;

		g_variant_get(params, "(u)", &id);
		for (gl = g_st->groups; gl; gl = gl->next) {
			AppGroup *g = gl->data;
			GList *nl;

			for (nl = g->notifications; nl; nl = nl->next) {
				Notification *n = nl->data;

				if (n->id != id)
					continue;
				g->notifications = g_list_remove(g->notifications, n);
				if (g->unread_count > 0)
					g->unread_count--;
				free_notification(n);
				rebuild_notif();
				if (g_st->notif_conn)
					g_dbus_connection_emit_signal(g_st->notif_conn, NULL,
						"/org/freedesktop/Notifications",
						"org.freedesktop.Notifications",
						"NotificationClosed",
						g_variant_new("(uu)", id, 3u), NULL);
				goto closed;
			}
		}
closed:
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	if (g_strcmp0(method, "GetCapabilities") == 0) {
		const gchar *caps[] = { "body", "icon-static", "persistence", NULL };

		g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", caps));
		return;
	}
	if (g_strcmp0(method, "GetServerInformation") == 0) {
		g_dbus_method_invocation_return_value(inv,
			g_variant_new("(ssss)", "axiom-panel", "axiom", "1.0", "1.2"));
		return;
	}
}

static const GDBusInterfaceVTable notif_vt = {
	.method_call = notif_method,
	.get_property = NULL,
	.set_property = NULL
};

static void
on_notif_acquired(GDBusConnection *c, const gchar *name, gpointer u)
{
	const gchar *xml =
		"<node><interface name='org.freedesktop.Notifications'>"
		"<method name='Notify'>"
		"<arg type='s' name='app_name' direction='in'/>"
		"<arg type='u' name='replaces_id' direction='in'/>"
		"<arg type='s' name='app_icon' direction='in'/>"
		"<arg type='s' name='summary' direction='in'/>"
		"<arg type='s' name='body' direction='in'/>"
		"<arg type='as' name='actions' direction='in'/>"
		"<arg type='a{sv}' name='hints' direction='in'/>"
		"<arg type='i' name='expire_timeout' direction='in'/>"
		"<arg type='u' name='id' direction='out'/>"
		"</method>"
		"<method name='CloseNotification'><arg type='u' name='id' direction='in'/></method>"
		"<method name='GetCapabilities'><arg type='as' name='caps' direction='out'/></method>"
		"<method name='GetServerInformation'>"
		"<arg type='s' name='name' direction='out'/>"
		"<arg type='s' name='vendor' direction='out'/>"
		"<arg type='s' name='version' direction='out'/>"
		"<arg type='s' name='spec' direction='out'/>"
		"</method>"
		"</interface></node>";
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, NULL);

	(void)name;
	(void)u;
	g_st->notif_conn = c;
	g_dbus_connection_register_object(c, "/org/freedesktop/Notifications",
		info->interfaces[0], &notif_vt, NULL, NULL, NULL);
	g_dbus_node_info_unref(info);
}

static void
own_buses(void)
{
	gchar *host;

	g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.Notifications",
	               G_BUS_NAME_OWNER_FLAGS_REPLACE | G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
	               on_notif_acquired, NULL, NULL, NULL, NULL);
	g_bus_own_name(G_BUS_TYPE_SESSION, "org.kde.StatusNotifierWatcher",
	               G_BUS_NAME_OWNER_FLAGS_REPLACE | G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
	               on_watcher_acquired, NULL, NULL, NULL, NULL);
	host = g_strdup_printf("org.kde.StatusNotifierHost-%d", (int)getpid());
	g_bus_own_name(G_BUS_TYPE_SESSION, host, G_BUS_NAME_OWNER_FLAGS_NONE,
	               NULL, NULL, NULL, NULL, NULL);
	g_free(host);
}

/* ── public ────────────────────────────────────────────────────────── */

GtkWidget *
status_section_new(GtkWidget *panel)
{
	GtkWidget *box;

	g_st = g_new0(Status, 1);
	g_st->panel = panel;
	g_st->next_id = 1;
	backlight_init();
	battery_init();

	g_st->pill = gtk_button_new();
	gtk_button_set_relief(GTK_BUTTON(g_st->pill), GTK_RELIEF_NONE);
	gtk_style_context_add_class(gtk_widget_get_style_context(g_st->pill), "status-pill");

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	g_st->wifi_icon = gtk_image_new_from_icon_name("network-wireless-offline-symbolic", GTK_ICON_SIZE_MENU);
	gtk_image_set_pixel_size(GTK_IMAGE(g_st->wifi_icon), 15);
	g_st->battery_icon = gtk_image_new_from_icon_name("battery-full-symbolic", GTK_ICON_SIZE_MENU);
	gtk_image_set_pixel_size(GTK_IMAGE(g_st->battery_icon), 15);
	gtk_widget_set_no_show_all(g_st->battery_icon, TRUE);
	g_st->clock_label = gtk_label_new("--:--");
	gtk_box_pack_start(GTK_BOX(box), g_st->wifi_icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), g_st->battery_icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), g_st->clock_label, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(g_st->pill), box);

	g_st->panel_window = create_settings_window();
	g_st->notif_window = create_notif_window();
	g_signal_connect(g_st->pill, "clicked", G_CALLBACK(toggle_panel), NULL);

	own_buses();
	refresh_pill();
	g_timeout_add_seconds(5, periodic, NULL);
	return g_st->pill;
}

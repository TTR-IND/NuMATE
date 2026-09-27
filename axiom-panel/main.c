#include "panel.h"

#include <gio/gio.h>
#include <string.h>

#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

static GtkWidget *g_panel_window;
static GtkWidget *g_shelf;

void
panel_dismiss_overlays(void)
{
	menu_dismiss();
	status_dismiss();
}

static gint64 g_hold_dismiss_until;

void
panel_hold_dismiss(int ms)
{
	gint64 now = g_get_monotonic_time();

	if (ms < 0)
		ms = 0;
	g_hold_dismiss_until = now + (gint64)ms * 1000;
}

static gboolean
dismiss_held(void)
{
	return g_get_monotonic_time() < g_hold_dismiss_until;
}

static Window
widget_xid(GtkWidget *w)
{
	GdkWindow *gw;

	if (!w || !gtk_widget_get_realized(w))
		return None;
	gw = gtk_widget_get_window(w);
	return gw ? GDK_WINDOW_XID(gw) : None;
}

static gboolean
xid_is_ours(Window xid)
{
	GtkWidget *menu_w, *sw, *nw;

	if (!xid)
		return FALSE;
	if (xid == widget_xid(g_panel_window))
		return TRUE;
	menu_w = menu_popup_window();
	if (xid == widget_xid(menu_w))
		return TRUE;
	status_overlay_windows(&sw, &nw);
	if (xid == widget_xid(sw) || xid == widget_xid(nw))
		return TRUE;
	return FALSE;
}

static Window
read_active_window(Display *dpy)
{
	Atom net_active, type = None;
	int fmt = 0;
	unsigned long n = 0, extra = 0;
	unsigned char *data = NULL;
	Window w = None;

	net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), net_active,
	                       0, 1, False, XA_WINDOW,
	                       &type, &fmt, &n, &extra, &data) == Success && data && n)
		w = *(Window *)data;
	if (data)
		XFree(data);
	return w;
}

static GdkFilterReturn
root_filter(GdkXEvent *gxev, GdkEvent *gev, gpointer data)
{
	XEvent *ev = (XEvent *)gxev;
	Display *dpy;

	(void)gev;
	(void)data;
	dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());

	/* KeyRelease so auto-repeat on Super does not flutter the menu. */
	if (ev->type == KeyRelease) {
		KeySym sym = XLookupKeysym(&ev->xkey, 0);

		if (sym == XK_Super_L || sym == XK_Super_R ||
		    sym == XK_Meta_L || sym == XK_Meta_R) {
			menu_toggle_apps();
			return GDK_FILTER_REMOVE;
		}
	}
	if (ev->type == PropertyNotify) {
		Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);

		if (ev->xproperty.atom == net_active) {
			Window aw;

			if (dismiss_held())
				return GDK_FILTER_CONTINUE;
			aw = read_active_window(dpy);
			if (aw && !xid_is_ours(aw))
				panel_dismiss_overlays();
		}
	}
	return GDK_FILTER_CONTINUE;
}

static int
x_ignore_badaccess(Display *dpy, XErrorEvent *err)
{
	(void)dpy;
	if (err->error_code == BadAccess)
		return 0;
	return 0;
}

static void
install_dismiss_watch(void)
{
	Display *dpy;
	Window root;
	XWindowAttributes wa;
	int (*prev)(Display *, XErrorEvent *);

	dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
	root = DefaultRootWindow(dpy);
	if (XGetWindowAttributes(dpy, root, &wa))
		XSelectInput(dpy, root, wa.your_event_mask | PropertyChangeMask);

	prev = XSetErrorHandler(x_ignore_badaccess);
	{
		KeyCode l = XKeysymToKeycode(dpy, XK_Super_L);
		KeyCode r = XKeysymToKeycode(dpy, XK_Super_R);
		unsigned int mods[] = { 0, Mod2Mask, LockMask, Mod2Mask | LockMask };
		unsigned int i;

		for (i = 0; i < 4; i++) {
			if (l)
				XGrabKey(dpy, (int)l, mods[i], root, True, GrabModeAsync, GrabModeAsync);
			if (r)
				XGrabKey(dpy, (int)r, mods[i], root, True, GrabModeAsync, GrabModeAsync);
		}
	}
	XSync(dpy, False);
	XSetErrorHandler(prev);

	/* Global filter: a foreign-root GdkWindow does not see grabbed keys. */
	gdk_window_add_filter(NULL, root_filter, NULL);
}

static GtkCssProvider *g_shelf_css;
static int g_shelf_radius = 16;
static int g_shelf_from = 16;
static int g_shelf_to = 16;
static GtkCssProvider *g_appearance_css;
static GSettings *g_iface_settings;
static gboolean g_panel_light;

static const char css[] =
	"window.axiom-panel { background: transparent; }"
	".shelf {"
	"  background: rgba(32, 33, 36, 0.96);"
	"  border-radius: 16px 16px 0 0;"
	"}"
	".card, #GonzoMenu {"
	"  background-color: rgba(32, 33, 36, 0.98);"
	"  border-radius: 16px;"
	"  border: 1px solid rgba(255,255,255,0.08);"
	"  background-image: none;"
	"}"
	".launcher-btn, .launcher-btn:hover, .launcher-btn:active {"
	"  background: none; border: none; box-shadow: none; outline: none; padding: 8px;"
	"}"
	".launcher-btn:hover image { -gtk-icon-transform: rotate(35deg); }"
	".app-btn { border-radius: 50%; border: none; background: transparent; padding: 0; }"
	".app-btn:hover, .app-btn:active { background: transparent; }"
	".running-indicator {"
	"  background: white; min-height: 2px; min-width: 10px;"
	"  border-radius: 1px;"
	"}"
	".status-pill {"
	"  background: rgba(255,255,255,0.05);"
	"  border-radius: 24px;"
	"  padding: 2px 16px;"
	"  margin: 8px 6px;"
	"  border: none;"
	"}"
	".status-pill:hover { background: rgba(255,255,255,0.12); }"
	"label { color: #e8eaed; }"
	"scale { border: none; box-shadow: none; outline: none; background: none; }"
	"scale trough { border: none; box-shadow: none; outline: none; background: rgba(255,255,255,0.2); border-radius: 10px; min-height: 4px; }"
	"scale highlight { background: #B5342A; border: none; box-shadow: none; }"
	"scale contents { background: rgba(255,255,255,0.1); border-radius: 8px; }"
	"scale slider {"
	"  min-width: 22px; min-height: 22px; background: white;"
	"  border-radius: 50%; box-shadow: 0 2px 8px rgba(0,0,0,0.4);"
	"}"
	"#PanelCard {"
	"  background-color: rgba(32, 33, 36, 0.98);"
	"  border-radius: 38px;"
	"  padding: 20px 18px 26px;"
	"  border: 1px solid rgba(255,255,255,0.08);"
	"}"
	".tile {"
	"  background: rgba(255,255,255,0.08);"
	"  border-radius: 24px; border: none; outline: none; color: white;"
	"  font-weight: 500; font-size: 13px;"
	"  transition: background 0.2s, margin 0.1s;"
	"  box-shadow: 0 2px 6px rgba(0,0,0,0.25);"
	"}"
	".tile:active { margin: 2px; background: rgba(255,255,255,0.18); }"
	".tile.active { background: #B5342A; color: white; }"
	".tile.active label { color: white; }"
	".tile label { font-size: 12px; font-weight: 500; color: rgba(255,255,255,0.9); }"
	".action-btn, .power-btn {"
	"  background: rgba(255,255,255,0.07);"
	"  border-radius: 20px; padding: 10px 20px;"
	"  font-weight: 500; font-size: 14px; color: rgba(255,255,255,0.9); border: none;"
	"}"
	"#GonzoMenuSearch {"
	"  background-color: rgba(255,255,255,0.08);"
	"  border-radius: 16px; padding: 6px 12px; color: white; margin: 10px;"
	"}"
	"#GonzoMenuSearch entry {"
	"  background-color: transparent; border: none; box-shadow: none; padding: 0; color: white;"
	"}"
	"#GonzoMenuAppList, #GonzoMenuAppList row, viewport {"
	"  background-color: transparent; border: none; box-shadow: none;"
	"}"
	"#GonzoMenuAppList row:hover { background-color: rgba(255,255,255,0.1); }"
	"#GonzoMenuAppList row:selected { background-color: rgba(255,255,255,0.15); }"
	"#GonzoMenu scrollbar { background: transparent; border: none; }"
	"#GonzoMenu scrollbar slider {"
	"  background: rgba(255,255,255,0.25); border: none; border-radius: 999px;"
	"  min-width: 8px; margin: 4px 12px 4px 3px;"
	"}"
	"#NotifOuter {"
	"  background-color: rgba(32, 33, 36, 0.98);"
	"  border-radius: 38px; border: 1px solid rgba(255,255,255,0.08);"
	"}"
	"#NotifHeader { padding: 16px; }"
	"#NotifTitle { font-size: 16px; font-weight: 500; color: #ffffff; }"
	"#NotifClearBtn {"
	"  font-size: 13px; color: #B5342A; background: rgba(181,52,42,0.14);"
	"  border: none; border-radius: 20px; padding: 6px 14px; font-weight: 500;"
	"}"
	"#NotifClearBtn:hover { background: rgba(181,52,42,0.26); }"
	"#NotifCard {"
	"  background: rgba(255,255,255,0.05); border-radius: 22px;"
	"  margin: 4px 8px; border: 1px solid rgba(255,255,255,0.04);"
	"}"
	"#NotifAppName { font-size: 15px; font-weight: 500; color: #ffffff; }"
	"#NotifAppStatus { font-size: 12px; color: #9aa0a6; }"
	"#NotifBadge {"
	"  background: #B5342A; color: white; border-radius: 20px;"
	"  padding: 2px 8px; font-size: 12px; font-weight: 600;"
	"}"
	"#NotifBubble {"
	"  background: rgba(255,255,255,0.07); border-radius: 18px;"
	"  padding: 12px 14px;"
	"}"
	"#NotifSender { font-size: 13px; font-weight: 600; color: #ffffff; }"
	"#NotifTime { font-size: 11px; color: #8e9297; }"
	"#NotifContent { font-size: 13px; color: #c4c7cc; }"
	"window.dock-preview { background: transparent; }"
	"#DockPreview {"
	"  background-color: rgba(32, 33, 36, 0.98);"
	"  border-radius: 16px;"
	"  border: 1px solid rgba(255,255,255,0.10);"
	"  padding: 10px;"
	"}"
	".thumb-card {"
	"  background: rgba(255,255,255,0.06);"
	"  border-radius: 10px;"
	"  border: 1px solid rgba(255,255,255,0.08);"
	"  padding: 6px;"
	"}"
	".thumb-card:hover { background: rgba(255,255,255,0.12); }"
	".thumb-shot {"
	"  background: rgba(0,0,0,0.45);"
	"  border-radius: 6px;"
	"}"
	".thumb-title {"
	"  font-size: 12px; color: #e8eaed;"
	"  padding: 0 2px;"
	"}"
	".thumb-close {"
	"  padding: 0; min-width: 20px; min-height: 20px;"
	"  background: transparent; border: none; color: #c4c7cc;"
	"}"
	".thumb-close:hover {"
	"  color: #ffffff; background: rgba(181,52,42,0.7); border-radius: 10px;"
	"}";


static const char light_css[] =
	"window.axiom-panel { background: transparent; }"
	".shelf {"
	"  background: rgba(255,255,255,0.78);"
	"}"
	".card, #GonzoMenu {"
	"  background-color: rgba(255,255,255,0.86);"
	"  border: 1px solid rgba(0,0,0,0.08);"
	"  background-image: none;"
	"}"
	".running-indicator { background: #3c4043; }"
	".status-pill {"
	"  background: rgba(255,255,255,0.55);"
	"  border: 1px solid rgba(0,0,0,0.06);"
	"}"
	".status-pill:hover { background: rgba(255,255,255,0.78); }"
	"label { color: #3c4043; }"
	"scale trough { background: rgba(0,0,0,0.14); }"
	"scale contents { background: rgba(0,0,0,0.08); }"
	"scale slider { background: #ffffff; box-shadow: 0 2px 8px rgba(0,0,0,0.18); }"
	"#PanelCard, #NotifOuter, #DockPreview {"
	"  background-color: rgba(255,255,255,0.88);"
	"  border: 1px solid rgba(255,255,255,0.7);"
	"}"
	".tile {"
	"  background: rgba(255,255,255,0.72);"
	"  color: #3c4043;"
	"  box-shadow: 0 1px 3px rgba(0,0,0,0.08);"
	"}"
	".tile:active { background: rgba(255,255,255,0.95); }"
	".tile.active { background: #B5342A; color: white; }"
	".tile.active label { color: white; }"
	".tile label { color: #3c4043; }"
	".action-btn, .power-btn {"
	"  background: rgba(255,255,255,0.7); color: #3c4043;"
	"}"
	"#GonzoMenuSearch {"
	"  background-color: rgba(255,255,255,0.7);"
	"  color: #3c4043;"
	"}"
	"#GonzoMenuSearch entry { color: #3c4043; }"
	"#GonzoMenuAppList row:hover { background-color: rgba(0,0,0,0.05); }"
	"#GonzoMenuAppList row:selected { background-color: rgba(0,0,0,0.08); }"
	"#GonzoMenu scrollbar slider { background: rgba(0,0,0,0.22); }"
	"#NotifTitle, #NotifAppName, #NotifSender { color: #202124; }"
	"#NotifAppStatus, #NotifTime { color: #5f6368; }"
	"#NotifContent { color: #3c4043; }"
	"#NotifCard {"
	"  background: rgba(255,255,255,0.55);"
	"  border: 1px solid rgba(0,0,0,0.05);"
	"}"
	"#NotifBubble { background: rgba(255,255,255,0.65); }"
	".thumb-card {"
	"  background: rgba(255,255,255,0.7);"
	"  border: 1px solid rgba(0,0,0,0.06);"
	"}"
	".thumb-title { color: #202124; }"
	".thumb-close { color: #5f6368; }"
	".thumb-shot { background: rgba(0,0,0,0.08); }";


static gboolean
gtk_theme_is_light(void)
{
	gchar *name;
	gboolean light = FALSE;

	if (!g_iface_settings)
		return FALSE;
	name = g_settings_get_string(g_iface_settings, "gtk-theme");
	if (name && strstr(name, "Light"))
		light = TRUE;
	g_free(name);
	return light;
}

static void
panel_apply_appearance(gboolean light)
{
	if (!g_appearance_css) {
		g_appearance_css = gtk_css_provider_new();
		gtk_style_context_add_provider_for_screen(
			gdk_screen_get_default(),
			GTK_STYLE_PROVIDER(g_appearance_css),
			GTK_STYLE_PROVIDER_PRIORITY_USER);
	}
	g_panel_light = light;
	g_object_set(gtk_settings_get_default(),
	             "gtk-application-prefer-dark-theme", !light,
	             NULL);
	gtk_css_provider_load_from_data(g_appearance_css,
	                                light ? light_css : "", -1, NULL);
}

static void
on_gtk_theme_changed(GSettings *s, gchar *key, gpointer u)
{
	(void)s;
	(void)key;
	(void)u;
	panel_apply_appearance(gtk_theme_is_light());
}

void
panel_apply_css(void)
{
	GtkCssProvider *provider;
	const char *schemas[] = { "org.mate.interface", "org.gnome.desktop.interface", NULL };
	int i;

	provider = gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(
		gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(provider),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);

	for (i = 0; schemas[i]; i++) {
		GSettingsSchemaSource *src = g_settings_schema_source_get_default();
		GSettingsSchema *sch = g_settings_schema_source_lookup(src, schemas[i], TRUE);

		if (!sch)
			continue;
		if (g_settings_schema_has_key(sch, "gtk-theme")) {
			g_iface_settings = g_settings_new(schemas[i]);
			g_settings_schema_unref(sch);
			break;
		}
		g_settings_schema_unref(sch);
	}
	if (g_iface_settings)
		g_signal_connect(g_iface_settings, "changed::gtk-theme",
		                 G_CALLBACK(on_gtk_theme_changed), NULL);
	panel_apply_appearance(gtk_theme_is_light());
}

void
panel_ensure_rgba(GtkWidget *win)
{
	GdkVisual *visual = gdk_screen_get_rgba_visual(gdk_screen_get_default());

	if (visual)
		gtk_widget_set_visual(win, visual);
	gtk_widget_set_app_paintable(win, TRUE);
}

static void
shelf_apply_radius(int r)
{
	char buf[96];

	if (r < 0)
		r = 0;
	if (r > 16)
		r = 16;
	g_shelf_radius = r;
	if (!g_shelf_css) {
		g_shelf_css = gtk_css_provider_new();
		gtk_style_context_add_provider_for_screen(
			gdk_screen_get_default(),
			GTK_STYLE_PROVIDER(g_shelf_css),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	g_snprintf(buf, sizeof buf, ".shelf { border-radius: %dpx %dpx 0 0; }", r, r);
	gtk_css_provider_load_from_data(g_shelf_css, buf, -1, NULL);
}

static void
shelf_radius_tick(double t, gpointer user)
{
	(void)user;
	shelf_apply_radius(g_shelf_from + (int)((g_shelf_to - g_shelf_from) * t));
}

void
panel_set_shelf_rounded(GtkWidget *shelf, gboolean rounded)
{
	int target = rounded ? 16 : 0;

	if (!shelf)
		return;
	if (!gtk_style_context_has_class(gtk_widget_get_style_context(shelf), "shelf"))
		gtk_style_context_add_class(gtk_widget_get_style_context(shelf), "shelf");
	if (target == g_shelf_to)
		return;
	g_shelf_from = g_shelf_radius;
	g_shelf_to = target;
	anim_run(shelf, 280, shelf_radius_tick, NULL, NULL);
}

gboolean
panel_primary_geo(GdkRectangle *geo)
{
	GdkDisplay *display;
	GdkMonitor *monitor;

	display = gdk_display_get_default();
	monitor = gdk_display_get_primary_monitor(display);
	if (!monitor)
		monitor = gdk_display_get_monitor(display, 0);
	if (!monitor)
		return FALSE;
	gdk_monitor_get_geometry(monitor, geo);
	return TRUE;
}

void
panel_place_bottom(GtkWidget *window)
{
	GdkRectangle geo;

	if (!panel_primary_geo(&geo))
		return;
	gtk_widget_set_size_request(window, geo.width, PANEL_HEIGHT);
	gtk_window_set_default_size(GTK_WINDOW(window), geo.width, PANEL_HEIGHT);
	gtk_window_resize(GTK_WINDOW(window), geo.width, PANEL_HEIGHT);
	gtk_window_move(GTK_WINDOW(window), geo.x, geo.y + geo.height - PANEL_HEIGHT);
}

void
panel_apply_strut(GtkWidget *window)
{
	GdkWindow *gw;
	Display *dpy;
	Window xid;
	Atom strut, strut_partial, typedock, wintype;
	long s[4] = { 0 };
	long p[12] = { 0 };
	GdkRectangle geo;

	gw = gtk_widget_get_window(window);
	if (!gw)
		return;

	dpy = GDK_WINDOW_XDISPLAY(gw);
	xid = GDK_WINDOW_XID(gw);

	if (panel_primary_geo(&geo)) {
		p[3] = s[3] = PANEL_HEIGHT;
		p[10] = geo.x;
		p[11] = geo.x + geo.width - 1;
	} else {
		p[3] = s[3] = PANEL_HEIGHT;
	}

	strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
	strut_partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
	wintype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	typedock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);

	XChangeProperty(dpy, xid, strut, XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)s, 4);
	XChangeProperty(dpy, xid, strut_partial, XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)p, 12);
	XChangeProperty(dpy, xid, wintype, XA_ATOM, 32,
	                PropModeReplace, (unsigned char *)&typedock, 1);
	XSync(dpy, False);
}

static gboolean
atom_list_has(Display *dpy, Window w, Atom prop, Atom needle)
{
	Atom type = None;
	int fmt = 0;
	unsigned long n = 0, extra = 0, i;
	unsigned char *data = NULL;
	gboolean found = FALSE;

	if (XGetWindowProperty(dpy, w, prop, 0, 64, False, XA_ATOM,
	                       &type, &fmt, &n, &extra, &data) != Success)
		return FALSE;
	if (type == XA_ATOM && data) {
		Atom *atoms = (Atom *)data;

		for (i = 0; i < n; i++) {
			if (atoms[i] == needle) {
				found = TRUE;
				break;
			}
		}
	}
	if (data)
		XFree(data);
	return found;
}

static unsigned long
cardinal_prop(Display *dpy, Window w, Atom prop)
{
	Atom type = None;
	int fmt = 0;
	unsigned long n = 0, extra = 0;
	unsigned char *data = NULL;
	unsigned long value = 0xFFFFFFFFul;

	if (XGetWindowProperty(dpy, w, prop, 0, 1, False, XA_CARDINAL,
	                       &type, &fmt, &n, &extra, &data) == Success && data && n)
		value = *(unsigned long *)data;
	if (data)
		XFree(data);
	return value;
}

/* Flat shelf when any non-minimized task on this desktop is maximized.
 * Not the focused window — the whole workspace. */
static gboolean
any_visible_maximized(void)
{
	Display *dpy;
	Window root;
	Atom net_list, net_state, max_v, max_h, hidden, skip;
	Atom type_atom, dock, desktop, splash, net_desk, net_cur;
	Atom type = None;
	int fmt = 0;
	unsigned long n = 0, extra = 0, i;
	unsigned char *data = NULL;
	Window *list;
	unsigned long cur_desk;

	dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
	root = DefaultRootWindow(dpy);
	net_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	net_state = XInternAtom(dpy, "_NET_WM_STATE", False);
	max_v = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
	max_h = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
	hidden = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
	skip = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
	type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
	desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
	splash = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
	net_desk = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
	net_cur = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);

	cur_desk = cardinal_prop(dpy, root, net_cur);

	if (XGetWindowProperty(dpy, root, net_list, 0, 4096, False, XA_WINDOW,
	                       &type, &fmt, &n, &extra, &data) != Success || !data)
		return FALSE;
	list = (Window *)data;
	for (i = 0; i < n; i++) {
		Window w = list[i];
		unsigned long desk;

		if (atom_list_has(dpy, w, net_state, skip))
			continue;
		if (atom_list_has(dpy, w, type_atom, dock) ||
		    atom_list_has(dpy, w, type_atom, desktop) ||
		    atom_list_has(dpy, w, type_atom, splash))
			continue;
		if (atom_list_has(dpy, w, net_state, hidden))
			continue;
		desk = cardinal_prop(dpy, w, net_desk);
		if (desk != 0xFFFFFFFFul && cur_desk != 0xFFFFFFFFul && desk != cur_desk)
			continue;
		if (atom_list_has(dpy, w, net_state, max_v) &&
		    atom_list_has(dpy, w, net_state, max_h)) {
			XFree(data);
			return TRUE;
		}
	}
	XFree(data);
	return FALSE;
}

static gboolean
tick_rounding(gpointer unused)
{
	panel_set_shelf_rounded(g_shelf, !any_visible_maximized());
	return G_SOURCE_CONTINUE;
}

static void
on_realize(GtkWidget *window, gpointer unused)
{
	panel_apply_strut(window);
	/*
	 * mate-session holds the Panel phase until every app in that
	 * phase RegisterClient()s, or GSM_MANAGER_PHASE_TIMEOUT (30s)
	 * fires. Without this call the Desktop phase — nemo-desktop —
	 * cannot start until that timeout.
	 */
	panel_session_register();
	install_dismiss_watch();
}

static void
on_monitors_changed(GdkDisplay *display, gpointer window)
{
	panel_place_bottom(GTK_WIDGET(window));
	if (gtk_widget_get_realized(GTK_WIDGET(window)))
		panel_apply_strut(GTK_WIDGET(window));
}

int
main(int argc, char **argv)
{
	GtkWidget *window;
	GtkWidget *shelf;
	GdkDisplay *display;

	gtk_init(&argc, &argv);
	panel_apply_css();

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_panel_window = window;
	panel_ensure_rgba(window);
	gtk_widget_set_name(window, "axiom-panel");
	gtk_style_context_add_class(gtk_widget_get_style_context(window), "axiom-panel");
	gtk_window_set_title(GTK_WINDOW(window), "axiom-shell");
	gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
	gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
	gtk_window_stick(GTK_WINDOW(window));
	gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
	gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_DOCK);

	shelf = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	g_shelf = shelf;
	gtk_widget_set_hexpand(shelf, TRUE);
	gtk_widget_set_vexpand(shelf, TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(shelf), "shelf");
	gtk_container_add(GTK_CONTAINER(window), shelf);

	gtk_box_pack_start(GTK_BOX(shelf), menu_section_new(window), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(shelf), windows_section_new(window), TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(shelf), status_section_new(window), FALSE, FALSE, 0);

	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(window, "realize", G_CALLBACK(on_realize), NULL);

	display = gdk_display_get_default();
	g_signal_connect(display, "monitor-added", G_CALLBACK(on_monitors_changed), window);
	g_signal_connect(display, "monitor-removed", G_CALLBACK(on_monitors_changed), window);

	panel_place_bottom(window);
	gtk_widget_show_all(window);
	g_timeout_add_seconds(1, tick_rounding, NULL);
	gtk_main();
	return 0;
}

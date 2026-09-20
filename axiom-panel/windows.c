#include "panel.h"

#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gio/gdesktopappinfo.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

/*
 * One dock slot per application, not per X window.
 *
 * A slot's identity is a desktop-file id when we can resolve one
 * (pinned ids win if their Exec / WM class / Gtk app-id match the
 * window). Unresolvable windows fall back to "class:<WM_CLASS>".
 * Every window that shares that key lives on the same slot. The
 * icon and label stay the application's. Window titles live on
 * the hover preview, nowhere else.
 */

typedef struct AppSlot AppSlot;

typedef struct {
	Display   *dpy;
	Window     root;
	Window     panel_xid;
	GtkWidget *box;
	GtkWidget *panel;
	GHashTable *slots;      /* identity key -> AppSlot */
	GHashTable *xid_index;  /* Window -> AppSlot (unowned) */
	Atom net_client_list;
	Atom net_active_window;
	Atom net_wm_name;
	Atom net_wm_icon;
	Atom net_wm_state;
	Atom net_wm_window_type;
	Atom net_wm_pid;
	Atom gtk_app_id;
	Atom utf8;
	Atom skip_taskbar;
	Atom type_dock;
	Atom type_desktop;
	Atom type_splash;
	Atom wm_change_state;
	Atom net_close;
	Atom net_wm_desktop;
	Atom state_add;
	Atom max_v;
	Atom max_h;
	Atom above;
	char *pinned_ids[64];
	int pinned_count;
	GtkWidget *drag_widget;
	GtkWidget *drag_ghost;
	GAppInfo *drag_app;
	gboolean drag_active;
	double drag_x;
	double drag_y;
	int drag_insert;
	GtkWidget *preview;
	AppSlot *preview_slot;
	guint preview_show_id;
	guint preview_hide_id;
} Windows;

struct AppSlot {
	char *key;
	GAppInfo *app;
	GArray *xids; /* Window */
	GtkWidget *widget;
};

typedef struct {
	GtkWidget *widget;
	double gap_s, gap_e;
	double gap_s_t, gap_e_t;
	guint gap_anim;
} DockSlot;

static Windows *g_win;

static void preview_hide(Windows *s);
static void preview_show(Windows *s, AppSlot *slot);
static gboolean on_preview_show_timeout(gpointer user);
static void refresh(Windows *s);

static gboolean
prop_atom_has(Display *dpy, Window w, Atom prop, Atom needle)
{
	Atom type = None;
	int fmt = 0;
	unsigned long n = 0, extra = 0;
	unsigned char *data = NULL;
	unsigned long i;
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

static gboolean
is_taskable(Windows *s, Window w)
{
	XWindowAttributes attr;

	if (w == s->panel_xid)
		return FALSE;
	gdk_x11_display_error_trap_push(gdk_display_get_default());
	if (!XGetWindowAttributes(s->dpy, w, &attr)) {
		gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
		return FALSE;
	}
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());

	if (prop_atom_has(s->dpy, w, s->net_wm_state, s->skip_taskbar))
		return FALSE;
	if (prop_atom_has(s->dpy, w, s->net_wm_window_type, s->type_dock))
		return FALSE;
	if (prop_atom_has(s->dpy, w, s->net_wm_window_type, s->type_desktop))
		return FALSE;
	if (prop_atom_has(s->dpy, w, s->net_wm_window_type, s->type_splash))
		return FALSE;
	return TRUE;
}

static char *
window_title(Windows *s, Window w)
{
	Atom type;
	int fmt;
	unsigned long n, extra;
	unsigned char *data = NULL;
	char *out = NULL;

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	if (XGetWindowProperty(s->dpy, w, s->net_wm_name, 0, 1024, False, s->utf8,
	                       &type, &fmt, &n, &extra, &data) == Success && data && n) {
		out = g_strndup((char *)data, n);
	}
	if (data)
		XFree(data);
	if (!out) {
		XTextProperty tp;

		if (XGetWMName(s->dpy, w, &tp) && tp.value) {
			out = g_strdup((char *)tp.value);
			XFree(tp.value);
		}
	}
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
	if (!out)
		out = g_strdup("?");
	return out;
}

static GdkPixbuf *
window_icon(Windows *s, Window w)
{
	Atom type;
	int fmt;
	unsigned long n, extra;
	unsigned char *data = NULL;
	gulong *p;
	int width, height, i;
	GdkPixbuf *pb, *scaled;
	guchar *pixels;

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	if (XGetWindowProperty(s->dpy, w, s->net_wm_icon, 0, 256 * 256 + 2, False, XA_CARDINAL,
	                       &type, &fmt, &n, &extra, &data) != Success || !data || n < 3) {
		if (data)
			XFree(data);
		gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
		return NULL;
	}
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());

	p = (gulong *)data;
	width = (int)p[0];
	height = (int)p[1];
	if (width <= 0 || height <= 0 || (unsigned long)(width * height + 2) > n) {
		XFree(data);
		return NULL;
	}

	pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
	pixels = gdk_pixbuf_get_pixels(pb);
	for (i = 0; i < width * height; i++) {
		guint32 argb = (guint32)p[2 + i];
		pixels[i * 4 + 0] = (argb >> 16) & 0xff;
		pixels[i * 4 + 1] = (argb >> 8) & 0xff;
		pixels[i * 4 + 2] = argb & 0xff;
		pixels[i * 4 + 3] = (argb >> 24) & 0xff;
	}
	XFree(data);

	if (width >= PANEL_ICON + NUDGE_PX && height >= PANEL_ICON + NUDGE_PX)
		return pb;
	scaled = gdk_pixbuf_scale_simple(pb, PANEL_ICON + NUDGE_PX, PANEL_ICON + NUDGE_PX, GDK_INTERP_BILINEAR);
	g_object_unref(pb);
	return scaled;
}

static Window
active_window(Windows *s)
{
	Atom type;
	int fmt;
	unsigned long n, extra;
	unsigned char *data = NULL;
	Window w = None;

	if (XGetWindowProperty(s->dpy, s->root, s->net_active_window, 0, 1, False, XA_WINDOW,
	                       &type, &fmt, &n, &extra, &data) == Success && data && n)
		w = *(Window *)data;
	if (data)
		XFree(data);
	return w;
}

static void
activate_or_iconify(Windows *s, Window w)
{
	XClientMessageEvent ev;
	Window active;

	memset(&ev, 0, sizeof ev);
	active = active_window(s);

	if (active == w) {
		ev.type = ClientMessage;
		ev.window = w;
		ev.message_type = s->wm_change_state;
		ev.format = 32;
		ev.data.l[0] = IconicState;
		XSendEvent(s->dpy, s->root, False,
		           SubstructureNotifyMask | SubstructureRedirectMask, (XEvent *)&ev);
	} else {
		ev.type = ClientMessage;
		ev.window = w;
		ev.message_type = s->net_active_window;
		ev.format = 32;
		ev.data.l[0] = 2;
		ev.data.l[1] = CurrentTime;
		ev.data.l[2] = s->panel_xid;
		XSendEvent(s->dpy, s->root, False,
		           SubstructureNotifyMask | SubstructureRedirectMask, (XEvent *)&ev);
	}
	XSync(s->dpy, False);
}

static void
activate_window(Windows *s, Window w)
{
	XClientMessageEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.type = ClientMessage;
	ev.window = w;
	ev.message_type = s->net_active_window;
	ev.format = 32;
	ev.data.l[0] = 2;
	ev.data.l[1] = CurrentTime;
	ev.data.l[2] = s->panel_xid;
	XSendEvent(s->dpy, s->root, False,
	           SubstructureNotifyMask | SubstructureRedirectMask, (XEvent *)&ev);
	XSync(s->dpy, False);
}

typedef struct {
	GtkImage *img;
	int start, end, steps, count;
} TweenData;

static int
icon_shown_size(GtkImage *image)
{
	int stored = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(image), "nudge-size"));
	int pixel;

	if (stored > 0)
		return stored;
	pixel = gtk_image_get_pixel_size(image);
	return pixel > 0 ? pixel : PANEL_ICON;
}

static void
apply_icon_size(GtkImage *image, int size)
{
	GdkPixbuf *src;

	if (size < 1)
		size = 1;
	g_object_set_data(G_OBJECT(image), "nudge-size", GINT_TO_POINTER(size));
	src = g_object_get_data(G_OBJECT(image), "src-pixbuf");
	if (src) {
		GdkPixbuf *frame = gdk_pixbuf_scale_simple(src, size, size, GDK_INTERP_BILINEAR);

		gtk_image_set_from_pixbuf(image, frame);
		g_object_unref(frame);
		return;
	}
	gtk_image_set_pixel_size(image, size);
}

static gboolean
nudge_tick(gpointer data)
{
	TweenData *tween = data;
	double progress, eased;
	int size;

	if (!GTK_IS_IMAGE(tween->img))
		return G_SOURCE_REMOVE;
	tween->count++;
	progress = (double)tween->count / tween->steps;
	eased = sin(progress * (M_PI / 2.0));
	size = tween->start + (int)((tween->end - tween->start) * eased);
	apply_icon_size(tween->img, size);
	if (tween->count >= tween->steps) {
		apply_icon_size(tween->img, tween->end);
		g_object_set_data(G_OBJECT(tween->img), "tween-data", NULL);
		g_object_set_data(G_OBJECT(tween->img), "tween-timer", NULL);
		g_free(tween);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void
nudge_icon(GtkImage *image, gboolean entering)
{
	guint timer_id;
	TweenData *old, *tween;

	timer_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(image), "tween-timer"));
	if (timer_id)
		g_source_remove(timer_id);
	old = g_object_get_data(G_OBJECT(image), "tween-data");
	g_free(old);

	tween = g_new0(TweenData, 1);
	tween->img = image;
	tween->start = icon_shown_size(image);
	tween->end = entering ? (PANEL_ICON + NUDGE_PX) : PANEL_ICON;
	tween->steps = 6;
	g_object_set_data(G_OBJECT(image), "tween-data", tween);
	g_object_set_data(G_OBJECT(image), "tween-timer",
	                  GUINT_TO_POINTER(g_timeout_add(16, nudge_tick, tween)));
}

static void
on_image_destroy(GtkWidget *widget, gpointer u)
{
	guint timer_id;
	TweenData *tween;

	(void)u;
	timer_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "tween-timer"));
	if (timer_id)
		g_source_remove(timer_id);
	tween = g_object_get_data(G_OBJECT(widget), "tween-data");
	g_free(tween);
}

static gchar *
pins_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "axiom-shell", "pinned", NULL);
}

static void
pins_save(Windows *s)
{
	gchar *dir, *path, *body;
	GString *out;
	int i;

	path = pins_path();
	dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	out = g_string_new(NULL);
	for (i = 0; i < s->pinned_count; i++) {
		g_string_append(out, s->pinned_ids[i]);
		g_string_append_c(out, '\n');
	}
	body = g_string_free(out, FALSE);
	g_file_set_contents(path, body, -1, NULL);
	g_free(body);
	g_free(dir);
	g_free(path);
}

static void
pins_load(Windows *s)
{
	gchar *path, *txt = NULL;
	gchar **lines;
	int i;

	path = pins_path();
	if (!g_file_get_contents(path, &txt, NULL, NULL) || !txt) {
		g_free(path);
		return;
	}
	lines = g_strsplit(txt, "\n", -1);
	for (i = 0; lines[i] && s->pinned_count < 64; i++) {
		g_strstrip(lines[i]);
		if (lines[i][0])
			s->pinned_ids[s->pinned_count++] = g_strdup(lines[i]);
	}
	g_strfreev(lines);
	g_free(txt);
	g_free(path);
}

gboolean
dock_is_pinned(const char *desktop_id)
{
	int i;

	if (!g_win || !desktop_id)
		return FALSE;
	for (i = 0; i < g_win->pinned_count; i++)
		if (g_strcmp0(g_win->pinned_ids[i], desktop_id) == 0)
			return TRUE;
	return FALSE;
}

void
dock_pin(const char *desktop_id)
{
	if (!g_win || !desktop_id || !*desktop_id)
		return;
	if (dock_is_pinned(desktop_id))
		return;
	if (g_win->pinned_count >= 64)
		return;
	g_win->pinned_ids[g_win->pinned_count++] = g_strdup(desktop_id);
	pins_save(g_win);
	refresh(g_win);
}

void
dock_unpin(const char *desktop_id)
{
	int i;

	if (!g_win || !desktop_id)
		return;
	for (i = 0; i < g_win->pinned_count; i++) {
		if (g_strcmp0(g_win->pinned_ids[i], desktop_id) != 0)
			continue;
		g_free(g_win->pinned_ids[i]);
		for (; i < g_win->pinned_count - 1; i++)
			g_win->pinned_ids[i] = g_win->pinned_ids[i + 1];
		g_win->pinned_count--;
		pins_save(g_win);
		refresh(g_win);
		return;
	}
}

static void
ewmh_client(Windows *s, Window w, Atom type, long a, long b, long c)
{
	XClientMessageEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.type = ClientMessage;
	ev.window = w;
	ev.message_type = type;
	ev.format = 32;
	ev.data.l[0] = a;
	ev.data.l[1] = b;
	ev.data.l[2] = c;
	XSendEvent(s->dpy, s->root, False,
	           SubstructureNotifyMask | SubstructureRedirectMask, (XEvent *)&ev);
	XSync(s->dpy, False);
}

static void
on_menu_launch(GtkMenuItem *item, gpointer app)
{
	(void)item;
	if (app)
		g_app_info_launch(G_APP_INFO(app), NULL, NULL, NULL);
}

static void
on_menu_pin(GtkMenuItem *item, gpointer app)
{
	const char *id;

	(void)item;
	id = app ? g_app_info_get_id(G_APP_INFO(app)) : NULL;
	if (id)
		dock_pin(id);
}

static void
on_menu_unpin(GtkMenuItem *item, gpointer app)
{
	const char *id;

	(void)item;
	id = app ? g_app_info_get_id(G_APP_INFO(app)) : NULL;
	if (id)
		dock_unpin(id);
}

static void
on_menu_close_all(GtkMenuItem *item, gpointer slot_widget)
{
	AppSlot *slot;
	guint i;

	(void)item;
	if (!g_win || !slot_widget)
		return;
	slot = g_object_get_data(G_OBJECT(slot_widget), "app-slot");
	if (!slot)
		return;
	for (i = 0; i < slot->xids->len; i++)
		ewmh_client(g_win, g_array_index(slot->xids, Window, i),
		            g_win->net_close, CurrentTime, 2, 0);
}

static void
class_hint(Windows *s, Window w, char **res_name, char **res_class)
{
	XClassHint hint;

	*res_name = NULL;
	*res_class = NULL;
	memset(&hint, 0, sizeof hint);
	gdk_x11_display_error_trap_push(gdk_display_get_default());
	if (XGetClassHint(s->dpy, w, &hint)) {
		if (hint.res_name)
			*res_name = g_strdup(hint.res_name);
		if (hint.res_class)
			*res_class = g_strdup(hint.res_class);
		if (hint.res_name)
			XFree(hint.res_name);
		if (hint.res_class)
			XFree(hint.res_class);
	}
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
}

static char *
window_gtk_app_id(Windows *s, Window w)
{
	Atom type = None;
	int fmt = 0;
	unsigned long n = 0, extra = 0;
	unsigned char *data = NULL;
	char *out = NULL;

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	if (XGetWindowProperty(s->dpy, w, s->gtk_app_id, 0, 256, False, s->utf8,
	                       &type, &fmt, &n, &extra, &data) == Success && data && n)
		out = g_strndup((char *)data, n);
	if (data)
		XFree(data);
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
	return out;
}

static char *
window_exec_base(Windows *s, Window w)
{
	Atom type = None;
	int fmt = 0;
	unsigned long n = 0, extra = 0;
	unsigned char *data = NULL;
	char path[64], dest[256];
	ssize_t len;
	unsigned long pid = 0;

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	if (XGetWindowProperty(s->dpy, w, s->net_wm_pid, 0, 1, False, XA_CARDINAL,
	                       &type, &fmt, &n, &extra, &data) == Success && data && n)
		pid = *(unsigned long *)data;
	if (data)
		XFree(data);
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
	if (!pid)
		return NULL;
	g_snprintf(path, sizeof path, "/proc/%lu/exe", pid);
	len = readlink(path, dest, sizeof dest - 1);
	if (len <= 0)
		return NULL;
	dest[len] = '\0';
	return g_path_get_basename(dest);
}

static gboolean
same_token(const char *a, const char *b)
{
	return a && b && a[0] && b[0] && !g_ascii_strcasecmp(a, b);
}

static gboolean
app_matches_hints(GAppInfo *app, const char *res_name, const char *res_class,
                  const char *exec_base, const char *gtk_id)
{
	GDesktopAppInfo *dai;
	const char *id, *startup, *exec, *dot;
	char *stem, *base;
	gboolean hit = FALSE;

	if (!app || !G_IS_DESKTOP_APP_INFO(app))
		return FALSE;
	dai = G_DESKTOP_APP_INFO(app);
	id = g_app_info_get_id(app);
	startup = g_desktop_app_info_get_startup_wm_class(dai);
	if (same_token(startup, res_class) || same_token(startup, res_name))
		return TRUE;
	if (gtk_id && id) {
		char *want = g_strdup_printf("%s.desktop", gtk_id);

		hit = !g_ascii_strcasecmp(id, want);
		g_free(want);
		if (hit)
			return TRUE;
	}
	if (id && (res_class || res_name)) {
		dot = strstr(id, ".desktop");
		stem = dot ? g_strndup(id, (gsize)(dot - id)) : g_strdup(id);
		hit = same_token(stem, res_class) || same_token(stem, res_name);
		g_free(stem);
		if (hit)
			return TRUE;
	}
	exec = g_app_info_get_executable(app);
	if (exec && (exec_base || res_class || res_name)) {
		base = g_path_get_basename(exec);
		hit = same_token(base, exec_base) ||
		      same_token(base, res_class) ||
		      same_token(base, res_name);
		g_free(base);
	}
	return hit;
}

static GAppInfo *
app_from_hints(const char *res_name, const char *res_class,
               const char *exec_base, const char *gtk_id)
{
	GList *all, *l;
	GAppInfo *found = NULL;

	all = g_app_info_get_all();
	for (l = all; l; l = l->next) {
		if (!G_IS_DESKTOP_APP_INFO(l->data))
			continue;
		if (!app_matches_hints(l->data, res_name, res_class, exec_base, gtk_id))
			continue;
		found = g_object_ref(l->data);
		break;
	}
	g_list_free_full(all, g_object_unref);
	return found;
}

static char *
identity_for_window(Windows *s, Window w, GAppInfo **out_app)
{
	char *res_name = NULL, *res_class = NULL, *exec_base, *gtk_id, *key = NULL;
	GAppInfo *app = NULL;
	int i;

	*out_app = NULL;
	class_hint(s, w, &res_name, &res_class);
	exec_base = window_exec_base(s, w);
	gtk_id = window_gtk_app_id(s, w);

	/* A pin the user already chose wins over whatever desktop file
	 * g_app_info_get_all() happens to return first. That is the
	 * Nemo case: folder windows must stay on nemo.desktop / "Files". */
	for (i = 0; i < s->pinned_count; i++) {
		GDesktopAppInfo *dai = g_desktop_app_info_new(s->pinned_ids[i]);

		if (!dai)
			continue;
		if (app_matches_hints(G_APP_INFO(dai), res_name, res_class, exec_base, gtk_id)) {
			app = G_APP_INFO(dai);
			key = g_strdup(s->pinned_ids[i]);
			break;
		}
		g_object_unref(dai);
	}

	if (!key) {
		app = app_from_hints(res_name, res_class, exec_base, gtk_id);
		if (app && g_app_info_get_id(app))
			key = g_strdup(g_app_info_get_id(app));
	}

	if (!key && res_class)
		key = g_strdup_printf("class:%s", res_class);
	else if (!key && res_name)
		key = g_strdup_printf("class:%s", res_name);
	else if (!key)
		key = g_strdup_printf("xid:%lu", (unsigned long)w);

	g_free(res_name);
	g_free(res_class);
	g_free(exec_base);
	g_free(gtk_id);
	*out_app = app;
	return key;
}

static AppSlot *
slot_of_widget(GtkWidget *widget)
{
	return widget ? g_object_get_data(G_OBJECT(widget), "app-slot") : NULL;
}

static void
popup_window_menu(GtkWidget *slot_w, GdkEventButton *event)
{
	GtkWidget *menu, *item;
	AppSlot *slot;
	GAppInfo *app;
	const char *id;

	slot = slot_of_widget(slot_w);
	app = slot ? slot->app : g_object_get_data(G_OBJECT(slot_w), "app_info");
	id = app ? g_app_info_get_id(app) : NULL;

	menu = gtk_menu_new();
	if (id) {
		if (dock_is_pinned(id)) {
			item = gtk_menu_item_new_with_label("Unpin from Dock");
			g_signal_connect(item, "activate", G_CALLBACK(on_menu_unpin), app);
		} else {
			item = gtk_menu_item_new_with_label("Pin to Dock");
			g_signal_connect(item, "activate", G_CALLBACK(on_menu_pin), app);
		}
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}
	if (app) {
		if (id)
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
		item = gtk_menu_item_new_with_label("Open New Window");
		g_signal_connect(item, "activate", G_CALLBACK(on_menu_launch), app);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}
	if (slot && slot->xids->len) {
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
		item = gtk_menu_item_new_with_label(
			slot->xids->len > 1 ? "Close All Windows" : "Close");
		g_signal_connect(item, "activate", G_CALLBACK(on_menu_close_all), slot_w);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}

	gtk_widget_show_all(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

void
dock_popup_app_menu(GtkWidget *anchor, GAppInfo *app, GdkEventButton *event)
{
	GtkWidget *menu, *item;
	const char *id;

	(void)anchor;
	if (!app)
		return;
	menu = gtk_menu_new();
	id = g_app_info_get_id(app);
	if (id && dock_is_pinned(id)) {
		item = gtk_menu_item_new_with_label("Unpin from Dock");
		g_signal_connect(item, "activate", G_CALLBACK(on_menu_unpin), app);
	} else {
		item = gtk_menu_item_new_with_label("Pin to Dock");
		g_signal_connect(item, "activate", G_CALLBACK(on_menu_pin), app);
	}
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	item = gtk_menu_item_new_with_label("Open New Window");
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_launch), app);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	gtk_widget_show_all(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

static void
dock_slot_free(gpointer p)
{
	DockSlot *slot = p;

	if (slot->gap_anim)
		g_source_remove(slot->gap_anim);
	g_free(slot);
}

static DockSlot *
dock_slot_of(GtkWidget *widget)
{
	DockSlot *slot = g_object_get_data(G_OBJECT(widget), "dock-slot");

	if (slot)
		return slot;
	slot = g_new0(DockSlot, 1);
	slot->widget = widget;
	g_object_set_data_full(G_OBJECT(widget), "dock-slot", slot, dock_slot_free);
	return slot;
}

static gboolean
gap_tick(gpointer data)
{
	DockSlot *slot = data;
	const double speed = 0.3;

	if (!GTK_IS_WIDGET(slot->widget))
		return G_SOURCE_REMOVE;
	slot->gap_s += (slot->gap_s_t - slot->gap_s) * speed;
	slot->gap_e += (slot->gap_e_t - slot->gap_e) * speed;
	gtk_widget_set_margin_start(slot->widget, (int)slot->gap_s);
	gtk_widget_set_margin_end(slot->widget, (int)slot->gap_e);
	if (fabs(slot->gap_s - slot->gap_s_t) < 1 && fabs(slot->gap_e - slot->gap_e_t) < 1) {
		slot->gap_s = slot->gap_s_t;
		slot->gap_e = slot->gap_e_t;
		gtk_widget_set_margin_start(slot->widget, (int)slot->gap_s);
		gtk_widget_set_margin_end(slot->widget, (int)slot->gap_e);
		slot->gap_anim = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void
gap_run(DockSlot *slot)
{
	if (!slot->gap_anim)
		slot->gap_anim = g_timeout_add(16, gap_tick, slot);
}

static GList *
dock_children(Windows *s)
{
	return gtk_container_get_children(GTK_CONTAINER(s->box));
}

static int
drag_insert_at(Windows *s, double x_root)
{
	GdkWindow *win;
	int dock_x = 0, visual = 0, candidate;
	GList *kids, *l;

	win = gtk_widget_get_window(s->box);
	if (!win)
		return 0;
	gdk_window_get_origin(win, &dock_x, NULL);
	kids = dock_children(s);
	candidate = (int)g_list_length(kids);
	for (l = kids; l; l = l->next) {
		GtkWidget *child = l->data;
		GtkAllocation alloc;

		if (child == s->drag_widget)
			continue;
		gtk_widget_get_allocation(child, &alloc);
		if (x_root - dock_x < alloc.x + alloc.width / 2) {
			candidate = visual;
			break;
		}
		visual++;
	}
	g_list_free(kids);
	return candidate;
}

static void
animate_drag_gaps(Windows *s)
{
	GList *kids, *l;
	int visual = 0, n;

	kids = dock_children(s);
	n = (int)g_list_length(kids);
	for (l = kids; l; l = l->next) {
		GtkWidget *child = l->data;
		DockSlot *slot = dock_slot_of(child);

		if (child == s->drag_widget) {
			slot->gap_s_t = 0;
			slot->gap_e_t = 0;
		} else {
			int ms = 0, me = 0;

			if (visual == s->drag_insert)
				ms = DRAG_GAP_PX;
			else if (s->drag_insert >= n && !l->next)
				me = DRAG_GAP_PX;
			slot->gap_s_t = ms;
			slot->gap_e_t = me;
			visual++;
		}
		gap_run(slot);
	}
	g_list_free(kids);
}

static void
reset_drag_gaps(Windows *s)
{
	GList *kids, *l;

	kids = dock_children(s);
	for (l = kids; l; l = l->next) {
		DockSlot *slot = dock_slot_of(l->data);

		slot->gap_s_t = 0;
		slot->gap_e_t = 0;
		gap_run(slot);
	}
	g_list_free(kids);
}

static GtkWidget *
make_drag_ghost(GtkWidget *slot)
{
	GtkWidget *win, *img;
	GtkImage *src;
	GdkPixbuf *pb;

	win = gtk_window_new(GTK_WINDOW_POPUP);
	gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DND);
	gtk_widget_set_app_paintable(win, TRUE);
	panel_ensure_rgba(win);
	src = g_object_get_data(G_OBJECT(slot), "img-ref");
	pb = src ? g_object_get_data(G_OBJECT(src), "src-pixbuf") : NULL;
	if (pb) {
		GdkPixbuf *shown = gdk_pixbuf_scale_simple(pb, PANEL_ICON, PANEL_ICON, GDK_INTERP_BILINEAR);

		img = gtk_image_new_from_pixbuf(shown);
		g_object_unref(shown);
	} else {
		GAppInfo *app = g_object_get_data(G_OBJECT(slot), "app_info");
		GIcon *icon = app ? g_app_info_get_icon(app) : NULL;

		if (icon)
			img = gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_DIALOG);
		else
			img = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
		gtk_image_set_pixel_size(GTK_IMAGE(img), PANEL_ICON);
	}
	gtk_container_add(GTK_CONTAINER(win), img);
	gtk_widget_set_opacity(win, 0.75);
	gtk_widget_show_all(win);
	return win;
}

static void
commit_drag(Windows *s)
{
	const char *dragged_id;
	char *remaining[64];
	int remaining_count = 0, i, clamped, new_count = 0;

	if (!s->drag_app)
		return;
	dragged_id = g_app_info_get_id(s->drag_app);
	if (!dragged_id)
		return;
	for (i = 0; i < s->pinned_count && remaining_count < 64; i++) {
		if (g_strcmp0(s->pinned_ids[i], dragged_id) != 0)
			remaining[remaining_count++] = s->pinned_ids[i];
		else
			g_free(s->pinned_ids[i]);
	}
	clamped = s->drag_insert;
	if (clamped < 0)
		clamped = 0;
	if (clamped > remaining_count)
		clamped = remaining_count;
	for (i = 0; i < clamped; i++)
		s->pinned_ids[new_count++] = remaining[i];
	s->pinned_ids[new_count++] = g_strdup(dragged_id);
	for (i = clamped; i < remaining_count; i++)
		s->pinned_ids[new_count++] = remaining[i];
	s->pinned_count = new_count;
	pins_save(s);
}

static void
clear_drag(Windows *s)
{
	if (s->drag_ghost) {
		gtk_widget_destroy(s->drag_ghost);
		s->drag_ghost = NULL;
	}
	if (s->drag_app) {
		g_object_unref(s->drag_app);
		s->drag_app = NULL;
	}
	s->drag_widget = NULL;
	s->drag_active = FALSE;
}

static void
slot_activate(Windows *s, AppSlot *slot)
{
	Window focus;
	guint i;

	if (!slot)
		return;
	if (slot->xids->len == 0) {
		if (slot->app)
			g_app_info_launch(slot->app, NULL, NULL, NULL);
		return;
	}
	if (slot->xids->len == 1) {
		activate_or_iconify(s, g_array_index(slot->xids, Window, 0));
		return;
	}
	focus = active_window(s);
	for (i = 0; i < slot->xids->len; i++) {
		if (g_array_index(slot->xids, Window, i) == focus) {
			ewmh_client(s, focus, s->wm_change_state, IconicState, 0, 0);
			return;
		}
	}
	activate_window(s, g_array_index(slot->xids, Window, slot->xids->len - 1));
}

static gboolean
on_dock_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user)
{
	Windows *s = user;
	double dx, dy;
	int insert;

	(void)widget;
	if (!s->drag_widget)
		return FALSE;
	if (!s->drag_active) {
		dx = event->x_root - s->drag_x;
		dy = event->y_root - s->drag_y;
		if (sqrt(dx * dx + dy * dy) < DRAG_THRESHOLD)
			return FALSE;
		s->drag_active = TRUE;
		preview_hide(s);
		s->drag_ghost = make_drag_ghost(s->drag_widget);
		s->drag_insert = drag_insert_at(s, event->x_root);
		animate_drag_gaps(s);
	}
	if (s->drag_ghost)
		gtk_window_move(GTK_WINDOW(s->drag_ghost),
		                (int)event->x_root - PANEL_ICON / 2,
		                (int)event->y_root - PANEL_ICON / 2);
	insert = drag_insert_at(s, event->x_root);
	if (insert != s->drag_insert) {
		s->drag_insert = insert;
		animate_drag_gaps(s);
	}
	gdk_event_request_motions(event);
	return TRUE;
}

static gboolean
on_dock_release(GtkWidget *widget, GdkEventButton *event, gpointer user)
{
	Windows *s = user;
	gboolean was_drag;
	GdkSeat *seat;

	(void)widget;
	if (event->button != 1 || !s->drag_widget)
		return FALSE;
	seat = gdk_display_get_default_seat(gdk_display_get_default());
	gdk_seat_ungrab(seat);
	was_drag = s->drag_active;
	if (!was_drag)
		slot_activate(s, slot_of_widget(s->drag_widget));
	else
		commit_drag(s);
	reset_drag_gaps(s);
	clear_drag(s);
	if (was_drag)
		refresh(s);
	return TRUE;
}

static gboolean
on_slot_press(GtkWidget *slot, GdkEventButton *event, gpointer user)
{
	Windows *s = user;
	GdkWindow *grab_win;
	GAppInfo *app;

	if (event->button == 3) {
		preview_hide(s);
		popup_window_menu(slot, event);
		return TRUE;
	}
	if (event->button != 1)
		return FALSE;

	clear_drag(s);
	s->drag_widget = slot;
	app = g_object_get_data(G_OBJECT(slot), "app_info");
	s->drag_app = app ? g_object_ref(app) : NULL;
	s->drag_x = event->x_root;
	s->drag_y = event->y_root;
	s->drag_active = FALSE;
	s->drag_insert = 0;
	grab_win = gtk_widget_get_window(s->panel);
	if (grab_win)
		gdk_seat_grab(gdk_display_get_default_seat(gdk_display_get_default()),
		              grab_win, GDK_SEAT_CAPABILITY_ALL_POINTING,
		              FALSE, NULL, (GdkEvent *)event, NULL, NULL);
	return TRUE;
}

static void preview_cancel_timers(Windows *s);

static gboolean
on_slot_enter(GtkWidget *slot_w, GdkEventCrossing *event, gpointer user)
{
	Windows *s = user;
	GtkImage *img = g_object_get_data(G_OBJECT(slot_w), "img-ref");
	AppSlot *slot;

	if (event->detail == GDK_NOTIFY_INFERIOR)
		return FALSE;
	if (img)
		nudge_icon(img, TRUE);
	slot = slot_of_widget(slot_w);
	if (!s || !slot || s->drag_active)
		return FALSE;
	preview_cancel_timers(s);
	if (slot->xids->len == 0)
		return FALSE;
	if (s->preview_slot == slot && s->preview && gtk_widget_get_visible(s->preview))
		return FALSE;
	s->preview_slot = slot;
	s->preview_show_id = g_timeout_add(PREVIEW_SHOW_MS, on_preview_show_timeout, s);
	return FALSE;
}

static gboolean
on_preview_show_timeout(gpointer user)
{
	Windows *s = user;

	s->preview_show_id = 0;
	if (s->preview_slot)
		preview_show(s, s->preview_slot);
	return G_SOURCE_REMOVE;
}

static gboolean
on_preview_hide_timeout(gpointer user)
{
	Windows *s = user;

	s->preview_hide_id = 0;
	preview_hide(s);
	return G_SOURCE_REMOVE;
}

static void
preview_cancel_timers(Windows *s)
{
	if (s->preview_show_id) {
		g_source_remove(s->preview_show_id);
		s->preview_show_id = 0;
	}
	if (s->preview_hide_id) {
		g_source_remove(s->preview_hide_id);
		s->preview_hide_id = 0;
	}
}

static gboolean
on_slot_leave(GtkWidget *slot_w, GdkEventCrossing *event, gpointer user)
{
	Windows *s = user;
	GtkImage *img = g_object_get_data(G_OBJECT(slot_w), "img-ref");

	if (event->detail == GDK_NOTIFY_INFERIOR)
		return FALSE;
	if (img)
		nudge_icon(img, FALSE);
	if (!s)
		return FALSE;
	preview_cancel_timers(s);
	s->preview_hide_id = g_timeout_add(PREVIEW_HIDE_MS, on_preview_hide_timeout, s);
	return FALSE;
}

static gboolean
on_preview_enter(GtkWidget *w, GdkEventCrossing *event, gpointer user)
{
	Windows *s = user;

	(void)w;
	(void)event;
	preview_cancel_timers(s);
	return FALSE;
}

static gboolean
on_preview_leave(GtkWidget *w, GdkEventCrossing *event, gpointer user)
{
	Windows *s = user;

	(void)w;
	if (event->detail == GDK_NOTIFY_INFERIOR)
		return FALSE;
	preview_cancel_timers(s);
	s->preview_hide_id = g_timeout_add(PREVIEW_HIDE_MS, on_preview_hide_timeout, s);
	return FALSE;
}

static GdkPixbuf *
thumb_canvas(GdkPixbuf *src)
{
	GdkPixbuf *canvas, *fitted;
	int sw, sh, tw, th, ox, oy;
	double scale;

	canvas = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, PREVIEW_THUMB_W, PREVIEW_THUMB_H);
	gdk_pixbuf_fill(canvas, 0x121314ff);
	if (!src)
		return canvas;
	sw = gdk_pixbuf_get_width(src);
	sh = gdk_pixbuf_get_height(src);
	if (sw < 1 || sh < 1)
		return canvas;
	scale = MIN((double)PREVIEW_THUMB_W / sw, (double)PREVIEW_THUMB_H / sh);
	if (scale > 1.0)
		scale = 1.0;
	tw = MAX(1, (int)(sw * scale));
	th = MAX(1, (int)(sh * scale));
	fitted = gdk_pixbuf_scale_simple(src, tw, th, GDK_INTERP_BILINEAR);
	ox = (PREVIEW_THUMB_W - tw) / 2;
	oy = (PREVIEW_THUMB_H - th) / 2;
	gdk_pixbuf_copy_area(fitted, 0, 0, tw, th, canvas, ox, oy);
	g_object_unref(fitted);
	return canvas;
}

static GdkPixbuf *
window_shot(Windows *s, Window w)
{
	XWindowAttributes attr;
	GdkWindow *gw;
	GdkPixbuf *full, *framed;

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	if (!XGetWindowAttributes(s->dpy, w, &attr) ||
	    attr.map_state != IsViewable || attr.width < 8 || attr.height < 8) {
		gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
		return NULL;
	}
	gw = gdk_x11_window_foreign_new_for_display(gdk_display_get_default(), w);
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
	if (!gw)
		return NULL;
	full = gdk_pixbuf_get_from_window(gw, 0, 0, attr.width, attr.height);
	g_object_unref(gw);
	if (!full)
		return NULL;
	framed = thumb_canvas(full);
	g_object_unref(full);
	return framed;
}

static void
on_thumb_click(GtkWidget *card, GdkEventButton *event, gpointer user)
{
	Windows *s = g_win;
	Window w = (Window)GPOINTER_TO_UINT(user);

	(void)card;
	if (!s || event->button != 1)
		return;
	activate_window(s, w);
	preview_hide(s);
}

static void
on_thumb_close(GtkButton *btn, gpointer user)
{
	Windows *s = g_win;
	Window w = (Window)GPOINTER_TO_UINT(user);

	(void)btn;
	if (!s || !w)
		return;
	ewmh_client(s, w, s->net_close, CurrentTime, 2, 0);
}

static GtkWidget *
make_thumb_card(Windows *s, AppSlot *slot, Window w)
{
	GtkWidget *card, *box, *header, *title, *close, *img, *shot_box;
	GdkPixbuf *shot, *icon_pb = NULL;
	char *name;

	card = gtk_event_box_new();
	gtk_widget_set_size_request(card, PREVIEW_CARD_W, PREVIEW_CARD_H);
	gtk_widget_set_hexpand(card, FALSE);
	gtk_widget_set_vexpand(card, FALSE);
	gtk_style_context_add_class(gtk_widget_get_style_context(card), "thumb-card");
	gtk_widget_add_events(card, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(card, "button-press-event", G_CALLBACK(on_thumb_click),
	                 GUINT_TO_POINTER((guint)w));

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_container_add(GTK_CONTAINER(card), box);

	header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_size_request(header, PREVIEW_THUMB_W, 20);
	name = window_title(s, w);
	title = gtk_label_new(name);
	g_free(name);
	gtk_style_context_add_class(gtk_widget_get_style_context(title), "thumb-title");
	gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
	gtk_label_set_single_line_mode(GTK_LABEL(title), TRUE);
	gtk_label_set_xalign(GTK_LABEL(title), 0.0);
	gtk_widget_set_size_request(title, PREVIEW_THUMB_W - 28, 20);
	gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);

	close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
	gtk_style_context_add_class(gtk_widget_get_style_context(close), "thumb-close");
	gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
	g_signal_connect(close, "clicked", G_CALLBACK(on_thumb_close),
	                 GUINT_TO_POINTER((guint)w));
	gtk_box_pack_end(GTK_BOX(header), close, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);

	shot_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_size_request(shot_box, PREVIEW_THUMB_W, PREVIEW_THUMB_H);
	gtk_widget_set_halign(shot_box, GTK_ALIGN_CENTER);
	gtk_style_context_add_class(gtk_widget_get_style_context(shot_box), "thumb-shot");

	shot = window_shot(s, w);
	if (!shot) {
		GIcon *icon = slot->app ? g_app_info_get_icon(slot->app) : NULL;
		GtkIconInfo *info;

		if (icon) {
			info = gtk_icon_theme_lookup_by_gicon(gtk_icon_theme_get_default(),
			                                      icon, 48, 0);
			if (info) {
				icon_pb = gtk_icon_info_load_icon(info, NULL);
				g_object_unref(info);
			}
		}
		shot = thumb_canvas(icon_pb);
		if (icon_pb)
			g_object_unref(icon_pb);
	}
	img = gtk_image_new_from_pixbuf(shot);
	g_object_unref(shot);
	gtk_widget_set_size_request(img, PREVIEW_THUMB_W, PREVIEW_THUMB_H);
	gtk_box_pack_start(GTK_BOX(shot_box), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), shot_box, FALSE, FALSE, 0);
	return card;
}

static void
slot_root_xy(GtkWidget *slot_w, int *x, int *y)
{
	GtkWidget *top;
	GdkWindow *gw;
	int lx = 0, ly = 0, ox = 0, oy = 0;

	top = gtk_widget_get_toplevel(slot_w);
	gtk_widget_translate_coordinates(slot_w, top, 0, 0, &lx, &ly);
	gw = gtk_widget_get_window(top);
	if (gw)
		gdk_window_get_origin(gw, &ox, &oy);
	*x = ox + lx;
	*y = oy + ly;
}

static void
preview_place(Windows *s, GtkWidget *slot_w)
{
	GdkRectangle geo;
	GtkAllocation slot_alloc;
	GtkRequisition nat;
	int slot_x = 0, slot_y = 0, pw, ph, x, y;

	if (!panel_primary_geo(&geo) || !s->preview)
		return;

	slot_root_xy(slot_w, &slot_x, &slot_y);
	gtk_widget_get_allocation(slot_w, &slot_alloc);
	gtk_widget_get_preferred_size(s->preview, NULL, &nat);
	pw = nat.width > 1 ? nat.width : PREVIEW_CARD_W + 20;
	ph = nat.height > 1 ? nat.height : PREVIEW_CARD_H + 20;

	/* Centre on the hovered icon, sit just above it. */
	x = slot_x + slot_alloc.width / 2 - pw / 2;
	y = slot_y - 8 - ph;
	if (x < geo.x + 8)
		x = geo.x + 8;
	if (x + pw > geo.x + geo.width - 8)
		x = geo.x + geo.width - 8 - pw;
	if (y < geo.y + 8)
		y = geo.y + 8;
	gtk_window_move(GTK_WINDOW(s->preview), x, y);
}

static void
preview_hide(Windows *s)
{
	preview_cancel_timers(s);
	if (s->preview)
		gtk_widget_hide(s->preview);
	s->preview_slot = NULL;
}

static void
preview_show(Windows *s, AppSlot *slot)
{
	GtkWidget *row;
	GList *kids, *l;
	guint i;

	if (!slot || slot->xids->len == 0)
		return;

	if (!s->preview) {
		GtkWidget *chrome;

		s->preview = gtk_window_new(GTK_WINDOW_POPUP);
		panel_ensure_rgba(s->preview);
		gtk_window_set_decorated(GTK_WINDOW(s->preview), FALSE);
		gtk_window_set_type_hint(GTK_WINDOW(s->preview), GDK_WINDOW_TYPE_HINT_TOOLTIP);
		gtk_window_set_skip_taskbar_hint(GTK_WINDOW(s->preview), TRUE);
		gtk_window_set_skip_pager_hint(GTK_WINDOW(s->preview), TRUE);
		gtk_window_set_keep_above(GTK_WINDOW(s->preview), TRUE);
		gtk_window_set_resizable(GTK_WINDOW(s->preview), FALSE);
		gtk_style_context_add_class(gtk_widget_get_style_context(s->preview), "dock-preview");
		gtk_widget_add_events(s->preview, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
		g_signal_connect(s->preview, "enter-notify-event", G_CALLBACK(on_preview_enter), s);
		g_signal_connect(s->preview, "leave-notify-event", G_CALLBACK(on_preview_leave), s);
		chrome = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_set_name(chrome, "DockPreview");
		gtk_container_add(GTK_CONTAINER(s->preview), chrome);
		row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_container_add(GTK_CONTAINER(chrome), row);
		g_object_set_data(G_OBJECT(s->preview), "row", row);
	}

	row = g_object_get_data(G_OBJECT(s->preview), "row");
	kids = gtk_container_get_children(GTK_CONTAINER(row));
	for (l = kids; l; l = l->next)
		gtk_widget_destroy(GTK_WIDGET(l->data));
	g_list_free(kids);

	for (i = 0; i < slot->xids->len; i++)
		gtk_box_pack_start(GTK_BOX(row),
		                   make_thumb_card(s, slot, g_array_index(slot->xids, Window, i)),
		                   FALSE, FALSE, 0);

	s->preview_slot = slot;
	gtk_widget_show_all(s->preview);
	{
		GtkRequisition nat;

		gtk_widget_get_preferred_size(s->preview, NULL, &nat);
		if (nat.width > 0 && nat.height > 0)
			gtk_window_resize(GTK_WINDOW(s->preview), nat.width, nat.height);
	}
	while (gtk_events_pending())
		gtk_main_iteration();
	preview_place(s, slot->widget);
}

static void
slot_sync_chrome(Windows *s, AppSlot *slot)
{
	GtkWidget *ind;
	Window focus;
	gboolean running, active;
	guint i;

	(void)s;
	ind = g_object_get_data(G_OBJECT(slot->widget), "indicator");
	running = slot->xids->len > 0;
	focus = active_window(g_win);
	active = FALSE;
	for (i = 0; i < slot->xids->len; i++) {
		if (g_array_index(slot->xids, Window, i) == focus) {
			active = TRUE;
			break;
		}
	}
	if (ind) {
		if (!running)
			gtk_widget_hide(ind);
		else {
			gtk_widget_set_size_request(ind,
				active ? INDICATOR_ACTIVE_W : INDICATOR_W, 2);
			gtk_widget_show(ind);
		}
	}
	if (slot->app)
		gtk_widget_set_tooltip_text(slot->widget, g_app_info_get_display_name(slot->app));
	gtk_widget_set_has_tooltip(slot->widget, !running);
}

static GtkWidget *
make_app_widget(Windows *s, AppSlot *slot)
{
	GtkWidget *wslot, *overlay, *vbox, *img, *indicator;
	GIcon *gicon;

	wslot = gtk_event_box_new();
	gtk_event_box_set_above_child(GTK_EVENT_BOX(wslot), TRUE);
	gtk_widget_set_size_request(wslot, DOCK_SLOT_W, PANEL_HEIGHT);
	gtk_widget_set_hexpand(wslot, FALSE);
	gtk_widget_set_valign(wslot, GTK_ALIGN_FILL);
	gtk_widget_add_events(wslot, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK);
	gtk_style_context_add_class(gtk_widget_get_style_context(wslot), "app-btn");

	overlay = gtk_overlay_new();
	gtk_container_add(GTK_CONTAINER(wslot), overlay);
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
	gtk_container_add(GTK_CONTAINER(overlay), vbox);

	gicon = slot->app ? g_app_info_get_icon(slot->app) : NULL;
	if (gicon) {
		img = gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_DIALOG);
		gtk_image_set_pixel_size(GTK_IMAGE(img), PANEL_ICON);
	} else if (slot->xids->len) {
		GdkPixbuf *icon = window_icon(s, g_array_index(slot->xids, Window, 0));

		if (icon) {
			GdkPixbuf *shown = gdk_pixbuf_scale_simple(icon, PANEL_ICON, PANEL_ICON, GDK_INTERP_BILINEAR);

			img = gtk_image_new_from_pixbuf(shown);
			g_object_unref(shown);
			g_object_set_data_full(G_OBJECT(img), "src-pixbuf", icon, g_object_unref);
		} else {
			img = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
			gtk_image_set_pixel_size(GTK_IMAGE(img), PANEL_ICON);
		}
	} else {
		img = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
		gtk_image_set_pixel_size(GTK_IMAGE(img), PANEL_ICON);
	}
	g_object_set_data(G_OBJECT(img), "nudge-size", GINT_TO_POINTER(PANEL_ICON));
	gtk_box_pack_start(GTK_BOX(vbox), img, FALSE, FALSE, 0);

	indicator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_size_request(indicator, INDICATOR_W, 2);
	gtk_widget_set_halign(indicator, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(indicator, GTK_ALIGN_END);
	gtk_widget_set_margin_bottom(indicator, 2);
	gtk_style_context_add_class(gtk_widget_get_style_context(indicator), "running-indicator");
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), indicator);
	gtk_widget_set_no_show_all(indicator, TRUE);
	gtk_widget_set_visible(indicator, FALSE);

	if (slot->app) {
		gtk_widget_set_tooltip_text(wslot, g_app_info_get_display_name(slot->app));
		g_object_set_data_full(G_OBJECT(wslot), "app_info",
		                       g_object_ref(slot->app), g_object_unref);
	}

	g_object_set_data(G_OBJECT(wslot), "app-slot", slot);
	g_object_set_data(G_OBJECT(wslot), "img-ref", img);
	g_object_set_data(G_OBJECT(wslot), "indicator", indicator);
	dock_slot_of(wslot);
	g_signal_connect(img, "destroy", G_CALLBACK(on_image_destroy), NULL);
	g_signal_connect(wslot, "button-press-event", G_CALLBACK(on_slot_press), s);
	g_signal_connect(wslot, "enter-notify-event", G_CALLBACK(on_slot_enter), s);
	g_signal_connect(wslot, "leave-notify-event", G_CALLBACK(on_slot_leave), s);
	gtk_widget_show_all(wslot);
	gtk_widget_set_visible(indicator, FALSE);
	return wslot;
}

static void
app_slot_free(gpointer p)
{
	AppSlot *slot = p;
	guint i;

	if (!slot)
		return;
	if (g_win) {
		for (i = 0; i < slot->xids->len; i++)
			g_hash_table_remove(g_win->xid_index,
			                    GUINT_TO_POINTER((guint)g_array_index(slot->xids, Window, i)));
		if (g_win->preview_slot == slot)
			preview_hide(g_win);
	}
	if (slot->widget) {
		if (gtk_widget_get_parent(slot->widget))
			gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(slot->widget)), slot->widget);
		g_object_unref(slot->widget);
	}
	if (slot->app)
		g_object_unref(slot->app);
	g_array_free(slot->xids, TRUE);
	g_free(slot->key);
	g_free(slot);
}

static AppSlot *
slot_ensure(Windows *s, const char *key, GAppInfo *app)
{
	AppSlot *slot;

	slot = g_hash_table_lookup(s->slots, key);
	if (slot) {
		if (!slot->app && app)
			slot->app = g_object_ref(app);
		return slot;
	}
	slot = g_new0(AppSlot, 1);
	slot->key = g_strdup(key);
	slot->app = app ? g_object_ref(app) : NULL;
	slot->xids = g_array_new(FALSE, FALSE, sizeof(Window));
	slot->widget = make_app_widget(s, slot);
	g_object_ref_sink(slot->widget);
	g_hash_table_insert(s->slots, g_strdup(key), slot);
	return slot;
}

static gboolean
slot_has_xid(AppSlot *slot, Window w)
{
	guint i;

	for (i = 0; i < slot->xids->len; i++)
		if (g_array_index(slot->xids, Window, i) == w)
			return TRUE;
	return FALSE;
}

static void
slot_add_xid(Windows *s, AppSlot *slot, Window w)
{
	if (slot_has_xid(slot, w))
		return;
	g_array_append_val(slot->xids, w);
	g_hash_table_insert(s->xid_index, GUINT_TO_POINTER((guint)w), slot);
}

static void
update_indicators(Windows *s)
{
	GHashTableIter iter;
	gpointer key, val;

	g_hash_table_iter_init(&iter, s->slots);
	while (g_hash_table_iter_next(&iter, &key, &val))
		slot_sync_chrome(s, val);
}

static void
refresh(Windows *s)
{
	Atom type;
	int fmt;
	unsigned long n = 0, extra;
	unsigned char *data = NULL;
	Window *list;
	GHashTable *seen;
	unsigned long i;
	GHashTableIter iter;
	gpointer key, val;
	GPtrArray *order;
	int p;

	if (s->drag_active)
		return;

	if (s->panel_xid == None && gtk_widget_get_realized(s->panel)) {
		GdkWindow *gw = gtk_widget_get_window(s->panel);
		if (gw)
			s->panel_xid = GDK_WINDOW_XID(gw);
	}

	if (XGetWindowProperty(s->dpy, s->root, s->net_client_list, 0, 4096, False, XA_WINDOW,
	                       &type, &fmt, &n, &extra, &data) != Success || !data) {
		if (data)
			XFree(data);
		return;
	}

	list = (Window *)data;
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; i < n; i++) {
		Window w = list[i];
		char *idkey;
		GAppInfo *app = NULL;
		AppSlot *slot;

		if (!is_taskable(s, w))
			continue;
		g_hash_table_add(seen, GUINT_TO_POINTER((guint)w));
		idkey = identity_for_window(s, w, &app);
		slot = slot_ensure(s, idkey, app);
		slot_add_xid(s, slot, w);
		if (app)
			g_object_unref(app);
		g_free(idkey);
	}

	g_hash_table_iter_init(&iter, s->slots);
	while (g_hash_table_iter_next(&iter, &key, &val)) {
		AppSlot *slot = val;
		guint k = 0;

		while (k < slot->xids->len) {
			Window w = g_array_index(slot->xids, Window, k);

			if (g_hash_table_contains(seen, GUINT_TO_POINTER((guint)w))) {
				k++;
				continue;
			}
			g_hash_table_remove(s->xid_index, GUINT_TO_POINTER((guint)w));
			g_array_remove_index(slot->xids, k);
		}
		if (slot->xids->len == 0 && !dock_is_pinned(slot->key)) {
			if (gtk_widget_get_parent(slot->widget))
				gtk_container_remove(GTK_CONTAINER(s->box), slot->widget);
			g_hash_table_iter_remove(&iter);
		}
	}

	for (p = 0; p < s->pinned_count; p++) {
		GDesktopAppInfo *dai = g_desktop_app_info_new(s->pinned_ids[p]);

		slot_ensure(s, s->pinned_ids[p], dai ? G_APP_INFO(dai) : NULL);
		if (dai)
			g_object_unref(dai);
	}

	order = g_ptr_array_new();
	{
		GHashTable *placed = g_hash_table_new(g_str_hash, g_str_equal);

		for (p = 0; p < s->pinned_count; p++) {
			AppSlot *slot = g_hash_table_lookup(s->slots, s->pinned_ids[p]);

			if (!slot)
				continue;
			g_ptr_array_add(order, slot->widget);
			g_hash_table_add(placed, slot->key);
		}
		g_hash_table_iter_init(&iter, s->slots);
		while (g_hash_table_iter_next(&iter, &key, &val)) {
			AppSlot *slot = val;

			if (g_hash_table_contains(placed, slot->key))
				continue;
			if (slot->xids->len)
				g_ptr_array_add(order, slot->widget);
		}
		g_hash_table_destroy(placed);
	}

	{
		GList *children = gtk_container_get_children(GTK_CONTAINER(s->box));
		GList *c;
		GHashTable *wanted = g_hash_table_new(g_direct_hash, g_direct_equal);

		for (i = 0; i < order->len; i++)
			g_hash_table_add(wanted, order->pdata[i]);
		for (c = children; c; c = c->next) {
			if (g_hash_table_contains(wanted, c->data))
				continue;
			gtk_container_remove(GTK_CONTAINER(s->box), GTK_WIDGET(c->data));
		}
		g_list_free(children);
		g_hash_table_destroy(wanted);
	}

	for (i = 0; i < order->len; i++) {
		GtkWidget *slot_w = order->pdata[i];

		if (!gtk_widget_get_parent(slot_w))
			gtk_box_pack_start(GTK_BOX(s->box), slot_w, FALSE, FALSE, 0);
		gtk_box_reorder_child(GTK_BOX(s->box), slot_w, (int)i);
		gtk_widget_show(slot_w);
	}
	g_ptr_array_free(order, TRUE);
	g_hash_table_destroy(seen);
	XFree(data);
	update_indicators(s);

	if (s->preview && gtk_widget_get_visible(s->preview) && s->preview_slot) {
		if (s->preview_slot->xids->len == 0)
			preview_hide(s);
		else
			preview_show(s, s->preview_slot);
	}
}

static GdkFilterReturn
filter_root(GdkXEvent *xevent, GdkEvent *event, gpointer user)
{
	XEvent *ev = (XEvent *)xevent;
	Windows *s = user;

	(void)event;
	if (ev->type == PropertyNotify) {
		if (ev->xproperty.atom == s->net_active_window)
			update_indicators(s);
		else if (ev->xproperty.atom == s->net_client_list)
			refresh(s);
	}
	return GDK_FILTER_CONTINUE;
}

static void
on_panel_realize(GtkWidget *panel, gpointer user)
{
	Windows *s = user;
	GdkWindow *gw = gtk_widget_get_window(panel);

	if (gw)
		s->panel_xid = GDK_WINDOW_XID(gw);
	refresh(s);
}

static void
windows_free(gpointer p)
{
	Windows *s = p;
	int i;

	preview_hide(s);
	if (s->preview)
		gtk_widget_destroy(s->preview);
	g_hash_table_destroy(s->slots);
	g_hash_table_destroy(s->xid_index);
	for (i = 0; i < s->pinned_count; i++)
		g_free(s->pinned_ids[i]);
	if (g_win == s)
		g_win = NULL;
	g_free(s);
}

GtkWidget *
windows_section_new(GtkWidget *panel)
{
	Windows *s;
	GdkWindow *rootw;
	GdkDisplay *gdpy;

	s = g_new0(Windows, 1);
	s->panel = panel;
	s->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_halign(s->box, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(s->box, GTK_ALIGN_FILL);
	s->slots = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, app_slot_free);
	s->xid_index = g_hash_table_new(g_direct_hash, g_direct_equal);

	gdpy = gdk_display_get_default();
	s->dpy = GDK_DISPLAY_XDISPLAY(gdpy);
	s->root = DefaultRootWindow(s->dpy);

	s->net_client_list   = XInternAtom(s->dpy, "_NET_CLIENT_LIST", False);
	s->net_active_window = XInternAtom(s->dpy, "_NET_ACTIVE_WINDOW", False);
	s->net_wm_name       = XInternAtom(s->dpy, "_NET_WM_NAME", False);
	s->net_wm_icon       = XInternAtom(s->dpy, "_NET_WM_ICON", False);
	s->net_wm_state      = XInternAtom(s->dpy, "_NET_WM_STATE", False);
	s->net_wm_window_type= XInternAtom(s->dpy, "_NET_WM_WINDOW_TYPE", False);
	s->net_wm_pid        = XInternAtom(s->dpy, "_NET_WM_PID", False);
	s->gtk_app_id        = XInternAtom(s->dpy, "_GTK_APPLICATION_ID", False);
	s->utf8              = XInternAtom(s->dpy, "UTF8_STRING", False);
	s->skip_taskbar      = XInternAtom(s->dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
	s->type_dock         = XInternAtom(s->dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
	s->type_desktop      = XInternAtom(s->dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
	s->type_splash       = XInternAtom(s->dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
	s->wm_change_state   = XInternAtom(s->dpy, "WM_CHANGE_STATE", False);
	s->net_close         = XInternAtom(s->dpy, "_NET_CLOSE_WINDOW", False);
	s->max_v             = XInternAtom(s->dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
	s->max_h             = XInternAtom(s->dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
	s->above             = XInternAtom(s->dpy, "_NET_WM_STATE_ABOVE", False);

	g_win = s;
	pins_load(s);

	rootw = gdk_x11_window_foreign_new_for_display(gdpy, s->root);
	gdk_window_set_events(rootw, gdk_window_get_events(rootw) | GDK_PROPERTY_CHANGE_MASK);
	XSelectInput(s->dpy, s->root, PropertyChangeMask);
	gdk_window_add_filter(rootw, filter_root, s);

	g_signal_connect(panel, "realize", G_CALLBACK(on_panel_realize), s);
	gtk_widget_add_events(panel, GDK_POINTER_MOTION_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_PRESS_MASK);
	g_signal_connect(panel, "motion-notify-event", G_CALLBACK(on_dock_motion), s);
	g_signal_connect(panel, "button-release-event", G_CALLBACK(on_dock_release), s);
	g_object_set_data_full(G_OBJECT(s->box), "windows", s, windows_free);
	gtk_widget_set_hexpand(s->box, TRUE);
	return s->box;
}

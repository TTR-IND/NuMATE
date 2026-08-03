/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║                                                              ║
 * ║   ████████╗████████╗██████╗       ██╗███╗   ██╗██████╗       ║
 * ║   ╚══██╔══╝╚══██╔══╝██╔══██╗      ██║████╗  ██║██╔══██╗      ║
 * ║      ██║      ██║   ██████╔╝      ██║██╔██╗ ██║██║  ██║      ║
 * ║      ██║      ██║   ██╔══██╗      ██║██║╚██╗██║██║  ██║      ║
 * ║      ██║      ██║   ██║  ██║      ██║██║ ╚████║██████╔╝      ║
 * ║      ╚═╝      ╚═╝   ╚═╝  ╚═╝      ╚═╝╚═╝  ╚═══╝╚═════╝       ║
 * ║                                                              ║
 * ║       Torfaen Technology Research — IND                      ║
 * ║       Copyright © 2026                                       ║
 * ║       Licensed under AGPLV3                                  ║
 * ║                                                              ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * gonzo-shell.c — Gonzo Shell — the NuMATE desktop interface
 *
 * Build: gcc -O2 -Wall -Wextra gonzo-shell.c -o gonzo-shell
 *        $(pkg-config --cflags --libs gtk+-3.0 libwnck-3.0 json-glib-1.0 dbusmenu-gtk3-0.4 libmatemixer)
 *        -lX11 -lm
 */

#define WNCK_I_KNOW_THIS_IS_UNSTABLE 1

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <libwnck/libwnck.h>
#include <libdbusmenu-gtk/menu.h>
#include <libmatemixer/matemixer.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <stdint.h>
#include <dirent.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/utsname.h>

/* ═══════════════════════════════════════════════════════════════════════
 * LOGGING
 * ═══════════════════════════════════════════════════════════════════════ */

#define GONZO_LOG(fmt, ...) \
    do { \
        time_t _now = time(NULL); \
        struct tm *_t = localtime(&_now); \
        char _b[32]; \
        strftime(_b, sizeof(_b), "%H:%M:%S", _t); \
        fprintf(stderr, "[%s] GonzoCC: " fmt "\n", _b, ##__VA_ARGS__); \
    } while(0)

/* ═══════════════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════ */

#define SHELF_H              48
#define POPUP_GAP            0
#define MENU_GAP             8
#define DOCK_ICON_SIZE       34
#define NUDGE_PX             10
#define DRAG_THRESHOLD       10
#define DRAG_GAP_PX          40
#define POLL_INTERVAL_MS     50
#define APP_ICON_SIZE        24
#define MENU_BUS_NAME        "org.ukui.menu"
#define MENU_OBJECT_PATH     "/org/ukui/menu"
#define PANEL_MARGIN         12
#define NOTIF_PANEL_GAP      10

#define GONZO_ACCENT           "#B5342A"
#define GONZO_ACCENT_SOFT      "rgba(181,52,42,0.14)"
#define GONZO_ACCENT_SOFT_HI   "rgba(181,52,42,0.26)"

#define TILE_ICON_PIXELS       28
#define PANEL_ICON_PIXELS      22
#define PANEL_ROW_SPACING      12
#define PANEL_ROW_MARGIN_SIDE  24
#define PANEL_ROW_MARGIN_TB     6

#define WIFI_POLL_IDLE_MS        5000
#define WIFI_POLL_CONNECTING_MS   700

#define NOTIF_MAX_STRING_LENGTH  4096
#define TRAY_ICON_MAX_DIM        256

/* ═══════════════════════════════════════════════════════════════════════
 * DATA TYPES
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    WIFI_OFF = 0,
    WIFI_DISCONNECTED,
    WIFI_CONNECTING,
    WIFI_CONNECTED
} WifiState;

typedef struct {
    guint32      id;
    gchar       *app_name;
    gchar       *app_icon;
    gchar       *summary;
    gchar       *body;
    gchar       *sender;
    gint64       timestamp;
} Notification;

typedef struct {
    gchar       *app_id;
    gchar       *desktop_id;
    gchar       *app_name;
    gchar       *app_icon;
    GdkPixbuf   *icon_pixbuf;
    GList       *notifications;
    gint         unread_count;
    GtkWidget   *card_widget;
    GtkWidget   *stack_widget;
    GtkWidget   *arrow_icon;
    GtkWidget   *badge_label;
    GtkWidget   *status_label;
    gboolean     collapsed;
    gboolean     has_tray_item;
    gchar       *sender_bus_name;
    gchar       *object_path;
    gchar       *menu_path;
    GtkWidget   *dbus_menu;
} AppNotificationGroup;

typedef struct {
    GtkImage *img;
    int start, end, steps, count;
} TweenData;

typedef struct {
    gchar *id;
    gchar *name;
    gchar *exec;
    gchar *description;
    gchar *categories;
    gchar *icon_name;
    GDesktopAppInfo *dai;
} AppInfo;

typedef enum {
    VIEW_ALL = 0,
    VIEW_SEARCH = 1
} ViewMode;

typedef struct {
    GtkWidget *window;
    GtkWidget *search_entry;
    GtkWidget *app_list;
    GtkWidget *search_list;
    GtkWidget *stack;
    GPtrArray *apps;
    ViewMode   current_view;
} MenuData;

typedef struct {
    GtkWidget  *widget;
    GtkImage   *image;
    GAppInfo   *app;
    GtkWidget  *indicator;
    int32_t     gap_margin_start;
    int32_t     gap_margin_end;
    int32_t     gap_margin_start_target;
    int32_t     gap_margin_end_target;
    guint       gap_anim_id;
} DockSlot;

typedef struct {
    GtkWidget  *shelf_window;
    GtkWidget  *shelf_box;
    GtkWidget  *dock_box;
    GtkWidget  *clock_label;
    WnckScreen *screen;
    WnckHandle *handle;
    char       *pinned_ids[64];
    int         pinned_count;
    DockSlot   *slots[128];
    int         slot_count;
    GAppInfo   *drag_app;
    GtkWidget  *drag_widget;
    double      drag_start_x;
    double      drag_start_y;
    gboolean    drag_active;
    GtkWidget  *drag_ghost;
    int         drag_insert_index;
    GHashTable *identity_table;
    gboolean    shelf_rounded;
    
    GtkWidget  *panel_window;
    GtkWidget  *notif_window;
    MenuData   *menu;
    GtkWidget  *instance_popup;
    GtkWidget  *current_dock_icon;
    guint       popup_poll_timer;
    
    GDBusProxy *upower_proxy;
    MateMixerContext       *audio_context;
    MateMixerStreamControl *audio_control;

    gboolean    wifi_enabled;
    gboolean    bt_enabled;
    WifiState   wifi_state;
    guint       wifi_poll_timer;
    gboolean    wifi_poll_pending;
    gboolean    bt_poll_pending;
    
    GList           *app_groups;
    GDBusConnection *notif_dbus_connection;
    guint32          next_notification_id;
    
    GtkWidget *battery_box;
    GtkWidget *battery_icon;
    GtkWidget *battery_label;
    GtkWidget *shelf_battery_icon;
    GtkWidget *shelf_wifi_icon;
    GtkWidget *username_label;
    GtkWidget *brightness_slider;
    GtkWidget *brightness_icon;
    GtkWidget *volume_slider;
    GtkWidget *volume_icon;
    GtkWidget *wifi_tile;
    GtkWidget *wifi_icon;
    GtkWidget *wifi_spinner;
    GtkWidget *wifi_icon_stack;
    GtkWidget *wifi_label;
    GtkWidget *bt_tile;
    GtkWidget *airplane_tile;
    GtkWidget *notif_inner_box;
    GtkWidget *notif_title_label;
    GtkWidget *notif_clear_button;

    gboolean    volume_fallback_pending;
    GDBusProxy *backlight_proxy;
} GonzoShell;

static GonzoShell *g_shell = NULL;

/* ═══════════════════════════════════════════════════════════════════════
 * FORWARD DECLARATIONS
 * ═══════════════════════════════════════════════════════════════════════ */

static void ensure_rgba_visual(GtkWidget *win);
static void update_shelf_style(void);
static void check_active_window_rounding(void);
static void launch_cmd_cb(GtkWidget *widget, gpointer user_data);
static void on_brightness_slider_changed(GtkRange *range, gpointer user_data);
static void on_volume_slider_changed(GtkRange *range, gpointer user_data);
static void update_volume_icon_display(void);
static void refresh_volume_ui(void);
static void refresh_battery_ui(void);
static void backlight_init(void);
static void toggle_panel(GtkWidget *button, gpointer user_data);
static GtkWidget *create_quick_settings_panel(void);
static GtkWidget *create_notification_center(void);
static void rebuild_notification_ui(void);
static void menu_toggle(MenuData *menu);
static void on_window_opened(WnckScreen *screen, WnckWindow *window, gpointer user_data);
static void on_window_closed(WnckScreen *screen, WnckWindow *window, gpointer user_data);
static void on_active_window_changed(WnckScreen *screen, WnckWindow *prev, gpointer data);
static void on_window_state_changed(WnckWindow *win, WnckWindowState changed, WnckWindowState new_state, gpointer data);
static void identity_register(const char *desktop_id, GtkWidget *widget);
static GtkWidget *identity_lookup(const char *desktop_id);
static void identity_unregister_widget(GtkWidget *widget);
static void cancel_instance_popup(void);
static void show_context_menu(GtkWidget *widget, GAppInfo *app, GdkEventButton *event);
static void on_dock_item_destroy(GtkWidget *widget, gpointer user_data);
static void show_instance_popup(GtkWidget *dock_icon);
static void save_config(void);
static void refresh_dock(void);
static void close_app_action_widget(GtkWidget *widget);
static void refresh_brightness_slider(void);

/* ═══════════════════════════════════════════════════════════════════════
 * ASYNC COMMAND RUNNER
 * ═══════════════════════════════════════════════════════════════════════ */

typedef void (*AsyncCommandCallback)(const gchar *output, gpointer user_data);

typedef struct {
    gchar       *cmd;
    AsyncCommandCallback callback;
    gpointer     user_data;
    GIOChannel  *stdout_ch;
    GString     *stdout_buf;
    GPid         child_pid;
    guint        child_watch;
    guint        io_watch;
} AsyncCommand;

static void async_command_free(AsyncCommand *ac)
{
    if (ac->stdout_ch) g_io_channel_unref(ac->stdout_ch);
    if (ac->stdout_buf) g_string_free(ac->stdout_buf, TRUE);
    if (ac->child_watch) g_source_remove(ac->child_watch);
    if (ac->io_watch) g_source_remove(ac->io_watch);
    g_spawn_close_pid(ac->child_pid);
    g_free(ac->cmd);
    g_free(ac);
}

static gboolean async_command_stdout_cb(GIOChannel *channel, GIOCondition condition, gpointer data)
{
    AsyncCommand *ac = data;
    if (condition & G_IO_IN) {
        gchar buf[512];
        gsize bytes_read;
        GError *error = NULL;
        g_io_channel_read_chars(channel, buf, sizeof(buf), &bytes_read, &error);
        if (!error && bytes_read > 0)
            g_string_append_len(ac->stdout_buf, buf, bytes_read);
        else if (error)
            g_error_free(error);
    }
    if (condition & (G_IO_HUP | G_IO_ERR)) {
        ac->io_watch = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void async_command_child_watch(GPid pid, gint status, gpointer data)
{
    AsyncCommand *ac = data;
    (void)pid;
    (void)status;
    if (ac->io_watch) {
        g_source_remove(ac->io_watch);
        ac->io_watch = 0;
    }
    if (ac->stdout_ch) {
        GIOStatus stat;
        do {
            gchar buf[256];
            gsize bytes_read;
            stat = g_io_channel_read_chars(ac->stdout_ch, buf, sizeof(buf), &bytes_read, NULL);
            if (stat == G_IO_STATUS_NORMAL && bytes_read > 0)
                g_string_append_len(ac->stdout_buf, buf, bytes_read);
        } while (stat == G_IO_STATUS_NORMAL);
    }

    gchar *output = g_string_free(ac->stdout_buf, FALSE);
    ac->stdout_buf = NULL;
    g_strstrip(output);
    if (output && strlen(output) == 0) {
        g_free(output);
        output = NULL;
    }

    ac->callback(output, ac->user_data);
    g_free(output);
    async_command_free(ac);
}

static void
run_command_async(const gchar *cmd, AsyncCommandCallback callback, gpointer user_data)
{
    AsyncCommand *ac = g_new0(AsyncCommand, 1);
    ac->cmd = g_strdup(cmd);
    ac->callback = callback;
    ac->user_data = user_data;

    gchar *shell_cmd = g_strdup_printf("sh -c \"%s\"", cmd);
    gchar **argv = NULL;
    GError *error = NULL;

    if (!g_shell_parse_argv(shell_cmd, NULL, &argv, &error)) {
        GONZO_LOG("async cmd parse error: %s", error->message);
        g_error_free(error);
        g_free(shell_cmd);
        callback(NULL, user_data);
        g_free(ac);
        return;
    }
    g_free(shell_cmd);

    gint child_stdout = -1;
    GPid child_pid = 0;
    gboolean spawned = g_spawn_async_with_pipes(
        NULL, argv, NULL,
        G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
        NULL, NULL, &child_pid,
        NULL, &child_stdout, NULL, &error);
    g_strfreev(argv);

    if (!spawned) {
        GONZO_LOG("async spawn failed: %s", error->message);
        g_error_free(error);
        callback(NULL, user_data);
        g_free(ac);
        return;
    }

    ac->child_pid = child_pid;
    ac->child_watch = g_child_watch_add(child_pid, async_command_child_watch, ac);

    ac->stdout_ch = g_io_channel_unix_new(child_stdout);
    g_io_channel_set_encoding(ac->stdout_ch, NULL, NULL);
    g_io_channel_set_buffered(ac->stdout_ch, FALSE);
    ac->stdout_buf = g_string_new(NULL);
    ac->io_watch = g_io_add_watch(ac->stdout_ch, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                  async_command_stdout_cb, ac);
}

/* ═══════════════════════════════════════════════════════════════════════
 * UTILITY
 * ═══════════════════════════════════════════════════════════════════════ */

static gchar *
safe_truncate(const gchar *str, gsize max_len)
{
    if (!str) return NULL;
    if (strlen(str) <= max_len) return g_strdup(str);
    return g_strndup(str, max_len);
}

static void
ensure_rgba_visual(GtkWidget *win)
{
    GdkVisual *visual = gdk_screen_get_rgba_visual(gdk_screen_get_default());
    if (visual) gtk_widget_set_visual(win, visual);
    gtk_widget_set_app_paintable(win, TRUE);
}

/* ═══════════════════════════════════════════════════════════════════════
 * SHELF APPEARANCE
 * ═══════════════════════════════════════════════════════════════════════ */

static void
update_shelf_style(void)
{
    if (!g_shell || !g_shell->shelf_box) return;
    GtkStyleContext *context = gtk_widget_get_style_context(g_shell->shelf_box);
    if (g_shell->shelf_rounded) {
        gtk_style_context_remove_class(context, "shelf-flat");
        gtk_style_context_add_class(context, "shelf");
    } else {
        gtk_style_context_remove_class(context, "shelf");
        gtk_style_context_add_class(context, "shelf-flat");
    }
}

static void
check_active_window_rounding(void)
{
    if (!g_shell || !g_shell->screen) return;

    WnckWorkspace *active_ws = wnck_screen_get_active_workspace(g_shell->screen);
    gboolean any_maximized = FALSE;

    GList *windows = wnck_screen_get_windows(g_shell->screen);
    for (GList *l = windows; l; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        if (wnck_window_is_skip_tasklist(w)) continue;
        if (wnck_window_is_minimized(w)) continue;
        if (active_ws && !wnck_window_is_on_workspace(w, active_ws)) continue;
        if (wnck_window_is_maximized(w)) { any_maximized = TRUE; break; }
    }
    g_shell->shelf_rounded = !any_maximized;
    update_shelf_style();
}

static void
on_active_window_changed(WnckScreen *screen, WnckWindow *previous_active, gpointer user_data)
{
    (void)screen; (void)previous_active; (void)user_data;
    check_active_window_rounding();
}

static void
on_window_state_changed(WnckWindow *window, WnckWindowState changed_mask,
                        WnckWindowState new_state, gpointer user_data)
{
    (void)window; (void)changed_mask; (void)new_state; (void)user_data;
    check_active_window_rounding();
}

/* ═══════════════════════════════════════════════════════════════════════
 * CSS
 * ═══════════════════════════════════════════════════════════════════════ */

static void
apply_styles(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    
    const char *css =
        "window, .GonzoMenuWindow { background: transparent; }\n"
        ".shelf { background: rgba(32, 33, 36, 0.96); border-radius: 16px 16px 0 0; }\n"
        ".shelf-flat { background: rgba(32, 33, 36, 0.96); border-radius: 0; }\n"
        ".card, #GonzoMenu { background-color: rgba(32, 33, 36, 0.98); border-radius: 16px; border: 1px solid rgba(255,255,255,0.08); background-image: none; }\n"
        ".launcher-btn, .launcher-btn:hover, .launcher-btn:active { background: none; border: none; box-shadow: none; outline: none; padding: 8px; }\n"
        ".launcher-btn:hover image { -gtk-icon-transform: rotate(35deg); }\n"
        ".app-btn { border-radius: 50%; border: none; background: transparent; transition: background 0.2s; padding: 5px; }\n"
        ".app-btn:hover { background: rgba(255,255,255,0.1); }\n"
        ".running-indicator { background: white; min-height: 2px; min-width: 10px; margin-top: 2px; border-radius: 1px; }\n"
        ".status-pill { background: rgba(255,255,255,0.05); border-radius: 24px; padding: 2px 16px; margin: 8px 6px; border: none; transition: background 0.2s; outline: none; }\n"
        ".status-pill:hover { background: rgba(255,255,255,0.12); }\n"
        ".status-pill:active { background: rgba(255,255,255,0.2); }\n"
        "label { color: #e8eaed; }\n"
        /*
         * scale's border/frame comes from the base GTK theme's default
         * widget styling, not from anything set here. Killing it
         * explicitly (rather than fighting it with a matching override)
         * is the one-line fix; the frame is gone regardless of which
         * color drives the highlight.
         */
        "scale { border: none; box-shadow: none; outline: none; background: none; }\n"
        "scale trough { border: none; box-shadow: none; outline: none; }\n"
        "scale highlight { background: " GONZO_ACCENT "; border: none; box-shadow: none; }\n"
        "scale contents { background: rgba(255,255,255,0.1); border-radius: 8px; }\n"
        "scale trough { background: rgba(255,255,255,0.2); border-radius: 10px; min-height: 4px; }\n"
        "scale slider { min-width: 22px; min-height: 22px; background: white; border-radius: 50%; box-shadow: 0 2px 8px rgba(0,0,0,0.4); }\n"
        "#PanelCard { background-color: rgba(32, 33, 36, 0.98); border-radius: 38px; padding: 20px 18px 26px; border: 1px solid rgba(255,255,255,0.08); }\n"
        ".tile { background: rgba(255,255,255,0.08); border-radius: 24px; border: none; outline: none; color: white; font-weight: 500; font-size: 13px; transition: background 0.2s, margin 0.1s; box-shadow: 0 2px 6px rgba(0,0,0,0.25); }\n"
        ".tile:active { margin: 2px; background: rgba(255,255,255,0.18); }\n"
        ".tile.active { background: " GONZO_ACCENT "; color: white; }\n"
        ".tile.active label { color: white; }\n"
        ".tile label { font-size: 12px; font-weight: 500; color: rgba(255,255,255,0.9); }\n"
        ".action-btn, .power-btn { background: rgba(255,255,255,0.07); border-radius: 20px; padding: 10px 20px; font-weight: 500; font-size: 14px; color: rgba(255,255,255,0.9); border: none; }\n"
        ".action-btn:active, .power-btn:active { background: rgba(255,255,255,0.2); }\n"
        "#GonzoMenuSearch { background-color: rgba(255,255,255,0.08); border-radius: 16px; padding: 6px 12px; color: white; margin: 10px; }\n"
        "#GonzoMenuSearch entry { background-color: transparent; background-image: none; border: none; box-shadow: none; padding: 0; }\n"
        "#GonzoMenuAppList, #GonzoMenuAppList row, viewport { background-color: transparent; background-image: none; border: none; box-shadow: none; }\n"
        "#GonzoMenuAppList row:hover { background-color: rgba(255,255,255,0.1); }\n"
        "#GonzoMenuAppList row:selected { background-color: rgba(255,255,255,0.15); }\n"
        "#GonzoMenu scrollbar { background: transparent; border: none; }\n"
        "#GonzoMenu scrollbar trough { background: transparent; border: none; box-shadow: none; }\n"
        "#GonzoMenu scrollbar slider { background: rgba(255,255,255,0.25); border: none; border-radius: 999px; min-width: 8px; margin: 4px 12px 4px 3px; }\n"
        "#GonzoMenu scrollbar slider:hover { background: rgba(255,255,255,0.4); }\n"
        "#GonzoMenu scrollbar slider:active { background: rgba(255,255,255,0.55); }\n"
        "#ChevronButton { background: rgba(32, 33, 36, 0.98); border-radius: 50%; border: 1px solid rgba(255,255,255,0.08); box-shadow: 0 4px 12px rgba(0,0,0,0.4); min-width: 44px; min-height: 44px; padding: 0; }\n"
        "#ChevronButton:hover { background: rgba(40, 42, 48, 0.98); }\n"
        "#GonzoNotifCenter { background: transparent; }\n"
        "#NotifOuter { background-color: rgba(32, 33, 36, 0.98); border-radius: 38px; border: 1px solid rgba(255,255,255,0.08); }\n"
        "#NotifHeader { padding: 16px; }\n"
        "#NotifTitle { font-size: 16px; font-weight: 500; color: #ffffff; }\n"
        "#NotifClearBtn { font-size: 13px; color: " GONZO_ACCENT "; background: " GONZO_ACCENT_SOFT "; border: none; border-radius: 20px; padding: 6px 14px; font-weight: 500; }\n"
        "#NotifClearBtn:hover { background: " GONZO_ACCENT_SOFT_HI "; }\n"
        "#NotifScroll { background: transparent; border: none; }\n"
        "#NotifInnerBox { background: transparent; padding: 4px; }\n"
        "#NotifCard { background: rgba(255,255,255,0.05); border-radius: 22px; margin: 4px 8px; border: 1px solid rgba(255,255,255,0.04); }\n"
        "#NotifAppBar { background: transparent; border-radius: 22px; }\n"
        "#NotifAppBar:hover { background: rgba(255,255,255,0.06); }\n"
        "#NotifAppName { font-size: 15px; font-weight: 500; color: #ffffff; }\n"
        "#NotifAppStatus { font-size: 12px; color: #9aa0a6; }\n"
        "#NotifBadge { background: " GONZO_ACCENT "; color: white; border-radius: 20px; padding: 2px 8px; font-size: 12px; font-weight: 600; }\n"
        "#NotifOptionsBtn, #NotifArrowBtn { background: transparent; border: none; border-radius: 50%; min-width: 32px; min-height: 32px; padding: 0; }\n"
        "#NotifOptionsBtn:hover, #NotifArrowBtn:hover { background: rgba(255,255,255,0.12); }\n"
        "#NotifStack { padding: 12px; }\n"
        "#NotifBubble { background: rgba(255,255,255,0.07); border-radius: 18px; padding: 12px 14px; border: 1px solid rgba(255,255,255,0.04); }\n"
        "#NotifBubble:hover { background: rgba(255,255,255,0.11); }\n"
        "#NotifSender { font-size: 13px; font-weight: 600; color: #ffffff; }\n"
        "#NotifTime { font-size: 11px; color: #8e9297; }\n"
        "#NotifContent { font-size: 13px; color: #c4c7cc; line-height: 1.4; }\n"
        "#NotifFooter { padding: 12px; border-top: 1px solid rgba(255,255,255,0.06); }\n"
        "#NotifFooterBtn { font-size: 13px; font-weight: 500; color: #c4c7cc; background: rgba(255,255,255,0.05); border: none; border-radius: 20px; padding: 8px 16px; }\n"
        "#NotifFooterBtn:hover { background: rgba(255,255,255,0.12); }\n"
        "#NotifFooterLabel { font-size: 11px; color: #9aa0a6; opacity: 0.5; }\n";
    
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ═══════════════════════════════════════════════════════════════════════
 * SYSTEM BACKEND (battery, brightness, volume)
 * ═══════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════
 * BRIGHTNESS (org.mate.PowerManager.Backlight, session bus)
 *
 * GetBrightness/SetBrightness on this interface are already expressed as
 * a 0-100 percentage — that is precisely why it replaces the helper: the
 * helper's two-step "read max, scale, clamp, spawn pkexec" dance existed
 * only to translate raw hardware units into a percentage by hand. A single
 * D-Bus method call on the session bus does the same job with no spawned
 * process, no polkit round-trip, and no cached maximum to keep in sync.
 * ═══════════════════════════════════════════════════════════════════════ */

static void
backlight_set_brightness_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
    if (!ret) {
        GONZO_LOG("SetBrightness failed: %s", error->message);
        g_error_free(error);
        return;
    }
    g_variant_unref(ret);
}

static void
set_brightness_percent_async(int percent)
{
    if (!g_shell->backlight_proxy) return;
    guint32 value = (guint32)CLAMP(percent, 0, 100);
    g_dbus_proxy_call(g_shell->backlight_proxy, "SetBrightness",
                      g_variant_new("(u)", value),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                      backlight_set_brightness_done, NULL);
}

static void
backlight_get_brightness_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
    if (!ret) {
        GONZO_LOG("GetBrightness failed: %s", error->message);
        g_error_free(error);
        return;
    }
    guint32 value = 0;
    g_variant_get(ret, "(u)", &value);
    g_variant_unref(ret);

    if (g_shell->brightness_slider) {
        g_signal_handlers_block_by_func(g_shell->brightness_slider,
            G_CALLBACK(on_brightness_slider_changed), NULL);
        gtk_range_set_value(GTK_RANGE(g_shell->brightness_slider), CLAMP(value, 0, 100));
        g_signal_handlers_unblock_by_func(g_shell->brightness_slider,
            G_CALLBACK(on_brightness_slider_changed), NULL);
    }
}

static void
refresh_brightness_slider(void)
{
    if (!g_shell->brightness_slider || !g_shell->backlight_proxy) return;
    g_dbus_proxy_call(g_shell->backlight_proxy, "GetBrightness",
                      NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                      backlight_get_brightness_done, NULL);
}

static void
on_backlight_signal(GDBusProxy *proxy, const gchar *sender, const gchar *signal_name,
                    GVariant *parameters, gpointer user_data)
{
    (void)proxy; (void)sender; (void)user_data;
    if (g_strcmp0(signal_name, "BrightnessChanged") != 0) return;
    guint32 value = 0;
    g_variant_get(parameters, "(u)", &value);
    if (g_shell->brightness_slider) {
        g_signal_handlers_block_by_func(g_shell->brightness_slider,
            G_CALLBACK(on_brightness_slider_changed), NULL);
        gtk_range_set_value(GTK_RANGE(g_shell->brightness_slider), CLAMP(value, 0, 100));
        g_signal_handlers_unblock_by_func(g_shell->brightness_slider,
            G_CALLBACK(on_brightness_slider_changed), NULL);
    }
}

static void
backlight_init(void)
{
    GError *error = NULL;
    g_shell->backlight_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.mate.PowerManager",
        "/org/mate/PowerManager/Backlight",
        "org.mate.PowerManager.Backlight", NULL, &error);

    if (!g_shell->backlight_proxy) {
        GONZO_LOG("mate-power-manager Backlight proxy failed (%s); brightness control disabled",
                  error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return;
    }

    g_signal_connect(g_shell->backlight_proxy, "g-signal",
                     G_CALLBACK(on_backlight_signal), NULL);
}

/* ═══════════════════════════════════════════════════════════════════════
 * AUDIO (libmatemixer with async amixer fallback)
 * ═══════════════════════════════════════════════════════════════════════ */

static gboolean
audio_has_volume(void)
{
    if (!g_shell->audio_control) return FALSE;
    MateMixerStreamControlFlags flags =
        mate_mixer_stream_control_get_flags(g_shell->audio_control);
    return (flags & MATE_MIXER_STREAM_CONTROL_VOLUME_READABLE) != 0 &&
           (flags & MATE_MIXER_STREAM_CONTROL_VOLUME_WRITABLE) != 0;
}

static gboolean
audio_has_mute(void)
{
    if (!g_shell->audio_control) return FALSE;
    return (mate_mixer_stream_control_get_flags(g_shell->audio_control)
            & MATE_MIXER_STREAM_CONTROL_MUTE_READABLE) != 0;
}

static int
get_volume_percent(void)
{
    if (audio_has_volume()) {
        MateMixerStreamControl *c = g_shell->audio_control;
        guint min = mate_mixer_stream_control_get_min_volume(c);
        guint max = mate_mixer_stream_control_get_max_volume(c);
        if (max > min) {
            guint v = mate_mixer_stream_control_get_volume(c);
            if (v < min) v = min;
            if (v > max) v = max;
            return CLAMP((int)((((gdouble)(v - min)) / (max - min)) * 100.0 + 0.5), 0, 100);
        }
    }
    return 50;
}

static gboolean
get_mute_state(void)
{
    if (audio_has_mute())
        return mate_mixer_stream_control_get_mute(g_shell->audio_control);
    return FALSE;
}

static void
set_volume_percent(int percent)
{
    percent = CLAMP(percent, 0, 100);
    if (audio_has_volume()) {
        MateMixerStreamControl *c = g_shell->audio_control;
        guint min = mate_mixer_stream_control_get_min_volume(c);
        guint max = mate_mixer_stream_control_get_max_volume(c);
        if (max > min) {
            guint v = (guint)(min + ((max - min) * (percent / 100.0)) + 0.5);
            mate_mixer_stream_control_set_volume(c, v);
            return;
        }
    }
    gchar *cmd = g_strdup_printf("amixer sset Master %d%% 2>/dev/null", percent);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
}

static void
volume_fallback_async_cb(const gchar *output, gpointer user_data)
{
    (void)user_data;
    g_shell->volume_fallback_pending = FALSE;
    if (!output) return;

    int percent = 50;
    gboolean muted = FALSE;
    gchar **lines = g_strsplit(output, "\n", 0);
    for (int i = 0; lines[i]; i++) {
        if (strstr(lines[i], "Playback") && strstr(lines[i], "%")) {
            gchar *p = strstr(lines[i], "[");
            if (p) {
                p++;
                char *end = strchr(p, '%');
                if (end) {
                    *end = '\0';
                    percent = atoi(p);
                }
            }
        }
        if (strstr(lines[i], "off")) muted = TRUE;
        if (strstr(lines[i], "on"))  muted = FALSE;
    }
    g_strfreev(lines);

    percent = CLAMP(percent, 0, 100);

    if (g_shell->volume_slider) {
        g_signal_handlers_block_by_func(g_shell->volume_slider,
            G_CALLBACK(on_volume_slider_changed), NULL);
        gtk_range_set_value(GTK_RANGE(g_shell->volume_slider), percent);
        g_signal_handlers_unblock_by_func(g_shell->volume_slider,
            G_CALLBACK(on_volume_slider_changed), NULL);
    }
    if (g_shell->volume_icon) {
        const char *icon_name;
        if (muted) icon_name = "audio-volume-muted-symbolic";
        else if (percent > 70) icon_name = "audio-volume-high-symbolic";
        else if (percent > 30) icon_name = "audio-volume-medium-symbolic";
        else icon_name = "audio-volume-low-symbolic";
        gtk_image_set_from_icon_name(GTK_IMAGE(g_shell->volume_icon), icon_name, GTK_ICON_SIZE_DND);
        gtk_image_set_pixel_size(GTK_IMAGE(g_shell->volume_icon), PANEL_ICON_PIXELS);
    }
}

static void
refresh_volume_ui_fallback_async(void)
{
    if (g_shell->volume_fallback_pending) return;
    g_shell->volume_fallback_pending = TRUE;
    run_command_async("amixer sget Master", volume_fallback_async_cb, NULL);
}

static void
update_volume_icon_display(void)
{
    if (!g_shell->volume_icon) return;
    if (!audio_has_volume() && !g_shell->volume_fallback_pending) {
        refresh_volume_ui_fallback_async();
        return;
    }
    gboolean muted = get_mute_state();
    int volume = get_volume_percent();
    const char *icon_name;
    if (muted) icon_name = "audio-volume-muted-symbolic";
    else if (volume > 70) icon_name = "audio-volume-high-symbolic";
    else if (volume > 30) icon_name = "audio-volume-medium-symbolic";
    else icon_name = "audio-volume-low-symbolic";
    
    gtk_image_set_from_icon_name(GTK_IMAGE(g_shell->volume_icon), icon_name, GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(g_shell->volume_icon), PANEL_ICON_PIXELS);
}

static void
refresh_volume_ui(void)
{
    if (audio_has_volume()) {
        if (g_shell->volume_slider) {
            g_signal_handlers_block_by_func(g_shell->volume_slider,
                G_CALLBACK(on_volume_slider_changed), NULL);
            gtk_range_set_value(GTK_RANGE(g_shell->volume_slider), get_volume_percent());
            g_signal_handlers_unblock_by_func(g_shell->volume_slider,
                G_CALLBACK(on_volume_slider_changed), NULL);
        }
        update_volume_icon_display();
    } else {
        refresh_volume_ui_fallback_async();
    }
}

static void
on_audio_control_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object; (void)pspec; (void)user_data;
    refresh_volume_ui();
}

static void
audio_bind_default_control(void)
{
    if (g_shell->audio_control) {
        g_signal_handlers_disconnect_by_func(g_shell->audio_control,
            G_CALLBACK(on_audio_control_changed), NULL);
        g_object_unref(g_shell->audio_control);
        g_shell->audio_control = NULL;
    }

    MateMixerStream *stream =
        mate_mixer_context_get_default_output_stream(g_shell->audio_context);
    if (!stream) return;

    MateMixerStreamControl *ctrl = mate_mixer_stream_get_default_control(stream);
    if (!ctrl) return;

    g_shell->audio_control = g_object_ref(ctrl);
    g_signal_connect(ctrl, "notify::volume", G_CALLBACK(on_audio_control_changed), NULL);
    g_signal_connect(ctrl, "notify::mute",   G_CALLBACK(on_audio_control_changed), NULL);
    refresh_volume_ui();
}

static void
on_audio_context_state_notify(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object; (void)pspec; (void)user_data;
    if (mate_mixer_context_get_state(g_shell->audio_context) == MATE_MIXER_STATE_READY)
        audio_bind_default_control();
}

static void
on_audio_default_stream_notify(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object; (void)pspec; (void)user_data;
    audio_bind_default_control();
}

static void
audio_init(void)
{
    if (!mate_mixer_init()) {
        GONZO_LOG("libmatemixer init failed; volume falls back to amixer");
        return;
    }

    g_shell->audio_context = mate_mixer_context_new();
    mate_mixer_context_set_app_name(g_shell->audio_context, "Gonzo Shell");

    if (!mate_mixer_context_open(g_shell->audio_context)) {
        GONZO_LOG("no libmatemixer backend available; volume falls back to amixer");
        g_object_unref(g_shell->audio_context);
        g_shell->audio_context = NULL;
        return;
    }

    g_signal_connect(g_shell->audio_context, "notify::state",
                     G_CALLBACK(on_audio_context_state_notify), NULL);
    g_signal_connect(g_shell->audio_context, "notify::default-output-stream",
                     G_CALLBACK(on_audio_default_stream_notify), NULL);

    if (mate_mixer_context_get_state(g_shell->audio_context) == MATE_MIXER_STATE_READY)
        audio_bind_default_control();
}

/* ═══════════════════════════════════════════════════════════════════════
 * POWER (UPower)
 * ═══════════════════════════════════════════════════════════════════════ */

static const gchar *
battery_icon_for_percentage(int percent, gboolean charging)
{
    if (charging) return "battery-good-charging-symbolic";
    if (percent >= 95) return "battery-full-symbolic";
    if (percent >= 60) return "battery-good-symbolic";
    if (percent >= 30) return "battery-low-symbolic";
    if (percent >= 10) return "battery-caution-symbolic";
    return "battery-empty-symbolic";
}

static void
refresh_battery_ui(void)
{
    if (!g_shell->battery_label || !g_shell->battery_icon || !g_shell->battery_box) return;

    if (!g_shell->upower_proxy) {
        GONZO_LOG("refresh_battery_ui: no UPower proxy; hiding battery indicator");
        gtk_widget_set_visible(g_shell->battery_box, FALSE);
        if (g_shell->shelf_battery_icon) gtk_widget_set_visible(g_shell->shelf_battery_icon, FALSE);
        return;
    }

    GVariant *present_v = g_dbus_proxy_get_cached_property(g_shell->upower_proxy, "IsPresent");
    gboolean present = present_v ? g_variant_get_boolean(present_v) : FALSE;
    if (present_v) g_variant_unref(present_v);

    gtk_widget_set_visible(g_shell->battery_box, present);
    if (g_shell->shelf_battery_icon) gtk_widget_set_visible(g_shell->shelf_battery_icon, present);
    if (!present) return;

    GVariant *pct_v = g_dbus_proxy_get_cached_property(g_shell->upower_proxy, "Percentage");
    int percent = pct_v ? (int)(g_variant_get_double(pct_v) + 0.5) : 0;
    if (pct_v) g_variant_unref(pct_v);

    GVariant *state_v = g_dbus_proxy_get_cached_property(g_shell->upower_proxy, "State");
    guint32 state = state_v ? g_variant_get_uint32(state_v) : 0;
    if (state_v) g_variant_unref(state_v);

    gboolean charging = (state == 1 || state == 5);

    const gchar *icon = NULL;
    GVariant *icon_v = g_dbus_proxy_get_cached_property(g_shell->upower_proxy, "IconName");
    if (icon_v) {
        const gchar *name = g_variant_get_string(icon_v, NULL);
        if (name && *name && gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), name))
            icon = name;
    }
    const gchar *resolved_icon = icon ? icon : battery_icon_for_percentage(percent, charging);

    gtk_image_set_from_icon_name(GTK_IMAGE(g_shell->battery_icon), resolved_icon, GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(g_shell->battery_icon), 18);
    if (icon_v) g_variant_unref(icon_v);

    gchar *text = g_strdup_printf("%d%%", CLAMP(percent, 0, 100));
    gtk_label_set_text(GTK_LABEL(g_shell->battery_label), text);
    g_free(text);

    const gchar *tooltip = state == 4 ? "Fully charged" : (charging ? "Charging" : "On battery");
    gtk_widget_set_tooltip_text(g_shell->battery_box, tooltip);

    if (g_shell->shelf_battery_icon) {
        gtk_image_set_from_icon_name(GTK_IMAGE(g_shell->shelf_battery_icon), resolved_icon, GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(g_shell->shelf_battery_icon), 15);
        gchar *shelf_tip = g_strdup_printf("%s — %d%%", tooltip, CLAMP(percent, 0, 100));
        gtk_widget_set_tooltip_text(g_shell->shelf_battery_icon, shelf_tip);
        g_free(shelf_tip);
    }
}

static void
on_upower_properties_changed(GDBusProxy *proxy, GVariant *changed,
                             GStrv invalidated, gpointer user_data)
{
    (void)proxy; (void)changed; (void)invalidated; (void)user_data;
    refresh_battery_ui();
}

static void
battery_init(void)
{
    GError *error = NULL;
    g_shell->upower_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.freedesktop.UPower",
        "/org/freedesktop/UPower/devices/DisplayDevice",
        "org.freedesktop.UPower.Device", NULL, &error);

    if (!g_shell->upower_proxy) {
        GONZO_LOG("UPower proxy failed (%s); battery falls back to sysfs",
                  error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return;
    }

    gchar *owner = g_dbus_proxy_get_name_owner(g_shell->upower_proxy);
    if (!owner) {
        GONZO_LOG("upowerd not running; battery falls back to sysfs");
        g_object_unref(g_shell->upower_proxy);
        g_shell->upower_proxy = NULL;
        return;
    }
    g_free(owner);

    g_signal_connect(g_shell->upower_proxy, "g-properties-changed",
                     G_CALLBACK(on_upower_properties_changed), NULL);

    /*
     * battery_box does not exist yet at this call site (it's built later
     * in create_quick_settings_panel, which does its own initial refresh).
     * This call is here so battery_init() stays correct on its own terms:
     * if the panel is ever built before the proxy, or the proxy is ever
     * created asynchronously, the indicator still gets populated the
     * moment the proxy becomes ready. refresh_battery_ui() no-ops safely
     * when the widgets aren't built yet.
     */
    refresh_battery_ui();
}

/* ═══════════════════════════════════════════════════════════════════════
 * NOTIFICATION CENTER
 * ═══════════════════════════════════════════════════════════════════════ */

static gint
compare_app_groups(gconstpointer a, gconstpointer b)
{
    const AppNotificationGroup *ga = a;
    const AppNotificationGroup *gb = b;
    if (ga->unread_count > 0 && gb->unread_count == 0) return -1;
    if (ga->unread_count == 0 && gb->unread_count > 0) return 1;
    return g_strcmp0(ga->app_name, gb->app_name);
}

static GDesktopAppInfo *
find_desktop_entry_for_token(const gchar *token)
{
    if (!token || !*token) return NULL;
    if (strlen(token) > NOTIF_MAX_STRING_LENGTH) return NULL;

    gchar *lower = g_ascii_strdown(token, -1);
    gchar *dashed = g_strdup(lower);
    for (gchar *p = dashed; *p; p++) if (*p == '_' || *p == ' ') *p = '-';

    const gchar *stems[] = { token, lower, dashed };
    GDesktopAppInfo *app = NULL;

    for (guint i = 0; i < G_N_ELEMENTS(stems) && !app; i++) {
        gchar *id = g_str_has_suffix(stems[i], ".desktop")
                    ? g_strdup(stems[i])
                    : g_strdup_printf("%s.desktop", stems[i]);
        app = g_desktop_app_info_new(id);
        g_free(id);
    }

    g_free(dashed);
    g_free(lower);
    return app;
}

static void
resolve_notification_app_identity(const gchar *app_name, const gchar *desktop_hint,
                                  const gchar *app_icon,
                                  gchar **out_name, gchar **out_icon,
                                  gchar **out_desktop_id)
{
    *out_name = NULL;
    *out_icon = NULL;
    if (out_desktop_id) *out_desktop_id = NULL;

    GDesktopAppInfo *app = find_desktop_entry_for_token(desktop_hint);
    if (!app) app = find_desktop_entry_for_token(app_name);

    if (app) {
        const gchar *name = g_app_info_get_display_name(G_APP_INFO(app));
        if (name && *name) *out_name = g_strdup(name);

        GIcon *icon = g_app_info_get_icon(G_APP_INFO(app));
        if (icon) {
            gchar *icon_str = g_icon_to_string(icon);
            if (icon_str && *icon_str) *out_icon = icon_str;
            else g_free(icon_str);
        }
        if (out_desktop_id) {
            const gchar *id = g_app_info_get_id(G_APP_INFO(app));
            if (id) *out_desktop_id = g_strdup(id);
        }
        g_object_unref(app);
    }

    if (!*out_name)
        *out_name = safe_truncate((app_name && *app_name) ? app_name : "Unknown App",
                                  NOTIF_MAX_STRING_LENGTH);

    if (!*out_icon && app_icon && *app_icon)
        *out_icon = safe_truncate(app_icon, NOTIF_MAX_STRING_LENGTH);

    GtkIconTheme *theme = gtk_icon_theme_get_default();
    if (!*out_icon ||
        (!g_path_is_absolute(*out_icon) && !gtk_icon_theme_has_icon(theme, *out_icon))) {
        g_free(*out_icon);
        *out_icon = g_strdup("application-x-executable");
    }
}

static AppNotificationGroup*
find_app_group(const gchar *app_id)
{
    for (GList *l = g_shell->app_groups; l; l = l->next) {
        AppNotificationGroup *group = l->data;
        if (g_strcmp0(group->app_id, app_id) == 0) return group;
    }
    return NULL;
}

static AppNotificationGroup*
find_or_create_app_group(const gchar *app_id, const gchar *app_name, const gchar *app_icon,
                         const gchar *desktop_id)
{
    AppNotificationGroup *existing = find_app_group(app_id);
    if (existing) return existing;

    AppNotificationGroup *group = g_new0(AppNotificationGroup, 1);
    group->app_id = g_strdup(app_id ? app_id : "unknown");
    group->app_name = safe_truncate(app_name, NOTIF_MAX_STRING_LENGTH);
    group->app_icon = g_strdup(app_icon ? app_icon : "application-x-executable");
    group->desktop_id = desktop_id ? g_strdup(desktop_id) : NULL;
    group->collapsed = FALSE;
    g_shell->app_groups = g_list_prepend(g_shell->app_groups, group);
    return group;
}

static Notification*
create_notification(guint32 id, const gchar *app_name, const gchar *app_icon,
                    const gchar *summary, const gchar *body, const gchar *sender)
{
    Notification *notif = g_new0(Notification, 1);
    notif->id = id;
    notif->app_name = safe_truncate(app_name, NOTIF_MAX_STRING_LENGTH);
    notif->app_icon = safe_truncate(app_icon, NOTIF_MAX_STRING_LENGTH);
    notif->summary = safe_truncate(summary, NOTIF_MAX_STRING_LENGTH);
    notif->body = safe_truncate(body, NOTIF_MAX_STRING_LENGTH);
    notif->sender = safe_truncate(sender ? sender : app_name, NOTIF_MAX_STRING_LENGTH);
    notif->timestamp = time(NULL);
    return notif;
}

static void
free_notification(Notification *notif)
{
    if (!notif) return;
    g_free(notif->app_name);
    g_free(notif->app_icon);
    g_free(notif->summary);
    g_free(notif->body);
    g_free(notif->sender);
    g_free(notif);
}

static void
add_notification_to_group(AppNotificationGroup *group, Notification *notif)
{
    group->notifications = g_list_append(group->notifications, notif);
    group->unread_count++;
}

static void
clear_group_notifications(AppNotificationGroup *group)
{
    g_list_free_full(group->notifications, (GDestroyNotify)free_notification);
    group->notifications = NULL;
    group->unread_count = 0;
}

static void
clear_all_notifications(void)
{
    for (GList *l = g_shell->app_groups; l; l = l->next)
        clear_group_notifications(l->data);
    rebuild_notification_ui();
}

static gchar*
format_timestamp(gint64 timestamp)
{
    time_t now = time(NULL);
    gint64 diff = now - timestamp;
    if (diff < 60) return g_strdup("Just now");
    if (diff < 3600) return g_strdup_printf("%ld min ago", (long)(diff / 60));
    if (diff < 86400) return g_strdup_printf("%ld hours ago", (long)(diff / 3600));
    return g_strdup_printf("%ld days ago", (long)(diff / 86400));
}

static void
on_clear_all_clicked(GtkWidget *button, gpointer user_data)
{
    (void)button; (void)user_data;
    clear_all_notifications();
}

static void
on_app_bar_clicked(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    AppNotificationGroup *group = user_data;
    group->collapsed = !group->collapsed;
    if (group->stack_widget)
        gtk_widget_set_visible(group->stack_widget, !group->collapsed);
    if (group->arrow_icon) {
        gtk_image_set_from_icon_name(GTK_IMAGE(group->arrow_icon),
            group->collapsed ? "pan-down-symbolic" : "pan-up-symbolic",
            GTK_ICON_SIZE_MENU);
    }
}

static void
quit_app_by_desktop_id(const gchar *desktop_id)
{
    if (!desktop_id || !*desktop_id) return;
    GtkWidget *dock_widget = identity_lookup(desktop_id);
    if (dock_widget)
        close_app_action_widget(dock_widget);
}

static void
on_quit_app_clicked(GtkWidget *item, gpointer user_data)
{
    (void)item;
    AppNotificationGroup *group = user_data;
    if (group->desktop_id) {
        quit_app_by_desktop_id(group->desktop_id);
        return;
    }
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_shell->dock_box));
    for (GList *l = children; l; l = l->next) {
        GtkWidget *child = GTK_WIDGET(l->data);
        GAppInfo *app = G_APP_INFO(g_object_get_data(G_OBJECT(child), "app_info"));
        if (!app) continue;
        const gchar *name = g_app_info_get_display_name(app);
        if (name && g_strcmp0(name, group->app_name) == 0) {
            close_app_action_widget(child);
            break;
        }
    }
    g_list_free(children);
}

static void
on_mute_app_clicked(GtkWidget *item, gpointer user_data)
{
    (void)item;
    GONZO_LOG("Mute notifications for %s", (char*)user_data);
}

static void
on_app_options_clicked(GtkWidget *button, gpointer user_data)
{
    AppNotificationGroup *group = user_data;
    
    if (group->dbus_menu) {
        gtk_menu_popup_at_widget(GTK_MENU(group->dbus_menu), button,
                                 GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST, NULL);
        return;
    }
    
    if (group->has_tray_item && group->sender_bus_name && group->object_path) {
        GtkWidget *toplevel = gtk_widget_get_toplevel(button);
        gint local_x = 0, local_y = 0;
        gtk_widget_translate_coordinates(button, toplevel, 0, 0, &local_x, &local_y);
        GtkAllocation btn_alloc;
        gtk_widget_get_allocation(button, &btn_alloc);
        local_y += btn_alloc.height;
        
        GdkWindow *top_win = gtk_widget_get_window(toplevel);
        gint origin_x = 0, origin_y = 0;
        if (top_win) gdk_window_get_origin(top_win, &origin_x, &origin_y);
        gint root_x = origin_x + local_x;
        gint root_y = origin_y + local_y;
        
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        if (bus) {
            g_dbus_connection_call(bus, group->sender_bus_name, group->object_path,
                                   "org.kde.StatusNotifierItem", "ContextMenu",
                                   g_variant_new("(ii)", root_x, root_y),
                                   NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
            g_object_unref(bus);
        }
        return;
    }
    
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mute_item = gtk_menu_item_new_with_label("Mute Notifications");
    g_signal_connect(mute_item, "activate", G_CALLBACK(on_mute_app_clicked), group->app_name);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mute_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit App");
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_app_clicked), group);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), button,
                             GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_WEST, NULL);
}

static GtkWidget*
create_notification_bubble(Notification *notif)
{
    GtkWidget *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_name(bubble, "NotifBubble");
    
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *sender_label = gtk_label_new(notif->sender);
    gtk_widget_set_halign(sender_label, GTK_ALIGN_START);
    gtk_widget_set_name(sender_label, "NotifSender");
    
    gchar *time_str = format_timestamp(notif->timestamp);
    GtkWidget *time_label = gtk_label_new(time_str);
    gtk_widget_set_name(time_label, "NotifTime");
    g_free(time_str);
    
    gtk_box_pack_start(GTK_BOX(header), sender_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(header), time_label, FALSE, FALSE, 0);
    
    GtkWidget *content_label = gtk_label_new(notif->body);
    gtk_widget_set_halign(content_label, GTK_ALIGN_START);
    gtk_widget_set_name(content_label, "NotifContent");
    gtk_label_set_line_wrap(GTK_LABEL(content_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(content_label), 40);
    
    gtk_box_pack_start(GTK_BOX(bubble), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bubble), content_label, FALSE, FALSE, 0);
    return bubble;
}

static GtkWidget*
create_app_card(AppNotificationGroup *group)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(card, "NotifCard");
    
    GtkWidget *app_bar = gtk_event_box_new();
    gtk_widget_set_name(app_bar, "NotifAppBar");
    g_signal_connect(app_bar, "button-press-event", G_CALLBACK(on_app_bar_clicked), group);
    
    GtkWidget *bar_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(bar_box), 12);
    
    GtkWidget *icon;
    if (group->icon_pixbuf) {
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(group->icon_pixbuf, 36, 36, GDK_INTERP_BILINEAR);
        icon = gtk_image_new_from_pixbuf(scaled);
        if (scaled) g_object_unref(scaled);
    } else if (group->app_icon && g_path_is_absolute(group->app_icon)) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(group->app_icon, 36, 36, NULL);
        if (pb) {
            icon = gtk_image_new_from_pixbuf(pb);
            g_object_unref(pb);
        } else {
            icon = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DND);
            gtk_image_set_pixel_size(GTK_IMAGE(icon), 36);
        }
    } else {
        icon = gtk_image_new_from_icon_name(group->app_icon, GTK_ICON_SIZE_DND);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 36);
    }
    
    GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *name_label = gtk_label_new(group->app_name);
    gtk_widget_set_halign(name_label, GTK_ALIGN_START);
    gtk_widget_set_name(name_label, "NotifAppName");
    
    group->status_label = gtk_label_new(
        group->unread_count > 0
        ? g_strdup_printf("%d new messages", group->unread_count)
        : (group->has_tray_item ? "Running in background..." : "No new messages"));
    gtk_widget_set_halign(group->status_label, GTK_ALIGN_START);
    gtk_widget_set_name(group->status_label, "NotifAppStatus");
    
    gtk_box_pack_start(GTK_BOX(info_box), name_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info_box), group->status_label, FALSE, FALSE, 0);
    
    GtkWidget *badge_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    group->badge_label = gtk_label_new(g_strdup_printf("%d", group->unread_count));
    gtk_widget_set_name(group->badge_label, "NotifBadge");
    gtk_widget_set_visible(group->badge_label, group->unread_count > 0);
    gtk_box_pack_end(GTK_BOX(badge_box), group->badge_label, FALSE, FALSE, 0);
    
    GtkWidget *options_btn = gtk_button_new();
    gtk_widget_set_name(options_btn, "NotifOptionsBtn");
    GtkWidget *options_icon = gtk_image_new_from_icon_name("view-more-symbolic", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(options_icon), 18);
    gtk_container_add(GTK_CONTAINER(options_btn), options_icon);
    g_signal_connect(options_btn, "clicked", G_CALLBACK(on_app_options_clicked), group);
    
    GtkWidget *arrow_btn = gtk_button_new();
    gtk_widget_set_name(arrow_btn, "NotifArrowBtn");
    group->arrow_icon = gtk_image_new_from_icon_name("pan-up-symbolic", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(group->arrow_icon), 16);
    gtk_container_add(GTK_CONTAINER(arrow_btn), group->arrow_icon);
    g_signal_connect(arrow_btn, "clicked", G_CALLBACK(on_app_bar_clicked), group);
    gtk_widget_set_no_show_all(arrow_btn, group->notifications == NULL);
    gtk_widget_set_visible(arrow_btn, group->notifications != NULL);
    
    gtk_box_pack_start(GTK_BOX(bar_box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar_box), info_box, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(bar_box), arrow_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar_box), options_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar_box), badge_box, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app_bar), bar_box);
    gtk_box_pack_start(GTK_BOX(card), app_bar, FALSE, FALSE, 0);
    
    group->stack_widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_name(group->stack_widget, "NotifStack");
    gtk_container_set_border_width(GTK_CONTAINER(group->stack_widget), 12);
    
    for (GList *l = group->notifications; l; l = l->next) {
        Notification *notif = l->data;
        GtkWidget *bubble = create_notification_bubble(notif);
        gtk_box_pack_start(GTK_BOX(group->stack_widget), bubble, FALSE, FALSE, 0);
    }
    
    gtk_box_pack_start(GTK_BOX(card), group->stack_widget, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(group->stack_widget, group->notifications == NULL);
    gtk_widget_set_visible(group->stack_widget, group->notifications != NULL);
    
    group->card_widget = card;
    return card;
}

static void
rebuild_notification_ui(void)
{
    if (!g_shell->notif_inner_box) return;
    
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_shell->notif_inner_box));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(children);
    
    g_shell->app_groups = g_list_sort(g_shell->app_groups, compare_app_groups);
    
    gboolean has_notifications = FALSE;
    for (GList *l = g_shell->app_groups; l; l = l->next) {
        AppNotificationGroup *group = l->data;
        if (group->notifications || group->has_tray_item) {
            has_notifications = TRUE;
            GtkWidget *card = create_app_card(group);
            gtk_box_pack_start(GTK_BOX(g_shell->notif_inner_box), card, FALSE, FALSE, 0);
        }
    }
    
    if (!has_notifications && g_shell->notif_title_label)
        gtk_label_set_text(GTK_LABEL(g_shell->notif_title_label), "No Notifications");
    gtk_widget_show_all(g_shell->notif_inner_box);
}

static GtkWidget *
create_notification_center(void)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_POPUP);
    ensure_rgba_visual(window);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_widget_set_name(window, "GonzoNotifCenter");
    
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(outer, "NotifOuter");
    gtk_container_add(GTK_CONTAINER(window), outer);
    
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_name(header, "NotifHeader");
    gtk_container_set_border_width(GTK_CONTAINER(header), 16);
    
    g_shell->notif_title_label = gtk_label_new("Notifications");
    gtk_widget_set_name(g_shell->notif_title_label, "NotifTitle");
    gtk_widget_set_halign(g_shell->notif_title_label, GTK_ALIGN_START);
    
    g_shell->notif_clear_button = gtk_button_new_with_label("Clear all");
    gtk_widget_set_name(g_shell->notif_clear_button, "NotifClearBtn");
    g_signal_connect(g_shell->notif_clear_button, "clicked", G_CALLBACK(on_clear_all_clicked), NULL);
    
    gtk_box_pack_start(GTK_BOX(header), g_shell->notif_title_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(header), g_shell->notif_clear_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);
    
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(scroll, "NotifScroll");
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_NONE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    
    g_shell->notif_inner_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_name(g_shell->notif_inner_box, "NotifInnerBox");
    gtk_container_set_border_width(GTK_CONTAINER(g_shell->notif_inner_box), 8);
    gtk_container_add(GTK_CONTAINER(scroll), g_shell->notif_inner_box);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
    
    gtk_widget_show_all(outer);
    return window;
}

/* ═══════════════════════════════════════════════════════════════════════
 * QUICK SETTINGS PANEL
 * ═══════════════════════════════════════════════════════════════════════ */

static void
launch_cmd_cb(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    if (user_data) g_spawn_command_line_async((const char *)user_data, NULL);
}

static void
on_brightness_slider_changed(GtkRange *range, gpointer user_data)
{
    (void)user_data;
    int val = (int)round(gtk_range_get_value(range));
    set_brightness_percent_async(val);
}

static void
on_volume_slider_changed(GtkRange *range, gpointer user_data)
{
    (void)user_data;
    set_volume_percent((int)round(gtk_range_get_value(range)));
}

/* ═══════════════════════════════════════════════════════════════════════
 * WI-FI STATE (async)
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    WifiState  state;
    gint       signal;
    gchar     *ssid;
} WifiStatus;

static gchar **
nmcli_split_fields(const gchar *line)
{
    GPtrArray *fields = g_ptr_array_new();
    GString *cur = g_string_new(NULL);

    for (const gchar *p = line; *p; p++) {
        if (*p == '\\' && p[1]) { g_string_append_c(cur, *++p); continue; }
        if (*p == ':') {
            g_ptr_array_add(fields, g_strdup(cur->str));
            g_string_truncate(cur, 0);
            continue;
        }
        g_string_append_c(cur, *p);
    }
    g_ptr_array_add(fields, g_strdup(cur->str));
    g_ptr_array_add(fields, NULL);

    g_string_free(cur, TRUE);
    return (gchar **)g_ptr_array_free(fields, FALSE);
}

static void
wifi_status_from_output(const gchar *raw, WifiStatus *out)
{
    out->state = WIFI_OFF;
    out->signal = 0;
    out->ssid = NULL;
    if (!raw) return;

    gchar **lines = g_strsplit(raw, "\n", -1);
    int section = 0;

    for (int i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (g_strcmp0(line, "@") == 0) { section++; continue; }
        if (!*line) continue;

        if (section == 0) {
            if (g_strcmp0(line, "enabled") == 0) out->state = WIFI_DISCONNECTED;
            continue;
        }
        if (out->state == WIFI_OFF) continue;

        if (section == 1) {
            gchar **f = nmcli_split_fields(line);
            if (f[0] && f[1] && g_strcmp0(f[0], "wifi") == 0) {
                if (g_str_has_prefix(f[1], "connecting")) {
                    out->state = WIFI_CONNECTING;
                } else if (g_strcmp0(f[1], "connected") == 0 && out->state != WIFI_CONNECTING) {
                    out->state = WIFI_CONNECTED;
                    g_free(out->ssid);
                    out->ssid = g_strdup(f[2] ? f[2] : "");
                }
            }
            g_strfreev(f);
            continue;
        }

        gchar **f = nmcli_split_fields(line);
        if (f[0] && f[1] && g_strcmp0(f[0], "*") == 0)
            out->signal = CLAMP(atoi(f[1]), 0, 100);
        g_strfreev(f);
    }

    g_strfreev(lines);
    if (out->state == WIFI_CONNECTED && (!out->ssid || !*out->ssid)) {
        g_free(out->ssid);
        out->ssid = NULL;
        out->state = WIFI_DISCONNECTED;
    }
}

static const gchar *
wifi_icon_for_signal(gint signal)
{
    if (signal >= 80) return "network-wireless-signal-excellent-symbolic";
    if (signal >= 55) return "network-wireless-signal-good-symbolic";
    if (signal >= 30) return "network-wireless-signal-ok-symbolic";
    if (signal >=  5) return "network-wireless-signal-weak-symbolic";
    return "network-wireless-signal-none-symbolic";
}

static void
apply_wifi_status(const WifiStatus *st)
{
    if (!g_shell->wifi_tile) return;

    g_shell->wifi_state = st->state;
    g_shell->wifi_enabled = (st->state != WIFI_OFF);

    gboolean connecting = (st->state == WIFI_CONNECTING);

    gtk_stack_set_visible_child(GTK_STACK(g_shell->wifi_icon_stack),
                                connecting ? g_shell->wifi_spinner : g_shell->wifi_icon);
    if (connecting) gtk_spinner_start(GTK_SPINNER(g_shell->wifi_spinner));
    else            gtk_spinner_stop(GTK_SPINNER(g_shell->wifi_spinner));

    if (!connecting) {
        const gchar *icon = (st->state == WIFI_CONNECTED)
                            ? wifi_icon_for_signal(st->signal)
                            : "network-wireless-offline-symbolic";
        gtk_image_set_from_icon_name(GTK_IMAGE(g_shell->wifi_icon), icon, GTK_ICON_SIZE_DND);
        gtk_image_set_pixel_size(GTK_IMAGE(g_shell->wifi_icon), TILE_ICON_PIXELS);

        if (g_shell->shelf_wifi_icon) {
            gtk_image_set_from_icon_name(GTK_IMAGE(g_shell->shelf_wifi_icon), icon, GTK_ICON_SIZE_MENU);
            gtk_image_set_pixel_size(GTK_IMAGE(g_shell->shelf_wifi_icon), 15);
        }
    } else if (g_shell->shelf_wifi_icon) {
        gtk_image_set_from_icon_name(GTK_IMAGE(g_shell->shelf_wifi_icon),
            "network-wireless-acquiring-symbolic", GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(g_shell->shelf_wifi_icon), 15);
    }

    const gchar *label = (st->state == WIFI_CONNECTED && st->ssid) ? st->ssid
                       : (connecting ? "Connecting…" : "Wi‑Fi");
    gtk_label_set_text(GTK_LABEL(g_shell->wifi_label), label);
    gtk_widget_set_tooltip_text(g_shell->wifi_tile, label);
    if (g_shell->shelf_wifi_icon) gtk_widget_set_tooltip_text(g_shell->shelf_wifi_icon, label);

    GtkStyleContext *ctx = gtk_widget_get_style_context(g_shell->wifi_tile);
    if (g_shell->wifi_enabled) gtk_style_context_add_class(ctx, "active");
    else gtk_style_context_remove_class(ctx, "active");
}

static void wifi_schedule_poll(void);

static void
wifi_async_callback(const gchar *output, gpointer user_data)
{
    (void)user_data;
    g_shell->wifi_poll_pending = FALSE;
    WifiStatus st;
    wifi_status_from_output(output, &st);
    apply_wifi_status(&st);
    g_free(st.ssid);
    wifi_schedule_poll();
}

static gboolean
wifi_poll_tick(gpointer user_data)
{
    (void)user_data;
    if (g_shell->wifi_poll_pending) return G_SOURCE_CONTINUE;
    g_shell->wifi_poll_pending = TRUE;
    run_command_async(
        "nmcli radio wifi 2>/dev/null; echo '@'; "
        "nmcli -t -f TYPE,STATE,CONNECTION device status 2>/dev/null; echo '@'; "
        "nmcli -t -f IN-USE,SIGNAL device wifi 2>/dev/null",
        wifi_async_callback, NULL);
    g_shell->wifi_poll_timer = 0;
    return G_SOURCE_REMOVE;
}

static void
wifi_schedule_poll(void)
{
    guint interval = (g_shell->wifi_state == WIFI_CONNECTING)
                     ? WIFI_POLL_CONNECTING_MS : WIFI_POLL_IDLE_MS;
    if (g_shell->wifi_poll_timer)
        g_source_remove(g_shell->wifi_poll_timer);
    g_shell->wifi_poll_timer = g_timeout_add(interval, wifi_poll_tick, NULL);
}

static void
wifi_poll_boost(void)
{
    if (g_shell->wifi_poll_pending) return;
    if (g_shell->wifi_poll_timer) g_source_remove(g_shell->wifi_poll_timer);
    g_shell->wifi_poll_timer = 0;
    wifi_poll_tick(NULL);
}

static void
start_wifi_polling(void)
{
    if (g_shell->wifi_poll_timer) return;
    wifi_poll_tick(NULL);
}

static void
toggle_wifi(GtkWidget *tile, gpointer user_data)
{
    (void)user_data;
    gboolean active = gtk_style_context_has_class(gtk_widget_get_style_context(tile), "active");
    g_spawn_command_line_async(active ? "nmcli radio wifi off" : "nmcli radio wifi on", NULL);
    wifi_poll_boost();
}

/* ═══════════════════════════════════════════════════════════════════════
 * BLUETOOTH ASYNC
 * ═══════════════════════════════════════════════════════════════════════ */

static void
bt_async_callback(const gchar *output, gpointer user_data)
{
    (void)user_data;
    g_shell->bt_poll_pending = FALSE;
    gboolean bt_enabled = FALSE;
    if (output) {
        bt_enabled = (g_strcmp0(output, "yes") == 0);
    }
    g_shell->bt_enabled = bt_enabled;
    GtkStyleContext *bt_ctx = gtk_widget_get_style_context(g_shell->bt_tile);
    if (bt_enabled) gtk_style_context_add_class(bt_ctx, "active");
    else gtk_style_context_remove_class(bt_ctx, "active");
    gboolean airplane = !g_shell->wifi_enabled && !g_shell->bt_enabled;
    GtkStyleContext *ap_ctx = gtk_widget_get_style_context(g_shell->airplane_tile);
    if (airplane) gtk_style_context_add_class(ap_ctx, "active");
    else gtk_style_context_remove_class(ap_ctx, "active");
}

static void
update_tile_states_async(void)
{
    if (!g_shell->wifi_tile || !g_shell->bt_tile || !g_shell->airplane_tile) return;
    if (g_shell->bt_poll_pending) return;
    g_shell->bt_poll_pending = TRUE;
    run_command_async("bluetoothctl show | grep -oP 'Powered: \\K\\w+'", bt_async_callback, NULL);
}

static void
toggle_bluetooth(GtkWidget *tile, gpointer user_data)
{
    (void)user_data;
    gboolean active = gtk_style_context_has_class(gtk_widget_get_style_context(tile), "active");
    g_spawn_command_line_async(active ? "bluetoothctl power off" : "bluetoothctl power on", NULL);
}

static void
toggle_airplane_mode(GtkWidget *tile, gpointer user_data)
{
    (void)user_data;
    gboolean active = gtk_style_context_has_class(gtk_widget_get_style_context(tile), "active");
    g_spawn_command_line_async(active ? "nmcli radio all on" : "nmcli radio all off", NULL);
    wifi_poll_boost();
}

/* ═══════════════════════════════════════════════════════════════════════
 * SLIDER ROWS
 * ═══════════════════════════════════════════════════════════════════════ */

static GtkWidget *
make_settings_tile(const char *icon_name, const char *label_text, GCallback click_callback,
                   GtkWidget **out_icon, GtkWidget **out_label,
                   GtkWidget **out_spinner, GtkWidget **out_icon_stack)
{
    GtkWidget *tile = gtk_button_new();
    gtk_widget_set_size_request(tile, 100, 100);
    gtk_style_context_add_class(gtk_widget_get_style_context(tile), "tile");
    
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), TILE_ICON_PIXELS);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);

    GtkWidget *icon_slot = icon;
    GtkWidget *spinner = NULL;
    if (out_spinner) {
        spinner = gtk_spinner_new();
        gtk_widget_set_size_request(spinner, TILE_ICON_PIXELS, TILE_ICON_PIXELS);

        GtkWidget *stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_widget_set_halign(stack, GTK_ALIGN_CENTER);
        gtk_stack_add_named(GTK_STACK(stack), icon, "icon");
        gtk_stack_add_named(GTK_STACK(stack), spinner, "spinner");
        icon_slot = stack;
        if (out_icon_stack) *out_icon_stack = stack;
    }

    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 11);

    gtk_box_pack_start(GTK_BOX(box), icon_slot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(tile), box);
    g_signal_connect(tile, "clicked", click_callback, NULL);

    if (out_icon)    *out_icon = icon;
    if (out_label)   *out_label = label;
    if (out_spinner) *out_spinner = spinner;
    return tile;
}

static GtkWidget *
make_slider_row(const char *icon_name, GtkWidget **out_icon, GtkWidget **out_slider,
                GCallback changed_cb, int initial_value)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_ROW_SPACING);
    gtk_widget_set_margin_start(row, PANEL_ROW_MARGIN_SIDE);
    gtk_widget_set_margin_end(row, PANEL_ROW_MARGIN_SIDE);
    gtk_widget_set_margin_top(row, PANEL_ROW_MARGIN_TB);
    gtk_widget_set_margin_bottom(row, PANEL_ROW_MARGIN_TB);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), PANEL_ICON_PIXELS);
    gtk_widget_set_size_request(icon, PANEL_ICON_PIXELS, PANEL_ICON_PIXELS);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

    GtkWidget *slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(slider), FALSE);
    gtk_widget_set_hexpand(slider, TRUE);
    gtk_widget_set_valign(slider, GTK_ALIGN_CENTER);
    g_signal_connect(slider, "value-changed", changed_cb, NULL);
    g_signal_handlers_block_by_func(slider, changed_cb, NULL);
    gtk_range_set_value(GTK_RANGE(slider), initial_value);
    g_signal_handlers_unblock_by_func(slider, changed_cb, NULL);

    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), slider, TRUE, TRUE, 0);

    *out_icon = icon;
    *out_slider = slider;
    return row;
}

/* ═══════════════════════════════════════════════════════════════════════
 * PANEL PERIODIC REFRESH
 * ═══════════════════════════════════════════════════════════════════════ */

static gboolean
panel_periodic_refresh(gpointer user_data)
{
    (void)user_data;
    update_tile_states_async();
    
    if (g_shell->username_label) {
        struct utsname uts;
        uname(&uts);
        gchar *hostname = g_strdup_printf("%s@%s", g_get_user_name(), uts.nodename);
        gtk_label_set_text(GTK_LABEL(g_shell->username_label), hostname);
        g_free(hostname);
    }
    
    refresh_brightness_slider();
    
    if (!audio_has_volume()) refresh_volume_ui();

    return G_SOURCE_CONTINUE;
}

/* ═══════════════════════════════════════════════════════════════════════
 * QUICK SETTINGS PANEL CREATION
 * ═══════════════════════════════════════════════════════════════════════ */

static GtkWidget *
create_quick_settings_panel(void)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_POPUP);
    ensure_rgba_visual(window);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_widget_set_name(window, "GonzoPanel");
    
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(card, "PanelCard");
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
    gtk_container_add(GTK_CONTAINER(window), card);
    
    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(status_bar, 16);
    gtk_widget_set_margin_end(status_bar, 16);
    gtk_widget_set_margin_top(status_bar, 12);
    gtk_widget_set_margin_bottom(status_bar, 8);
    
    g_shell->username_label = gtk_label_new("");
    gtk_widget_set_halign(g_shell->username_label, GTK_ALIGN_START);
    
    GtkWidget *battery_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_shell->battery_box = battery_box;
    /*
     * no_show_all is set on battery_box alone, not its children, and that
     * is deliberate: gtk_widget_show_all() stops recursing the instant it
     * hits a no_show_all widget, so anything packed inside would never be
     * shown by the show_all() call below. The icon/label are shown here,
     * once, unconditionally — they are static children that should always
     * be visible whenever the box itself is visible. From this point on,
     * battery_box's own visibility (set explicitly in refresh_battery_ui,
     * which bypasses no_show_all) is the single switch that controls
     * whether the indicator appears at all.
     */
    gtk_widget_set_no_show_all(battery_box, TRUE);
    g_shell->battery_icon = gtk_image_new_from_icon_name("battery-full-symbolic", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(g_shell->battery_icon), 18);
    g_shell->battery_label = gtk_label_new("100%");
    gtk_box_pack_start(GTK_BOX(battery_box), g_shell->battery_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(battery_box), g_shell->battery_label, FALSE, FALSE, 0);
    gtk_widget_show(g_shell->battery_icon);
    gtk_widget_show(g_shell->battery_label);
    
    gtk_box_pack_start(GTK_BOX(status_bar), g_shell->username_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(status_bar), battery_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), status_bar, FALSE, FALSE, 0);
    
    GtkWidget *tile_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(tile_grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(tile_grid), 12);
    gtk_widget_set_margin_start(tile_grid, 16);
    gtk_widget_set_margin_end(tile_grid, 16);
    gtk_widget_set_margin_top(tile_grid, 8);
    gtk_widget_set_margin_bottom(tile_grid, 8);
    
    g_shell->wifi_tile = make_settings_tile(
        "network-wireless-offline-symbolic", "Wi‑Fi", G_CALLBACK(toggle_wifi),
        &g_shell->wifi_icon, &g_shell->wifi_label,
        &g_shell->wifi_spinner, &g_shell->wifi_icon_stack);
    g_shell->bt_tile = make_settings_tile(
        "bluetooth-active-symbolic", "Bluetooth", G_CALLBACK(toggle_bluetooth),
        NULL, NULL, NULL, NULL);
    g_shell->airplane_tile = make_settings_tile(
        "airplane-mode-symbolic", "Airplane", G_CALLBACK(toggle_airplane_mode),
        NULL, NULL, NULL, NULL);
    gtk_grid_attach(GTK_GRID(tile_grid), g_shell->wifi_tile, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(tile_grid), g_shell->bt_tile, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(tile_grid), g_shell->airplane_tile, 2, 0, 1, 1);
    gtk_box_pack_start(GTK_BOX(card), tile_grid, FALSE, FALSE, 0);
    
    GtkWidget *brightness_row = make_slider_row("display-brightness-symbolic",
        &g_shell->brightness_icon, &g_shell->brightness_slider,
        G_CALLBACK(on_brightness_slider_changed), 50);
    gtk_box_pack_start(GTK_BOX(card), brightness_row, FALSE, FALSE, 0);

    GtkWidget *volume_row = make_slider_row("audio-volume-high-symbolic",
        &g_shell->volume_icon, &g_shell->volume_slider,
        G_CALLBACK(on_volume_slider_changed), 50);
    gtk_box_pack_start(GTK_BOX(card), volume_row, FALSE, FALSE, 0);
    
    GtkWidget *bottom_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(bottom_row, 16);
    gtk_widget_set_margin_end(bottom_row, 16);
    gtk_widget_set_margin_top(bottom_row, 8);
    gtk_widget_set_margin_bottom(bottom_row, 12);
    GtkWidget *settings_btn = gtk_button_new_with_label("Settings");
    gtk_style_context_add_class(gtk_widget_get_style_context(settings_btn), "action-btn");
    g_signal_connect(settings_btn, "clicked", G_CALLBACK(launch_cmd_cb), "numate-settings");
    GtkWidget *power_btn = gtk_button_new_with_label("Power");
    gtk_style_context_add_class(gtk_widget_get_style_context(power_btn), "power-btn");
    g_signal_connect(power_btn, "clicked", G_CALLBACK(launch_cmd_cb),
                     "mate-session-save --shutdown-dialog");
    gtk_box_pack_start(GTK_BOX(bottom_row), settings_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bottom_row), power_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card), bottom_row, FALSE, FALSE, 0);
    
    gtk_widget_show_all(card);
    start_wifi_polling();
    update_tile_states_async();
    refresh_volume_ui();
    refresh_battery_ui();
    
    if (g_shell->username_label) {
        struct utsname uts;
        uname(&uts);
        gchar *hostname = g_strdup_printf("%s@%s", g_get_user_name(), uts.nodename);
        gtk_label_set_text(GTK_LABEL(g_shell->username_label), hostname);
        g_free(hostname);
    }
    g_timeout_add(5000, panel_periodic_refresh, NULL);
    return window;
}

static void
toggle_panel(GtkWidget *button, gpointer user_data)
{
    (void)button; (void)user_data;
    
    if (gtk_widget_get_visible(g_shell->panel_window)) {
        gtk_widget_hide(g_shell->panel_window);
        gtk_widget_hide(g_shell->notif_window);
        return;
    }
    
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    
    gtk_window_move(GTK_WINDOW(g_shell->panel_window), -10000, -10000);
    gtk_widget_show_all(g_shell->panel_window);
    while (gtk_events_pending()) gtk_main_iteration();
    
    GtkWidget *card = gtk_bin_get_child(GTK_BIN(g_shell->panel_window));
    int panel_height = gtk_widget_get_allocated_height(card);
    int panel_width = gtk_widget_get_allocated_width(g_shell->panel_window);
    int panel_x = geometry.x + geometry.width - panel_width - PANEL_MARGIN;
    int panel_y = geometry.y + geometry.height - SHELF_H - MENU_GAP - panel_height;
    gtk_window_move(GTK_WINDOW(g_shell->panel_window), panel_x, panel_y);
    
    rebuild_notification_ui();
    gtk_widget_set_size_request(g_shell->notif_window, panel_width, -1);
    gtk_window_move(GTK_WINDOW(g_shell->notif_window), -10000, -10000);
    gtk_widget_show_all(g_shell->notif_window);
    while (gtk_events_pending()) gtk_main_iteration();
    
    GtkRequisition notif_natural;
    gtk_widget_get_preferred_size(g_shell->notif_window, NULL, &notif_natural);
    int available_height = panel_y - geometry.y - NOTIF_PANEL_GAP;
    int notif_height = MIN(notif_natural.height, available_height);
    if (notif_height < 1) notif_height = 1;
    
    gtk_widget_set_size_request(g_shell->notif_window, panel_width, notif_height);
    gtk_window_resize(GTK_WINDOW(g_shell->notif_window), panel_width, notif_height);
    int notif_x = panel_x;
    int notif_y = panel_y - NOTIF_PANEL_GAP - notif_height;
    gtk_window_move(GTK_WINDOW(g_shell->notif_window), notif_x, notif_y);
}

/* ═══════════════════════════════════════════════════════════════════════
 * APP MENU
 * ═══════════════════════════════════════════════════════════════════════ */

static gboolean
desktop_file_has_misplaced_nodisplay(GDesktopAppInfo *desktop_app)
{
    const gchar *filename = g_desktop_app_info_get_filename(desktop_app);
    if (!filename) return FALSE;
    GKeyFile *kf = g_key_file_new();
    gboolean found = FALSE;
    if (g_key_file_load_from_file(kf, filename, G_KEY_FILE_NONE, NULL)) {
        gsize n_groups = 0;
        gchar **groups = g_key_file_get_groups(kf, &n_groups);
        for (gsize i = 0; i < n_groups && !found; i++) {
            GError *err = NULL;
            gboolean val = g_key_file_get_boolean(kf, groups[i], "NoDisplay", &err);
            if (!err && val) found = TRUE;
            if (err) g_error_free(err);
        }
        g_strfreev(groups);
    }
    g_key_file_free(kf);
    return found;
}

static AppInfo *
app_info_new(GDesktopAppInfo *desktop_app)
{
    const gchar *id = g_app_info_get_id(G_APP_INFO(desktop_app));
    if (!id) return NULL;
    if (g_desktop_app_info_get_nodisplay(desktop_app)) return NULL;
    if (desktop_file_has_misplaced_nodisplay(desktop_app)) return NULL;
    
    const gchar *executable = g_app_info_get_executable(G_APP_INFO(desktop_app));
    if (!executable || !*executable) return NULL;
    
    AppInfo *app = g_new0(AppInfo, 1);
    app->id = g_strdup(id);
    app->name = g_strdup(g_app_info_get_display_name(G_APP_INFO(desktop_app)));
    app->exec = g_strdup(executable);
    app->description = g_strdup(g_app_info_get_description(G_APP_INFO(desktop_app)));
    app->categories = g_strdup(g_desktop_app_info_get_categories(desktop_app));
    GIcon *icon = g_app_info_get_icon(G_APP_INFO(desktop_app));
    if (icon) app->icon_name = g_icon_to_string(icon);
    app->dai = g_object_ref(desktop_app);
    return app;
}

static void
app_info_free(AppInfo *app)
{
    if (!app) return;
    g_free(app->id);
    g_free(app->name);
    g_free(app->exec);
    g_free(app->description);
    g_free(app->categories);
    g_free(app->icon_name);
    if (app->dai) g_object_unref(app->dai);
    g_free(app);
}

static gint
app_info_compare(gconstpointer a, gconstpointer b)
{
    const AppInfo *app_a = *(const AppInfo **)a;
    const AppInfo *app_b = *(const AppInfo **)b;
    return g_utf8_collate(app_a->name ? app_a->name : "", app_b->name ? app_b->name : "");
}

static GPtrArray *
discover_applications(void)
{
    GPtrArray *apps = g_ptr_array_new_with_free_func((GDestroyNotify)app_info_free);
    GList *all_apps = g_app_info_get_all();
    for (GList *l = all_apps; l; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data)) continue;
        AppInfo *app = app_info_new(G_DESKTOP_APP_INFO(l->data));
        if (app) g_ptr_array_add(apps, app);
    }
    g_list_free_full(all_apps, g_object_unref);
    g_ptr_array_sort(apps, app_info_compare);
    return apps;
}

static gboolean
on_app_list_row_press(GtkWidget *event_box, GdkEventButton *event, gpointer user_data)
{
    AppInfo *app = user_data;
    if (event->button != 3 || !app || !app->dai) return FALSE;
    /* Same context menu the dock uses (pin/unpin, open, windows) — the
     * app menu has no window list for this widget, so that section of
     * the menu is simply absent here, not reimplemented. */
    show_context_menu(event_box, G_APP_INFO(app->dai), event);
    return TRUE;
}

static GtkWidget *
create_app_list_row(AppInfo *app)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *event_box = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(row), event_box);
    
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 4);
    GtkWidget *icon;
    if (app->icon_name)
        icon = gtk_image_new_from_icon_name(app->icon_name, GTK_ICON_SIZE_MENU);
    else
        icon = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), APP_ICON_SIZE);
    GtkWidget *label = gtk_label_new(app->name);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(event_box), hbox);
    g_object_set_data(G_OBJECT(row), "app-info", app);
    g_signal_connect(event_box, "button-press-event",
                      G_CALLBACK(on_app_list_row_press), app);
    return row;
}

static void
on_search_entry_changed(GtkSearchEntry *entry, MenuData *menu)
{
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!query || !*query) {
        menu->current_view = VIEW_ALL;
        gtk_stack_set_visible_child_name(GTK_STACK(menu->stack), "all");
        return;
    }
    
    GList *children = gtk_container_get_children(GTK_CONTAINER(menu->search_list));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    
    gchar *query_lower = g_utf8_strdown(query, -1);
    for (guint i = 0; i < menu->apps->len; i++) {
        AppInfo *app = menu->apps->pdata[i];
        gchar *name_lower = g_utf8_strdown(app->name ? app->name : "", -1);
        if (strstr(name_lower, query_lower)) {
            GtkWidget *row = create_app_list_row(app);
            gtk_list_box_insert(GTK_LIST_BOX(menu->search_list), row, -1);
        }
        g_free(name_lower);
    }
    g_free(query_lower);
    gtk_widget_show_all(menu->search_list);
    menu->current_view = VIEW_SEARCH;
    gtk_stack_set_visible_child_name(GTK_STACK(menu->stack), "search");
}

static void
on_app_list_row_activated(GtkListBox *list_box, GtkListBoxRow *row, MenuData *menu)
{
    (void)list_box;
    AppInfo *app = g_object_get_data(G_OBJECT(row), "app-info");
    if (app && app->dai) g_app_info_launch(G_APP_INFO(app->dai), NULL, NULL, NULL);
    gtk_widget_hide(menu->window);
    gtk_entry_set_text(GTK_ENTRY(menu->search_entry), "");
}

static void
on_apps_installed_changed(MenuData *menu)
{
    if (menu->apps) g_ptr_array_free(menu->apps, TRUE);
    menu->apps = discover_applications();
    if (!menu->app_list) return;
    
    GList *children = gtk_container_get_children(GTK_CONTAINER(menu->app_list));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    
    for (guint i = 0; i < menu->apps->len; i++) {
        GtkWidget *row = create_app_list_row(menu->apps->pdata[i]);
        gtk_list_box_insert(GTK_LIST_BOX(menu->app_list), row, -1);
    }
    gtk_widget_show_all(menu->app_list);
}

static void
menu_show(MenuData *menu)
{
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    
    gtk_window_move(GTK_WINDOW(menu->window), -10000, -10000);
    gtk_widget_show_all(menu->window);
    while (gtk_events_pending()) gtk_main_iteration();
    
    GtkAllocation allocation;
    gtk_widget_get_allocation(menu->window, &allocation);
    int x = geometry.x + 4;
    int y = geometry.y + geometry.height - SHELF_H - MENU_GAP - allocation.height;
    gtk_window_move(GTK_WINDOW(menu->window), x, y);
    gtk_window_present(GTK_WINDOW(menu->window));
    gtk_widget_grab_focus(menu->search_entry);

    /* GTK focus alone does not route X11 key events to an override-redirect
     * window; take the seat's keyboard explicitly. Same idiom on_dock_press
     * already uses for pointer grabs during drag. */
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    gdk_seat_grab(seat, gtk_widget_get_window(menu->window),
                  GDK_SEAT_CAPABILITY_KEYBOARD, TRUE,
                  NULL, NULL, NULL, NULL);
}

static void
menu_hide(MenuData *menu)
{
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    gdk_seat_ungrab(seat);
    gtk_widget_hide(menu->window);
    gtk_entry_set_text(GTK_ENTRY(menu->search_entry), "");
}

static void
menu_toggle(MenuData *menu)
{
    if (gtk_widget_get_visible(menu->window)) menu_hide(menu);
    else menu_show(menu);
}

static MenuData *
menu_create(void)
{
    MenuData *menu = g_new0(MenuData, 1);
    menu->window = gtk_window_new(GTK_WINDOW_POPUP);
    ensure_rgba_visual(menu->window);
    gtk_window_set_decorated(GTK_WINDOW(menu->window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(menu->window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(menu->window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(menu->window), TRUE);
    /*
     * GTK_WINDOW_POPUP is override-redirect: the WM (Marco) never assigns
     * it input focus by policy, regardless of gtk_widget_grab_focus(),
     * which only sets GTK's internal focus widget and does nothing at the
     * X11 level. Without DIALOG type-hint + an explicit keyboard grab on
     * show, the search entry never receives key events and "search-changed"
     * never fires. The instance popup and drag ghost stay GTK_WINDOW_POPUP
     * correctly — neither takes text input — so this is scoped to the menu.
     */
    gtk_window_set_type_hint(GTK_WINDOW(menu->window), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_accept_focus(GTK_WINDOW(menu->window), TRUE);
    
    GtkWidget *outer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(outer_box, "GonzoMenu");
    gtk_style_context_add_class(gtk_widget_get_style_context(outer_box), "card");
    gtk_widget_set_size_request(outer_box, 300, 450);
    gtk_container_add(GTK_CONTAINER(menu->window), outer_box);
    
    menu->search_entry = gtk_search_entry_new();
    gtk_widget_set_name(menu->search_entry, "GonzoMenuSearch");
    gtk_widget_set_margin_start(menu->search_entry, 8);
    gtk_widget_set_margin_end(menu->search_entry, 8);
    gtk_widget_set_margin_top(menu->search_entry, 8);
    gtk_box_pack_start(GTK_BOX(outer_box), menu->search_entry, FALSE, FALSE, 0);
    
    menu->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(menu->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(menu->stack), 80);
    gtk_widget_set_margin_bottom(menu->stack, 10);
    gtk_box_pack_start(GTK_BOX(outer_box), menu->stack, TRUE, TRUE, 0);
    
    GtkWidget *all_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(all_scroll), GTK_SHADOW_NONE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(all_scroll), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    menu->app_list = gtk_list_box_new();
    gtk_widget_set_name(menu->app_list, "GonzoMenuAppList");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(menu->app_list), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(all_scroll), menu->app_list);
    gtk_stack_add_named(GTK_STACK(menu->stack), all_scroll, "all");
    
    GtkWidget *search_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(search_scroll), GTK_SHADOW_NONE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(search_scroll), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    menu->search_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(menu->search_list), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(search_scroll), menu->search_list);
    gtk_stack_add_named(GTK_STACK(menu->stack), search_scroll, "search");
    
    g_signal_connect(menu->search_entry, "search-changed", G_CALLBACK(on_search_entry_changed), menu);
    g_signal_connect(menu->app_list, "row-activated", G_CALLBACK(on_app_list_row_activated), menu);
    g_signal_connect(menu->search_list, "row-activated", G_CALLBACK(on_app_list_row_activated), menu);
    
    menu->apps = discover_applications();
    GAppInfoMonitor *monitor = g_app_info_monitor_get();
    g_signal_connect_swapped(monitor, "changed", G_CALLBACK(on_apps_installed_changed), menu);
    
    for (guint i = 0; i < menu->apps->len; i++) {
        GtkWidget *row = create_app_list_row(menu->apps->pdata[i]);
        gtk_list_box_insert(GTK_LIST_BOX(menu->app_list), row, -1);
    }
    return menu;
}

/* ═══════════════════════════════════════════════════════════════════════
 * DOCK — identity tracking
 * ═══════════════════════════════════════════════════════════════════════ */

static void
identity_register(const char *desktop_id, GtkWidget *widget)
{
    if (!desktop_id || !desktop_id[0] || !widget) return;
    if (!g_hash_table_contains(g_shell->identity_table, desktop_id))
        g_hash_table_insert(g_shell->identity_table, g_strdup(desktop_id), widget);
}

static GtkWidget *
identity_lookup(const char *desktop_id)
{
    if (!desktop_id || !desktop_id[0]) return NULL;
    return g_hash_table_lookup(g_shell->identity_table, desktop_id);
}

static void
identity_unregister_widget(GtkWidget *widget)
{
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, g_shell->identity_table);
    while (g_hash_table_iter_next(&iter, &key, &value))
        if (value == (gpointer)widget) g_hash_table_iter_remove(&iter);
}

/* ═══════════════════════════════════════════════════════════════════════
 * DOCK — window identity matching
 * ═══════════════════════════════════════════════════════════════════════ */

static void
get_window_wm_class(WnckWindow *window, const char **res_class, const char **res_name)
{
    WnckClassGroup *class_group = wnck_window_get_class_group(window);
    *res_class = class_group ? wnck_class_group_get_id(class_group) : NULL;
    *res_name = wnck_window_get_class_instance_name(window);
}

static void
find_desktop_id_for_window(WnckWindow *window, char *output, int output_size)
{
    WnckClassGroup *class_group = wnck_window_get_class_group(window);
    const char *res_class = class_group ? wnck_class_group_get_id(class_group) : NULL;
    const char *res_name = wnck_window_get_class_instance_name(window);

    for (int i = 0; i < g_shell->pinned_count; i++) {
        const char *pinned_id = g_shell->pinned_ids[i];
        if (!pinned_id) continue;

        char pinned_stem[128] = {0};
        size_t si = 0;
        for (; pinned_id[si] && pinned_id[si] != '.' && pinned_id[si] != '-' && si < sizeof(pinned_stem) - 1; si++)
            pinned_stem[si] = pinned_id[si];

        char pinned_full_stem[128] = {0};
        const char *pdot = strstr(pinned_id, ".desktop");
        size_t plen = pdot ? (size_t)(pdot - pinned_id) : strlen(pinned_id);
        if (plen >= sizeof(pinned_full_stem)) plen = sizeof(pinned_full_stem) - 1;
        memcpy(pinned_full_stem, pinned_id, plen);
        pinned_full_stem[plen] = '\0';

        gboolean pinned_match = FALSE;
        if (pinned_full_stem[0] && res_class && !g_ascii_strcasecmp(pinned_full_stem, res_class)) pinned_match = TRUE;
        if (pinned_full_stem[0] && res_name && !g_ascii_strcasecmp(pinned_full_stem, res_name)) pinned_match = TRUE;
        if (pinned_stem[0] && res_class && !g_ascii_strcasecmp(pinned_stem, res_class)) pinned_match = TRUE;
        if (pinned_stem[0] && res_name && !g_ascii_strcasecmp(pinned_stem, res_name)) pinned_match = TRUE;

        if (pinned_match) {
            GtkWidget *cached = res_class ? identity_lookup(res_class) : NULL;
            if (!cached && res_name) cached = identity_lookup(res_name);
            GAppInfo *cached_app = cached ? G_APP_INFO(g_object_get_data(G_OBJECT(cached), "app_info")) : NULL;
            const char *cached_id = cached_app ? g_app_info_get_id(cached_app) : NULL;

            if (g_strcmp0(cached_id, pinned_id) != 0) {
                GList *children = gtk_container_get_children(GTK_CONTAINER(g_shell->dock_box));
                for (GList *l = children; l; l = l->next) {
                    GtkWidget *child = GTK_WIDGET(l->data);
                    GAppInfo *app = G_APP_INFO(g_object_get_data(G_OBJECT(child), "app_info"));
                    if (app && g_strcmp0(g_app_info_get_id(app), pinned_id) == 0) {
                        if (res_class) g_hash_table_insert(g_shell->identity_table, g_strdup(res_class), child);
                        if (res_name) g_hash_table_insert(g_shell->identity_table, g_strdup(res_name), child);
                        break;
                    }
                }
                g_list_free(children);
            }
            g_strlcpy(output, pinned_id, output_size);
            return;
        }
    }

    GtkWidget *existing = NULL;
    if (res_class) existing = identity_lookup(res_class);
    if (!existing && res_name) existing = identity_lookup(res_name);
    if (existing) {
        GAppInfo *app = G_APP_INFO(g_object_get_data(G_OBJECT(existing), "app_info"));
        const char *id = app ? g_app_info_get_id(app) : NULL;
        if (id) { g_strlcpy(output, id, output_size); return; }
    }
    
    GList *all_apps = g_app_info_get_all();
    for (int pass = 0; pass < 2 && output[0] == '\0'; pass++) {
        for (GList *l = all_apps; l; l = l->next) {
            GAppInfo *app_info = l->data;
            if (!G_IS_DESKTOP_APP_INFO(app_info)) continue;
            GDesktopAppInfo *desktop_info = G_DESKTOP_APP_INFO(app_info);
            if (pass == 0 && g_desktop_app_info_get_nodisplay(desktop_info)) continue;
            if (pass == 1 && !g_desktop_app_info_get_nodisplay(desktop_info)) continue;
            
            const char *app_id = g_app_info_get_id(app_info);
            const char *startup_wm = g_desktop_app_info_get_startup_wm_class(desktop_info);
            const char *executable = g_app_info_get_executable(app_info);
            const char *exe_name = NULL;
            if (executable) {
                const char *slash = strrchr(executable, '/');
                exe_name = slash ? slash + 1 : executable;
            }
            
            char stem[128] = {0};
            if (app_id) {
                size_t i = 0;
                for (; app_id[i] && app_id[i] != '.' && app_id[i] != '-' && i < sizeof(stem) - 1; i++)
                    stem[i] = app_id[i];
            }
            char full_id_stem[128] = {0};
            if (app_id) {
                const char *dot = strstr(app_id, ".desktop");
                size_t len = dot ? (size_t)(dot - app_id) : strlen(app_id);
                if (len >= sizeof(full_id_stem)) len = sizeof(full_id_stem) - 1;
                memcpy(full_id_stem, app_id, len);
                full_id_stem[len] = '\0';
            }

            gboolean match = FALSE;
            if (startup_wm && res_class && !g_ascii_strcasecmp(res_class, startup_wm)) match = TRUE;
            if (startup_wm && res_name && !g_ascii_strcasecmp(res_name, startup_wm)) match = TRUE;
            if (full_id_stem[0] && res_class && !g_ascii_strcasecmp(full_id_stem, res_class)) match = TRUE;
            if (full_id_stem[0] && res_name && !g_ascii_strcasecmp(full_id_stem, res_name)) match = TRUE;
            if (stem[0] && res_class && !g_ascii_strcasecmp(stem, res_class)) match = TRUE;
            if (stem[0] && res_name && !g_ascii_strcasecmp(stem, res_name)) match = TRUE;
            if (exe_name && res_class && !g_ascii_strcasecmp(exe_name, res_class)) match = TRUE;
            if (exe_name && res_name && !g_ascii_strcasecmp(exe_name, res_name)) match = TRUE;
            
            if (match) {
                g_strlcpy(output, app_id, output_size);
                break;
            }
        }
    }
    g_list_free_full(all_apps, g_object_unref);
}

/* ═══════════════════════════════════════════════════════════════════════
 * DOCK — icon animations
 * ═══════════════════════════════════════════════════════════════════════ */

static gboolean
nudge_animation_tick(gpointer data)
{
    TweenData *tween = data;
    if (!GTK_IS_IMAGE(tween->img)) return G_SOURCE_REMOVE;
    tween->count++;
    double progress = (double)tween->count / tween->steps;
    double eased = sin(progress * (M_PI / 2.0));
    int new_size = tween->start + (int)((tween->end - tween->start) * eased);
    gtk_image_set_pixel_size(tween->img, new_size);
    
    if (tween->count >= tween->steps) {
        gtk_image_set_pixel_size(tween->img, tween->end);
        g_object_set_data(G_OBJECT(tween->img), "tween-data", NULL);
        g_object_set_data(G_OBJECT(tween->img), "tween-timer", NULL);
        g_free(tween);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void
nudge_icon(GtkImage *image, gboolean entering, int base_size)
{
    guint timer_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(image), "tween-timer"));
    if (timer_id) g_source_remove(timer_id);
    
    TweenData *old_tween = g_object_get_data(G_OBJECT(image), "tween-data");
    if (old_tween) g_free(old_tween);
    
    TweenData *tween = g_new0(TweenData, 1);
    tween->img = image;
    tween->start = gtk_image_get_pixel_size(image);
    tween->end = entering ? (base_size + NUDGE_PX) : base_size;
    tween->steps = 6;
    g_object_set_data(G_OBJECT(image), "tween-data", tween);
    g_object_set_data(G_OBJECT(image), "tween-timer",
                      GUINT_TO_POINTER(g_timeout_add(16, nudge_animation_tick, tween)));
}

static void
on_image_destroy_cleanup(GtkWidget *widget, gpointer user_data)
{
    (void)user_data;
    guint timer_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "tween-timer"));
    if (timer_id) g_source_remove(timer_id);
    TweenData *tween = g_object_get_data(G_OBJECT(widget), "tween-data");
    if (tween) g_free(tween);
}

/* ═══════════════════════════════════════════════════════════════════════
 * DOCK — item creation
 * ═══════════════════════════════════════════════════════════════════════ */

static GtkWidget *
create_dock_item(GAppInfo *app, int icon_size, int widget_width, int widget_height)
{
    GtkWidget *event_box = gtk_event_box_new();
    gtk_widget_set_size_request(event_box, widget_width, widget_height);
    gtk_widget_add_events(event_box,
        GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
        GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
    
    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(event_box), overlay);
    
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(overlay), vbox);
    
    GIcon *app_icon = g_app_info_get_icon(app);
    GtkWidget *icon_widget;
    if (app_icon)
        icon_widget = gtk_image_new_from_gicon(app_icon, GTK_ICON_SIZE_DIALOG);
    else
        icon_widget = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(icon_widget), icon_size);
    gtk_box_pack_start(GTK_BOX(vbox), icon_widget, FALSE, FALSE, 0);
    
    GtkWidget *indicator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(indicator, 10, 2);
    gtk_widget_set_halign(indicator, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(indicator, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(indicator, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(indicator), "running-indicator");
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), indicator);
    gtk_widget_set_no_show_all(indicator, TRUE);
    
    gtk_style_context_add_class(gtk_widget_get_style_context(event_box), "app-btn");
    
    g_object_set_data(G_OBJECT(event_box), "indicator-ref", indicator);
    g_object_set_data(G_OBJECT(event_box), "icon-size", GINT_TO_POINTER(icon_size));
    g_object_set_data(G_OBJECT(event_box), "img-ref", icon_widget);
    g_signal_connect(icon_widget, "destroy", G_CALLBACK(on_image_destroy_cleanup), NULL);
    
    g_signal_connect_swapped(event_box, "destroy", G_CALLBACK(cancel_instance_popup), NULL);
    g_signal_connect(event_box, "destroy", G_CALLBACK(on_dock_item_destroy), NULL);
    
    return event_box;
}

/* ═══════════════════════════════════════════════════════════════════════
 * DOCK — instance popup
 * ═══════════════════════════════════════════════════════════════════════ */

static void
on_dock_item_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)user_data;
    g_list_free(g_object_get_data(G_OBJECT(widget), "win_list"));
    g_object_set_data(G_OBJECT(widget), "win_list", NULL);

    if (g_shell->drag_widget == widget) {
        g_shell->drag_widget = NULL;
        g_shell->drag_active = FALSE;
    }
}

static void
cancel_instance_popup(void)
{
    if (g_shell->popup_poll_timer) {
        g_source_remove(g_shell->popup_poll_timer);
        g_shell->popup_poll_timer = 0;
    }
    if (g_shell->instance_popup) {
        gtk_widget_destroy(g_shell->instance_popup);
        g_shell->instance_popup = NULL;
        g_shell->current_dock_icon = NULL;
    }
}

static void
on_instance_item_clicked(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    WnckWindow *window = WNCK_WINDOW(user_data);
    if (window) wnck_window_activate(window, gtk_get_current_event_time());
    cancel_instance_popup();
}

static gboolean
is_pointer_over_widget(GtkWidget *widget)
{
    if (!widget || !gtk_widget_get_window(widget)) return FALSE;
    GdkDisplay *display = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(display);
    GdkDevice *pointer = gdk_seat_get_pointer(seat);
    int pointer_x, pointer_y;
    gdk_device_get_position(pointer, NULL, &pointer_x, &pointer_y);
    gint widget_x, widget_y;
    gdk_window_get_origin(gtk_widget_get_window(widget), &widget_x, &widget_y);
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    return (pointer_x >= widget_x && pointer_x <= widget_x + allocation.width &&
            pointer_y >= widget_y && pointer_y <= widget_y + allocation.height);
}

static gboolean
popup_poll_check(gpointer user_data)
{
    (void)user_data;
    if (!g_shell->instance_popup) {
        g_shell->popup_poll_timer = 0;
        return FALSE;
    }
    if (is_pointer_over_widget(g_shell->current_dock_icon) ||
        is_pointer_over_widget(g_shell->instance_popup))
        return TRUE;
    cancel_instance_popup();
    return FALSE;
}

static void
start_popup_polling(void)
{
    if (g_shell->popup_poll_timer) return;
    g_shell->popup_poll_timer = g_timeout_add(POLL_INTERVAL_MS, popup_poll_check, NULL);
}

static void
show_instance_popup(GtkWidget *dock_icon)
{
    if (g_shell->instance_popup && g_shell->current_dock_icon != dock_icon)
        cancel_instance_popup();
    if (g_shell->instance_popup) {
        start_popup_polling();
        return;
    }
    
    GList *windows = g_object_get_data(G_OBJECT(dock_icon), "win_list");
    if (!windows || g_list_length(windows) <= 1) return;
    
    g_shell->instance_popup = gtk_window_new(GTK_WINDOW_POPUP);
    ensure_rgba_visual(g_shell->instance_popup);
    gtk_window_set_decorated(GTK_WINDOW(g_shell->instance_popup), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(g_shell->instance_popup), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(g_shell->instance_popup), TRUE);
    
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    
    for (GList *l = windows; l; l = l->next) {
        WnckWindow *window = WNCK_WINDOW(l->data);
        const char *title = wnck_window_get_name(window);
        GtkWidget *button = gtk_button_new_with_label(title ? title : "Untitled");
        gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
        g_signal_connect(button, "clicked", G_CALLBACK(on_instance_item_clicked), window);
        gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    }
    
    gtk_container_add(GTK_CONTAINER(card), box);
    gtk_container_add(GTK_CONTAINER(g_shell->instance_popup), card);
    gtk_widget_show_all(g_shell->instance_popup);
    
    GtkRequisition requisition;
    gtk_widget_get_preferred_size(g_shell->instance_popup, NULL, &requisition);
    GtkAllocation icon_allocation;
    gtk_widget_get_allocation(dock_icon, &icon_allocation);
    int icon_x, icon_y;
    gdk_window_get_origin(gtk_widget_get_window(dock_icon), &icon_x, &icon_y);
    int popup_x = icon_x + (icon_allocation.width / 2) - (requisition.width / 2);
    int popup_y = icon_y - requisition.height - POPUP_GAP;
    gtk_window_move(GTK_WINDOW(g_shell->instance_popup), popup_x, popup_y);
    
    g_shell->current_dock_icon = dock_icon;
    start_popup_polling();
}

/* ═══════════════════════════════════════════════════════════════════════
 * DOCK — hover & context menu
 * ═══════════════════════════════════════════════════════════════════════ */

static gboolean
on_dock_item_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
    (void)event;
    DockSlot *slot = user_data;
    nudge_icon(slot->image, TRUE, DOCK_ICON_SIZE);
    if (g_shell->instance_popup && g_shell->current_dock_icon != widget)
        cancel_instance_popup();
    GList *windows = g_object_get_data(G_OBJECT(slot->widget), "win_list");
    if (g_list_length(windows) > 1) show_instance_popup(slot->widget);
    return FALSE;
}

static gboolean
on_dock_item_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
    (void)widget; (void)event;
    DockSlot *slot = user_data;
    nudge_icon(slot->image, FALSE, DOCK_ICON_SIZE);
    return FALSE;
}

static void
on_open_clicked(GtkWidget *item, gpointer user_data)
{
    (void)item;
    GAppInfo *app = G_APP_INFO(user_data);
    g_app_info_launch(app, NULL, NULL, NULL);
    g_object_unref(app);
}

static void
on_context_menu_instance_clicked(GtkWidget *item, gpointer user_data)
{
    (void)item;
    WnckWindow *window = WNCK_WINDOW(user_data);
    if (WNCK_IS_WINDOW(window)) {
        if (wnck_window_is_minimized(window))
            wnck_window_unminimize(window, gtk_get_current_event_time());
        wnck_window_activate(window, gtk_get_current_event_time());
    }
}

static gpointer
copy_object_ref(gconstpointer src, gpointer user_data)
{
    (void)user_data;
    return g_object_ref(G_OBJECT((gpointer)src));
}

static void
close_app_action_widget(GtkWidget *widget)
{
    if (!widget) return;

    GList *snapshot = g_list_copy_deep(g_object_get_data(G_OBJECT(widget), "win_list"),
                                       copy_object_ref, NULL);
    guint32 timestamp = gtk_get_current_event_time();

    for (GList *l = snapshot; l; l = l->next)
        if (WNCK_IS_WINDOW(l->data))
            wnck_window_close(WNCK_WINDOW(l->data), timestamp);

    g_list_free_full(snapshot, g_object_unref);
}

static void
pin_action(gpointer user_data)
{
    const char *desktop_id = g_app_info_get_id(G_APP_INFO(user_data));
    for (int i = 0; i < g_shell->pinned_count; i++) {
        if (g_strcmp0(g_shell->pinned_ids[i], desktop_id) == 0) {
            g_object_unref(user_data);
            return;
        }
    }
    if (g_shell->pinned_count < 64) {
        g_shell->pinned_ids[g_shell->pinned_count] = g_strdup(desktop_id);
        g_shell->pinned_count++;
        save_config();
        refresh_dock();
    }
    g_object_unref(user_data);
}

static void
unpin_action(gpointer user_data)
{
    const char *desktop_id = g_app_info_get_id(G_APP_INFO(user_data));
    for (int i = 0; i < g_shell->pinned_count; i++) {
        if (g_strcmp0(g_shell->pinned_ids[i], desktop_id) == 0) {
            g_free(g_shell->pinned_ids[i]);
            for (int j = i; j < g_shell->pinned_count - 1; j++)
                g_shell->pinned_ids[j] = g_shell->pinned_ids[j + 1];
            g_shell->pinned_count--;
            save_config();
            refresh_dock();
            break;
        }
    }
    g_object_unref(user_data);
}

static void
show_context_menu(GtkWidget *widget, GAppInfo *app, GdkEventButton *event)
{
    GtkWidget *menu = gtk_menu_new();
    const char *desktop_id = g_app_info_get_id(app);
    
    gboolean is_pinned = FALSE;
    for (int i = 0; i < g_shell->pinned_count; i++) {
        if (g_strcmp0(g_shell->pinned_ids[i], desktop_id) == 0) {
            is_pinned = TRUE;
            break;
        }
    }
    
    if (is_pinned) {
        GtkWidget *item = gtk_menu_item_new_with_label("Unpin from Dock");
        g_signal_connect_swapped(item, "activate", G_CALLBACK(unpin_action), g_object_ref(app));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    } else {
        GtkWidget *item = gtk_menu_item_new_with_label("Pin to Dock");
        g_signal_connect_swapped(item, "activate", G_CALLBACK(pin_action), g_object_ref(app));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    
    GtkWidget *new_window_item = gtk_menu_item_new_with_label("Open New Window");
    g_signal_connect(new_window_item, "activate", G_CALLBACK(on_open_clicked), g_object_ref(app));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), new_window_item);
    
    GList *windows = g_object_get_data(G_OBJECT(widget), "win_list");
    if (windows) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        GtkWidget *windows_header = gtk_menu_item_new_with_label("Windows");
        GtkWidget *windows_menu = gtk_menu_new();
        GList *windows_copy = g_list_copy(windows);
        windows_copy = g_list_reverse(windows_copy);
        for (GList *l = windows_copy; l; l = l->next) {
            WnckWindow *window = WNCK_WINDOW(l->data);
            if (!WNCK_IS_WINDOW(window)) continue;
            const char *title = wnck_window_get_name(window);
            GtkWidget *item = gtk_menu_item_new_with_label(title ? title : "(untitled)");
            g_signal_connect(item, "activate", G_CALLBACK(on_context_menu_instance_clicked), window);
            gtk_menu_shell_append(GTK_MENU_SHELL(windows_menu), item);
        }
        g_list_free(windows_copy);
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(windows_header), windows_menu);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), windows_header);
        
        GtkWidget *close_all_item = gtk_menu_item_new_with_label("Close All");
        g_signal_connect_swapped(close_all_item, "activate", G_CALLBACK(close_app_action_widget), widget);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_all_item);
    }
    
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

/* ═══════════════════════════════════════════════════════════════════════
 * DOCK — click & drag handling
 * ═══════════════════════════════════════════════════════════════════════ */

static gboolean
on_dock_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    (void)user_data;
    GAppInfo *app = G_APP_INFO(g_object_get_data(G_OBJECT(widget), "app_info"));
    if (!app) return FALSE;
    
    if (event->button == 3) {
        show_context_menu(widget, app, event);
        return TRUE;
    }
    if (event->button != 1) return FALSE;
    
    if (g_shell->drag_app) g_object_unref(g_shell->drag_app);
    g_shell->drag_app = g_object_ref(app);
    g_shell->drag_widget = widget;
    g_shell->drag_start_x = event->x_root;
    g_shell->drag_start_y = event->y_root;
    g_shell->drag_active = FALSE;
    g_shell->drag_insert_index = g_shell->slot_count;
    
    gdk_seat_grab(gdk_display_get_default_seat(gdk_display_get_default()),
        gtk_widget_get_window(g_shell->shelf_window),
        GDK_SEAT_CAPABILITY_ALL_POINTING, FALSE, NULL, (GdkEvent *)event, NULL, NULL);
    return TRUE;
}

static gboolean
gap_animation_tick(gpointer user_data)
{
    DockSlot *slot = user_data;
    const double speed = 0.3;
    slot->gap_margin_start += (slot->gap_margin_start_target - slot->gap_margin_start) * speed;
    slot->gap_margin_end   += (slot->gap_margin_end_target   - slot->gap_margin_end)   * speed;
    gtk_widget_set_margin_start(slot->widget, (int)slot->gap_margin_start);
    gtk_widget_set_margin_end(slot->widget,   (int)slot->gap_margin_end);
    if (abs(slot->gap_margin_start - slot->gap_margin_start_target) < 1 &&
        abs(slot->gap_margin_end   - slot->gap_margin_end_target)   < 1) {
        slot->gap_margin_start = slot->gap_margin_start_target;
        slot->gap_margin_end   = slot->gap_margin_end_target;
        slot->gap_anim_id = 0;
        return FALSE;
    }
    return TRUE;
}

static int
compute_drag_insert_index(double x_root)
{
    GdkWindow *dock_window = gtk_widget_get_window(g_shell->dock_box);
    if (!dock_window) return g_shell->slot_count;
    int dock_x = 0;
    gdk_window_get_origin(dock_window, &dock_x, NULL);
    double local_x = x_root - dock_x;
    const char *drag_id = g_shell->drag_app ? g_app_info_get_id(g_shell->drag_app) : NULL;
    int candidate = g_shell->slot_count;
    int visual_position = 0;
    
    for (int i = 0; i < g_shell->slot_count; i++) {
        DockSlot *slot = g_shell->slots[i];
        if (drag_id && slot->app && g_strcmp0(drag_id, g_app_info_get_id(slot->app)) == 0)
            continue;
        GtkAllocation allocation;
        gtk_widget_get_allocation(slot->widget, &allocation);
        if (local_x < allocation.x + allocation.width / 2) {
            candidate = visual_position;
            break;
        }
        visual_position++;
    }
    return candidate;
}

static void
animate_drag_gaps(void)
{
    int insert_index = g_shell->drag_insert_index;
    const char *drag_id = g_shell->drag_app ? g_app_info_get_id(g_shell->drag_app) : NULL;
    int visual_position = 0;
    
    for (int i = 0; i < g_shell->slot_count; i++) {
        DockSlot *slot = g_shell->slots[i];
        gboolean is_source = drag_id && slot->app &&
            (g_strcmp0(drag_id, g_app_info_get_id(slot->app)) == 0);
        if (is_source) {
            slot->gap_margin_start_target = 0;
            slot->gap_margin_end_target = 0;
        } else {
            int margin_start = 0, margin_end = 0;
            if (visual_position == insert_index)
                margin_start = DRAG_GAP_PX;
            else if (insert_index >= g_shell->slot_count && i == g_shell->slot_count - 1)
                margin_end = DRAG_GAP_PX;
            slot->gap_margin_start_target = margin_start;
            slot->gap_margin_end_target = margin_end;
            visual_position++;
        }
        if (!slot->gap_anim_id)
            slot->gap_anim_id = g_timeout_add(16, gap_animation_tick, slot);
    }
}

static gboolean
on_shelf_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
    (void)widget; (void)user_data;
    if (!g_shell->drag_app) return FALSE;
    
    if (!g_shell->drag_active) {
        double dx = event->x_root - g_shell->drag_start_x;
        double dy = event->y_root - g_shell->drag_start_y;
        if (sqrt(dx * dx + dy * dy) < DRAG_THRESHOLD) return FALSE;
        g_shell->drag_active = TRUE;
        g_shell->drag_ghost = gtk_window_new(GTK_WINDOW_POPUP);
        gtk_window_set_type_hint(GTK_WINDOW(g_shell->drag_ghost), GDK_WINDOW_TYPE_HINT_DND);
        gtk_widget_set_app_paintable(g_shell->drag_ghost, TRUE);
        ensure_rgba_visual(g_shell->drag_ghost);
        GIcon *icon = g_app_info_get_icon(g_shell->drag_app);
        GtkWidget *ghost_image = gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(ghost_image), DOCK_ICON_SIZE);
        gtk_widget_set_opacity(g_shell->drag_ghost, 0.75);
        gtk_container_add(GTK_CONTAINER(g_shell->drag_ghost), ghost_image);
        gtk_widget_show_all(g_shell->drag_ghost);
    }
    
    if (g_shell->drag_ghost) {
        int offset = DOCK_ICON_SIZE / 2;
        gtk_window_move(GTK_WINDOW(g_shell->drag_ghost),
                        (int)event->x_root - offset, (int)event->y_root - offset);
    }
    
    int new_index = compute_drag_insert_index(event->x_root);
    if (new_index != g_shell->drag_insert_index) {
        g_shell->drag_insert_index = new_index;
        animate_drag_gaps();
    }
    
    gdk_event_request_motions(event);
    return FALSE;
}

static gboolean
on_dock_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    (void)widget; (void)user_data;
    if (event->button != 1 || !g_shell->drag_app) return FALSE;
    
    gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
    gboolean was_drag = g_shell->drag_active;
    
    if (!was_drag) {
        GAppInfo *app = g_shell->drag_app;
        GtkWidget *clicked_widget = g_shell->drag_widget;
        GList *windows = clicked_widget ? g_object_get_data(G_OBJECT(clicked_widget), "win_list") : NULL;
        
        if (!windows) {
            g_app_info_launch(app, NULL, NULL, NULL);
        } else {
            WnckWindow *top_window = NULL;
            GList *stacked = wnck_screen_get_windows_stacked(g_shell->screen);
            for (GList *l = stacked; l; l = l->next) {
                WnckWindow *w = WNCK_WINDOW(l->data);
                if (!wnck_window_is_skip_tasklist(w)) top_window = w;
            }
            
            gboolean app_is_focused = FALSE;
            WnckWindow *focused_instance = NULL;
            for (GList *l = windows; l; l = l->next) {
                WnckWindow *w = WNCK_WINDOW(l->data);
                if (w == top_window) { app_is_focused = TRUE; focused_instance = w; break; }
            }
            
            if (app_is_focused && !wnck_window_is_minimized(focused_instance)) {
                wnck_window_minimize(focused_instance);
            } else {
                WnckWindow *target = NULL;
                for (GList *l = windows; l; l = l->next) {
                    WnckWindow *w = WNCK_WINDOW(l->data);
                    if (wnck_window_is_minimized(w)) { target = w; break; }
                }
                if (!target) target = WNCK_WINDOW(windows->data);
                if (wnck_window_is_minimized(target))
                    wnck_window_unminimize(target, (guint32)event->time);
                wnck_window_activate(target, (guint32)event->time);
            }
        }
    } else {
        const char *dragged_id = g_app_info_get_id(g_shell->drag_app);
        int insert_at = g_shell->drag_insert_index;
        char *remaining[64];
        int remaining_count = 0;
        for (int i = 0; i < g_shell->pinned_count; i++)
            if (g_strcmp0(g_shell->pinned_ids[i], dragged_id) != 0)
                remaining[remaining_count++] = g_shell->pinned_ids[i];
        
        int clamped_index = CLAMP(insert_at, 0, remaining_count);
        int new_count = 0;
        for (int i = 0; i < clamped_index; i++)
            g_shell->pinned_ids[new_count++] = remaining[i];
        g_shell->pinned_ids[new_count++] = g_strdup(dragged_id);
        for (int i = clamped_index; i < remaining_count; i++)
            g_shell->pinned_ids[new_count++] = remaining[i];
        g_shell->pinned_count = new_count;
        save_config();
    }
    
    if (g_shell->drag_ghost) { gtk_widget_destroy(g_shell->drag_ghost); g_shell->drag_ghost = NULL; }
    if (g_shell->drag_app)   { g_object_unref(g_shell->drag_app);   g_shell->drag_app = NULL; }
    g_shell->drag_active = FALSE;
    
    for (int i = 0; i < g_shell->slot_count; i++) {
        DockSlot *slot = g_shell->slots[i];
        slot->gap_margin_start_target = 0;
        slot->gap_margin_end_target = 0;
        if (!slot->gap_anim_id)
            slot->gap_anim_id = g_timeout_add(16, gap_animation_tick, slot);
    }
    
    if (was_drag) refresh_dock();
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════════
 * DOCK — window tracking
 * ═══════════════════════════════════════════════════════════════════════ */

static void
on_window_opened(WnckScreen *screen, WnckWindow *window, gpointer user_data)
{
    (void)screen; (void)user_data;
    if (wnck_window_is_skip_tasklist(window)) return;
    
    const char *res_class = NULL, *res_name = NULL;
    get_window_wm_class(window, &res_class, &res_name);
    
    char desktop_id[256] = {0};
    find_desktop_id_for_window(window, desktop_id, sizeof(desktop_id));
    if (!desktop_id[0]) return;
    
    GtkWidget *target_widget = identity_lookup(desktop_id);
    if (!target_widget && res_class) target_widget = identity_lookup(res_class);
    if (!target_widget && res_name) target_widget = identity_lookup(res_name);
    
    if (!target_widget) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(g_shell->dock_box));
        for (GList *l = children; l; l = l->next) {
            GtkWidget *child = GTK_WIDGET(l->data);
            GAppInfo *app = G_APP_INFO(g_object_get_data(G_OBJECT(child), "app_info"));
            if (app && g_strcmp0(g_app_info_get_id(app), desktop_id) == 0) {
                target_widget = child;
                break;
            }
        }
        g_list_free(children);
    }
    
    if (!target_widget) {
        GDesktopAppInfo *desktop_app = g_desktop_app_info_new(desktop_id);
        if (!desktop_app) return;
        target_widget = create_dock_item(G_APP_INFO(desktop_app), DOCK_ICON_SIZE, DOCK_ICON_SIZE + 16, SHELF_H);
        g_object_set_data_full(G_OBJECT(target_widget), "app_info", desktop_app, g_object_unref);
        g_object_set_data(G_OBJECT(target_widget), "win_list", NULL);
        g_signal_connect(target_widget, "button-press-event", G_CALLBACK(on_dock_press), G_APP_INFO(desktop_app));
        gtk_box_pack_start(GTK_BOX(g_shell->dock_box), target_widget, FALSE, FALSE, 0);
        gtk_widget_show_all(target_widget);
    }
    
    identity_register(desktop_id, target_widget);
    if (res_class) identity_register(res_class, target_widget);
    if (res_name) identity_register(res_name, target_widget);
    
    GList *window_list = g_object_get_data(G_OBJECT(target_widget), "win_list");
    if (!g_list_find(window_list, window)) {
        window_list = g_list_prepend(window_list, window);
        g_object_set_data(G_OBJECT(target_widget), "win_list", window_list);
    }
    
    if (!g_object_get_data(G_OBJECT(window), "state-changed-connected")) {
        g_signal_connect(window, "state-changed", G_CALLBACK(on_window_state_changed), NULL);
        g_object_set_data(G_OBJECT(window), "state-changed-connected", GINT_TO_POINTER(1));
    }
    
    GtkWidget *indicator = GTK_WIDGET(g_object_get_data(G_OBJECT(target_widget), "indicator-ref"));
    if (indicator) gtk_widget_set_visible(indicator, TRUE);
    check_active_window_rounding();
}

static void
on_window_closed(WnckScreen *screen, WnckWindow *window, gpointer user_data)
{
    (void)screen; (void)user_data;
    
    const char *res_class = NULL, *res_name = NULL;
    get_window_wm_class(window, &res_class, &res_name);
    char desktop_id[256] = {0};
    find_desktop_id_for_window(window, desktop_id, sizeof(desktop_id));
    
    GtkWidget *target_widget = NULL;
    if (desktop_id[0]) target_widget = identity_lookup(desktop_id);
    if (!target_widget && res_class) target_widget = identity_lookup(res_class);
    if (!target_widget && res_name) target_widget = identity_lookup(res_name);
    
    if (!target_widget && desktop_id[0]) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(g_shell->dock_box));
        for (GList *l = children; l; l = l->next) {
            GtkWidget *child = GTK_WIDGET(l->data);
            GAppInfo *app = G_APP_INFO(g_object_get_data(G_OBJECT(child), "app_info"));
            if (app && g_strcmp0(g_app_info_get_id(app), desktop_id) == 0) {
                target_widget = child;
                break;
            }
        }
        g_list_free(children);
    }
    if (!target_widget) return;
    
    GList *window_list = g_object_get_data(G_OBJECT(target_widget), "win_list");
    window_list = g_list_remove(window_list, window);
    g_object_set_data(G_OBJECT(target_widget), "win_list", window_list);
    
    if (window_list == NULL) {
        GtkWidget *indicator = GTK_WIDGET(g_object_get_data(G_OBJECT(target_widget), "indicator-ref"));
        if (indicator) gtk_widget_set_visible(indicator, FALSE);
        
        gboolean is_pinned = FALSE;
        GAppInfo *app = G_APP_INFO(g_object_get_data(G_OBJECT(target_widget), "app_info"));
        const char *app_id = app ? g_app_info_get_id(app) : NULL;
        for (int i = 0; i < g_shell->pinned_count; i++) {
            if (g_strcmp0(g_shell->pinned_ids[i], app_id) == 0) {
                is_pinned = TRUE;
                break;
            }
        }
        if (!is_pinned) {
            if (g_shell->current_dock_icon == target_widget)
                cancel_instance_popup();
            identity_unregister_widget(target_widget);
            gtk_widget_destroy(target_widget);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * DOCK — refresh & config
 * ═══════════════════════════════════════════════════════════════════════ */

static void
refresh_dock(void)
{
    for (int i = 0; i < 128; i++) {
        DockSlot *slot = g_shell->slots[i];
        if (slot) {
            if (slot->gap_anim_id) g_source_remove(slot->gap_anim_id);
            if (slot->app) g_object_unref(slot->app);
            g_free(slot);
            g_shell->slots[i] = NULL;
        }
    }
    g_shell->slot_count = 0;
    
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_shell->dock_box));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    
    g_hash_table_remove_all(g_shell->identity_table);
    
    for (int i = 0; i < g_shell->pinned_count; i++) {
        GDesktopAppInfo *desktop_app = g_desktop_app_info_new(g_shell->pinned_ids[i]);
        if (!desktop_app) continue;
        
        DockSlot *slot = g_new0(DockSlot, 1);
        slot->app = G_APP_INFO(desktop_app);
        slot->widget = create_dock_item(slot->app, DOCK_ICON_SIZE, DOCK_ICON_SIZE + 16, SHELF_H);
        g_object_set_data_full(G_OBJECT(slot->widget), "app_info", g_object_ref(desktop_app), g_object_unref);
        g_object_set_data(G_OBJECT(slot->widget), "win_list", NULL);
        slot->image = GTK_IMAGE(g_object_get_data(G_OBJECT(slot->widget), "img-ref"));
        slot->indicator = GTK_WIDGET(g_object_get_data(G_OBJECT(slot->widget), "indicator-ref"));
        gtk_widget_set_visible(slot->indicator, FALSE);
        
        g_signal_connect(slot->widget, "enter-notify-event", G_CALLBACK(on_dock_item_enter), slot);
        g_signal_connect(slot->widget, "leave-notify-event", G_CALLBACK(on_dock_item_leave), slot);
        g_signal_connect(slot->widget, "button-press-event", G_CALLBACK(on_dock_press), slot->app);
        
        g_shell->slots[g_shell->slot_count] = slot;
        g_shell->slot_count++;
        gtk_box_pack_start(GTK_BOX(g_shell->dock_box), slot->widget, FALSE, FALSE, 0);
        identity_register(g_shell->pinned_ids[i], slot->widget);
    }
    
    GList *all_windows = wnck_screen_get_windows(g_shell->screen);
    for (GList *l = all_windows; l; l = l->next)
        on_window_opened(g_shell->screen, WNCK_WINDOW(l->data), NULL);
    gtk_widget_show_all(g_shell->dock_box);
}

static void
load_config(void)
{
    gchar *config_path = g_build_filename(g_get_user_config_dir(), "gonzo-shell.json", NULL);
    if (!g_file_test(config_path, G_FILE_TEST_EXISTS)) { g_free(config_path); return; }
    
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (json_parser_load_from_file(parser, config_path, &error)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_ARRAY(root)) {
            JsonArray *array = json_node_get_array(root);
            guint length = json_array_get_length(array);
            g_shell->pinned_count = MIN(length, 64);
            for (int i = 0; i < g_shell->pinned_count; i++) {
                const char *id = json_array_get_string_element(array, i);
                if (id && *id) g_shell->pinned_ids[i] = g_strdup(id);
                else { g_shell->pinned_count = i; break; }
            }
        }
    } else if (error) g_error_free(error);
    g_object_unref(parser);
    g_free(config_path);
}

static void
save_config(void)
{
    JsonArray *array = json_array_new();
    for (int i = 0; i < g_shell->pinned_count; i++)
        json_array_add_string_element(array, g_shell->pinned_ids[i]);
    
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_node_new(JSON_NODE_ARRAY);
    json_node_set_array(root, array);
    json_generator_set_root(generator, root);
    
    gchar *config_path = g_build_filename(g_get_user_config_dir(), "gonzo-shell.json", NULL);
    json_generator_to_file(generator, config_path, NULL);
    g_free(config_path);
    g_object_unref(generator);
    json_node_unref(root);
}

/* ═══════════════════════════════════════════════════════════════════════
 * STRUT (reserve screen edge)
 * ═══════════════════════════════════════════════════════════════════════ */

static void
set_shelf_strut(GtkWidget *window, int monitor_x, int monitor_y,
                int monitor_width, int monitor_height,
                int screen_height, int shelf_height)
{
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    if (!gdk_window) return;
    Display *display = GDK_WINDOW_XDISPLAY(gdk_window);
    Window xwindow = GDK_WINDOW_XID(gdk_window);
    
    unsigned long strut[12] = {0};
    strut[3] = screen_height - monitor_y - monitor_height + shelf_height;
    strut[10] = monitor_x;
    strut[11] = monitor_x + monitor_width - 1;
    
    Atom atom_partial = XInternAtom(display, "_NET_WM_STRUT_PARTIAL", False);
    Atom atom_strut = XInternAtom(display, "_NET_WM_STRUT", False);
    XChangeProperty(display, xwindow, atom_partial, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut, 12);
    XChangeProperty(display, xwindow, atom_strut, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut, 4);
    XFlush(display);
}

/* ═══════════════════════════════════════════════════════════════════════
 * STATUS NOTIFIER WATCHER (system tray)
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    gchar *sender_bus_name;
    gchar *object_path;
} PendingSNIRegistration;

/* Type-safe GdkPixbufDestroyNotify wrapper to avoid cast warning */
static void
pixbuf_free_wrapper(guchar *pixels, gpointer data)
{
    (void)data;
    g_free(pixels);
}

static GdkPixbuf *
parse_icon_pixmap(GVariant *pixmap_array)
{
    if (!pixmap_array) return NULL;
    gint best_width = 0, best_height = 0;
    GVariant *best_bytes = NULL;
    
    GVariantIter iter;
    g_variant_iter_init(&iter, pixmap_array);
    gint32 w, h;
    GVariant *bytes_v;
    while (g_variant_iter_loop(&iter, "(ii@ay)", &w, &h, &bytes_v)) {
        if (w <= 0 || h <= 0 || w > TRAY_ICON_MAX_DIM || h > TRAY_ICON_MAX_DIM)
            continue;
        if (w > best_width) {
            if (best_bytes) g_variant_unref(best_bytes);
            best_width = w;
            best_height = h;
            best_bytes = g_variant_ref(bytes_v);
        }
    }
    if (!best_bytes || best_width <= 0 || best_height <= 0) {
        if (best_bytes) g_variant_unref(best_bytes);
        return NULL;
    }
    
    gsize raw_len = 0;
    const guint8 *raw = g_variant_get_fixed_array(best_bytes, &raw_len, sizeof(guint8));
    gsize expected_len = (gsize)best_width * (gsize)best_height * 4;
    if ((gsize)best_width > G_MAXSIZE / 4 / (gsize)best_height || raw_len < expected_len) {
        GONZO_LOG("tray: IconPixmap data invalid (width=%d height=%d, expected %zu bytes, got %zu) — skipping",
                  best_width, best_height, expected_len, raw_len);
        g_variant_unref(best_bytes);
        return NULL;
    }
    
    guint8 *pixels = g_malloc(expected_len);
    for (gsize i = 0; i < expected_len; i += 4) {
        guint8 a = raw[i + 0];
        guint8 r = raw[i + 1];
        guint8 g = raw[i + 2];
        guint8 b = raw[i + 3];
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = a;
    }
    g_variant_unref(best_bytes);
    
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, TRUE, 8,
                                                 best_width, best_height, best_width * 4,
                                                 pixbuf_free_wrapper, NULL);
    return pixbuf;
}

static gchar *
lookup_desktop_name_from_exe(const gchar *exe_path)
{
    if (!exe_path || !*exe_path) return NULL;
    GList *all_apps = g_app_info_get_all();
    gchar *found_name = NULL;
    
    for (GList *l = all_apps; l && !found_name; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data)) continue;
        GDesktopAppInfo *dai = G_DESKTOP_APP_INFO(l->data);
        const gchar *exec = g_app_info_get_executable(G_APP_INFO(dai));
        if (!exec) continue;
        gchar *resolved_exec = realpath(exec, NULL);
        const gchar *compare_path = resolved_exec ? resolved_exec : exec;
        if (g_strcmp0(compare_path, exe_path) == 0) {
            const gchar *name = g_app_info_get_display_name(G_APP_INFO(dai));
            if (name && *name) found_name = g_strdup(name);
        }
        g_free(resolved_exec);
    }
    
    if (!found_name) {
        gchar *basename = g_path_get_basename(exe_path);
        for (gchar *p = basename; *p; p++) if (*p == '_') *p = '-';
        gchar *desktop_id = g_strdup_printf("%s.desktop", basename);
        GDesktopAppInfo *app = g_desktop_app_info_new(desktop_id);
        if (app) {
            const gchar *name = g_app_info_get_display_name(G_APP_INFO(app));
            if (name && *name) found_name = g_strdup(name);
            g_object_unref(app);
        }
        g_free(desktop_id);
        g_free(basename);
    }
    
    g_list_free_full(all_apps, g_object_unref);
    return found_name;
}

static void
on_sni_properties_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    PendingSNIRegistration *pending = user_data;
    GError *error = NULL;
    GVariant *props_variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!props_variant) {
        GONZO_LOG("tray: failed to fetch properties for %s%s: %s",
                  pending->sender_bus_name, pending->object_path,
                  error ? error->message : "(unknown)");
        if (error) g_error_free(error);
        g_free(pending->sender_bus_name);
        g_free(pending->object_path);
        g_free(pending);
        return;
    }
    
    GVariant *props_dict = NULL;
    g_variant_get(props_variant, "(@a{sv})", &props_dict);
    
    gchar *item_id = NULL, *title = NULL, *icon_name = NULL;
    gchar *menu_path = NULL;
    
    GVariant *v;
    if ((v = g_variant_lookup_value(props_dict, "Id", G_VARIANT_TYPE_STRING)))
    { item_id = safe_truncate(g_variant_get_string(v, NULL), NOTIF_MAX_STRING_LENGTH); g_variant_unref(v); }
    if ((v = g_variant_lookup_value(props_dict, "Title", G_VARIANT_TYPE_STRING)))
    { title = safe_truncate(g_variant_get_string(v, NULL), NOTIF_MAX_STRING_LENGTH); g_variant_unref(v); }
    if ((v = g_variant_lookup_value(props_dict, "IconName", G_VARIANT_TYPE_STRING)))
    { icon_name = safe_truncate(g_variant_get_string(v, NULL), NOTIF_MAX_STRING_LENGTH); g_variant_unref(v); }
    if ((v = g_variant_lookup_value(props_dict, "Menu", G_VARIANT_TYPE_OBJECT_PATH)))
    { menu_path = g_variant_dup_string(v, NULL); g_variant_unref(v); }
    
    GdkPixbuf *icon_pixbuf = NULL;
    if (!icon_name || !*icon_name) {
        if ((v = g_variant_lookup_value(props_dict, "IconPixmap", G_VARIANT_TYPE("a(iiay)")))) {
            icon_pixbuf = parse_icon_pixmap(v);
            g_variant_unref(v);
        }
    }
    
    gchar *display_name = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus) {
        GVariant *pid_v = g_dbus_connection_call_sync(bus,
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "GetConnectionUnixProcessID",
            g_variant_new("(s)", pending->sender_bus_name),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        if (pid_v) {
            guint32 pid = 0;
            g_variant_get(pid_v, "(u)", &pid);
            g_variant_unref(pid_v);
            if (pid > 0) {
                gchar *exe_path = g_strdup_printf("/proc/%u/exe", pid);
                gchar real_path[4096] = {0};
                if (readlink(exe_path, real_path, sizeof(real_path) - 1) > 0) {
                    display_name = lookup_desktop_name_from_exe(real_path);
                }
                g_free(exe_path);
            }
        }
        g_object_unref(bus);
    }
    if (!display_name) display_name = g_strdup("Error retrieving application title");
    
    const gchar *app_id = item_id ? item_id : pending->sender_bus_name;
    const gchar *display_icon = (icon_name && *icon_name) ? icon_name : "application-x-executable";
    
    AppNotificationGroup *group = find_or_create_app_group(app_id, display_name, display_icon, NULL);
    group->has_tray_item = TRUE;
    g_free(group->app_name);
    group->app_name = safe_truncate(display_name, NOTIF_MAX_STRING_LENGTH);
    if (icon_name && *icon_name) { g_free(group->app_icon); group->app_icon = safe_truncate(icon_name, NOTIF_MAX_STRING_LENGTH); }
    if (group->icon_pixbuf) g_object_unref(group->icon_pixbuf);
    group->icon_pixbuf = icon_pixbuf;
    
    g_free(group->sender_bus_name);
    g_free(group->object_path);
    g_free(group->menu_path);
    group->sender_bus_name = g_strdup(pending->sender_bus_name);
    group->object_path = g_strdup(pending->object_path);
    group->menu_path = menu_path ? g_strdup(menu_path) : NULL;
    
    if (group->dbus_menu) { gtk_widget_destroy(group->dbus_menu); group->dbus_menu = NULL; }
    if (group->menu_path && pending->sender_bus_name) {
        DbusmenuGtkMenu *dm = dbusmenu_gtkmenu_new(pending->sender_bus_name, group->menu_path);
        if (dm) group->dbus_menu = g_object_ref_sink(GTK_WIDGET(dm));
    }
    
    rebuild_notification_ui();
    GONZO_LOG("tray: registered item app_id='%s' name='%s'", app_id, display_name);
    
    g_free(item_id); g_free(title); g_free(icon_name); g_free(menu_path);
    g_free(display_name);
    g_variant_unref(props_dict);
    g_variant_unref(props_variant);
    g_free(pending->sender_bus_name);
    g_free(pending->object_path);
    g_free(pending);
}

static void
handle_watcher_method_call(GDBusConnection *connection, const gchar *sender,
                           const gchar *object_path, const gchar *interface_name,
                           const gchar *method_name, GVariant *parameters,
                           GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)object_path; (void)interface_name; (void)user_data;
    if (g_strcmp0(method_name, "RegisterStatusNotifierItem") == 0) {
        const gchar *service_arg = NULL;
        g_variant_get(parameters, "(&s)", &service_arg);
        gchar *item_bus_name;
        gchar *item_object_path;
        if (service_arg && service_arg[0] == '/') {
            item_bus_name = g_strdup(sender);
            item_object_path = g_strdup(service_arg);
        } else {
            item_bus_name = g_strdup(service_arg ? service_arg : sender);
            item_object_path = g_strdup("/StatusNotifierItem");
        }
        PendingSNIRegistration *pending = g_new0(PendingSNIRegistration, 1);
        pending->sender_bus_name = item_bus_name;
        pending->object_path = item_object_path;
        g_dbus_connection_call(connection, pending->sender_bus_name, pending->object_path,
                               "org.freedesktop.DBus.Properties", "GetAll",
                               g_variant_new("(s)", "org.kde.StatusNotifierItem"),
                               G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1,
                               NULL, on_sni_properties_ready, pending);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (g_strcmp0(method_name, "RegisterStatusNotifierHost") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
}

static GVariant *
handle_watcher_get_property(GDBusConnection *connection, const gchar *sender,
                            const gchar *object_path, const gchar *interface_name,
                            const gchar *property_name, GError **error, gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)error; (void)user_data;
    if (g_strcmp0(property_name, "IsStatusNotifierHostRegistered") == 0)
        return g_variant_new_boolean(TRUE);
    if (g_strcmp0(property_name, "ProtocolVersion") == 0)
        return g_variant_new_int32(0);
    if (g_strcmp0(property_name, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
        for (GList *l = g_shell->app_groups; l; l = l->next) {
            AppNotificationGroup *group = l->data;
            if (group->has_tray_item && group->sender_bus_name)
                g_variant_builder_add(&builder, "s", group->sender_bus_name);
        }
        return g_variant_builder_end(&builder);
    }
    return NULL;
}

static const GDBusInterfaceVTable watcher_vtable = {
    .method_call = handle_watcher_method_call,
    .get_property = handle_watcher_get_property,
    .set_property = NULL
};

static void
on_watcher_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    (void)name; (void)user_data;
    const gchar *xml =
        "<node>"
        "  <interface name='org.kde.StatusNotifierWatcher'>"
        "    <method name='RegisterStatusNotifierItem'>"
        "      <arg type='s' name='service' direction='in'/>"
        "    </method>"
        "    <method name='RegisterStatusNotifierHost'>"
        "      <arg type='s' name='service' direction='in'/>"
        "    </method>"
        "    <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
        "    <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
        "    <property name='ProtocolVersion' type='i' access='read'/>"
        "    <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
        "    <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
        "    <signal name='StatusNotifierHostRegistered'/>"
        "  </interface>"
        "</node>";
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(xml, NULL);
    g_dbus_connection_register_object(connection, "/StatusNotifierWatcher",
        node_info->interfaces[0], &watcher_vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(node_info);
    GONZO_LOG("org.kde.StatusNotifierWatcher acquired — tray icons can now register");
}

static void
on_watcher_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    (void)connection; (void)user_data;
    GONZO_LOG("FAILED to acquire org.kde.StatusNotifierWatcher ('%s') — another tray host is likely running", name);
}

static void
own_status_notifier_host_name(void)
{
    gchar *host_name = g_strdup_printf("org.kde.StatusNotifierHost-%d", (int)getpid());
    g_bus_own_name(G_BUS_TYPE_SESSION, host_name, G_BUS_NAME_OWNER_FLAGS_NONE,
                   NULL, NULL, NULL, NULL, NULL);
    g_free(host_name);
}

/* ═══════════════════════════════════════════════════════════════════════
 * NOTIFICATIONS DBUS SERVICE
 * ═══════════════════════════════════════════════════════════════════════ */

static void
handle_notifications_method_call(GDBusConnection *connection, const gchar *sender,
                                 const gchar *object_path, const gchar *interface_name,
                                 const gchar *method_name, GVariant *parameters,
                                 GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)user_data;
    
    if (g_strcmp0(method_name, "Notify") == 0) {
        const gchar *app_name = NULL, *app_icon = NULL, *summary = NULL, *body = NULL;
        guint32 replaces_id = 0;
        GVariant *actions_v = NULL, *hints_v = NULL;
        gint32 expire_timeout = -1;
        g_variant_get(parameters, "(&su&s&s&s@as@a{sv}i)",
                     &app_name, &replaces_id, &app_icon, &summary, &body,
                     &actions_v, &hints_v, &expire_timeout);
        
        if (strlen(app_name) > NOTIF_MAX_STRING_LENGTH ||
            strlen(summary) > NOTIF_MAX_STRING_LENGTH ||
            strlen(body) > NOTIF_MAX_STRING_LENGTH) {
            g_dbus_method_invocation_return_error(invocation,
                G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                "String length exceeds maximum allowed (%d)", NOTIF_MAX_STRING_LENGTH);
            if (actions_v) g_variant_unref(actions_v);
            if (hints_v) g_variant_unref(hints_v);
            return;
        }

        gchar *desktop_hint = NULL;
        if (hints_v) {
            GVariant *de = g_variant_lookup_value(hints_v, "desktop-entry", G_VARIANT_TYPE_STRING);
            if (de) {
                desktop_hint = g_variant_dup_string(de, NULL);
                g_variant_unref(de);
            }
        }

        AppNotificationGroup *group = find_app_group(app_name);
        if (replaces_id != 0) {
            if (!group || !group->notifications) {
                replaces_id = 0;
            } else {
                gboolean found = FALSE;
                for (GList *n = group->notifications; n; n = n->next) {
                    if (((Notification*)n->data)->id == replaces_id) {
                        found = TRUE;
                        break;
                    }
                }
                if (!found) replaces_id = 0;
            }
        }

        if (!group) {
            gchar *display_name = NULL, *display_icon = NULL, *desktop_id = NULL;
            resolve_notification_app_identity(app_name, desktop_hint, app_icon,
                                              &display_name, &display_icon, &desktop_id);
            group = find_or_create_app_group(app_name, display_name, display_icon, desktop_id);
            g_free(display_name);
            g_free(display_icon);
            g_free(desktop_id);
        }

        guint32 notif_id = (replaces_id != 0) ? replaces_id : g_shell->next_notification_id++;
        Notification *notif = create_notification(notif_id, group->app_name, group->app_icon,
                                                  summary, body, group->app_name);
        if (replaces_id != 0) {
            for (GList *l = group->notifications; l; l = l->next) {
                Notification *old = l->data;
                if (old->id == replaces_id) {
                    group->notifications = g_list_remove(group->notifications, old);
                    if (group->unread_count > 0) group->unread_count--;
                    free_notification(old);
                    break;
                }
            }
        }
        add_notification_to_group(group, notif);
        rebuild_notification_ui();

        g_free(desktop_hint);
        if (actions_v) g_variant_unref(actions_v);
        if (hints_v) g_variant_unref(hints_v);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", notif_id));
        return;
    }
    
    if (g_strcmp0(method_name, "CloseNotification") == 0) {
        guint32 id = 0;
        g_variant_get(parameters, "(u)", &id);
        for (GList *l = g_shell->app_groups; l; l = l->next) {
            AppNotificationGroup *group = l->data;
            for (GList *n = group->notifications; n; n = n->next) {
                Notification *notif = n->data;
                if (notif->id == id) {
                    group->notifications = g_list_remove(group->notifications, notif);
                    if (group->unread_count > 0) group->unread_count--;
                    free_notification(notif);
                    rebuild_notification_ui();
                    if (g_shell->notif_dbus_connection)
                        g_dbus_connection_emit_signal(g_shell->notif_dbus_connection, NULL,
                            "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                            "NotificationClosed", g_variant_new("(uu)", id, 3u), NULL);
                    break;
                }
            }
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    
    if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        const gchar *caps[] = { "body", "icon-static", "persistence", NULL };
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", caps));
        return;
    }
    if (g_strcmp0(method_name, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(ssss)", "GonzOS Notifications", "GonzOS", "1.0", "1.2"));
        return;
    }
}

static const GDBusInterfaceVTable notifications_vtable = {
    .method_call = handle_notifications_method_call,
    .get_property = NULL,
    .set_property = NULL
};

static void
on_notifications_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    (void)name; (void)user_data;
    g_shell->notif_dbus_connection = connection;
    const gchar *xml =
        "<node>"
        "  <interface name='org.freedesktop.Notifications'>"
        "    <method name='Notify'>"
        "      <arg type='s' name='app_name' direction='in'/>"
        "      <arg type='u' name='replaces_id' direction='in'/>"
        "      <arg type='s' name='app_icon' direction='in'/>"
        "      <arg type='s' name='summary' direction='in'/>"
        "      <arg type='s' name='body' direction='in'/>"
        "      <arg type='as' name='actions' direction='in'/>"
        "      <arg type='a{sv}' name='hints' direction='in'/>"
        "      <arg type='i' name='expire_timeout' direction='in'/>"
        "      <arg type='u' name='id' direction='out'/>"
        "    </method>"
        "    <method name='CloseNotification'><arg type='u' name='id' direction='in'/></method>"
        "    <method name='GetCapabilities'><arg type='as' name='return_caps' direction='out'/></method>"
        "    <method name='GetServerInformation'>"
        "      <arg type='s' name='return_name' direction='out'/>"
        "      <arg type='s' name='return_vendor' direction='out'/>"
        "      <arg type='s' name='return_version' direction='out'/>"
        "      <arg type='s' name='return_spec_version' direction='out'/>"
        "    </method>"
        "    <signal name='NotificationClosed'><arg type='u' name='id'/><arg type='u' name='reason'/></signal>"
        "    <signal name='ActionInvoked'><arg type='u' name='id'/><arg type='s' name='action_key'/></signal>"
        "  </interface>"
        "</node>";
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(xml, NULL);
    g_dbus_connection_register_object(connection, "/org/freedesktop/Notifications",
        node_info->interfaces[0], &notifications_vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(node_info);
    GONZO_LOG("org.freedesktop.Notifications acquired");
}

static void
on_notifications_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    (void)connection; (void)user_data;
    GONZO_LOG("FAILED to acquire org.freedesktop.Notifications ('%s') — another daemon is likely running", name);
}

/* ═══════════════════════════════════════════════════════════════════════
 * DBUS INTERFACE (menu keybinding)
 * ═══════════════════════════════════════════════════════════════════════ */

static void
handle_dbus_method_call(GDBusConnection *connection, const gchar *sender,
                        const gchar *object_path, const gchar *interface_name,
                        const gchar *method_name, GVariant *parameters,
                        GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)parameters; (void)user_data;
    if (g_strcmp0(method_name, "WinKeyResponse") == 0) {
        menu_toggle(g_shell->menu);
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static const GDBusInterfaceVTable interface_vtable = {
    .method_call = handle_dbus_method_call,
    .get_property = NULL,
    .set_property = NULL
};

static void
on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    (void)name; (void)user_data;
    const gchar *xml =
        "<node>"
        "  <interface name='org.ukui.menu'>"
        "    <method name='WinKeyResponse'/>"
        "  </interface>"
        "</node>";
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(xml, NULL);
    g_dbus_connection_register_object(connection, MENU_OBJECT_PATH,
        node_info->interfaces[0], &interface_vtable, user_data, NULL, NULL);
    g_dbus_node_info_unref(node_info);
}

/* ═══════════════════════════════════════════════════════════════════════
 * CLOCK
 * ═══════════════════════════════════════════════════════════════════════ */

static gboolean
update_clock(gpointer user_data)
{
    (void)user_data;
    time_t now = time(NULL);
    struct tm *time_info = localtime(&now);
    char buffer[16];
    strftime(buffer, sizeof(buffer), "%H:%M", time_info);
    gtk_label_set_text(GTK_LABEL(g_shell->clock_label), buffer);
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════ */

int
main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    
    g_shell = g_new0(GonzoShell, 1);
    g_shell->identity_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_shell->shelf_rounded = TRUE;
    g_shell->next_notification_id = 1;
    
    apply_styles();
    load_config();
    audio_init();
    battery_init();
    backlight_init();
    
    g_shell->handle = wnck_handle_new(WNCK_CLIENT_TYPE_PAGER);
    g_shell->screen = wnck_handle_get_default_screen(g_shell->handle);
    wnck_screen_force_update(g_shell->screen);
    g_signal_connect(g_shell->screen, "window-opened", G_CALLBACK(on_window_opened), NULL);
    g_signal_connect(g_shell->screen, "window-closed", G_CALLBACK(on_window_closed), NULL);
    g_signal_connect(g_shell->screen, "active-window-changed", G_CALLBACK(on_active_window_changed), NULL);
    
    g_shell->notif_window = create_notification_center();
    
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.Notifications",
                   G_BUS_NAME_OWNER_FLAGS_NONE,
                   on_notifications_bus_acquired, NULL, on_notifications_name_lost, NULL, NULL);
    
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.kde.StatusNotifierWatcher",
                   G_BUS_NAME_OWNER_FLAGS_NONE,
                   on_watcher_bus_acquired, NULL, on_watcher_name_lost, NULL, NULL);
    own_status_notifier_host_name();
    
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    
    g_shell->shelf_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    ensure_rgba_visual(g_shell->shelf_window);
    gtk_window_set_type_hint(GTK_WINDOW(g_shell->shelf_window), GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_decorated(GTK_WINDOW(g_shell->shelf_window), FALSE);
    gtk_window_move(GTK_WINDOW(g_shell->shelf_window),
                    geometry.x, geometry.y + geometry.height - SHELF_H);
    gtk_window_set_default_size(GTK_WINDOW(g_shell->shelf_window), geometry.width, SHELF_H);
    gtk_widget_set_size_request(g_shell->shelf_window, geometry.width, SHELF_H);
    
    g_shell->shelf_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_shell->shelf_box), "shelf");
    
    GtkWidget *launcher_icon = gtk_image_new_from_icon_name(
        "process-working-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_set_pixel_size(GTK_IMAGE(launcher_icon), 32);
    GtkWidget *launcher_button = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(launcher_button), launcher_icon);
    gtk_style_context_add_class(gtk_widget_get_style_context(launcher_button), "launcher-btn");
    
    g_shell->menu = menu_create();
    g_signal_connect_swapped(launcher_button, "clicked", G_CALLBACK(menu_toggle), g_shell->menu);
    gtk_box_pack_start(GTK_BOX(g_shell->shelf_box), launcher_button, FALSE, FALSE, 0);
    
    g_shell->dock_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(g_shell->dock_box, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(g_shell->shelf_box), g_shell->dock_box, TRUE, FALSE, 0);
    
    g_shell->clock_label = gtk_label_new("00:00");

    g_shell->shelf_wifi_icon = gtk_image_new_from_icon_name("network-wireless-offline-symbolic", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(g_shell->shelf_wifi_icon), 15);

    /*
     * Same no_show_all pattern as battery_box, but this widget is a leaf
     * (no children), so there's no show_all()-recursion trap here — the
     * icon itself is the thing whose visibility toggles.
     */
    g_shell->shelf_battery_icon = gtk_image_new_from_icon_name("battery-full-symbolic", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(g_shell->shelf_battery_icon), 15);
    gtk_widget_set_no_show_all(g_shell->shelf_battery_icon, TRUE);

    /*
     * Uniform box spacing is optically wrong here: symbolic icons render
     * into a padded canvas whose built-in margin varies per glyph (a
     * battery outline fills its square more than a wifi arc does), while
     * the clock label's bounding box hugs its actual ink with no padding
     * of its own. The same spacing value next to two icons reads as much
     * larger than that value next to an icon and text. Spacing is set to
     * 0 and each gap is an explicit, individually-tuned margin instead of
     * one shared number standing in for two different kinds of gap.
     */
    GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_end(g_shell->shelf_wifi_icon, 5);
    gtk_widget_set_margin_end(g_shell->shelf_battery_icon, 9);
    gtk_box_pack_start(GTK_BOX(status_row), g_shell->shelf_wifi_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_row), g_shell->shelf_battery_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_row), g_shell->clock_label, FALSE, FALSE, 0);

    GtkWidget *clock_button = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(clock_button), status_row);
    gtk_style_context_add_class(gtk_widget_get_style_context(clock_button), "status-pill");
    
    g_shell->panel_window = create_quick_settings_panel();
    g_signal_connect(clock_button, "clicked", G_CALLBACK(toggle_panel), NULL);
    gtk_box_pack_end(GTK_BOX(g_shell->shelf_box), clock_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(g_shell->shelf_window), g_shell->shelf_box);
    
    gtk_widget_add_events(g_shell->shelf_window, GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(g_shell->shelf_window, "button-release-event", G_CALLBACK(on_dock_release), NULL);
    g_signal_connect(g_shell->shelf_window, "motion-notify-event", G_CALLBACK(on_shelf_motion), NULL);
    
    g_bus_own_name(G_BUS_TYPE_SESSION, MENU_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                   on_bus_acquired, NULL, NULL, g_shell->menu, NULL);
    
    gtk_widget_show_all(g_shell->shelf_window);
    
    GdkScreen *screen = gtk_widget_get_screen(g_shell->shelf_window);
    int screen_height = gdk_screen_get_height(screen);
    set_shelf_strut(g_shell->shelf_window, geometry.x, geometry.y,
                    geometry.width, geometry.height, screen_height, SHELF_H);
    
    g_timeout_add_seconds(1, update_clock, NULL);
    refresh_dock();
    check_active_window_rounding();
    
    gtk_main();
    return 0;
}
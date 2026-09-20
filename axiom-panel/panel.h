#ifndef AXIOM_PANEL_H
#define AXIOM_PANEL_H

#include <gtk/gtk.h>

#define PANEL_HEIGHT     48
#define PANEL_ICON       34
#define NUDGE_PX         10
#define DOCK_SLOT_W      (PANEL_ICON + 16)
#define PANEL_ICON_PX    22
#define PANEL_ROW_GAP    12
#define PANEL_ROW_SIDE   24
#define PANEL_ROW_TB     6
#define MENU_GAP         8
#define PANEL_MARGIN     12
#define NOTIF_PANEL_GAP  10
#define DRAG_THRESHOLD   10
#define DRAG_GAP_PX      40
#define INDICATOR_W      10
#define INDICATOR_ACTIVE_W 22
#define PREVIEW_THUMB_W  200
#define PREVIEW_THUMB_H  120
#define PREVIEW_CARD_W   (PREVIEW_THUMB_W + 16)
#define PREVIEW_CARD_H   (PREVIEW_THUMB_H + 36)
#define PREVIEW_SHOW_MS  280
#define PREVIEW_HIDE_MS  180
#define ANIM_SLIDE_MS    240
#define ANIM_FADE_MS     160

void       panel_apply_css(void);
void       panel_ensure_rgba(GtkWidget *win);
void       panel_set_shelf_rounded(GtkWidget *shelf, gboolean rounded);
void       panel_place_bottom(GtkWidget *window);
void       panel_apply_strut(GtkWidget *window);
gboolean   panel_primary_geo(GdkRectangle *geo);
void       panel_session_register(void);

void       anim_cancel(gpointer key);
void       anim_run(gpointer key, int ms,
                    void (*tick)(double t, gpointer user),
                    void (*done)(gpointer user),
                    gpointer user);
void       anim_window_slide(GtkWidget *win, gpointer key,
                             int x, int y0, int y1, int ms, gboolean hide_after);
void       anim_window_fade(GtkWidget *win, gpointer key,
                            double a0, double a1, int ms, gboolean hide_after);

GtkWidget *menu_section_new(GtkWidget *panel);
GtkWidget *windows_section_new(GtkWidget *panel);
GtkWidget *status_section_new(GtkWidget *panel);

gboolean   dock_is_pinned(const char *desktop_id);
void       dock_pin(const char *desktop_id);
void       dock_unpin(const char *desktop_id);
void       dock_popup_app_menu(GtkWidget *anchor, GAppInfo *app, GdkEventButton *event);

#endif

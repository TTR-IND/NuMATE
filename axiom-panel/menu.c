#include "panel.h"

#include <gio/gdesktopappinfo.h>
#include <string.h>

#define APP_ICON_SIZE 24

typedef struct {
	gchar *name;
	gchar *icon_name;
	GDesktopAppInfo *dai;
} AppInfo;

typedef struct {
	GtkWidget *window;
	GtkWidget *search_entry;
	GtkWidget *app_list;
	GtkWidget *search_list;
	GtkWidget *stack;
	GtkWidget *panel;
	GPtrArray *apps;
} MenuData;

static AppInfo *
app_info_new(GDesktopAppInfo *desktop_app)
{
	const gchar *id;
	const gchar *executable;
	AppInfo *app;
	GIcon *icon;

	id = g_app_info_get_id(G_APP_INFO(desktop_app));
	if (!id)
		return NULL;
	if (g_desktop_app_info_get_nodisplay(desktop_app))
		return NULL;
	executable = g_app_info_get_executable(G_APP_INFO(desktop_app));
	if (!executable || !*executable)
		return NULL;

	app = g_new0(AppInfo, 1);
	app->name = g_strdup(g_app_info_get_display_name(G_APP_INFO(desktop_app)));
	icon = g_app_info_get_icon(G_APP_INFO(desktop_app));
	if (icon)
		app->icon_name = g_icon_to_string(icon);
	app->dai = g_object_ref(desktop_app);
	return app;
}

static void
app_info_free(AppInfo *app)
{
	if (!app)
		return;
	g_free(app->name);
	g_free(app->icon_name);
	if (app->dai)
		g_object_unref(app->dai);
	g_free(app);
}

static gint
app_info_compare(gconstpointer a, gconstpointer b)
{
	const AppInfo *aa = *(const AppInfo **)a;
	const AppInfo *bb = *(const AppInfo **)b;

	return g_utf8_collate(aa->name ? aa->name : "", bb->name ? bb->name : "");
}

static GPtrArray *
discover_applications(void)
{
	GPtrArray *apps;
	GList *all, *l;

	apps = g_ptr_array_new_with_free_func((GDestroyNotify)app_info_free);
	all = g_app_info_get_all();
	for (l = all; l; l = l->next) {
		AppInfo *app;

		if (!G_IS_DESKTOP_APP_INFO(l->data))
			continue;
		app = app_info_new(G_DESKTOP_APP_INFO(l->data));
		if (app)
			g_ptr_array_add(apps, app);
	}
	g_list_free_full(all, g_object_unref);
	g_ptr_array_sort(apps, app_info_compare);
	return apps;
}

static GtkWidget *
create_app_list_row(AppInfo *app)
{
	GtkWidget *row, *hbox, *icon, *label;

	row = gtk_list_box_row_new();
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(hbox), 4);
	if (app->icon_name)
		icon = gtk_image_new_from_icon_name(app->icon_name, GTK_ICON_SIZE_MENU);
	else
		icon = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_MENU);
	gtk_image_set_pixel_size(GTK_IMAGE(icon), APP_ICON_SIZE);
	label = gtk_label_new(app->name);
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 4);
	gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(row), hbox);
	g_object_set_data(G_OBJECT(row), "app-info", app);
	return row;
}

static void
on_search_changed(GtkSearchEntry *entry, MenuData *menu)
{
	const gchar *query;
	GList *children, *l;
	gchar *query_lower;
	guint i;

	query = gtk_entry_get_text(GTK_ENTRY(entry));
	if (!query || !*query) {
		gtk_stack_set_visible_child_name(GTK_STACK(menu->stack), "all");
		return;
	}

	children = gtk_container_get_children(GTK_CONTAINER(menu->search_list));
	for (l = children; l; l = l->next)
		gtk_widget_destroy(GTK_WIDGET(l->data));
	g_list_free(children);

	query_lower = g_utf8_strdown(query, -1);
	for (i = 0; i < menu->apps->len; i++) {
		AppInfo *app = menu->apps->pdata[i];
		gchar *name_lower = g_utf8_strdown(app->name ? app->name : "", -1);

		if (strstr(name_lower, query_lower))
			gtk_list_box_insert(GTK_LIST_BOX(menu->search_list),
			                    create_app_list_row(app), -1);
		g_free(name_lower);
	}
	g_free(query_lower);
	gtk_widget_show_all(menu->search_list);
	gtk_stack_set_visible_child_name(GTK_STACK(menu->stack), "search");
}

static void
menu_hide(MenuData *menu)
{
	gtk_entry_set_text(GTK_ENTRY(menu->search_entry), "");
	if (menu->app_list)
		gtk_list_box_unselect_all(GTK_LIST_BOX(menu->app_list));
	if (menu->search_list)
		gtk_list_box_unselect_all(GTK_LIST_BOX(menu->search_list));
	if (!gtk_widget_get_visible(menu->window))
		return;
	anim_window_fade(menu->window, menu, 1.0, 0.0, ANIM_FADE_MS, TRUE);
}

static gboolean
on_list_button(GtkWidget *list, GdkEventButton *event, MenuData *menu)
{
	GtkListBoxRow *row;
	AppInfo *app;

	(void)menu;
	if (event->button != 3)
		return FALSE;
	row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (gint)event->y);
	if (!row)
		return FALSE;
	app = g_object_get_data(G_OBJECT(row), "app-info");
	if (!app || !app->dai)
		return FALSE;
	dock_popup_app_menu(GTK_WIDGET(row), G_APP_INFO(app->dai), event);
	return TRUE;
}

static void
on_row_activated(GtkListBox *list, GtkListBoxRow *row, MenuData *menu)
{
	AppInfo *app = g_object_get_data(G_OBJECT(row), "app-info");

	(void)list;
	if (app && app->dai)
		g_app_info_launch(G_APP_INFO(app->dai), NULL, NULL, NULL);
	menu_hide(menu);
}

static void
on_apps_changed(MenuData *menu)
{
	GList *children, *l;
	guint i;

	if (menu->apps)
		g_ptr_array_free(menu->apps, TRUE);
	menu->apps = discover_applications();
	if (!menu->app_list)
		return;

	children = gtk_container_get_children(GTK_CONTAINER(menu->app_list));
	for (l = children; l; l = l->next)
		gtk_widget_destroy(GTK_WIDGET(l->data));
	g_list_free(children);

	for (i = 0; i < menu->apps->len; i++)
		gtk_list_box_insert(GTK_LIST_BOX(menu->app_list),
		                    create_app_list_row(menu->apps->pdata[i]), -1);
	gtk_widget_show_all(menu->app_list);
}

static void
menu_show(MenuData *menu)
{
	GdkRectangle geo;
	GtkAllocation alloc;
	int x, y;

	if (!panel_primary_geo(&geo))
		return;

	gtk_window_move(GTK_WINDOW(menu->window), -10000, -10000);
	gtk_widget_set_opacity(menu->window, 0.0);
	gtk_widget_show_all(menu->window);
	while (gtk_events_pending())
		gtk_main_iteration();

	gtk_widget_get_allocation(menu->window, &alloc);
	x = geo.x + 4;
	y = geo.y + geo.height - PANEL_HEIGHT - MENU_GAP - alloc.height;
	gtk_window_move(GTK_WINDOW(menu->window), x, y);
	anim_window_fade(menu->window, menu, 0.0, 1.0, ANIM_FADE_MS, FALSE);
	gtk_widget_grab_focus(menu->search_entry);
}

static void
menu_toggle(GtkButton *btn, MenuData *menu)
{
	(void)btn;
	if (gtk_widget_get_visible(menu->window))
		menu_hide(menu);
	else
		menu_show(menu);
}

static MenuData *
menu_create(GtkWidget *panel)
{
	MenuData *menu;
	GtkWidget *outer, *all_scroll, *search_scroll;
	GAppInfoMonitor *monitor;
	guint i;

	menu = g_new0(MenuData, 1);
	menu->panel = panel;
	menu->window = gtk_window_new(GTK_WINDOW_POPUP);
	panel_ensure_rgba(menu->window);
	gtk_window_set_decorated(GTK_WINDOW(menu->window), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(menu->window), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(menu->window), TRUE);
	gtk_window_set_keep_above(GTK_WINDOW(menu->window), TRUE);

	outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_name(outer, "GonzoMenu");
	gtk_style_context_add_class(gtk_widget_get_style_context(outer), "card");
	gtk_widget_set_size_request(outer, 300, 450);
	gtk_container_add(GTK_CONTAINER(menu->window), outer);

	menu->search_entry = gtk_search_entry_new();
	gtk_widget_set_name(menu->search_entry, "GonzoMenuSearch");
	gtk_widget_set_margin_start(menu->search_entry, 8);
	gtk_widget_set_margin_end(menu->search_entry, 8);
	gtk_widget_set_margin_top(menu->search_entry, 8);
	gtk_box_pack_start(GTK_BOX(outer), menu->search_entry, FALSE, FALSE, 0);

	menu->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(menu->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(menu->stack), 80);
	gtk_widget_set_margin_bottom(menu->stack, 10);
	gtk_box_pack_start(GTK_BOX(outer), menu->stack, TRUE, TRUE, 0);

	all_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(all_scroll),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	menu->app_list = gtk_list_box_new();
	gtk_widget_set_name(menu->app_list, "GonzoMenuAppList");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(menu->app_list), GTK_SELECTION_NONE);
	gtk_container_add(GTK_CONTAINER(all_scroll), menu->app_list);
	gtk_stack_add_named(GTK_STACK(menu->stack), all_scroll, "all");

	search_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(search_scroll),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	menu->search_list = gtk_list_box_new();
	gtk_widget_set_name(menu->search_list, "GonzoMenuAppList");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(menu->search_list), GTK_SELECTION_NONE);
	gtk_container_add(GTK_CONTAINER(search_scroll), menu->search_list);
	gtk_stack_add_named(GTK_STACK(menu->stack), search_scroll, "search");

	g_signal_connect(menu->search_entry, "search-changed", G_CALLBACK(on_search_changed), menu);
	g_signal_connect(menu->app_list, "row-activated", G_CALLBACK(on_row_activated), menu);
	g_signal_connect(menu->search_list, "row-activated", G_CALLBACK(on_row_activated), menu);
	gtk_widget_add_events(menu->app_list, GDK_BUTTON_PRESS_MASK);
	gtk_widget_add_events(menu->search_list, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(menu->app_list, "button-press-event", G_CALLBACK(on_list_button), menu);
	g_signal_connect(menu->search_list, "button-press-event", G_CALLBACK(on_list_button), menu);

	menu->apps = discover_applications();
	monitor = g_app_info_monitor_get();
	g_signal_connect_swapped(monitor, "changed", G_CALLBACK(on_apps_changed), menu);

	for (i = 0; i < menu->apps->len; i++)
		gtk_list_box_insert(GTK_LIST_BOX(menu->app_list),
		                    create_app_list_row(menu->apps->pdata[i]), -1);
	return menu;
}

GtkWidget *
menu_section_new(GtkWidget *panel)
{
	MenuData *menu;
	GtkWidget *btn, *icon;

	menu = menu_create(panel);
	icon = gtk_image_new_from_icon_name("process-working-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
	gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
	btn = gtk_button_new();
	gtk_container_add(GTK_CONTAINER(btn), icon);
	gtk_style_context_add_class(gtk_widget_get_style_context(btn), "launcher-btn");
	gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
	g_signal_connect(btn, "clicked", G_CALLBACK(menu_toggle), menu);
	g_object_set_data(G_OBJECT(btn), "menu", menu);
	return btn;
}

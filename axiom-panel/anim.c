#include "panel.h"

/*
 * One tween. Keys cancel in-flight work so a second click does not
 * stack interpolations. t is eased (cubic out) in [0, 1].
 */

typedef struct {
	guint id;
	int ms;
	gint64 start;
	void (*tick)(double t, gpointer user);
	void (*done)(gpointer user);
	void (*dtor)(gpointer user);
	gpointer user;
	gpointer key;
} Anim;

static GHashTable *g_anims;

static double
ease_out_cubic(double t)
{
	double u = 1.0 - t;

	return 1.0 - u * u * u;
}

static void
anim_free(gpointer p)
{
	Anim *a = p;

	if (a->id)
		g_source_remove(a->id);
	if (a->dtor)
		a->dtor(a->user);
	g_free(a);
}

static gboolean
anim_step(gpointer p)
{
	Anim *a = p;
	double t;
	gint64 now;

	now = g_get_monotonic_time();
	t = (double)(now - a->start) / 1000.0 / (double)a->ms;
	if (t >= 1.0) {
		a->id = 0;
		if (a->tick)
			a->tick(1.0, a->user);
		if (a->done)
			a->done(a->user);
		a->dtor = NULL;
		a->user = NULL;
		if (g_anims)
			g_hash_table_remove(g_anims, a->key);
		return G_SOURCE_REMOVE;
	}
	if (a->tick)
		a->tick(ease_out_cubic(t), a->user);
	return G_SOURCE_CONTINUE;
}

void
anim_cancel(gpointer key)
{
	if (g_anims && key)
		g_hash_table_remove(g_anims, key);
}

void
anim_run(gpointer key, int ms,
         void (*tick)(double t, gpointer user),
         void (*done)(gpointer user),
         gpointer user)
{
	Anim *a;

	if (!key || ms < 1)
		return;
	if (!g_anims)
		g_anims = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, anim_free);
	g_hash_table_remove(g_anims, key);
	a = g_new0(Anim, 1);
	a->key = key;
	a->ms = ms;
	a->start = g_get_monotonic_time();
	a->tick = tick;
	a->done = done;
	a->dtor = g_free;
	a->user = user;
	a->id = g_timeout_add(16, anim_step, a);
	g_hash_table_insert(g_anims, key, a);
}

typedef struct {
	GtkWidget *win;
	int x, y0, y1;
	gboolean hide_after;
} Slide;

static void
slide_tick(double t, gpointer user)
{
	Slide *s = user;
	int y = s->y0 + (int)((s->y1 - s->y0) * t);

	gtk_window_move(GTK_WINDOW(s->win), s->x, y);
}

static void
slide_done(gpointer user)
{
	Slide *s = user;

	if (s->hide_after)
		gtk_widget_hide(s->win);
	g_free(s);
}

void
anim_window_slide(GtkWidget *win, gpointer key,
                  int x, int y0, int y1, int ms, gboolean hide_after)
{
	Slide *s;

	if (!win)
		return;
	s = g_new0(Slide, 1);
	s->win = win;
	s->x = x;
	s->y0 = y0;
	s->y1 = y1;
	s->hide_after = hide_after;
	gtk_window_move(GTK_WINDOW(win), x, y0);
	if (!gtk_widget_get_visible(win))
		gtk_widget_show_all(win);
	anim_run(key, ms, slide_tick, slide_done, s);
}

typedef struct {
	GtkWidget *win;
	double a0, a1;
	gboolean hide_after;
} Fade;

static void
fade_tick(double t, gpointer user)
{
	Fade *f = user;

	gtk_widget_set_opacity(f->win, f->a0 + (f->a1 - f->a0) * t);
}

static void
fade_done(gpointer user)
{
	Fade *f = user;

	gtk_widget_set_opacity(f->win, f->a1);
	if (f->hide_after) {
		gtk_widget_hide(f->win);
		gtk_widget_set_opacity(f->win, 1.0);
	}
	g_free(f);
}

void
anim_window_fade(GtkWidget *win, gpointer key,
                 double a0, double a1, int ms, gboolean hide_after)
{
	Fade *f;

	if (!win)
		return;
	f = g_new0(Fade, 1);
	f->win = win;
	f->a0 = a0;
	f->a1 = a1;
	f->hide_after = hide_after;
	gtk_widget_set_opacity(win, a0);
	if (!gtk_widget_get_visible(win))
		gtk_widget_show_all(win);
	anim_run(key, ms, fade_tick, fade_done, f);
}

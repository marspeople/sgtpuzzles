/*
 * gtk.c: GTK front end for my puzzle collection.
 */

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "puzzles.h"

/* ----------------------------------------------------------------------
 * Error reporting functions used elsewhere.
 */

void fatal(char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "fatal error: ");

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
    exit(1);
}

/* ----------------------------------------------------------------------
 * GTK front end to puzzles.
 */

struct font {
    GdkFont *font;
    int type;
    int size;
};

/*
 * This structure holds all the data relevant to a single window.
 * In principle this would allow us to open multiple independent
 * puzzle windows, although I can't currently see any real point in
 * doing so. I'm just coding cleanly because there's no
 * particularly good reason not to.
 */
struct frontend {
    GtkWidget *window;
    GtkWidget *area;
    GtkWidget *statusbar;
    guint statusctx;
    GdkPixmap *pixmap;
    GdkColor *colours;
    int ncolours;
    GdkColormap *colmap;
    int w, h;
    midend_data *me;
    GdkGC *gc;
    int bbox_l, bbox_r, bbox_u, bbox_d;
    int timer_active, timer_id;
    struct font *fonts;
    int nfonts, fontsize;
};

void frontend_default_colour(frontend *fe, float *output)
{
    GdkColor col = fe->window->style->bg[GTK_STATE_NORMAL];
    output[0] = col.red / 65535.0;
    output[1] = col.green / 65535.0;
    output[2] = col.blue / 65535.0;
}

void status_bar(frontend *fe, char *text)
{
    assert(fe->statusbar);

    gtk_statusbar_pop(GTK_STATUSBAR(fe->statusbar), fe->statusctx);
    gtk_statusbar_push(GTK_STATUSBAR(fe->statusbar), fe->statusctx, text);
}

void start_draw(frontend *fe)
{
    fe->gc = gdk_gc_new(fe->area->window);
    fe->bbox_l = fe->w;
    fe->bbox_r = 0;
    fe->bbox_u = fe->h;
    fe->bbox_d = 0;
}

void clip(frontend *fe, int x, int y, int w, int h)
{
    GdkRectangle rect;

    rect.x = x;
    rect.y = y;
    rect.width = w;
    rect.height = h;

    gdk_gc_set_clip_rectangle(fe->gc, &rect);
}

void unclip(frontend *fe)
{
    GdkRectangle rect;

    rect.x = 0;
    rect.y = 0;
    rect.width = fe->w;
    rect.height = fe->h;

    gdk_gc_set_clip_rectangle(fe->gc, &rect);
}

void draw_text(frontend *fe, int x, int y, int fonttype, int fontsize,
               int align, int colour, char *text)
{
    int i;

    /*
     * Find or create the font.
     */
    for (i = 0; i < fe->nfonts; i++)
        if (fe->fonts[i].type == fonttype && fe->fonts[i].size == fontsize)
            break;

    if (i == fe->nfonts) {
        if (fe->fontsize <= fe->nfonts) {
            fe->fontsize = fe->nfonts + 10;
            fe->fonts = sresize(fe->fonts, fe->fontsize, struct font);
        }

        fe->nfonts++;

        fe->fonts[i].type = fonttype;
        fe->fonts[i].size = fontsize;

        /*
         * FIXME: Really I should make at least _some_ effort to
         * pick the correct font.
         */
        fe->fonts[i].font = gdk_font_load("variable");
    }

    /*
     * Find string dimensions and process alignment.
     */
    {
        int lb, rb, wid, asc, desc;

        gdk_string_extents(fe->fonts[i].font, text,
                           &lb, &rb, &wid, &asc, &desc);
        if (align & ALIGN_VCENTRE)
            y += asc - (asc+desc)/2;

        if (align & ALIGN_HCENTRE)
            x -= wid / 2;
        else if (align & ALIGN_HRIGHT)
            x -= wid;

    }

    /*
     * Set colour and actually draw text.
     */
    gdk_gc_set_foreground(fe->gc, &fe->colours[colour]);
    gdk_draw_string(fe->pixmap, fe->fonts[i].font, fe->gc, x, y, text);
}

void draw_rect(frontend *fe, int x, int y, int w, int h, int colour)
{
    gdk_gc_set_foreground(fe->gc, &fe->colours[colour]);
    gdk_draw_rectangle(fe->pixmap, fe->gc, 1, x, y, w, h);
}

void draw_line(frontend *fe, int x1, int y1, int x2, int y2, int colour)
{
    gdk_gc_set_foreground(fe->gc, &fe->colours[colour]);
    gdk_draw_line(fe->pixmap, fe->gc, x1, y1, x2, y2);
}

void draw_polygon(frontend *fe, int *coords, int npoints,
                  int fill, int colour)
{
    GdkPoint *points = snewn(npoints, GdkPoint);
    int i;

    for (i = 0; i < npoints; i++) {
        points[i].x = coords[i*2];
        points[i].y = coords[i*2+1];
    }

    gdk_gc_set_foreground(fe->gc, &fe->colours[colour]);
    gdk_draw_polygon(fe->pixmap, fe->gc, fill, points, npoints);

    sfree(points);
}

void draw_update(frontend *fe, int x, int y, int w, int h)
{
    if (fe->bbox_l > x  ) fe->bbox_l = x  ;
    if (fe->bbox_r < x+w) fe->bbox_r = x+w;
    if (fe->bbox_u > y  ) fe->bbox_u = y  ;
    if (fe->bbox_d < y+h) fe->bbox_d = y+h;
}

void end_draw(frontend *fe)
{
    gdk_gc_unref(fe->gc);
    fe->gc = NULL;

    if (fe->bbox_l < fe->bbox_r && fe->bbox_u < fe->bbox_d) {
	gdk_draw_pixmap(fe->area->window,
			fe->area->style->fg_gc[GTK_WIDGET_STATE(fe->area)],
			fe->pixmap,
                        fe->bbox_l, fe->bbox_u,
                        fe->bbox_l, fe->bbox_u,
                        fe->bbox_r - fe->bbox_l, fe->bbox_d - fe->bbox_u);
    }
}

static void destroy(GtkWidget *widget, gpointer data)
{
    gtk_main_quit();
}

static gint key_event(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    frontend *fe = (frontend *)data;
    int keyval;

    if (!fe->pixmap)
        return TRUE;

    if (event->string[0] && !event->string[1])
        keyval = (unsigned char)event->string[0];
    else if (event->keyval == GDK_Up || event->keyval == GDK_KP_Up ||
             event->keyval == GDK_KP_8)
        keyval = CURSOR_UP;
    else if (event->keyval == GDK_Down || event->keyval == GDK_KP_Down ||
             event->keyval == GDK_KP_2)
        keyval = CURSOR_DOWN;
    else if (event->keyval == GDK_Left || event->keyval == GDK_KP_Left ||
             event->keyval == GDK_KP_4)
        keyval = CURSOR_LEFT;
    else if (event->keyval == GDK_Right || event->keyval == GDK_KP_Right ||
             event->keyval == GDK_KP_6)
        keyval = CURSOR_RIGHT;
    else if (event->keyval == GDK_KP_Home || event->keyval == GDK_KP_7)
        keyval = CURSOR_UP_LEFT;
    else if (event->keyval == GDK_KP_End || event->keyval == GDK_KP_1)
        keyval = CURSOR_DOWN_LEFT;
    else if (event->keyval == GDK_KP_Page_Up || event->keyval == GDK_KP_9)
        keyval = CURSOR_UP_RIGHT;
    else if (event->keyval == GDK_KP_Page_Down || event->keyval == GDK_KP_3)
        keyval = CURSOR_DOWN_RIGHT;
    else
        keyval = -1;

    if (keyval >= 0 &&
        !midend_process_key(fe->me, 0, 0, keyval))
	gtk_widget_destroy(fe->window);

    return TRUE;
}

static gint button_event(GtkWidget *widget, GdkEventButton *event,
                         gpointer data)
{
    frontend *fe = (frontend *)data;
    int button;

    if (!fe->pixmap)
        return TRUE;

    if (event->type != GDK_BUTTON_PRESS)
        return TRUE;

    if (event->button == 2 || (event->state & GDK_SHIFT_MASK))
	button = MIDDLE_BUTTON;
    else if (event->button == 1)
	button = LEFT_BUTTON;
    else if (event->button == 3)
	button = RIGHT_BUTTON;
    else
	return FALSE;		       /* don't even know what button! */

    if (!midend_process_key(fe->me, event->x, event->y, button))
	gtk_widget_destroy(fe->window);

    return TRUE;
}

static gint expose_area(GtkWidget *widget, GdkEventExpose *event,
                        gpointer data)
{
    frontend *fe = (frontend *)data;

    if (fe->pixmap) {
	gdk_draw_pixmap(widget->window,
			widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
			fe->pixmap,
			event->area.x, event->area.y,
			event->area.x, event->area.y,
			event->area.width, event->area.height);
    }
    return TRUE;
}

static gint map_window(GtkWidget *widget, GdkEvent *event,
		       gpointer data)
{
    frontend *fe = (frontend *)data;

    /*
     * Apparently we need to do this because otherwise the status
     * bar will fail to update immediately. Annoying, but there we
     * go.
     */
    gtk_widget_queue_draw(fe->window);

    return TRUE;
}

static gint configure_area(GtkWidget *widget,
                           GdkEventConfigure *event, gpointer data)
{
    frontend *fe = (frontend *)data;
    GdkGC *gc;

    if (fe->pixmap)
        gdk_pixmap_unref(fe->pixmap);

    fe->pixmap = gdk_pixmap_new(widget->window, fe->w, fe->h, -1);

    gc = gdk_gc_new(fe->area->window);
    gdk_gc_set_foreground(gc, &fe->colours[0]);
    gdk_draw_rectangle(fe->pixmap, gc, 1, 0, 0, fe->w, fe->h);
    gdk_gc_unref(gc);

    midend_redraw(fe->me);

    return TRUE;
}

static gint timer_func(gpointer data)
{
    frontend *fe = (frontend *)data;

    if (fe->timer_active)
        midend_timer(fe->me, 0.02);    /* may clear timer_active */

    return fe->timer_active;
}

void deactivate_timer(frontend *fe)
{
    if (fe->timer_active)
        gtk_timeout_remove(fe->timer_id);
    fe->timer_active = FALSE;
}

void activate_timer(frontend *fe)
{
    if (!fe->timer_active)
        fe->timer_id = gtk_timeout_add(20, timer_func, fe);
    fe->timer_active = TRUE;
}

static void menu_key_event(GtkMenuItem *menuitem, gpointer data)
{
    frontend *fe = (frontend *)data;
    int key = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(menuitem),
                                                  "user-data"));
    if (!midend_process_key(fe->me, 0, 0, key))
	gtk_widget_destroy(fe->window);
}

static void menu_preset_event(GtkMenuItem *menuitem, gpointer data)
{
    frontend *fe = (frontend *)data;
    game_params *params =
        (game_params *)gtk_object_get_data(GTK_OBJECT(menuitem), "user-data");
    int x, y;

    midend_set_params(fe->me, params);
    midend_new_game(fe->me, NULL);
    midend_size(fe->me, &x, &y);
    gtk_drawing_area_size(GTK_DRAWING_AREA(fe->area), x, y);
    fe->w = x;
    fe->h = y;
}

static GtkWidget *add_menu_item_with_key(frontend *fe, GtkContainer *cont,
                                         char *text, int key)
{
    GtkWidget *menuitem = gtk_menu_item_new_with_label(text);
    gtk_container_add(cont, menuitem);
    gtk_object_set_data(GTK_OBJECT(menuitem), "user-data",
                        GINT_TO_POINTER(key));
    gtk_signal_connect(GTK_OBJECT(menuitem), "activate",
		       GTK_SIGNAL_FUNC(menu_key_event), fe);
    gtk_widget_show(menuitem);
    return menuitem;
}

static void add_menu_separator(GtkContainer *cont)
{
    GtkWidget *menuitem = gtk_menu_item_new();
    gtk_container_add(cont, menuitem);
    gtk_widget_show(menuitem);
}

static frontend *new_window(void)
{
    frontend *fe;
    GtkBox *vbox;
    GtkWidget *menubar, *menu, *menuitem;
    int x, y, n;

    fe = snew(frontend);

    fe->me = midend_new(fe);
    midend_new_game(fe->me, NULL);

    fe->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(fe->window), game_name);
#if 0
    gtk_window_set_resizable(GTK_WINDOW(fe->window), FALSE);
#else
    gtk_window_set_policy(GTK_WINDOW(fe->window), FALSE, FALSE, TRUE);
#endif
    vbox = GTK_BOX(gtk_vbox_new(FALSE, 0));
    gtk_container_add(GTK_CONTAINER(fe->window), GTK_WIDGET(vbox));
    gtk_widget_show(GTK_WIDGET(vbox));

    menubar = gtk_menu_bar_new();
    gtk_box_pack_start(vbox, menubar, FALSE, FALSE, 0);
    gtk_widget_show(menubar);

    menuitem = gtk_menu_item_new_with_label("Game");
    gtk_container_add(GTK_CONTAINER(menubar), menuitem);
    gtk_widget_show(menuitem);

    menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menuitem), menu);

    add_menu_item_with_key(fe, GTK_CONTAINER(menu), "New", 'n');
    add_menu_item_with_key(fe, GTK_CONTAINER(menu), "Restart", 'r');

    if ((n = midend_num_presets(fe->me)) > 0) {
        GtkWidget *submenu;
        int i;

        menuitem = gtk_menu_item_new_with_label("Type");
        gtk_container_add(GTK_CONTAINER(menu), menuitem);
        gtk_widget_show(menuitem);

        submenu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(menuitem), submenu);

        for (i = 0; i < n; i++) {
            char *name;
            game_params *params;

            midend_fetch_preset(fe->me, i, &name, &params);

            menuitem = gtk_menu_item_new_with_label(name);
            gtk_container_add(GTK_CONTAINER(submenu), menuitem);
            gtk_object_set_data(GTK_OBJECT(menuitem), "user-data", params);
            gtk_signal_connect(GTK_OBJECT(menuitem), "activate",
                               GTK_SIGNAL_FUNC(menu_preset_event), fe);
            gtk_widget_show(menuitem);
        }
    }

    add_menu_separator(GTK_CONTAINER(menu));
    add_menu_item_with_key(fe, GTK_CONTAINER(menu), "Undo", 'u');
    add_menu_item_with_key(fe, GTK_CONTAINER(menu), "Redo", '\x12');
    add_menu_separator(GTK_CONTAINER(menu));
    add_menu_item_with_key(fe, GTK_CONTAINER(menu), "Exit", 'q');

    {
        int i, ncolours;
        float *colours;
        gboolean *success;

        fe->colmap = gdk_colormap_get_system();
        colours = midend_colours(fe->me, &ncolours);
        fe->ncolours = ncolours;
        fe->colours = snewn(ncolours, GdkColor);
        for (i = 0; i < ncolours; i++) {
            fe->colours[i].red = colours[i*3] * 0xFFFF;
            fe->colours[i].green = colours[i*3+1] * 0xFFFF;
            fe->colours[i].blue = colours[i*3+2] * 0xFFFF;
        }
        success = snewn(ncolours, gboolean);
        gdk_colormap_alloc_colors(fe->colmap, fe->colours, ncolours,
                                  FALSE, FALSE, success);
        for (i = 0; i < ncolours; i++) {
            if (!success[i])
                g_error("couldn't allocate colour %d (#%02x%02x%02x)\n",
                        i, fe->colours[i].red >> 8,
                        fe->colours[i].green >> 8,
                        fe->colours[i].blue >> 8);
        }
    }

    if (midend_wants_statusbar(fe->me)) {
	GtkWidget *viewport;
	GtkRequisition req;

	viewport = gtk_viewport_new(NULL, NULL);
	gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE);
	fe->statusbar = gtk_statusbar_new();
	gtk_container_add(GTK_CONTAINER(viewport), fe->statusbar);
	gtk_widget_show(viewport);
	gtk_box_pack_end(vbox, viewport, FALSE, FALSE, 0);
	gtk_widget_show(fe->statusbar);
	fe->statusctx = gtk_statusbar_get_context_id
	    (GTK_STATUSBAR(fe->statusbar), "game");
	gtk_statusbar_push(GTK_STATUSBAR(fe->statusbar), fe->statusctx,
			   "test");
	gtk_widget_size_request(fe->statusbar, &req);
#if 0
	/* For GTK 2.0, should we be using gtk_widget_set_size_request? */
#endif
	gtk_widget_set_usize(viewport, x, req.height);
    } else
	fe->statusbar = NULL;

    fe->area = gtk_drawing_area_new();
    midend_size(fe->me, &x, &y);
    gtk_drawing_area_size(GTK_DRAWING_AREA(fe->area), x, y);
    fe->w = x;
    fe->h = y;

    gtk_box_pack_end(vbox, fe->area, FALSE, FALSE, 0);

    fe->pixmap = NULL;
    fe->fonts = NULL;
    fe->nfonts = fe->fontsize = 0;

    fe->timer_active = FALSE;

    gtk_signal_connect(GTK_OBJECT(fe->window), "destroy",
		       GTK_SIGNAL_FUNC(destroy), fe);
    gtk_signal_connect(GTK_OBJECT(fe->window), "key_press_event",
		       GTK_SIGNAL_FUNC(key_event), fe);
    gtk_signal_connect(GTK_OBJECT(fe->area), "button_press_event",
		       GTK_SIGNAL_FUNC(button_event), fe);
    gtk_signal_connect(GTK_OBJECT(fe->area), "expose_event",
		       GTK_SIGNAL_FUNC(expose_area), fe);
    gtk_signal_connect(GTK_OBJECT(fe->window), "map_event",
		       GTK_SIGNAL_FUNC(map_window), fe);
    gtk_signal_connect(GTK_OBJECT(fe->area), "configure_event",
		       GTK_SIGNAL_FUNC(configure_area), fe);

    gtk_widget_add_events(GTK_WIDGET(fe->area), GDK_BUTTON_PRESS_MASK);

    gtk_widget_show(fe->area);
    gtk_widget_show(fe->window);

    return fe;
}

int main(int argc, char **argv)
{
    srand(time(NULL));

    gtk_init(&argc, &argv);
    (void) new_window();
    gtk_main();

    return 0;
}

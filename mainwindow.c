/*
 * mainwindow.c
 * This file is part of Stjerm
 *
 * Copyright (C) 2007 - Stjepan Glavina
 * Copyright (C) 2007 - Markus Groß
 * 
 * Stjerm is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Stjerm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Stjerm; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, 
 * Boston, MA  02110-1301  USA
 */


#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <vte/vte.h>
#include <stdlib.h>
#include "stjerm.h"


extern gboolean popupmenu_shown;

GtkWidget *mainwindow;
int activetab;
int tabcount;
GArray* tabs;
GtkNotebook* tabbar;

Window mw_xwin;
static Display *dpy = 0;
Atom opacityatom;
gboolean screen_is_composited;

void build_mainwindow(void);
void mainwindow_toggle(void);
void mainwindow_create_tab(void);
void mainwindow_close_tab(void);
int handle_x_error(Display *dpy, XErrorEvent *evt);

static void mainwindow_reset_position(void);
static void mainwindow_show(GtkWidget*, gpointer);
static void mainwindow_focus_out_event(GtkWindow*, GdkEventFocus*, gpointer);
static gboolean mainwindow_expose_event(GtkWidget*, GdkEventExpose*, gpointer);
static void mainwindow_destroy(GtkWidget*, gpointer);
static void mainwindow_window_title_changed(VteTerminal *vteterminal, 
		                                    gpointer user_data);
static void mainwindow_switch_tab(GtkNotebook     *notebook, 
		                          GtkNotebookPage *page,
                                  guint            page_num,
                                  gpointer         user_data);
static void mainwindow_next_tab(GtkWidget *widget, gpointer user_data);
static void mainwindow_prev_tab(GtkWidget *widget, gpointer user_data);
static void mainwindow_new_tab(GtkWidget *widget, gpointer user_data);
static void mainwindow_delete_tab(GtkWidget *widget, gpointer user_data);

void build_mainwindow(void)
{
	mainwindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	
	gtk_widget_set_app_paintable(mainwindow, TRUE);
	gtk_widget_set_size_request(mainwindow, conf_get_width(), conf_get_height());
	gtk_window_set_decorated(GTK_WINDOW(mainwindow), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(mainwindow), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(mainwindow), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(mainwindow), FALSE);
	mainwindow_reset_position();
	GtkAccelGroup* accel_group;
	GClosure *new_tab, *delete_tab, *next_tab, *prev_tab, *delete_all;
	
	accel_group = gtk_accel_group_new();
	gtk_window_add_accel_group(GTK_WINDOW(mainwindow), accel_group);

	new_tab = g_cclosure_new_swap(G_CALLBACK(mainwindow_new_tab), 
			                      NULL, NULL);
    gtk_accel_group_connect(accel_group, 't', GDK_CONTROL_MASK | 
                            GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE, new_tab);
    
	delete_tab = g_cclosure_new_swap(G_CALLBACK(mainwindow_delete_tab), 
			                         NULL, NULL);
    gtk_accel_group_connect(accel_group, 'w', GDK_CONTROL_MASK | 
                            GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE, delete_tab);
	
	next_tab = g_cclosure_new_swap(G_CALLBACK(mainwindow_next_tab), 
			                       NULL, NULL);
	gtk_accel_group_connect(accel_group, GDK_Page_Up, GDK_CONTROL_MASK | 
			                GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE, next_tab);

	prev_tab = g_cclosure_new_swap(G_CALLBACK(mainwindow_prev_tab), 
			                       NULL, NULL);
	gtk_accel_group_connect(accel_group, GDK_Page_Down, GDK_CONTROL_MASK | 
                            GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE, prev_tab);
    
	delete_all = g_cclosure_new_swap(G_CALLBACK(mainwindow_destroy), 
			                         NULL, NULL);
	gtk_accel_group_connect(accel_group, 'q', GDK_CONTROL_MASK | 
                            GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE, delete_all);
	    
	activetab = -1;
	tabs = g_array_new(TRUE, FALSE, sizeof(VteTerminal*));
	tabcount = 0;
	GtkVBox* mainbox = GTK_VBOX(gtk_vbox_new(FALSE, 0));
	tabbar = GTK_NOTEBOOK(gtk_notebook_new());
	g_signal_connect(G_OBJECT(tabbar), "switch-page", 
			         G_CALLBACK(mainwindow_switch_tab), NULL);
	
	if (conf_get_opacity() < 100)
	{
		GdkScreen *screen = gdk_screen_get_default();
		GdkColormap *colormap = gdk_screen_get_rgba_colormap(screen);
		screen_is_composited = (colormap != NULL && gdk_screen_is_composited(screen));
		
		if (screen_is_composited)
		{
			gtk_widget_set_colormap(GTK_WIDGET(mainwindow), colormap);
			gdk_screen_set_default_colormap(screen, colormap);
		}
	}
	
	gtk_box_pack_start(GTK_BOX(mainbox), GTK_WIDGET(tabbar), TRUE, TRUE, 0);
	
	mainwindow_create_tab();
	
	gtk_widget_show_all(GTK_WIDGET(mainbox));
	gtk_container_add(GTK_CONTAINER(mainwindow), GTK_WIDGET(mainbox));

	int border = conf_get_border();
	if (border == BORDER_THIN)
		gtk_container_set_border_width(GTK_CONTAINER(mainwindow), 1);
	else if (border == BORDER_THICK)
		gtk_container_set_border_width(GTK_CONTAINER(mainwindow), 5);
	if (border != BORDER_NONE)
		g_signal_connect(G_OBJECT(mainwindow), "expose-event",
		                 G_CALLBACK(mainwindow_expose_event), NULL);

	g_signal_connect(G_OBJECT(mainwindow), "focus-out-event",
	                 G_CALLBACK(mainwindow_focus_out_event), NULL);
	g_signal_connect(G_OBJECT(mainwindow), "show",
	                 G_CALLBACK(mainwindow_show), NULL);
	g_signal_connect(G_OBJECT(mainwindow), "destroy",
	                 G_CALLBACK(mainwindow_destroy), NULL);

	gtk_notebook_set_show_border(tabbar, FALSE);
	gtk_notebook_set_scrollable(tabbar, TRUE);
	if (conf_get_show_tab() == TABS_ONE || conf_get_show_tab() == TABS_NEVER)
		gtk_notebook_set_show_tabs(tabbar, FALSE);
	gtk_notebook_set_tab_pos(tabbar, GTK_POS_BOTTOM);

	
	XSetErrorHandler(handle_x_error);
	init_key();
	grab_key();
	g_thread_create((GThreadFunc)wait_key, NULL, FALSE, NULL);
}

void mainwindow_create_tab(void)
{
	GtkWidget* tmp_term = build_term();
	GtkVScrollbar *sbar = NULL;
	GtkHBox *tmp_box = GTK_HBOX(gtk_hbox_new(FALSE, 0));

	if (conf_get_scrollbar() == -1)
		gtk_box_pack_start(GTK_BOX(tmp_box), tmp_term, TRUE, TRUE, 0);
	else if (conf_get_scrollbar() == POS_LEFT)
	{
		sbar = GTK_VSCROLLBAR(gtk_vscrollbar_new(vte_terminal_get_adjustment(
				              VTE_TERMINAL(tmp_term))));
		gtk_box_pack_start(GTK_BOX(tmp_box), GTK_WIDGET(sbar), FALSE, FALSE, 0);
		gtk_box_pack_end(GTK_BOX(tmp_box), GTK_WIDGET(tmp_term), TRUE, TRUE, 0);
	} else // (conf_get_scrollbar() == POS_RIGHT)
	{
		sbar = GTK_VSCROLLBAR(gtk_vscrollbar_new(vte_terminal_get_adjustment(
				              VTE_TERMINAL(tmp_term))));
		gtk_box_pack_start(GTK_BOX(tmp_box), GTK_WIDGET(tmp_term), TRUE, TRUE, 0);
		gtk_box_pack_end(GTK_BOX(tmp_box), GTK_WIDGET(sbar), FALSE, FALSE, 0);
	}

	char buffer [100];
	sprintf(buffer, "%s %d", conf_get_term_name(), activetab + 1);
	GtkLabel* tmp_label = GTK_LABEL(gtk_label_new(buffer));
	
	if (conf_get_opacity() < 100)
	{
		if (screen_is_composited)
		{
			vte_terminal_set_background_transparent(VTE_TERMINAL(tmp_term), FALSE);
			vte_terminal_set_opacity(VTE_TERMINAL(tmp_term),
							         conf_get_opacity()/100 * 0xffff);
		}
		else
		{
		    vte_terminal_set_background_saturation(VTE_TERMINAL(tmp_term),
			                                       1.0 - conf_get_opacity()/100);
		    vte_terminal_set_background_transparent(VTE_TERMINAL(tmp_term), TRUE);
		}
	}
	
	if (conf_get_opacity() < 100 && screen_is_composited)
	{
		vte_terminal_set_background_transparent(VTE_TERMINAL(tmp_term), FALSE);
		vte_terminal_set_opacity(VTE_TERMINAL(tmp_term),
				                 conf_get_opacity()/100 * 0xffff);
	}
	g_signal_connect(G_OBJECT(tmp_term), "window-title-changed",
			         G_CALLBACK(mainwindow_window_title_changed), tmp_label);
	
	g_array_append_val(tabs, tmp_term);
	tabcount++;
	
	gtk_widget_show_all(GTK_WIDGET(tmp_box));
	gtk_notebook_append_page(tabbar, GTK_WIDGET(tmp_box), GTK_WIDGET(tmp_label));
	
	if (conf_get_show_tab() == TABS_ONE && tabcount > 1)
		gtk_notebook_set_show_tabs(tabbar, TRUE);
	
	activetab = tabcount - 1;
	gtk_notebook_set_current_page(tabbar, activetab);
}

void mainwindow_close_tab(void)
{
	if (activetab >= 0)
	{
		g_array_remove_index(tabs, activetab);
		tabcount--;
		if (tabcount <= 0)
			gtk_widget_destroy(GTK_WIDGET(mainwindow));
		else
		{
			gtk_notebook_remove_page(tabbar, activetab);
			activetab = gtk_notebook_get_current_page(tabbar);
			
			if (tabcount == 1 && conf_get_show_tab() == TABS_ONE)
				gtk_notebook_set_show_tabs(tabbar, FALSE);
		}
	}
	else
		gtk_widget_destroy(GTK_WIDGET(mainwindow));
}


void mainwindow_toggle(void)
{
	if (GTK_WIDGET_VISIBLE(mainwindow))
	{
		gdk_threads_enter();
		gtk_widget_hide(GTK_WIDGET(mainwindow));
		gdk_flush();
		gdk_threads_leave();
		return;
	}

	gdk_threads_enter();
	if (gtk_window_is_active(GTK_WINDOW(mainwindow)) == FALSE)
		gtk_window_present(GTK_WINDOW(mainwindow));
	else
		gtk_widget_show(mainwindow);
	gtk_window_stick(GTK_WINDOW(mainwindow));
	gtk_window_set_keep_above(GTK_WINDOW(mainwindow), TRUE);
	mainwindow_reset_position();
	gdk_window_focus(mainwindow->window, gtk_get_current_event_time());
	gdk_flush();
	gdk_threads_leave();
}


static void mainwindow_reset_position(void)
{
	int x, y;
	conf_get_position(&x, &y);
	gtk_window_move(GTK_WINDOW(mainwindow), x, y);
}


static void mainwindow_show(GtkWidget *widget, gpointer userdata)
{
	if (dpy != NULL) return;

	mw_xwin = GDK_WINDOW_XWINDOW(GTK_WIDGET(mainwindow)->window);
	dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
}

static void mainwindow_focus_out_event(GtkWindow* window,
                                       GdkEventFocus* event,
									   gpointer userdata)
{
	int revert;
	Window w;
	XGetInputFocus(dpy, &w, &revert);
	if (w == mw_xwin) return;
	// focus wasn't lost just by pressing the shortcut key
	
	if (popupmenu_shown == TRUE) return;
	// focus wasn't lost by popping up popupmenu

	gtk_widget_hide(GTK_WIDGET(mainwindow));
}


static gboolean mainwindow_expose_event(GtkWidget *widget,
                                        GdkEventExpose *event,
                                        gpointer user_data)
{
	gint winw, winh;
	gtk_window_get_size(GTK_WINDOW(widget), &winw, &winh);

	gdk_draw_rectangle(widget->window,
		               widget->style->black_gc,
		               FALSE, 0, 0, winw-1, winh-1);

	if (conf_get_border() == BORDER_THIN) return FALSE;

	gdk_draw_rectangle(widget->window,
	                   widget->style->bg_gc[GTK_STATE_SELECTED],
	                   TRUE, 1, 1, winw-2, winh-2);
	
	gdk_draw_rectangle(widget->window,
	                   widget->style->bg_gc[GTK_STATE_NORMAL],
	                   TRUE, 5, 5, winw-10, winh-10);

	return FALSE;
}


static void mainwindow_destroy(GtkWidget *widget, gpointer user_data)
{
	g_array_free(tabs, TRUE);
	gtk_main_quit();
}


static void mainwindow_window_title_changed(VteTerminal *vteterminal, 
		                                    gpointer user_data)
{
	if (vteterminal != NULL && user_data != NULL)
		gtk_label_set_label(GTK_LABEL(user_data), 
				            vte_terminal_get_window_title(vteterminal));
}


static void mainwindow_switch_tab(GtkNotebook     *notebook, 
		                           GtkNotebookPage *page,
                                   guint            page_num,
                                   gpointer         user_data)
{
	activetab = page_num;
}


static void mainwindow_next_tab(GtkWidget *widget, gpointer user_data)
{
	gtk_notebook_next_page(tabbar);
	activetab = gtk_notebook_get_current_page(tabbar);
}


static void mainwindow_prev_tab(GtkWidget *widget, gpointer user_data)
{
	gtk_notebook_prev_page(tabbar);
	activetab = gtk_notebook_get_current_page(tabbar);
}


static void mainwindow_new_tab(GtkWidget *widget, gpointer user_data)
{
	mainwindow_create_tab();
}


static void mainwindow_delete_tab(GtkWidget *widget, gpointer user_data)
{
	mainwindow_close_tab();
}


int handle_x_error(Display *dpy, XErrorEvent *evt)
{
	if (evt->error_code == BadAccess || evt->error_code == BadValue ||
	    evt->error_code == BadWindow)
	{
		fprintf(stderr, "error: unable to grab key, is stjerm is already running with the same key?\n");
		exit(1);
	}
	return 0;
}

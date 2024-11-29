/*
 * Copyright (C) 2010-2011 Andy Spencer <andy753421@gmail.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <time.h>
#include <grits.h>
#include <stdio.h>
#include <string.h>

#include "borders.h"


/********
 * Border Logic - this plugin draws state / country borders loaded from the data/borders.txt file. *
 ********/
GList* borders_parse(gchar *text){
	g_debug("GritsPluginBorders: borders_parse");
	
	GList* aBoarders = NULL;

	/* Split by newline. The first line is a header comment - ignore it */
	gchar **lines = g_strsplit(text, "\n", -1);
	for (gint li = 1; lines[li]; li++) {
		/* Split line - format: <State name>\t<lat / lon points in polygon> */
		gchar **sparts = g_strsplit(lines[li], "\t", 2);
		int     nparts = g_strv_length(sparts);
		if (nparts < 2) {
			g_strfreev(sparts);
			continue;
		}

		/* Create GritsPoly */
		GritsPoly *poly = grits_poly_parse(sparts[1], "\t", " ", ",");
		/* Force the polygon to always display no matter what the user is zoomed to. */
		GRITS_OBJECT(poly)->lod    = 0;

		/* Make boarders 2px wide */
		poly->width = 2;

		/* Configure polygon fill */
		poly->color[0]  = 1; /* fill red (0-1) */
		poly->color[1]  = 1; /* fill green */
		poly->color[2]  = 1; /* fill blue */
		poly->color[3]  = 0; /* fill alpha (0=transparent, 1=opaque) */

		/* Configure polygon outline */
		poly->border[0] = 1; /* line red */
		poly->border[1] = 1; /* line green */
		poly->border[2] = 1; /* line blue */
		poly->border[3] = 1; /* line alpha ? */

		/* Insert polygon into output list */
		aBoarders = g_list_prepend(aBoarders, poly);

		g_strfreev(sparts);
	}
	g_strfreev(lines);
	
	return aBoarders;
}

/* Callbacks */
static void _on_update(GritsPluginBorders *border)
{
	g_thread_pool_push(border->threads, NULL+1, NULL);
}


/* Methods */
GritsPluginBorders *grits_plugin_borders_new(GritsViewer *viewer, GritsPrefs *prefs)
{
	g_debug("GritsPluginBorders: new");
	GritsPluginBorders *borders = g_object_new(GRITS_TYPE_PLUGIN_BORDERS, NULL);
	borders->viewer  = g_object_ref(viewer);
	borders->prefs   = g_object_ref(prefs);

	for (GList *cur = borders->borders; cur; cur = cur->next)
		grits_viewer_add(viewer, cur->data, GRITS_LEVEL_WORLD+1, FALSE);


	_on_update(borders);
	return borders;
}

static GtkWidget *grits_plugin_borders_get_config(GritsPlugin *_borders)
{
	GritsPluginBorders *borders = GRITS_PLUGIN_BORDERS(_borders);
	return borders->config;
}


/* GObject code */
static void grits_plugin_borders_plugin_init(GritsPluginInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GritsPluginBorders, grits_plugin_borders, G_TYPE_OBJECT,
		G_IMPLEMENT_INTERFACE(GRITS_TYPE_PLUGIN,
			grits_plugin_borders_plugin_init));
static void grits_plugin_borders_plugin_init(GritsPluginInterface *iface)
{
	g_debug("GritsPluginBorders: plugin_init");
	/* Add methods to the interface */
	iface->get_config = grits_plugin_borders_get_config;
}
static void grits_plugin_borders_init(GritsPluginBorders *borders)
{
	g_debug("GritsPluginBorders: init");

	/* Load borders */
	gchar *text; gsize len;
	const gchar *file = PKGDATADIR G_DIR_SEPARATOR_S "borders.txt";
	if (!g_file_get_contents(file, &text, &len, NULL))
		g_error("GritsPluginBorders: init - error loading borders.txt polygons");
	borders->borders = borders_parse(text);
	g_free(text);
}
static void grits_plugin_borders_dispose(GObject *gobject)
{
	g_debug("GritsPluginBorders: dispose");
	GritsPluginBorders *borders = GRITS_PLUGIN_BORDERS(gobject);
	borders->aborted = TRUE;
	/* Drop references */
	if (borders->viewer) {
		GritsViewer *viewer = borders->viewer;
		if (borders->update_source)
			g_source_remove(borders->update_source);
		borders->viewer = NULL;
		for (GList *cur = borders->borders; cur; cur = cur->next)
			grits_object_destroy_pointer(&cur->data);
		g_object_unref(borders->prefs);
		g_object_unref(viewer);
	}
	G_OBJECT_CLASS(grits_plugin_borders_parent_class)->dispose(gobject);
}
static void grits_plugin_borders_finalize(GObject *gobject)
{
	g_debug("GritsPluginBorders: finalize");
	GritsPluginBorders *borders = GRITS_PLUGIN_BORDERS(gobject);
	g_list_free(borders->borders);
	G_OBJECT_CLASS(grits_plugin_borders_parent_class)->finalize(gobject);
}
static void grits_plugin_borders_class_init(GritsPluginBordersClass *klass)
{
	g_debug("GritsPluginBorders: class_init");
	GObjectClass *gobject_class = (GObjectClass*)klass;
	gobject_class->dispose  = grits_plugin_borders_dispose;
	gobject_class->finalize = grits_plugin_borders_finalize;
}

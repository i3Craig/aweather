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

#ifndef __BORDERS_H__
#define __BORDERS_H__

#include <glib-object.h>
#include <grits.h>

#define GRITS_TYPE_PLUGIN_BORDERS            (grits_plugin_borders_get_type ())
#define GRITS_PLUGIN_BORDERS(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),   GRITS_TYPE_PLUGIN_BORDERS, GritsPluginBorders))
#define GRITS_IS_PLUGIN_BORDERS(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),   GRITS_TYPE_PLUGIN_BORDERS))
#define GRITS_PLUGIN_BORDERS_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST   ((klass), GRITS_TYPE_PLUGIN_BORDERS, GritsPluginBordersClass))
#define GRITS_IS_PLUGIN_BORDERS_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE   ((klass), GRITS_TYPE_PLUGIN_BORDERS))
#define GRITS_PLUGIN_BORDERS_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),   GRITS_TYPE_PLUGIN_BORDERS, GritsPluginBordersClass))

typedef struct _GritsPluginBorders      GritsPluginBorders;
typedef struct _GritsPluginBordersClass GritsPluginBordersClass;

struct _GritsPluginBorders {
	GObject parent_instance;

	/* instance members */
	GritsViewer *viewer;
	GritsPrefs  *prefs;
	GtkWidget   *config;
	GtkWidget   *details;

	guint        refresh_id;
	guint        time_changed_id;
	guint        update_source;
	GThreadPool *threads;
	gboolean     aborted;

	time_t       updated;
	GList       *borders;
};

struct _GritsPluginBordersClass {
	GObjectClass parent_class;
};

GType grits_plugin_borders_get_type();

/* Methods */
GritsPluginBorders *grits_plugin_borders_new(GritsViewer *viewer, GritsPrefs *prefs);

#endif


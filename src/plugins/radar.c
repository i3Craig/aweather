/*
 * Copyright (C) 2009-2012 Andy Spencer <andy753421@gmail.com>
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

#define _XOPEN_SOURCE
#include <time.h>
#include <config.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <math.h>
#include <rsl.h>

#include <grits.h>

#include "radar.h"
#include "level2.h"
#include "../aweather-location.h"

#include "../compat.h"

static void aweather_bin_set_child(GtkBin *bin, GtkWidget *new)
{
	GtkWidget *old = gtk_bin_get_child(bin);
	if (old)
		gtk_widget_destroy(old);
	gtk_container_add(GTK_CONTAINER(bin), new);
	gtk_widget_show_all(new);
}

static gchar *_find_nearest(time_t time, GList *files,
		gsize offset)
{
	g_debug("RadarSite: find_nearest ...");
	time_t  nearest_time = 0;
	char   *nearest_file = NULL;

	struct tm tm = {};
	g_debug("Before for loop");
	for (GList *cur = files; cur; cur = cur->next) {
		gchar *file = cur->data;
	        g_debug("RadarSite: find_nearest - in loop. Current file: %s", file);
		sscanf(file+offset, "%4d%2d%2d_%2d%2d",
				&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
				&tm.tm_hour, &tm.tm_min);
		tm.tm_year -= 1900;
		tm.tm_mon  -= 1;
		if (ABS(time - mktime(&tm)) <
		    ABS(time - nearest_time)) {
			nearest_file = file;
			nearest_time = mktime(&tm);
		}
	}

	g_debug("RadarSite: find_nearest = %s", nearest_file);
	if (nearest_file)
		return g_strdup(nearest_file);
	else
		return NULL;
}


/**************
 * RadarSites *
 **************/
typedef enum {
	STATUS_UNLOADED,
	STATUS_LOADING,
	STATUS_LOADED,
} RadarSiteStatus;
struct _RadarSite {
	/* Information */
	city_t         *city;
	GritsMarker    *marker;      // Map marker for grits

	/* Stuff from the parents */
	GritsViewer    *viewer;
	GritsHttp      *http;
	GritsPrefs     *prefs;
	GtkWidget      *pconfig;

	/* When loaded */
	gboolean        hidden;
	RadarSiteStatus status;      // Loading status for the site
	GtkWidget      *config;
	AWeatherLevel2 *level2;      // The Level2 structure for the current volume

	/* Internal data */
	time_t          time;        // Current timestamp of the level2
	gchar          *message;     // Error message set while updating
	guint           time_id;     // "time-changed"     callback ID
	guint           refresh_id;  // "refresh"          callback ID
	guint           location_id; // "locaiton-changed" callback ID
	guint           idle_source; // _site_update_end idle source
};

/* format: http://mesonet.agron.iastate.edu/data/nexrd2/raw/KABR/KABR_20090510_0323 */
void _site_update_loading(gchar *file, goffset cur,
		goffset total, gpointer _site)
{
	RadarSite *site = _site;
	GtkWidget *progress_bar = gtk_bin_get_child(GTK_BIN(site->config));
	double percent = (double)cur/total;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), MIN(percent, 1.0));
	gchar *msg = g_strdup_printf("Loading... %5.1f%% (%.2f/%.2f MB)",
			percent*100, (double)cur/1000000, (double)total/1000000);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), msg);
	g_free(msg);
}
gboolean _site_update_end(gpointer _site)
{
	RadarSite *site = _site;
	if (site->message) {
		g_warning("RadarSite: update_end - %s", site->message);
		const char *fmt = "http://forecast.weather.gov/product.php?site=NWS&product=FTM&format=TXT&issuedby=%s";
		char       *uri = g_strdup_printf(fmt, site->city->code+1);
		GtkWidget  *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		GtkWidget  *msg = gtk_label_new(site->message);
		GtkWidget  *btn = gtk_link_button_new_with_label(uri, "View Radar Status");
		gtk_box_set_homogeneous(GTK_BOX(box), TRUE);
		gtk_box_pack_start(GTK_BOX(box), msg, TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(box), btn, TRUE, TRUE, 0);
		aweather_bin_set_child(GTK_BIN(site->config), box);
		g_free(uri);
	} else {
		/* Wrap the radar sweep / volume UI in a GtkScrolledWindow so that we don't use up too much screen space displaying all radar options on small screens.*/
		GtkWidget *sweepSelectionUiWidget = aweather_level2_get_config(site->level2);
		GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);

		/* Automatically show the scrollbars */
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

		/* Add the radar options UI to the scrolled window */
		gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scrolled_window), sweepSelectionUiWidget);

		/* Add the scrolled window to the config bin */
		aweather_bin_set_child(GTK_BIN(site->config), scrolled_window);
	}
	site->status = STATUS_LOADED;
	site->idle_source = 0;
	return FALSE;
}
gpointer _site_update_thread(gpointer _site)
{
	RadarSite *site = _site;
	g_debug("RadarSite: update_thread - %s", site->city->code);
	site->message = NULL;

	gboolean offline = grits_viewer_get_offline(site->viewer);
	gchar *nexrad_url = grits_prefs_get_string(site->prefs,
			"aweather/nexrad_url", NULL);

	/* Find nearest volume (temporally) */
	g_debug("RadarSite: update_thread - find nearest - %s", site->city->code);
	gchar *dir_list = g_strconcat(nexrad_url, "/", site->city->code,
			"/", "dir.list", NULL);
	GList *files = grits_http_available(site->http,
			"^\\w{4}_\\d{8}_\\d{6}.bz2$", site->city->code,
			"\\d+ (.*)", (offline ? NULL : dir_list));
	g_free(dir_list);
	gchar *nearest = _find_nearest(site->time, files, 5);
	g_list_foreach(files, (GFunc)g_free, NULL);
	g_list_free(files);
	if (!nearest) {
		site->message = "No suitable files found";
		goto out;
	}

	/* Fetch new volume */
	g_debug("RadarSite: update_thread - fetch");
	gchar *local = g_strconcat(site->city->code, "/", nearest, NULL);
	gchar *uri   = g_strconcat(nexrad_url, "/", local,   NULL);
	gchar *file  = grits_http_fetch(site->http, uri, local,
			offline ? GRITS_LOCAL : GRITS_UPDATE,
			_site_update_loading, site);
	g_free(nexrad_url);
	g_free(nearest);
	g_free(local);
	g_free(uri);
	if (!file) {
		site->message = "Fetch failed";
		goto out;
	}

	/* Load and add new volume */
	g_debug("RadarSite: update_thread - load - %s", site->city->code);
	site->level2 = aweather_level2_new_from_file(
			file, site->city->code, colormaps);
	g_free(file);
	if (!site->level2) {
		site->message = "Load failed";
		goto out;
	}
	grits_object_hide(GRITS_OBJECT(site->level2), site->hidden);
	grits_viewer_add(site->viewer, GRITS_OBJECT(site->level2),
			GRITS_LEVEL_WORLD+3, TRUE);

out:
	if (!site->idle_source)
		site->idle_source = g_idle_add(_site_update_end, site);
	return NULL;
}
void _site_update(RadarSite *site)
{
	if (site->status == STATUS_LOADING)
		return;
	site->status = STATUS_LOADING;

	site->time = grits_viewer_get_time(site->viewer);
	g_debug("RadarSite: update %s - %d",
			site->city->code, (gint)site->time);

	/* Add a progress bar */
	GtkWidget *progress = gtk_progress_bar_new();
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress), "Loading...");
	aweather_bin_set_child(GTK_BIN(site->config), progress);

	/* Remove old volume */
	g_debug("RadarSite: update - remove - %s", site->city->code);
	grits_object_destroy_pointer(&site->level2);

	/* Fork loading right away so updating the
	 * list of times doesn't take too long */
	g_thread_new("site-update-thread", _site_update_thread, site);
}

/* RadarSite methods */
void radar_site_unload(RadarSite *site)
{
	if (site->status != STATUS_LOADED)
		return; // Abort if it's still loading

	g_debug("RadarSite: unload %s", site->city->code);

	if (site->time_id)
		g_signal_handler_disconnect(site->viewer, site->time_id);
	if (site->refresh_id)
		g_signal_handler_disconnect(site->viewer, site->refresh_id);
	if (site->idle_source)
		g_source_remove(site->idle_source);
	site->idle_source = 0;

	/* Remove tab */
	if (site->config)
		gtk_widget_destroy(site->config);

	/* Remove radar */
	grits_object_destroy_pointer(&site->level2);

	site->status = STATUS_UNLOADED;
}

void radar_site_load(RadarSite *site)
{
	g_debug("RadarSite: load %s", site->city->code);

	/* Add tab page */
	site->config = gtk_alignment_new(0, 0, 1, 1);
	g_object_set_data(G_OBJECT(site->config), "site", site);
	gtk_notebook_append_page(GTK_NOTEBOOK(site->pconfig), site->config,
			gtk_label_new(site->city->name));
	gtk_widget_show_all(site->config);
	if (gtk_notebook_get_current_page(GTK_NOTEBOOK(site->pconfig)) == 0)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(site->pconfig), -1);

	/* Set up radar loading */
	site->time_id = g_signal_connect_swapped(site->viewer, "time-changed",
			G_CALLBACK(_site_update), site);
	site->refresh_id = g_signal_connect_swapped(site->viewer, "refresh",
			G_CALLBACK(_site_update), site);
	_site_update(site);
}

void _site_on_location_changed(GritsViewer *viewer,
		gdouble lat, gdouble lon, gdouble elev,
		gpointer _site)
{
	static gdouble min_dist = EARTH_R / 30;
	RadarSite *site = _site;

	/* Calculate distance, could cache xyz values */
	gdouble eye_xyz[3], site_xyz[3];
	lle2xyz(lat, lon, elev, &eye_xyz[0], &eye_xyz[1], &eye_xyz[2]);
	lle2xyz(site->city->pos.lat, site->city->pos.lon, site->city->pos.elev,
			&site_xyz[0], &site_xyz[1], &site_xyz[2]);
	gdouble dist = distd(site_xyz, eye_xyz);

	/* Load or unload the site if necessasairy */
	if (dist <= min_dist && dist < elev*1.25 && site->status == STATUS_UNLOADED)
		radar_site_load(site);
	else if (dist > 2*min_dist &&  site->status != STATUS_UNLOADED)
		radar_site_unload(site);
}

static gboolean on_marker_clicked(GritsObject *marker, GdkEvent *event, RadarSite *site)
{
	GritsViewer *viewer = site->viewer;
	GritsPoint center = marker->center;
	grits_viewer_set_location(viewer, center.lat, center.lon, EARTH_R/35);
	grits_viewer_set_rotation(viewer, 0, 0, 0);
	/* Recursivly set notebook tabs */
	GtkWidget *widget, *parent;
	for (widget = site->config; widget; widget = parent) {
		parent = gtk_widget_get_parent(widget);
		if (GTK_IS_NOTEBOOK(parent)) {
			gint i = gtk_notebook_page_num(GTK_NOTEBOOK(parent), widget);
			gtk_notebook_set_current_page(GTK_NOTEBOOK(parent), i);
		}
	}
	return TRUE;
}

RadarSite *radar_site_new(city_t *city, GtkWidget *pconfig,
		GritsViewer *viewer, GritsPrefs *prefs, GritsHttp *http)
{
	RadarSite *site = g_new0(RadarSite, 1);
	site->viewer  = g_object_ref(viewer);
	site->prefs   = g_object_ref(prefs);
	//site->http    = http;
	site->http    = grits_http_new(G_DIR_SEPARATOR_S
			"nexrad" G_DIR_SEPARATOR_S
			"level2" G_DIR_SEPARATOR_S);
	site->city    = city;
	site->pconfig = pconfig;
	site->hidden  = TRUE;

	/* Set initial location */
	gdouble lat, lon, elev;
	grits_viewer_get_location(viewer, &lat, &lon, &elev);
	_site_on_location_changed(viewer, lat, lon, elev, site);

	/* Add marker */
	site->marker = grits_marker_new(site->city->name);
	GRITS_OBJECT(site->marker)->center = site->city->pos;
	GRITS_OBJECT(site->marker)->lod    = EARTH_R*0.75*site->city->lod;
	grits_viewer_add(site->viewer, GRITS_OBJECT(site->marker),
			GRITS_LEVEL_HUD, FALSE);
	g_signal_connect(site->marker, "clicked",
			G_CALLBACK(on_marker_clicked), site);
	grits_object_set_cursor(GRITS_OBJECT(site->marker), GDK_HAND2);

	/* Connect signals */
	site->location_id  = g_signal_connect(viewer, "location-changed",
			G_CALLBACK(_site_on_location_changed), site);
	return site;
}

void radar_site_free(RadarSite *site)
{
	radar_site_unload(site);
	grits_object_destroy_pointer(&site->marker);
	if (site->location_id)
		g_signal_handler_disconnect(site->viewer, site->location_id);
	grits_http_free(site->http);
	g_object_unref(site->viewer);
	g_object_unref(site->prefs);
	g_free(site);
}


/**************
 * RadarConus *
 **************/
#define CONUS_NORTH       53.0
#define CONUS_WEST       -132.5
#define CONUS_WIDTH       4000.0
/* Note - this image is 2500 px tall, but OpenGL won't display the texture if it is > 2000 px tall */
#define CONUS_HEIGHT      2500.0
#define CONUS_DEG_PER_PX_VERTICAL  0.0128
#define CONUS_DEG_PER_PX_HORIZONTAL  0.0166

#define CONUS_TEXTURE_BUFFER_LENGTH 3000

struct _RadarConus {
	GritsViewer *viewer;
	GritsHttp   *http;
	GtkWidget   *config;
	time_t       time;
	const gchar *message;
	GMutex       loading;

	gchar       *path;
	GritsTile   *tile[2];

	guint        time_id;     // "time-changed"     callback ID
	guint        refresh_id;  // "refresh"          callback ID
	guint        idle_source; // _conus_update_end idle source
};

void _conus_update_loading(gchar *file, goffset cur,
		goffset total, gpointer _conus)
{
	RadarConus *conus = _conus;
	GtkWidget *progress_bar = gtk_bin_get_child(GTK_BIN(conus->config));
	double percent = (double)cur/total;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), MIN(percent, 1.0));
	gchar *msg = g_strdup_printf("Loading... %5.1f%% (%.2f/%.2f MB)",
			percent*100, (double)cur/1000000, (double)total/1000000);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), msg);
	g_free(msg);
}

/* Copy images to graphics memory */
static void _conus_update_end_copy(GritsTile *tile, guchar *pixels)
{
	if (!tile->tex)
		glGenTextures(1, &tile->tex);

	gchar *clear = g_malloc0(CONUS_TEXTURE_BUFFER_LENGTH*CONUS_TEXTURE_BUFFER_LENGTH*4);
	glBindTexture(GL_TEXTURE_2D, tile->tex);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, 4, CONUS_TEXTURE_BUFFER_LENGTH, CONUS_TEXTURE_BUFFER_LENGTH, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, clear);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 1,1, CONUS_WIDTH/2,CONUS_HEIGHT,
			GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	tile->coords.n = 1.0/(CONUS_WIDTH/2);
	tile->coords.w = 1.0/ CONUS_HEIGHT;
	tile->coords.s = tile->coords.n +  CONUS_HEIGHT   / CONUS_TEXTURE_BUFFER_LENGTH;
	tile->coords.e = tile->coords.w + (CONUS_WIDTH/2) / CONUS_TEXTURE_BUFFER_LENGTH;
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFlush();
	g_free(clear);
}

/* Pixel structure - packed so we can represent the 3-byte pixel */
typedef struct __attribute__((__packed__)) {
	unsigned char iRed;
	unsigned char iGreen;
	unsigned char iBlue;
} sPixel;

static sPixel* _getPixelAt(guchar *pixels, gint width, gint height, gint pxsize, int x, int y){
  if(y >= height){
	  y = height - 1;
  } else if(y < 0){
	  y = 0;
  }

  if(x >= width){
	  x = width - 1;
  } else if (x < 0){
	  x = 0;
  }

  return  (sPixel*) (pixels + ((y * width + x) * pxsize));
}

/* Pass in a pixel. Returns true if the pixel is part of a map border / road that should be removed. */
static int _isBorderPixel(sPixel* ipsPixel){
  return (ipsPixel->iRed == 0xff && ipsPixel->iGreen == 0xff && ipsPixel->iBlue == 0xff) /* state / country boarders */
	  || (ipsPixel->iRed == 0x6e && ipsPixel->iGreen == 0x6e && ipsPixel->iBlue == 0x6e) /* county borders */
	  || (ipsPixel->iRed == 0x8b && ipsPixel->iGreen == 0x47 && ipsPixel->iBlue == 0x26); /* interstates */
}

/* Locates the nearest non-boarder pixel in the map and returns it */
static sPixel* _getNearestNonBoarderPixel(guchar *pixels, gint width, gint height, gint pxsize, int ipiStartingX, int ipiStartingY){
  for(int iCurrentSearchBoxRadius = 0; iCurrentSearchBoxRadius < 10; ++iCurrentSearchBoxRadius){
    for(int x = ipiStartingX - iCurrentSearchBoxRadius; x <= ipiStartingX + iCurrentSearchBoxRadius; ++x){
      for(int y = ipiStartingY - iCurrentSearchBoxRadius; y <= ipiStartingY + iCurrentSearchBoxRadius; ++y){
        sPixel* sCurrentPixel = _getPixelAt(pixels, width, height, pxsize, x, y);
        if(!_isBorderPixel(sCurrentPixel)){
          return sCurrentPixel;
        }
      }
    }
  }
  
  /* If nothing was found, then give up and return the starting pixel */
  return _getPixelAt(pixels, width, height, pxsize, ipiStartingX, ipiStartingY);
}

static void _unprojectPoint(int ipiX, int ipiY, int* opiX, int* opiY){
  /* ChatGPT generated polynomial function to map the points below, which were derived by finding common points on the source
   * image (radar image) and destination image (Open Street Maps screenshot of CONUS).
   * See deriving-conus-projection.py for details.
   */
  
  /* Scale the points as the screenshot of OSM was not the same size as the CONUS radar image */
  double x = ipiX * 3 / 8; //ipiX * 1594 / 4000;
  double y = ipiY * 3 / 8; //ipiY * 911 / 2400;
  
  
  /* Formulas in bc format:
   * X:  -9.04063882*10^-01*y + -1.75317891*10^-04*y*y + -9.76238636*10^-08*y*y*y +  2.29786240*10^00*x + 1.17856633*10^-03*x*y + 4.20186006*10^-07*x*y*y + 2.55777372*10^-04*x*x + -2.91386724*10^-08*x*x*y + -1.13510117*10^-07*x*x*x + 2.65699057*10^01
   * Y: 2.25170021*10^00*y +  6.61600795*10^-04*y*y +  8.72044698*10^-08*y*y*y + 8.60491270*10^-01*x + 5.80511426*10^-04*x*y + -4.39101569*10^-08*x*y*y + -5.89743092*10^-04*x*x + -3.74718041*10^-07*x*x*y + 6.42016503*10^-10*x*x*x + -2.84533069*10^02
   */
  *opiX = -9.04063882e-01*y + -1.75317891e-04*y*y + -9.76238636e-08*y*y*y +  2.29786240e+00*x + 1.17856633e-03*x*y + 4.20186006e-07*x*y*y + 2.55777372e-04*x*x + -2.91386724e-08*x*x*y + -1.13510117e-07*x*x*x + 2.65699057e+01;
  *opiY = 2.25170021e+00*y +  6.61600795e-04*y*y +  8.72044698e-08*y*y*y + 8.60491270e-01*x + 5.80511426e-04*x*y + -4.39101569e-08*x*y*y + -5.89743092e-04*x*x + -3.74718041e-07*x*x*y + 6.42016503e-10*x*x*x + -2.84533069e+02;
}

/* Split the pixbuf into east and west halves (with 2K sides)
 * Also map the pixbuf's alpha values */
static void _conus_update_end_split(guchar *pixels, guchar *west, guchar *east,
		gint width, gint height, gint pxsize)
{
	g_debug("Conus: update_end_split");
	guchar *out[] = {west,east};


	/* Change projection of map so it aligns with the globe */

	/* Copy the image to a temp memory buffer that we use below to copy from for the source of the projection.
	 * Without this, the projection can end up being used as source data for the next projected pixel.
	 */
	int iArrayLengthBytes = sizeof(guchar) * width * height * pxsize;
	guchar *aOriginalImage = malloc(iArrayLengthBytes);
	memcpy(aOriginalImage, pixels, iArrayLengthBytes);

	for(int y = 0; y < height; ++y)
	for(int x = 0; x < width; ++x){
		int srcX, srcY;
		_unprojectPoint(x, y, &srcX, &srcY);
		sPixel* src = _getNearestNonBoarderPixel(aOriginalImage, width, height, pxsize, srcX, srcY);
		sPixel* dest = _getPixelAt(pixels, width, height, pxsize, x, y);
		/* Copy pixel data from source to destination pixel */
		*dest = *src;
	}

	free(aOriginalImage);



	/* Split the image into two and alpha-map the image */
	for (int y = 0; y < height; y++)
	for (int x = 0; x < width;  x++) {
		gint subx = x % (width/2);
		gint idx  = x / (width/2);
		guchar *src = &pixels[(y*width+x)*pxsize];
		guchar *dst = &out[idx][(y*(width/2)+subx)*4];
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		dst[3] = 0xff * 0.75;
		/* Make black background transparent */
		if(src[0] == 0 && src[1] == 0 && src[2] == 0){
			dst[3] = 0x00;
		}
	}
}

gboolean _conus_update_end(gpointer _conus)
{
	RadarConus *conus = _conus;
	g_debug("Conus: update_end");

	/* Check error status */
	if (conus->message) {
		g_warning("Conus: update_end - %s", conus->message);
		aweather_bin_set_child(GTK_BIN(conus->config), gtk_label_new(conus->message));
		goto out;
	}

	/* Load and pixbuf */
	GError *error = NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(conus->path, &error);
	if (!pixbuf || error) {
		g_warning("Conus: update_end - error loading pixbuf: %s", conus->path);
		aweather_bin_set_child(GTK_BIN(conus->config), gtk_label_new("Error loading pixbuf"));
		g_remove(conus->path);
		goto out;
	}

	/* Split pixels into east/west parts */
	guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
	gint    width  = gdk_pixbuf_get_width(pixbuf);
	gint    height = gdk_pixbuf_get_height(pixbuf);
	gint    pxsize = gdk_pixbuf_get_has_alpha(pixbuf) ? 4 : 3;
	guchar *pixels_west = g_malloc(4*(width/2)*height);
	guchar *pixels_east = g_malloc(4*(width/2)*height);
	_conus_update_end_split(pixels, pixels_west, pixels_east,
			width, height, pxsize);
	g_object_unref(pixbuf);

	/* Copy pixels to graphics memory */
	_conus_update_end_copy(conus->tile[0], pixels_west);
	_conus_update_end_copy(conus->tile[1], pixels_east);
	g_free(pixels_west);
	g_free(pixels_east);

	/* Update GUI */
	gchar *label = g_path_get_basename(conus->path);
	aweather_bin_set_child(GTK_BIN(conus->config), gtk_label_new(label));
	grits_viewer_queue_draw(conus->viewer);
	g_free(label);

out:
	conus->idle_source = 0;
	g_free(conus->path);
	g_mutex_unlock(&conus->loading);
	return FALSE;
}

gpointer _conus_update_thread(gpointer _conus)
{
	RadarConus *conus = _conus;
	conus->message = NULL;

	/* Find nearest */
	g_debug("Conus: update_thread - nearest");
	gboolean offline = grits_viewer_get_offline(conus->viewer);
	// Could also use: https://radar.weather.gov/ridge/standard/CONUS-LARGE_0.gif
	gchar *conus_url = "https://atlas.niu.edu/analysis/radar/CONUS/archive_b/";
	gchar *nearest;
	if (!offline) {
		struct tm *tm = gmtime(&conus->time);
		time_t nearest5 = conus->time - 60*(tm->tm_min % 5); /* A new CONUS GIF comes out every 5 minutes. Find the GIF that is closest to the requested time. */
		tm = gmtime(&nearest5);
		nearest = g_strdup_printf("usrad_b.%04d%02d%02d.%02d%02d.gif",
				tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
				tm->tm_hour, tm->tm_min);
	} else {
		GList *files = grits_http_available(conus->http,
				"^usrad_b.[^\"]*.gif$", "", NULL, NULL);
		//GList *files = grits_http_available(conus->http,
		//		"^CONUS-LARGE.[^\"]*.gif$", "", NULL, NULL);
		nearest = _find_nearest(conus->time, files, 6);
		g_list_foreach(files, (GFunc)g_free, NULL);
		g_list_free(files);
		if (!nearest) {
			conus->message = "No suitable files";
			goto out;
		}
	}

	/* Fetch the image */
	g_debug("Conus: update_thread - fetch");
	gchar *uri  = g_strconcat(conus_url, nearest, NULL);
	conus->path = grits_http_fetch(conus->http, uri, nearest,
			offline ? GRITS_LOCAL : GRITS_ONCE,
			_conus_update_loading, conus);
	g_free(nearest);
	g_free(uri);
	if (!conus->path) {
		conus->message = "Fetch failed";
		goto out;
	}

out:
	g_debug("Conus: update_thread - done");
	if (!conus->idle_source)
		conus->idle_source = g_idle_add(_conus_update_end, conus);
	return NULL;
}

void _conus_update(RadarConus *conus)
{
	if (!g_mutex_trylock(&conus->loading))
		return;
	conus->time = grits_viewer_get_time(conus->viewer);
	g_debug("Conus: update - %d",
			(gint)conus->time);

	/* Add a progress bar */
	GtkWidget *progress = gtk_progress_bar_new();
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress), "Loading...");
	aweather_bin_set_child(GTK_BIN(conus->config), progress);

	g_thread_new("conus-update-thread", _conus_update_thread, conus);
}

RadarConus *radar_conus_new(GtkWidget *pconfig,
		GritsViewer *viewer, GritsHttp *http)
{
	RadarConus *conus = g_new0(RadarConus, 1);
	conus->viewer  = g_object_ref(viewer);
	conus->http    = http;
	conus->config  = gtk_alignment_new(0, 0, 1, 1);
	g_mutex_init(&conus->loading);

	gdouble south =  CONUS_NORTH - CONUS_DEG_PER_PX_VERTICAL*CONUS_HEIGHT;
	gdouble east  =  CONUS_WEST  + CONUS_DEG_PER_PX_HORIZONTAL*CONUS_WIDTH;
	gdouble mid   =  CONUS_WEST  + CONUS_DEG_PER_PX_HORIZONTAL*CONUS_WIDTH/2;
	conus->tile[0] = grits_tile_new(NULL, CONUS_NORTH, south, mid, CONUS_WEST);
	conus->tile[1] = grits_tile_new(NULL, CONUS_NORTH, south, east, mid);
	conus->tile[0]->zindex = 2;
	conus->tile[1]->zindex = 1;
	grits_viewer_add(viewer, GRITS_OBJECT(conus->tile[0]), GRITS_LEVEL_WORLD+2, FALSE);
	grits_viewer_add(viewer, GRITS_OBJECT(conus->tile[1]), GRITS_LEVEL_WORLD+2, FALSE);

	conus->time_id = g_signal_connect_swapped(viewer, "time-changed",
			G_CALLBACK(_conus_update), conus);
	conus->refresh_id = g_signal_connect_swapped(viewer, "refresh",
			G_CALLBACK(_conus_update), conus);

	g_object_set_data(G_OBJECT(conus->config), "conus", conus);
	gtk_notebook_append_page(GTK_NOTEBOOK(pconfig), conus->config,
			gtk_label_new("Conus"));

	_conus_update(conus);
	return conus;
}

void radar_conus_free(RadarConus *conus)
{
	g_signal_handler_disconnect(conus->viewer, conus->time_id);
	g_signal_handler_disconnect(conus->viewer, conus->refresh_id);
	if (conus->idle_source)
		g_source_remove(conus->idle_source);

	for (int i = 0; i < 2; i++)
		grits_object_destroy_pointer(&conus->tile[i]);

	g_object_unref(conus->viewer);
	g_free(conus);
}


/********************
 * GritsPluginRadar *
 ********************/
static void _draw_hud(GritsCallback *callback, GritsOpenGL *opengl, gpointer _self)
{
	g_debug("GritsPluginRadar: _draw_hud");
	/* Setup OpenGL */
	glMatrixMode(GL_MODELVIEW ); glLoadIdentity();
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glEnable(GL_COLOR_MATERIAL);

	GHashTableIter iter;
	gpointer name, _site;
	GritsPluginRadar *self = GRITS_PLUGIN_RADAR(_self);
	g_hash_table_iter_init(&iter, self->sites);
	while (g_hash_table_iter_next(&iter, &name, &_site)) {
		/* Pick correct colormaps */
		RadarSite *site = _site;
		if (site->hidden || !site->level2)
			continue;
		AWeatherColormap *colormap = site->level2->sweep_colors;

		/* Print the color table */
		glBegin(GL_QUADS);
		int len = colormap->len;
		for (int i = 0; i < len; i++) {
			glColor4ubv(colormap->data[i]);
			glVertex3f(-1.0, (float)((i  ) - len/2)/(len/2), 0.0); // bot left
			glVertex3f(-1.0, (float)((i+1) - len/2)/(len/2), 0.0); // top left
			glVertex3f(-0.9, (float)((i+1) - len/2)/(len/2), 0.0); // top right
			glVertex3f(-0.9, (float)((i  ) - len/2)/(len/2), 0.0); // bot right
		}
		glEnd();
	}
}

static void _load_colormap(gchar *filename, AWeatherColormap *cm)
{
	g_debug("GritsPluginRadar: _load_colormap - %s", filename);
	FILE *file = fopen(filename, "r");
	if (!file)
		g_error("GritsPluginRadar: open failed");
	guint8 color[4];
	GArray *array = g_array_sized_new(FALSE, TRUE, sizeof(color), 256);
	if (!fgets(cm->name, sizeof(cm->name), file)) goto out;
	if (!fscanf(file, "%f\n", &cm->scale))        goto out;
	if (!fscanf(file, "%f\n", &cm->shift))        goto out;
	int r, g, b, a;
	while (fscanf(file, "%d %d %d %d\n", &r, &g, &b, &a) == 4) {
		color[0] = r;
		color[1] = g;
		color[2] = b;
		color[3] = a;
		g_array_append_val(array, color);
	}
	cm->len  = (gint )array->len;
	cm->data = (void*)array->data;
out:
	g_array_free(array, FALSE);
	fclose(file);
}

static void _update_hidden(GtkNotebook *notebook,
		gpointer _, guint page_num, gpointer viewer)
{
	g_debug("GritsPluginRadar: _update_hidden - 0..%d = %d",
			gtk_notebook_get_n_pages(notebook), page_num);

	for (gint i = 0; i < gtk_notebook_get_n_pages(notebook); i++) {
		gboolean is_hidden = (i != page_num);
		GtkWidget  *config = gtk_notebook_get_nth_page(notebook, i);
		RadarConus *conus  = g_object_get_data(G_OBJECT(config), "conus");
		RadarSite  *site   = g_object_get_data(G_OBJECT(config), "site");

		/* Conus */
		if (conus) {
			grits_object_hide(GRITS_OBJECT(conus->tile[0]), is_hidden);
			grits_object_hide(GRITS_OBJECT(conus->tile[1]), is_hidden);
		} else if (site) {
			site->hidden = is_hidden;
			if (site->level2)
				grits_object_hide(GRITS_OBJECT(site->level2), is_hidden);
		} else {
			g_warning("GritsPluginRadar: _update_hidden - no site or counus found");
		}
	}
	grits_viewer_queue_draw(viewer);
}

/* Methods */
GritsPluginRadar *grits_plugin_radar_new(GritsViewer *viewer, GritsPrefs *prefs)
{
	/* TODO: move to constructor if possible */
	g_debug("GritsPluginRadar: new");
	GritsPluginRadar *self = g_object_new(GRITS_TYPE_PLUGIN_RADAR, NULL);
	self->viewer = g_object_ref(viewer);
	self->prefs  = g_object_ref(prefs);

	/* Setup page switching */
	self->tab_id = g_signal_connect(self->config, "switch-page",
			G_CALLBACK(_update_hidden), viewer);

	/* Load HUD */
	self->hud = grits_callback_new(_draw_hud, self);
	grits_viewer_add(viewer, GRITS_OBJECT(self->hud), GRITS_LEVEL_HUD, FALSE);

	/* Load Conus */
	self->conus = radar_conus_new(self->config, self->viewer, self->conus_http);

	/* Load radar sites */
	for (city_t *city = cities; city->type; city++) {
		if (city->type != LOCATION_CITY)
			continue;
		RadarSite *site = radar_site_new(city, self->config,
				self->viewer, self->prefs, self->sites_http);
		g_hash_table_insert(self->sites, city->code, site);
	}

	return self;
}

static GtkWidget *grits_plugin_radar_get_config(GritsPlugin *_self)
{
	GritsPluginRadar *self = GRITS_PLUGIN_RADAR(_self);
	return self->config;
}

/* GObject code */
static void grits_plugin_radar_plugin_init(GritsPluginInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GritsPluginRadar, grits_plugin_radar, G_TYPE_OBJECT,
		G_IMPLEMENT_INTERFACE(GRITS_TYPE_PLUGIN,
			grits_plugin_radar_plugin_init));
static void grits_plugin_radar_plugin_init(GritsPluginInterface *iface)
{
	g_debug("GritsPluginRadar: plugin_init");
	/* Add methods to the interface */
	iface->get_config = grits_plugin_radar_get_config;
}
static void grits_plugin_radar_init(GritsPluginRadar *self)
{
	g_debug("GritsPluginRadar: class_init");
	/* Set defaults */
	self->sites_http = grits_http_new(G_DIR_SEPARATOR_S
			"nexrad" G_DIR_SEPARATOR_S
			"level2" G_DIR_SEPARATOR_S);
	self->conus_http = grits_http_new(G_DIR_SEPARATOR_S
			"nexrad" G_DIR_SEPARATOR_S
			"conus"  G_DIR_SEPARATOR_S);
	self->sites      = g_hash_table_new_full(g_str_hash, g_str_equal,
				NULL, (GDestroyNotify)radar_site_free);
	self->config     = g_object_ref(gtk_notebook_new());

	/* Load colormaps */
	for (int i = 0; colormaps[i].file; i++) {
		gchar *file = g_build_filename(PKGDATADIR,
				"colors", colormaps[i].file, NULL);
		_load_colormap(file, &colormaps[i]);
		g_free(file);
	}

	/* Need to position on the top because of Win32 bug */
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(self->config), GTK_POS_LEFT);
}
static void grits_plugin_radar_dispose(GObject *gobject)
{
	g_debug("GritsPluginRadar: dispose");
	GritsPluginRadar *self = GRITS_PLUGIN_RADAR(gobject);
	if (self->viewer) {
		GritsViewer *viewer = self->viewer;
		self->viewer = NULL;
		g_signal_handler_disconnect(self->config, self->tab_id);
		grits_object_destroy_pointer(&self->hud);
		radar_conus_free(self->conus);
		g_hash_table_destroy(self->sites);
		g_object_unref(self->config);
		g_object_unref(self->prefs);
		g_object_unref(viewer);
	}
	/* Drop references */
	G_OBJECT_CLASS(grits_plugin_radar_parent_class)->dispose(gobject);
}
static void grits_plugin_radar_finalize(GObject *gobject)
{
	g_debug("GritsPluginRadar: finalize");
	GritsPluginRadar *self = GRITS_PLUGIN_RADAR(gobject);
	/* Free data */
	grits_http_free(self->conus_http);
	grits_http_free(self->sites_http);
	gtk_widget_destroy(self->config);
	G_OBJECT_CLASS(grits_plugin_radar_parent_class)->finalize(gobject);

}
static void grits_plugin_radar_class_init(GritsPluginRadarClass *klass)
{
	g_debug("GritsPluginRadar: class_init");
	GObjectClass *gobject_class = (GObjectClass*)klass;
	gobject_class->dispose  = grits_plugin_radar_dispose;
	gobject_class->finalize = grits_plugin_radar_finalize;
}

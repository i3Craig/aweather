/*
 * Copyright (C) 2009-2011 Andy Spencer <andy753421@gmail.com>
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

#include <bits/types/locale_t.h>
#include <config.h>
#include <math.h>
#include <glib/gstdio.h>
#include <grits.h>
#include <rsl.h>
#include <stdbool.h>

#include "level2.h"

#include "../compat.h"

#define ISO_MIN 30
#define ISO_MAX 80


/* Structure to store additional data associated with a sweep selection button, allowing us to align the buttons in a grid based on elevation and volume. */
typedef struct {
	double elevation; /* stores the elevation that this button will select */

	/* Stores an ID that makes this elevation unique.
	 * That is, if there is more than one sweep in the same volume at the same elevation, this will be set to a number > 1.
	 * However, if we are in "show all sweeps" mode,
	 * this stores the start time of the sweep so that columns are arranged by the time the sweep was captured.
	 */
	time_t instance;
	int iRowIndex; /* Stores the row that this button will go into */
	GtkWidget* objSweepSelectionButton; /* Stores the button that will be put in the UI sweep selection table */
} SweepSelectionButtonInfo;

/**************************
 * Data loading functions *
 **************************/

/* Copies the date and time info from the ray header passed in into the given RSL date time struct */
void copyRayHeaderDateTimeToRslDateTimeStruct(Ray_header* rayHeader, RslDateTime* dateTime){
	dateTime->month = rayHeader->month;
	dateTime->day = rayHeader->day;
	dateTime->year = rayHeader->year;
	dateTime->hour = rayHeader->hour;
	dateTime->minute = rayHeader->minute;
	dateTime->sec = rayHeader->sec;
}

/* Returns TRUE if the ray (header) A was captured before ray (header) B in a sweep. */
bool isRayABeforeRayB(Ray_header* rayHeaderA, Ray_header* rayHeaderB){
	return rayHeaderA->year < rayHeaderB->year
		|| (rayHeaderA->year	== rayHeaderB->year	&& rayHeaderA->month	< rayHeaderB->month)
		|| (rayHeaderA->month	== rayHeaderB->month	&& rayHeaderA->day	< rayHeaderB->day)
		|| (rayHeaderA->day	== rayHeaderB->day	&& rayHeaderA->hour	< rayHeaderB->hour)
		|| (rayHeaderA->hour	== rayHeaderB->hour	&& rayHeaderA->minute	< rayHeaderB->minute)
		|| (rayHeaderA->minute	== rayHeaderB->minute	&& rayHeaderA->sec	< rayHeaderB->sec);
}

/* Returns TRUE if the RslDateTime A is before RslDateTime B chronologically. */
bool isRslDateTimeABeforeB(const RslDateTime* rslDateTimeA, const RslDateTime* rslDateTimeB){
	return rslDateTimeA->year < rslDateTimeB->year
		|| (rslDateTimeA->year		== rslDateTimeB->year	&& rslDateTimeA->month	< rslDateTimeB->month)
		|| (rslDateTimeA->month		== rslDateTimeB->month	&& rslDateTimeA->day	< rslDateTimeB->day)
		|| (rslDateTimeA->day		== rslDateTimeB->day	&& rslDateTimeA->hour	< rslDateTimeB->hour)
		|| (rslDateTimeA->hour		== rslDateTimeB->hour	&& rslDateTimeA->minute	< rslDateTimeB->minute)
		|| (rslDateTimeA->minute	== rslDateTimeB->minute	&& rslDateTimeA->sec	< rslDateTimeB->sec);
}

/* Determines when the sweep passed in started and finished. Pass pointers to two RslDateTime structs that will be populated with the specified information if available in the sweep. */
void getSweepStartAndEndTime(Sweep *sweep, RslDateTime* sweepStartTime, RslDateTime* sweepFinishTime){
	/* Stores the oldest and newest ray header so we can determine the scan start and finish time */
	Ray_header* oldestRayHeader, *newestRayHeader;

	/* Initialize the oldest and newest ray headers so they are not null */
	oldestRayHeader = &sweep->ray[0]->h;
	newestRayHeader = &sweep->ray[0]->h;
	
	/* Find the oldest and newest rays. */
	for (int ri = 0; ri < sweep->h.nrays; ri++) {
		Ray *ray  = sweep->ray[ri];

		if(isRayABeforeRayB(newestRayHeader, &ray->h)){
			newestRayHeader = &ray->h;
		}
		if(isRayABeforeRayB(&ray->h, oldestRayHeader)){
			oldestRayHeader = &ray->h;
		}
	} /* For each ray in the sweep */

	/* Get the oldest ray - this is when the sweep started. The newest ray in the sweep will contain the sweep end date + time. */
	copyRayHeaderDateTimeToRslDateTimeStruct(oldestRayHeader, sweepStartTime);
	copyRayHeaderDateTimeToRslDateTimeStruct(newestRayHeader, sweepFinishTime);
}

gchar* formatSweepStartAndEndTimeForDisplay(RslDateTime* sweepStartTime, RslDateTime* sweepFinishTime){
	gchar *date_str = g_strdup_printf("<b><i>%04d-%02d-%02d %02d:%02d:%02.0f - %02d:%02d:%02.0f</i></b>",
		sweepStartTime->year, sweepStartTime->month, sweepStartTime->day,
		sweepStartTime->hour, sweepStartTime->minute, sweepStartTime->sec,
		sweepFinishTime->hour, sweepFinishTime->minute, sweepFinishTime->sec);
	return date_str;
}

time_t getTimeTFromRslDateTime(RslDateTime* rslDateTime){
	struct tm tm_time = {0};

	/* Copy RslDateTime over to the tm struct */
	tm_time.tm_year = rslDateTime->year - 1900; /* Stores years since 1900 */
	tm_time.tm_mon = rslDateTime->month - 1; /* Stores months as 0 to 11 */
	tm_time.tm_mday = rslDateTime->day;
	tm_time.tm_hour = rslDateTime->hour;
	tm_time.tm_min = rslDateTime->minute;
	tm_time.tm_sec = (int)rslDateTime->sec;

	/* Convert the time struct into a time_t integer to return */
	time_t utc_time = timegm(&tm_time);

	return utc_time;
}


/* Returns true if the elevations should be considered to be at the same elevation (are within a hard-coded deviation). Returns false otherwise */
bool aweatherLevel2AreTheseElevationsTheSame(float ipdElevationA, float ipdElevationB){
	/* Allow for up to 0.1 degrees of difference in elevation to consider the sweep to be at the same elevation */
	float dMaxAllowedElevationDeviation = 0.1;

	float dAngleDelta = ipdElevationA - ipdElevationB;
	float dAngleDeltaAbsolute = ABS(dAngleDelta);
	return dAngleDeltaAbsolute < dMaxAllowedElevationDeviation;
}

/* Internal function use in aweatherLevel2GetAllSweepsFromVolumeWithElevationSortedBySweepStartTime - Sort the sweeps by sweep start time */
static gint fCompareSweeps(const void* a, const void* b){
	const RslSweepDateTime *sweepInfoA = a;
	const RslSweepDateTime *sweepInfoB = b;

	return !isRslDateTimeABeforeB(&sweepInfoA->startDateTime, &sweepInfoB->finishDateTime);
};

/* Returns an array (GArray) of sweep indexes for the specified volume id ("type") sorted by sweep start time (oldest to newest). An empty array is returned of no sweeps are found matching the specified criteria.
 * The returned GArray must be released via g_array_free.
 */
GArray* aweatherLevel2GetAllSweepsFromVolumeWithElevationSortedBySweepStartTime(AWeatherLevel2* level2, int ipiVolumeId, float ipdElevation){
	/* Output array we return */
	GArray* opobjArray = g_array_new(FALSE, FALSE, sizeof(RslSweepDateTime));
	
	Volume *volume = RSL_get_volume(level2->radar, ipiVolumeId);
	if(volume == NULL) goto out;

	/* Find all sweeps in the selected volume that are very close to the requested angle. */
	for(int iSweep = 0; iSweep < volume->h.nsweeps; ++iSweep){
		Sweep *sweep = volume->sweep[iSweep];
		if(aweatherLevel2AreTheseElevationsTheSame(sweep->h.elev, ipdElevation)){
			RslSweepDateTime sweepInfo;
			sweepInfo.iVolumeId = ipiVolumeId;
			sweepInfo.iSweepId = iSweep;
			getSweepStartAndEndTime(sweep, &sweepInfo.startDateTime, &sweepInfo.finishDateTime);
			g_array_append_val(opobjArray, sweepInfo);
		} /* If angle is within allowed tolerance */
	} /* For each sweep */

	g_array_sort(opobjArray, fCompareSweeps);

out:
	return opobjArray;
}

/* Convert a sweep to an 2d array of data points. */
static void _bscan_sweep(Sweep *sweep, AWeatherColormap *colormap,
		guint8 **data, int *width, int *height)
{
	g_debug("AWeatherLevel2: _bscan_sweep - %p, %p, %p",
			sweep, colormap, data);
	/* Calculate max number of bins */
	int max_bins = 0;
	for (int i = 0; i < sweep->h.nrays; i++)
		max_bins = MAX(max_bins, sweep->ray[i]->h.nbins);

	/* Allocate buffer using max number of bins for each ray */
	guint8 *buf = g_malloc0(sweep->h.nrays * max_bins * 4);

	/* Fill the data */
	for (int ri = 0; ri < sweep->h.nrays; ri++) {
		Ray *ray  = sweep->ray[ri];

		for (int bi = 0; bi < ray->h.nbins; bi++) {
			guint  buf_i = (ri*max_bins+bi)*4;
			float  value = ray->h.f(ray->range[bi]);

			/* Check for bad values */
			if (value == BADVAL     || value == RFVAL      || value == APFLAG ||
			    value == NOTFOUND_H || value == NOTFOUND_V || value == NOECHO) {
				buf[buf_i+3] = 0x00; // transparent
				continue;
			}

			/* Copy color to buffer */
			guint8 *data = colormap_get(colormap, value);
			buf[buf_i+0] = data[0];
			buf[buf_i+1] = data[1];
			buf[buf_i+2] = data[2];
			buf[buf_i+3] = data[3]*0.75; // TESTING
		}
	}

	/* set output */
	*width  = max_bins;
	*height = sweep->h.nrays;
	*data   = buf;
}

/* Load a sweep into an OpenGL texture */
static void _load_sweep_gl(AWeatherLevel2 *level2)
{
	g_debug("AWeatherLevel2: _load_sweep_gl");
	guint8 *data;
	gint width, height;
	_bscan_sweep(level2->sweep, level2->sweep_colors, &data, &width, &height);
	gint tex_width  = pow(2, ceil(log(width )/log(2)));
	gint tex_height = pow(2, ceil(log(height)/log(2)));
	level2->sweep_coords[0] = (double)width  / tex_width;
	level2->sweep_coords[1] = (double)height / tex_height;

	if (!level2->sweep_tex)
		 glGenTextures(1, &level2->sweep_tex);
	glBindTexture(GL_TEXTURE_2D, level2->sweep_tex);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_width, tex_height, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0,0, width,height,
			GL_RGBA, GL_UNSIGNED_BYTE, data);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	g_free(data);
}

static void _updateSweepTimestampGui(AWeatherLevel2* level2){
	/* Update the GUI to show the user when the sweep started and ended */
	RslDateTime sweepStartTime, sweepFinishTime;
	if(level2->date_label != NULL){
		getSweepStartAndEndTime(level2->sweep, &sweepStartTime, &sweepFinishTime);
		gchar *date_str = formatSweepStartAndEndTimeForDisplay(&sweepStartTime, &sweepFinishTime);
		gtk_label_set_markup(GTK_LABEL(level2->date_label), date_str);
		g_free(date_str);
	}

}

/* Decompress a radar file using wsr88dec */
static gboolean _decompress_radar(const gchar *file, const gchar *raw)
{
	g_debug("AWeatherLevel2: _decompress_radar - \n\t%s\n\t%s", file, raw);
	char *argv[] = {"wsr88ddec", (gchar*)file, (gchar*)raw, NULL};
	gint status;
	GError *error = NULL;
	g_spawn_sync(
		NULL,    // const gchar *working_directory
		argv,    // gchar **argv
		NULL,    // gchar **envp
		G_SPAWN_SEARCH_PATH, // GSpawnFlags flags
		NULL,    // GSpawnChildSetupFunc child_setup
		NULL,    // gpointer user_data
		NULL,    // gchar *standard_output
		NULL,    // gchar *standard_output
		&status, // gint *exit_status
		&error); // GError **error
	if (error) {
		g_warning("AWeatherLevel2: _decompress_radar - %s", error->message);
		g_error_free(error);
		return FALSE;
	}
	if (status != 0) {
		gchar *msg = g_strdup_printf("wsr88ddec exited with status %d", status);
		g_warning("AWeatherLevel2: _decompress_radar - %s", msg);
		g_free(msg);
		return FALSE;
	}
	return TRUE;
}

/* Load the radar into a Grits Volume */
static void _cart_to_sphere(VolCoord *out, VolCoord *in)
{
	gdouble angle = in->x;
	gdouble dist  = in->y;
	gdouble tilt  = in->z;
	gdouble lx    = sin(angle);
	gdouble ly    = cos(angle);
	gdouble lz    = sin(tilt);
	//out->x = (ly*dist)/20000;
	//out->y = (lz*dist)/10000-0.5;
	//out->z = (lx*dist)/20000-1.5;
	out->x = (lx*dist);
	out->y = (ly*dist);
	out->z = (lz*dist);
}

static VolGrid *_load_grid(Volume *vol)
{
	g_debug("AWeatherLevel2: _load_grid");

	Sweep *sweep   = vol->sweep[0];
	Ray   *ray     = sweep->ray[0];
	gint nsweeps   = vol->h.nsweeps;
	gint nrays     = sweep->h.nrays/(1/sweep->h.beam_width)+1;
	gint nbins     = ray->h.nbins  /(1000/ray->h.gate_size);
	nbins = MIN(nbins, 150);

	VolGrid  *grid = vol_grid_new(nrays, nbins, nsweeps);

	gint rs, bs, val;
	gint si=0, ri=0, bi=0;
	for (si = 0; si < nsweeps; si++) {
		sweep = vol->sweep[si];
		rs    = 1.0/sweep->h.beam_width;
	for (ri = 0; ri < nrays; ri++) {
		/* TODO: missing rays, pick ri based on azmith */
		ray   = sweep->ray[(ri*rs) % sweep->h.nrays];
		bs    = 1000/ray->h.gate_size;
	for (bi = 0; bi < nbins; bi++) {
		if (bi*bs >= ray->h.nbins)
			break;
		val   = ray->h.f(ray->range[bi*bs]);
		if (val == BADVAL     || val == RFVAL      ||
		    val == APFLAG     || val == NOECHO     ||
		    val == NOTFOUND_H || val == NOTFOUND_V ||
		    val > 80)
			val = 0;
		VolPoint *point = vol_grid_get(grid, ri, bi, si);
		point->value = val;
		point->c.x = deg2rad(ray->h.azimuth);
		point->c.y = bi*bs*ray->h.gate_size + ray->h.range_bin1;
		point->c.z = deg2rad(ray->h.elev);
	} } }

	for (si = 0; si < nsweeps; si++)
	for (ri = 0; ri < nrays; ri++)
	for (bi = 0; bi < nbins; bi++) {
		VolPoint *point = vol_grid_get(grid, ri, bi, si);
		if (point->c.y == 0)
			point->value = nan("");
		else
			_cart_to_sphere(&point->c, &point->c);
	}
	return grid;
}


/*********************
 * Drawing functions *
 *********************/
void aweather_level2_draw(GritsObject *_level2, GritsOpenGL *opengl)
{
	AWeatherLevel2 *level2 = AWEATHER_LEVEL2(_level2);
	if (!level2->sweep || !level2->sweep_tex)
		return;

	/* Draw wsr88d */
	Sweep *sweep = level2->sweep;
	//glDisable(GL_ALPHA_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(1.0, -2.0);
	glColor4f(1,1,1,1);

	/* Draw the rays */
	gdouble xscale = level2->sweep_coords[0];
	gdouble yscale = level2->sweep_coords[1];
	glBindTexture(GL_TEXTURE_2D, level2->sweep_tex);
	glBegin(GL_TRIANGLE_STRIP);
	for (int ri = 0; ri <= sweep->h.nrays; ri++) {
		Ray  *ray = NULL;
		double angle = 0;
		if (ri < sweep->h.nrays) {
			ray = sweep->ray[ri];
			angle = deg2rad(ray->h.azimuth - ((double)ray->h.beam_width/2.));
		} else {
			/* Do the right side of the last sweep */
			ray = sweep->ray[ri-1];
			angle = deg2rad(ray->h.azimuth + ((double)ray->h.beam_width/2.));
		}

		double lx = sin(angle);
		double ly = cos(angle);

		double near_dist = ray->h.range_bin1 - ((double)ray->h.gate_size/2.);
		double far_dist  = near_dist + (double)ray->h.nbins*ray->h.gate_size;

		/* (find middle of bin) / scale for opengl */
		// near left
		glTexCoord2f(0.0, ((double)ri/sweep->h.nrays)*yscale);
		glVertex3f(lx*near_dist, ly*near_dist, 2.0);

		// far  left
		// todo: correct range-height function
		double height = sin(deg2rad(ray->h.elev)) * far_dist;
		glTexCoord2f(xscale, ((double)ri/sweep->h.nrays)*yscale);
		glVertex3f(lx*far_dist,  ly*far_dist, height);
	}
	glEnd();
	//g_print("ri=%d, nr=%d, bw=%f\n", _ri, sweep->h.nrays, sweep->h.beam_width);

	/* Texture debug */
	//glBegin(GL_QUADS);
	//glTexCoord2d( 0.,  0.); glVertex3f(-500.,   0., 0.); // bot left
	//glTexCoord2d( 0.,  1.); glVertex3f(-500., 500., 0.); // top left
	//glTexCoord2d( 1.,  1.); glVertex3f( 0.,   500., 3.); // top right
	//glTexCoord2d( 1.,  0.); glVertex3f( 0.,     0., 3.); // bot right
	//glEnd();
}

void aweather_level2_hide(GritsObject *_level2, gboolean hidden)
{
	AWeatherLevel2 *level2 = AWEATHER_LEVEL2(_level2);
	if (level2->volume)
		grits_object_hide(GRITS_OBJECT(level2->volume), hidden);
}


/***********
 * Methods *
 ***********/
static gboolean _set_sweep_cb(gpointer _level2)
{
	g_debug("AWeatherLevel2: _set_sweep_cb");
	AWeatherLevel2 *level2 = _level2;
	_load_sweep_gl(level2);
	_updateSweepTimestampGui(level2);
	grits_object_queue_draw(_level2);
	g_object_unref(level2);

	/* If the callback function pointer is set to a non-null value, then execute the callback function and clear out the pointer. */
	if(level2->fAfterSetSweepOneTimeCustomCallback != NULL){
		level2->fAfterSetSweepOneTimeCustomCallback(level2->objAfterSetSweepOneTimeCustomCallbackData);
		level2->fAfterSetSweepOneTimeCustomCallback = NULL;
		level2->objAfterSetSweepOneTimeCustomCallbackData = NULL;
	}

	return FALSE;
}
void aweather_level2_set_sweep(AWeatherLevel2 *level2,
		int type, int ipiSweepIndex)
{
	g_debug("AWeatherLevel2: set_sweep - Type: %d, SweepIndex: %i", type, ipiSweepIndex);

	/* Find sweep */
	Volume *volume = RSL_get_volume(level2->radar, type);
	if (!volume) return;
	if(ipiSweepIndex < 0 || ipiSweepIndex >= volume->h.nsweeps){
		g_error("Error: Invalid sweep index passed in: %i. Number of sweeps: %i", ipiSweepIndex, volume->h.nsweeps);
		return;
	}
	level2->sweep = volume->sweep[ipiSweepIndex];
	if (!level2->sweep) return;

	/* Find colormap */
	level2->sweep_colors = NULL;
	for (int i = 0; level2->colormap[i].file; i++)
		if (level2->colormap[i].type == type)
			level2->sweep_colors = &level2->colormap[i];
	if (!level2->sweep_colors) {
		g_warning("AWeatherLevel2: set_sweep - missing colormap[%d]", type);
		level2->sweep_colors = &level2->colormap[0];
	}

	/* Load data on the UI thread as the OpenGL calls may not work in the background thread.
	 * We use G_PRIORITY_HIGH_IDLE so that the sweep change can happen even if the user is interacting with the app (for example, moving the map).
	 */
	g_object_ref(level2);
	g_idle_add_full(G_PRIORITY_HIGH_IDLE, _set_sweep_cb, level2, NULL);

	/* Store the selected volume and sweep id */
	level2->iSelectedVolumeId = type;
	level2->iSelectedSweepId = ipiSweepIndex;
	level2->dSelectedElevation = level2->sweep->h.elev;
}

void aweather_level2_set_iso(AWeatherLevel2 *level2, gfloat level, bool iplAsync)
{
	g_debug("AWeatherLevel2: set_iso - %f", level);

	if (!level2->volume) {
		g_debug("AWeatherLevel2: set_iso - creating new volume");
		Volume      *rvol = RSL_get_volume(level2->radar, DZ_INDEX);
		if(rvol == NULL){
			g_debug("aweather_level2_set_iso: This NEXRAD Level 2 file contains no volume and no sweeps. We are unable to set the iso. Giving up now.");
			return;
		}
		VolGrid     *grid = _load_grid(rvol);
		GritsVolume *vol  = grits_volume_new(grid);
		vol->proj = GRITS_VOLUME_CARTESIAN;
		vol->disp = GRITS_VOLUME_SURFACE;
		GRITS_OBJECT(vol)->center = GRITS_OBJECT(level2)->center;
		grits_viewer_add(GRITS_OBJECT(level2)->viewer,
				GRITS_OBJECT(vol), GRITS_LEVEL_WORLD+5, TRUE);
		level2->volume = vol;
	}

	if (ISO_MIN < level && level < ISO_MAX) {
		guint8 *data = colormap_get(&level2->colormap[0], level);
		level2->volume->color[0] = data[0];
		level2->volume->color[1] = data[1];
		level2->volume->color[2] = data[2];
		level2->volume->color[3] = data[3];
		/* If we were asked to do this async, then run the normal method. If we were asked to do this synchronously, then run the 'sync' function. */
		if(iplAsync)
			grits_volume_set_level(level2->volume, level);
		else
			grits_volume_set_level_sync(level2->volume, level);

		/* If the callback function pointer is set to a non-null value, then execute the callback function and clear out the pointer. */
		if(level2->fOnSetIsoOneTimeCustomCallback != NULL){
			level2->fOnSetIsoOneTimeCustomCallback(level2->objOnSetIsoOneTimeCustomCallbackData);
			level2->fOnSetIsoOneTimeCustomCallback = NULL;
			level2->objOnSetIsoOneTimeCustomCallbackData = NULL;
		}

		/* If this level2 object is currently hidden (for example, if we are animating and this frame is not needed),
		 * then do not try to show the 3D radar isosurface.
		 * Otherwise, show the isosurface.
		 */
		grits_object_hide(GRITS_OBJECT(level2->volume), GRITS_OBJECT(level2)->hidden);
	} else {
		grits_object_hide(GRITS_OBJECT(level2->volume), TRUE);
	}
}

AWeatherLevel2 *aweather_level2_new(Radar *radar, AWeatherColormap *colormap)
{
	g_debug("AWeatherLevel2: new - %s", radar->h.radar_name);
	RSL_sort_radar(radar);
	AWeatherLevel2 *level2 = g_object_new(AWEATHER_TYPE_LEVEL2, NULL);
	level2->fAfterSetSweepOneTimeCustomCallback = NULL; /* Ensure the callback function pointer is not pointing to anything */
	level2->objAfterSetSweepOneTimeCustomCallbackData = NULL;
	level2->fOnSetIsoOneTimeCustomCallback = NULL; /* Ensure the callback function pointer is not pointing to anything */
	level2->objOnSetIsoOneTimeCustomCallbackData = NULL;
	level2->radar    = radar;
	level2->colormap = colormap;

	/* Default to no sweep / volume selected yet */
	level2->iSelectedSweepId = AWEATHER_LEVEL2_SELECTED_SWEEP_ID_NONE;
	level2->iSelectedVolumeId = AWEATHER_LEVEL2_SELECTED_VOLUME_ID_NONE;

	aweather_level2_set_sweep(level2, DZ_INDEX, 0);

	GritsPoint center;
	Radar_header *h = &radar->h;
	center.lat  = (double)h->latd + (double)h->latm/60 + (double)h->lats/(60*60);
	center.lon  = (double)h->lond + (double)h->lonm/60 + (double)h->lons/(60*60);
	center.elev = h->height;
	GRITS_OBJECT(level2)->center = center;
	return level2;
}

AWeatherLevel2 *aweather_level2_new_from_file(const gchar *file, const gchar *site,
		AWeatherColormap *colormap, GritsPrefs *prefs)
{
	g_debug("AWeatherLevel2: new_from_file %s %s", site, file);

	/* Decompress radar */
	gchar *raw = g_strconcat(file, ".raw", NULL);
	if (g_file_test(raw, G_FILE_TEST_EXISTS)) {
		struct stat files, raws;
		g_stat(file, &files);
		g_stat(raw,  &raws);
		if (files.st_mtime > raws.st_mtime)
			if (!_decompress_radar(file, raw))
				return NULL;
	} else {
		if (!_decompress_radar(file, raw))
			return NULL;
	}

	/* Load the radar file */
	RSL_read_these_sweeps("all", NULL);
	g_debug("AWeatherLevel2: rsl read start");

	/* If the user wants to show the reflectivity data from the velocity sweeps, then disable the "merge split cuts" option in RSL so those
	 * sweeps are not removed.
	 * Enabling this option (setting it to true) will cause extra reflectivity sweeps to show up,
	 * which can be useful when looking for closer to real-time data.
	 */
	if(grits_prefs_get_boolean(prefs, "aweather/RSL_wsr88d_merge_split_cuts_off", NULL)){
		RSL_wsr88d_merge_split_cuts_off();
	} else {
		RSL_wsr88d_merge_split_cuts_on();
	}

	Radar *radar = RSL_wsr88d_to_radar(raw, (gchar*)site);
	g_debug("AWeatherLevel2: rsl read done");
	g_free(raw);
	if (!radar)
		return NULL;

	return aweather_level2_new(radar, colormaps);
}

static void _on_sweep_clicked(GtkRadioButton *button, gpointer _level2)
{
	AWeatherLevel2 *level2 = _level2;
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button))) {
		gint type = (glong)g_object_get_data(G_OBJECT(button), "type");
		gint sweepIndex = (glong)g_object_get_data(G_OBJECT(button), "sweepIndex");
		aweather_level2_set_sweep(level2, type, sweepIndex);
		//level2->colormap = level2->sweep_colors;
	}
}

static void _on_iso_changed(GtkRange *range, gpointer _level2)
{
	AWeatherLevel2 *level2 = _level2;
	gfloat level = gtk_range_get_value(range);
	aweather_level2_set_iso(level2, level, true /* generate isosurface async */);
}

static gchar *_on_format_value(GtkScale *scale, gdouble value, gpointer _level2)
{
	return g_strdup_printf("%.1lf dBZ ", value);
}

/* Internal function used in aweather_level2_get_config - Sort global elevation instances by elevation, then instance */
static gint compare_elevation_instance(const void *a, const void *b) {
	const SweepSelectionButtonInfo *ei_a = a;
	const SweepSelectionButtonInfo *ei_b = b;

	if (ei_a->elevation < ei_b->elevation) return -1;
	if (ei_a->elevation > ei_b->elevation) return 1;
	return (ei_a->instance < ei_b->instance) ? -1 : 1;
}


GtkWidget *aweather_level2_get_config(AWeatherLevel2 *level2, GritsPrefs *prefs)
{
	Radar *radar = level2->radar;
	g_debug("AWeatherLevel2: get_config - %p, %p", level2, radar);
	/* Clear existing items */
	guint rows = 1, cols = 1;
	gchar row_label_str[64], col_label_str[64], button_str[64];
	GtkWidget *row_label, *col_label, *button = NULL;
	GtkWidget *table = gtk_table_new(rows, cols, FALSE);

	/* Add date */
	gchar *date_str = g_strdup_printf("<b><i>%04d-%02d-%02d %02d:%02d</i></b>",
			radar->h.year, radar->h.month, radar->h.day,
			radar->h.hour, radar->h.minute);
	GtkWidget *date_label = gtk_label_new(date_str);
	gtk_label_set_use_markup(GTK_LABEL(date_label), TRUE);
	gtk_table_attach(GTK_TABLE(table), date_label,
			0,1, 0,1, GTK_FILL,GTK_FILL, 5,0);
	g_free(date_str);
	/* Copy over out pointer to the date label so we can update the text dynamically later */
	level2->date_label = date_label;

	/* Determine if we should order the columns by elevation angle and duplicate instance of that elevation angle (false)
	 * or by sweep start time (true).
	 */
	bool lIsShowAllSweepsEnabled = grits_prefs_get_boolean(prefs, "aweather/RSL_wsr88d_merge_split_cuts_off", NULL);

	/* Step 1: Collect unique global (elevation, instance) pairs */
	GList *global_elev_list = NULL;

	rows = 1; // Start with row 1 for column headers
	for (guint vi = 0; vi < radar->h.nvolumes; vi++) {
		Volume *vol = radar->v[vi];
		if (vol == NULL) continue;

		rows++;

		/* Add the row labels */
		g_snprintf(row_label_str, 64, "<b>%s:</b>", vol->h.type_str);
		row_label = gtk_label_new(row_label_str);
		gtk_label_set_use_markup(GTK_LABEL(row_label), TRUE);
		gtk_misc_set_alignment(GTK_MISC(row_label), 1, 0.5);
		gtk_table_attach(GTK_TABLE(table), row_label, 0, 1, rows - 1, rows, GTK_FILL, GTK_FILL, 5, 0);

		GHashTable *local_elev_count = g_hash_table_new(g_double_hash, g_double_equal);


		for (guint si = 0; si < vol->h.nsweeps; si++) {
			Sweep *sweep = vol->sweep[si];
			if (sweep == NULL || sweep->h.elev == 0) continue;


			time_t instance = 0;
			if(lIsShowAllSweepsEnabled){
				/* Use the sweep start time as the column for this sweep.
				 * In other words, each column in the sweep selection grid is a specific time.
				 */
				RslDateTime objSweepStartTime;
				RslDateTime objSweepFinishTime;
				getSweepStartAndEndTime(sweep, &objSweepStartTime, &objSweepFinishTime);
				instance = getTimeTFromRslDateTime(&objSweepStartTime);
			} else {
				/* Count sweeps at this elevation in the current volume */
				instance = GPOINTER_TO_INT(g_hash_table_lookup(local_elev_count, &sweep->h.elev)) + 1;
				g_debug("level2.c aweather_level2_get_config: Found sweep. Will put in row: %i, elevation: %f, instance: %li", rows, sweep->h.elev, instance);
				g_hash_table_insert(local_elev_count, &sweep->h.elev, GUINT_TO_POINTER(instance));
			}

			/* Create the sweep selection button that we will later place in the UI table */
			g_snprintf(button_str, 64, "%3.2f", sweep->h.elev);
			button = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(button), button_str);
			gtk_widget_set_size_request(button, -1, 26);
			g_object_set(button, "draw-indicator", FALSE, NULL);
			g_object_set_data(G_OBJECT(button), "level2", level2);
			g_object_set_data(G_OBJECT(button), "type", (gpointer)(guintptr)vi);
			g_object_set_data(G_OBJECT(button), "elev", (gpointer)(guintptr)(sweep->h.elev * 100));
			g_object_set_data(G_OBJECT(button), "sweepIndex", (gpointer)(guintptr)si);
			g_signal_connect(button, "clicked", G_CALLBACK(_on_sweep_clicked), level2);

			/* Add unique (elevation, instance) globally */
			SweepSelectionButtonInfo *ei = g_malloc(sizeof(SweepSelectionButtonInfo));
			ei->elevation = sweep->h.elev;
			ei->instance = instance;
			ei->iRowIndex = rows;
			ei->objSweepSelectionButton = button;
			global_elev_list = g_list_append(global_elev_list, ei);
		}

		g_hash_table_destroy(local_elev_count);
	}

	/* Sort the full list of all buttons by elevation, then instance so that we can align elevations and instances vertically as columns. */
	global_elev_list = g_list_sort(global_elev_list, (GCompareFunc)compare_elevation_instance);

	/* Step 2: Populate the table with the buttons we generated above. */
	double dCurrentElevation = 0;
	guint iCurrentInstance = 0;
	int iCurrentColumn = 1; /* Start at column 1 to stay to the right of the row headers */
	for (GList *node = global_elev_list; node; node = node->next) {
		SweepSelectionButtonInfo *ei = (SweepSelectionButtonInfo*)node->data;
		/* If we need a new column (hit a new elevation or instance of that duplicate elevation), then create a new column */
		if(ei->elevation != dCurrentElevation
			|| ei->instance != iCurrentInstance
		){
			++iCurrentColumn;
			dCurrentElevation = ei->elevation;
			iCurrentInstance = ei->instance;

			/* Add a header label for this new column */
			g_snprintf(col_label_str, 64, "<b>%.2f°</b>", ei->elevation);
			col_label = gtk_label_new(col_label_str);
			gtk_label_set_use_markup(GTK_LABEL(col_label), TRUE);
			gtk_widget_set_size_request(col_label, 50, -1);
			gtk_table_attach(GTK_TABLE(table), col_label, iCurrentColumn, iCurrentColumn + 1, 0, 1, GTK_FILL, GTK_FILL, 0, 0);
		}

		g_debug("level2.c aweather_level2_get_config: Adding button for elevation %f, instance: %li, row: %i, col: %i", ei->elevation, ei->instance, ei->iRowIndex, iCurrentColumn);

		/* Add the sweep selection button to the UI table */
		gtk_table_attach(
			GTK_TABLE(table),
			ei->objSweepSelectionButton,
			iCurrentColumn, iCurrentColumn + 1,
			ei->iRowIndex - 1, ei->iRowIndex,
			GTK_FILL, GTK_FILL, 0, 0
		); /* gtk_table_attach */
	} /* for (GList *node = global_elev_list; node; node = node->next) { */

	/* Cleanup buttons list */
	g_list_free_full(global_elev_list, g_free);

	/* Add Iso-surface volume */
	g_object_get(table, "n-columns", &cols, NULL);
	row_label = gtk_label_new("<b>Isosurface:</b>");
	gtk_label_set_use_markup(GTK_LABEL(row_label), TRUE);
	gtk_misc_set_alignment(GTK_MISC(row_label), 1, 0.5);
	gtk_table_attach(GTK_TABLE(table), row_label,
			0,1, rows,rows+1, GTK_FILL,GTK_FILL, 5,0);
	GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, ISO_MIN, ISO_MAX, 0.5);
	gtk_widget_set_size_request(scale, -1, 26);
	gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_LEFT);
	gtk_range_set_inverted(GTK_RANGE(scale), TRUE);
	gtk_range_set_value(GTK_RANGE(scale), ISO_MAX);
	g_signal_connect(scale, "value-changed", G_CALLBACK(_on_iso_changed),  level2);
	g_signal_connect(scale, "format-value",  G_CALLBACK(_on_format_value), level2);
	gtk_table_attach(GTK_TABLE(table), scale,
			1,cols+1, rows,rows+1, GTK_FILL|GTK_EXPAND,GTK_FILL, 0,0);
	/* Shove all the buttons to the left, but keep the slider expanded */
	gtk_table_attach(GTK_TABLE(table), gtk_label_new(""),
			cols,cols+1, 0,1, GTK_FILL|GTK_EXPAND,GTK_FILL, 0,0);
	return table;
}

/****************
 * GObject code *
 ****************/
G_DEFINE_TYPE(AWeatherLevel2, aweather_level2, GRITS_TYPE_OBJECT);
static void aweather_level2_init(AWeatherLevel2 *level2)
{
}
static void aweather_level2_dispose(GObject *_level2)
{
	AWeatherLevel2 *level2 = AWEATHER_LEVEL2(_level2);
	g_debug("AWeatherLevel2: dispose - %p", _level2);
	grits_object_destroy_pointer(&level2->volume);
	G_OBJECT_CLASS(aweather_level2_parent_class)->dispose(_level2);
}
static void aweather_level2_finalize(GObject *_level2)
{
	AWeatherLevel2 *level2 = AWEATHER_LEVEL2(_level2);
	g_debug("AWeatherLevel2: finalize - %p", _level2);
	RSL_free_radar(level2->radar);
	if (level2->sweep_tex)
		glDeleteTextures(1, &level2->sweep_tex);
	G_OBJECT_CLASS(aweather_level2_parent_class)->finalize(_level2);
}
static void aweather_level2_class_init(AWeatherLevel2Class *klass)
{
	G_OBJECT_CLASS(klass)->dispose  = aweather_level2_dispose;
	G_OBJECT_CLASS(klass)->finalize = aweather_level2_finalize;
	GRITS_OBJECT_CLASS(klass)->draw = aweather_level2_draw;
	GRITS_OBJECT_CLASS(klass)->hide = aweather_level2_hide;
}

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

#ifndef __AWEATHER_LEVEL2_H__
#define __AWEATHER_LEVEL2_H__

#include <grits.h>
#include "radar-info.h"

/* Level2 */
#define AWEATHER_TYPE_LEVEL2            (aweather_level2_get_type())
#define AWEATHER_LEVEL2(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),   AWEATHER_TYPE_LEVEL2, AWeatherLevel2))
#define AWEATHER_IS_LEVEL2(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),   AWEATHER_TYPE_LEVEL2))
#define AWEATHER_LEVEL2_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST   ((klass), AWEATHER_TYPE_LEVEL2, AWeatherLevel2Class))
#define AWEATHER_IS_LEVEL2_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE   ((klass), AWEATHER_TYPE_LEVEL2))
#define AWEATHER_LEVEL2_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),   AWEATHER_TYPE_LEVEL2, AWeatherLevel2Class))

/* These constants are stored in iSelectedSweepId and iSelectedVolumeId, respectively before the user selects a sweep / volume */
#define AWEATHER_LEVEL2_SELECTED_SWEEP_ID_NONE -1
#define AWEATHER_LEVEL2_SELECTED_VOLUME_ID_NONE -1

typedef struct _AWeatherLevel2      AWeatherLevel2;
typedef struct _AWeatherLevel2Class AWeatherLevel2Class;

/* Stores sweep texture related properties so we can cache them if we believe they may be needed later.
 * For example, if we are in an animation and the frame may be needed again, there is no need to recompute the sweep texture
 * and re-upload it to the GPU.
 */
typedef struct {
	guint             sweep_tex;
	gdouble           sweep_coords[2];
} SweepTexture;

struct _AWeatherLevel2 {
	GritsObject       parent;
	Radar            *radar;
	AWeatherColormap *colormap;

	/* Private */
	GritsVolume      *volume;
	Sweep            *sweep;
	AWeatherColormap *sweep_colors;
	SweepTexture     *objSweepTexture;

	/* Texture cache properties */
	bool            lEnableSweepTextureCache;
	SweepTexture     **aSweepTexturesCache;
	gint             iSweepTexturesCacheLength;

	GtkWidget	*date_label; /* Pointer to the date label in the GUI so we can update the text dynamically */

	int		iSelectedVolumeId;	/* Stores the volume id ("type") that the user has selected to view in this level2 file */
	int		iSelectedSweepId;	/* Stores the sweep id ("elevatioin id") that the user has selected to view in this volume file */
	float		dSelectedElevation;	/* Stores the elevation angle that the user has selected to view in this volume */
	
	Sweep            *objSelectedRslSweep; /* Stores the Sweep pointer the user just selected. This is copied to 'sweep' once all processing to change the sweep is complete. */
	Volume           *objSelectedRslVolume; /* Stores the volume the user selected that the selected sweep is contained in. */
	int              iPreviouslyDisplayedVolumeId; /* Stores the ID of the volume that we previously displayed to the user. */
	int              iPreviouslyDisplayedSweepId; /* Stores the ID (index in volume) of the sweep that was previously displayed to the user */

	void (*fAfterSetSweepOneTimeCustomCallback)(gpointer);	/* Stores a pointer to a custom function which if set to a non-null value will be executed exactly once on the main ui thread after the sweep has been loaded into memory (set sweep operation is complete). */
	gpointer objAfterSetSweepOneTimeCustomCallbackData; /* Stores data passed to this one-time callback */

	void (*fOnSetIsoOneTimeCustomCallback)(gpointer); /* Stores a pointer to a custom function which if set to a non-null value will be executed when the user changes the isosurface slider in the GUI, after volume->level is updated, but not necessarily after the isosurface is updated in the UI */
	gpointer objOnSetIsoOneTimeCustomCallbackData; /* Stores data passed to this one-time callback */
};

struct _AWeatherLevel2Class {
	GritsObjectClass parent_class;
};

/* Structure to store the date and time from the RSL structure, allowing us to store the start and stop times for a sweep */
typedef struct{
  int   month; /* Time for this ray; month (1-12). */
  int   day;   /* Time for this ray; day (1-31).   */
  int   year;  /* Time for this ray; year (eg. 1993). */
  int   hour;  /* Date for this ray; hour (0-23). */
  int   minute;/* Date for this ray; minute (0-59).*/
  float sec;   /* Date for this ray; second + fraction of second. */
} RslDateTime;

/* Structure to store the star and finish time for a sweep as well as the sweep id and volume id */
typedef struct{
	RslDateTime	startDateTime;
	RslDateTime	finishDateTime;
	int		iSweepId;
	int		iVolumeId;
} RslSweepDateTime;

GType aweather_level2_get_type(void);

AWeatherLevel2 *aweather_level2_new(Radar *radar, AWeatherColormap *colormap);

AWeatherLevel2 *aweather_level2_new_from_file(const gchar *file, const gchar *site,
		AWeatherColormap *colormap, GritsPrefs *prefs);

void aweather_level2_set_sweep(AWeatherLevel2 *level2,
		int type, int ipiSweepIndex);

/* Call to set the isosurface level for the given level2 frame. Pass in true for iplAsync to generate the isosurface asynchronously (in the UI thread).
 * Pass in false for iplAsync to generate the isosurface in the calling thread synchronously.
 */
void aweather_level2_set_iso(AWeatherLevel2 *level2, gfloat level, bool iplAsync);

GtkWidget *aweather_level2_get_config(AWeatherLevel2 *level2, GritsPrefs *prefs);


/* Copies the date and time info from the ray header passed in into the given RSL date time struct */ 
void copyRayHeaderDateTimeToRslDateTimeStruct(Ray_header* rayHeader, RslDateTime* dateTime);
 
/* Returns TRUE if the ray (header) A was captured before ray (header) B in a sweep. */ 
bool isRayABeforeRayB(Ray_header* rayHeaderA, Ray_header* rayHeaderB);

/* Returns TRUE if the RslDateTime A is before RslDateTime B chronologically. */ 
bool isRslDateTimeABeforeB(const RslDateTime* rslDateTimeA, const RslDateTime* rslDateTimeB);

/* Determines when the sweep passed in started and finished. Pass pointers to two RslDateTime structs that will be populated with the specified information if available in the sweep. */
void getSweepStartAndEndTime(Sweep *sweep, RslDateTime* sweepStartTime, RslDateTime* sweepFinishTime);

/* returns a string in the format <b><i>/yyyy-mm-dd hh:mm:ss - hh:mm:ss</b></i> for display in a GTK label with markup enabled to display the start and end time of a sweep.
 * Note: The returned string must be released with  g_free()
 */
gchar* formatSweepStartAndEndTimeForDisplay(RslDateTime* sweepStartTime, RslDateTime* sweepFinishTime);

/* Converts the RslDateTime given into a time_t time so it can be used more easily in C */
time_t getTimeTFromRslDateTime(RslDateTime* rslDateTime);

/* Returns true if the elevations should be considered to be at the same elevation (are within a hard-coded deviation). Returns false otherwise */
bool aweatherLevel2AreTheseElevationsTheSame(float ipdElevationA, float ipdElevationB);

/* Returns an array (GArray) of sweep indexes for the specified volume id ("type") sorted by sweep start time (oldest to newest). An empty array is returned of no sweeps are found matching the specified criteria.
 * The returned GArray must be released via g_array_free.
 */
GArray* aweatherLevel2GetAllSweepsFromVolumeWithElevationSortedBySweepStartTime(AWeatherLevel2* level2, int ipiVolumeId, float ipdElevation);

/* Call to update the GUI sweep timestamp label to reflect the time that the currently selected sweep was captured */
void aweatherLevel2UpdateSweepTimestampGui(AWeatherLevel2* level2);

#endif

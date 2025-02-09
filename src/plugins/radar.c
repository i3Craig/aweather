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
#include <stdatomic.h>
#include <stdbool.h>
#include <config.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
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


/* Internal function used by _find_nearest_return_GList_pointer - parses the file name into a time_t */
static time_t _parse_file_time(const gchar *file, gsize offset) {
	struct tm tm = {};
	sscanf(file + offset, "%4d%2d%2d_%2d%2d",
		&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		&tm.tm_hour, &tm.tm_min);
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	return mktime(&tm);
}

/* Internal function used by _find_nearest_return_GList_pointer - Compares times to allow us to sort the list of file names by timestamp. */
static gint _compare_files_by_time(const void *a, const void *b, gpointer offsetPointer) {
	gsize offset = * (gsize *) offsetPointer;
	time_t time_a = _parse_file_time((const gchar *)a, offset);
	time_t time_b = _parse_file_time((const gchar *)b, offset);
	return (time_a - time_b) > 0;
}


/* This function returns the raw pointer to the GList struct that is the closest to the requested time.
 * Whereas the function below returns the name of the file only.
 * If sort_by_time is true, the files list is sorted by timestamp (descending) before finding the nearest file.
 * Note that if sorting is enabled, a new list is returned via the return GList of this function.
 * It is up to the caller to ensure this new list is deleted (g_list_free), but the contained strings do not need to be deleted as they point to strings in the provided list.
 * Recommended deleting method: g_list_free(returned_list); g_list_foreach(files, (GFunc)g_free, NULL); g_list_free(files);
 * If sorting is enabled, duplicate entries will be removed  in the returned list (duplicates can come from cached files that also exist on the server).
 */
static GList *_find_nearest_return_GList_pointer(time_t time, GList *files, gsize offset, gboolean sort_by_time)
{
	g_debug("RadarSite: find_nearest ...");

	/* If sorting is enabled, then pre-sort the array so the caller gets the list back in a consistent order */
	if (sort_by_time) {
		g_debug("RadarSite: Sorting files by timestamp");
		files = g_list_sort_with_data(files, _compare_files_by_time, &offset);
	}

	double nearest_time_delta = DBL_MAX;
	GList *nearest_file = NULL;

	/* Stores the list of unique file names - only used if sorting is enabled */
	GList* objUniqueFileNames = NULL;
	/* Used to detect if we hit a new element or if we found a duplicate */
	gchar* cPreviousFileName = NULL;

	g_debug("Before for loop");
	for (GList *cur = files; cur; cur = cur->next) {
		gchar *file = cur->data;

		if(g_strcmp0(file, cPreviousFileName) != 0){
			if(sort_by_time){
				/* We found a unique file - add it to the unique file list and get the pointer to the added element. */
				objUniqueFileNames = g_list_prepend(objUniqueFileNames, file);
			}

			cPreviousFileName = file;

			g_debug("RadarSite: find_nearest - in loop. Current file: %s", file);

			time_t file_time = _parse_file_time(file, offset);
			double file_time_delta = difftime(time, file_time);
			file_time_delta = ABS(file_time_delta);
			if(file_time_delta < nearest_time_delta){
				nearest_file = (sort_by_time ? objUniqueFileNames : cur); /* if sorting, return the unique element list instead of teh non-unique one */
				nearest_time_delta = file_time_delta;
			}
		} /* If we found a unique entry in the list */
	} /* for each file in the list */

	if(nearest_file != NULL){
		g_debug("RadarSite: find_nearest = %s", (gchar *)nearest_file->data);
	} else {
		g_debug("RadarSite: find_nearest = NULL (no nearest file found).");
	}
	return nearest_file;
}

static gchar *_find_nearest(time_t time, GList *files,
		gsize offset)
{
	GList* cFileNameListElement = _find_nearest_return_GList_pointer(time, files, offset, false);
	if(cFileNameListElement == NULL){
		return NULL;
	} else {
		return g_strdup(cFileNameListElement->data);
	}
}


/**************
 * RadarSites *
 **************/
typedef enum {
	STATUS_UNLOADED,
	STATUS_LOADING,
	STATUS_LOADED,
} RadarSiteStatus;

/* Types of frame modes - determines how we advance to the next frame */
typedef enum {
	NEXT_FRAME_FORWARD,
	NEXT_FRAME_BACKWARDS,
	NEXT_FRAME_UNCHANGED,
} NextFrameMode;


/* Animation details - this struct is only created when the user starts the animation. A pointer is stored in the RadarSite struct */
typedef struct {
	GThread*	objAnimationThread;	/* Stores a pointer to the GThread that executes the loading and playback of the animation loop */
	bool		lUserWantsToAnimate;	/* Stures TRUE if the user pressed the animate button to start animating or FALSE if they pressed the stop button to stop animating */
	bool		lIsAnimating;		/* Stores TRUE if we are actively animating or FALSE if we are not animating. */
	bool		lIsAnimationCleanupInProgress; /* Stores TRUE if the animation cleanup process needs to run or is running. Stores FALSE if the cleanup is done or not needed (cleaning up buttons in the UI) */
	int		iAnimationCurrentFrame;	/* Stores the current frame number that we are at in the animation process */
	int		iPreviousLevel2FrameThatWasVisible; /* Stores the previous frame that was displayed to the user so we know what we need to hide to swap to the next frame */
	bool		lAnimationLoading;	/* Stures TRUE if the animation is loading or FALSE if it is not loading */
	GtkWidget*	objAnimateButton;	/* Stores a reference to the animate button widget */
	int		iAnimationFrames;	/* Stores the number of animation frames */
	AWeatherLevel2 **aAnimationLevel2Frames; /* Stores an array of pointers to the level2 objects that contain each frame of data */
	guint		iAnimationFrameChangeIdleSource; /* Stores the callback that runs when all frames of the animation finish loading. */
	GtkWidget*	objAnimationFrameControlHbox; /* Stores a pointer to the horizontal box that contains the frame selection toggle buttons */
	GtkWidget**	aAnimationFrameSelectionToggleButtons; /* Stores an array of buttons (corresponds to aAnimationLevel2Frames) that allows the user to toggle the frames (enable them or disable them) */
	int		iAnimationFrameSelectionToggleButtonsLength; /* Stores the laength of the aFrameSelectionToggleButtons array. */
	int		iAnimationFrameLimit;	/* Stores the max number of animation frames we are allowed to load for this site */
	int		iAnimationSubframeNbr;	/* Stores the frame number inside of the curent file that we are currently animating on */
	char*		cAnimationCurrentFrameTimestampMsg; /* Stores the timestamp for the current frame in a string to be displayed to the user. This is produced by the animation thread and consumed by the UI thread */
	int		iAnimationPreviousFrameShownInUi; /* Stores the frame number that we previously showed as the current frame in the frame selection UI. This allows us to efficiently show which frame is live */
	bool*		aAnimationFrameDisabled; /* Stores an array of the frames that are enabled (false) or disabled (true) */
	GtkWidget*	objAnimateProgressBar;	/* Stores a pointer to the progress bar widget, which shows the download status */
	time_t		iAnimationStartTime;	/* Stores the time that the first frame in the animation was captured */
	time_t		iAnimationFinishTime;	/* Stores the time that the last frame in the animation was captured */
	time_t		iAnimationCurrentFrameTime; /* Stores the time that the current frame in the animation was captured */
	GArray*		aAnimationCurrentFileSortedSubframes; /* Stores the array of sorted subframes (RslSweepDateTime structs) of the current level2 frame */
	NextFrameMode	eAnimationNextFrameMode;	/* Stores which direction the user wants the animation to run in */
	bool		lIsAnimationPaused;		/* Stores true if the user paused the animation */
	GtkWidget*	objAnimationPausePlayBtn;	/* Stores a pointer to the play / pause button to temporarily pause or play the animation */
	GMutex		objBtnPressedGMutex;		/* Stores the mutex that we use to synchronize the GCond below */
	GCond		objBtnPressedGCond;		/* Stores a GCond which allows us to wake up the animation thread if the user presses a button (like changing sweep) so we can make the UI feel responsive */
	gulong		iAnimationKeyboardEventSignalHandlerEventId; /* Stores the signal handler id for the animation key event listener. */
} RadarAnimation;


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

	/* Animation data */
	RadarAnimation* objRadarAnimation; /* Pointer to the RadarAnimation struct, which contains details about the current state of the level2 animation */
};



/* Animation methods */

/* Call to poke the animation thread to wake it up from its sleep so it can respond to, for example, a change in the sweep elevation */
void _poke_animation_thread(RadarSite* site){
	g_mutex_lock(&site->objRadarAnimation->objBtnPressedGMutex);
	g_cond_signal(&site->objRadarAnimation->objBtnPressedGCond);
	g_mutex_unlock(&site->objRadarAnimation->objBtnPressedGMutex);
}

/* Internal function used in _animation_thread_usleep_or_wakeup_from_poke to translate the gpointer to a RadarSite*. */
static void fOnUiChangedCallback(gpointer _site){
	RadarSite* site = _site;
	/* User clicked a different sweep elevation or volume button or changed the isosurface slider. Tell the animation thread about this */
	_poke_animation_thread(site);
}

/* Call from the animation thread to sleep for the specified amount of time (microseconds).
 * If the _poke_animation_thread function is called above during the sleep, this function will return early and will return true.
 * If the timeout expires normally (not poked), then false is returned.
 */
bool _animation_thread_usleep_or_wakeup_from_poke(RadarSite* site, gint64 ipiMicrosecondsToSleep){
	/* Setup a callback so if the user clicks a button in the UI to change the sweep elevation or volume or changes the isosurface slider, we will automatically wake up the animation thread to apply the change. */
	site->level2->objAfterSetSweepOneTimeCustomCallbackData = (gpointer) site;
	site->level2->fAfterSetSweepOneTimeCustomCallback = fOnUiChangedCallback;
	site->level2->objOnSetIsoOneTimeCustomCallbackData = (gpointer) site;;
	site->level2->fOnSetIsoOneTimeCustomCallback = fOnUiChangedCallback;

	g_mutex_lock(&site->objRadarAnimation->objBtnPressedGMutex);

	/* Calculate when we should wake up */
	gint64 iEndTime = g_get_monotonic_time() + ipiMicrosecondsToSleep;
	bool lReturnValue = g_cond_wait_until(&site->objRadarAnimation->objBtnPressedGCond, &site->objRadarAnimation->objBtnPressedGMutex, iEndTime);
	
	g_mutex_unlock(&site->objRadarAnimation->objBtnPressedGMutex);

	/* Clear out the callback function so we don't try to run it when it is not needed. */
	site->level2->objAfterSetSweepOneTimeCustomCallbackData = NULL;
	site->level2->fAfterSetSweepOneTimeCustomCallback = NULL;
	site->level2->objOnSetIsoOneTimeCustomCallbackData = NULL;
	site->level2->fOnSetIsoOneTimeCustomCallback = NULL;

	return lReturnValue;
}

/* Called when the user clicks a toggle button to tenable or disable a frame */
static void _on_animation_frame_selection_toggle_button_toggle(GtkToggleButton *toggleButton, gpointer _site){
	RadarSite* site = _site;
	/* Get the stored frame ID back out of the button so we know which one was clicked */
	gint iFrameId = (glong)g_object_get_data(G_OBJECT(toggleButton), "iFrameId");

	site->objRadarAnimation->aAnimationFrameDisabled[iFrameId] = gtk_toggle_button_get_active(toggleButton);

	/* Update the button text to visually show the frame is disabled or not */
	if(site->objRadarAnimation->aAnimationFrameDisabled[iFrameId]){
		gtk_button_set_label(GTK_BUTTON(toggleButton), "X");
	} else {
		gtk_button_set_label(GTK_BUTTON(toggleButton), "");
	}
}

void _pause_animation(RadarSite* site){
	site->objRadarAnimation->lIsAnimationPaused = true;
	gtk_button_set_label(GTK_BUTTON(site->objRadarAnimation->objAnimationPausePlayBtn), "\u23f5"); /* Play button icon */
}

void _unpause_animation(RadarSite* site){
	site->objRadarAnimation->lIsAnimationPaused = false;
	gtk_button_set_label(GTK_BUTTON(site->objRadarAnimation->objAnimationPausePlayBtn), "\u23f8"); /* Pause button icon */
}

void _on_previous_frame_btn_clicked(GtkButton* ipobjButton, RadarSite* site){
	_pause_animation(site);
	site->objRadarAnimation->eAnimationNextFrameMode = NEXT_FRAME_BACKWARDS;
	_poke_animation_thread(site); /* Wake the animation thread up so it animates right away */
}

void _on_next_frame_btn_clicked(GtkButton* ipobjButton, RadarSite* site){
	_pause_animation(site);
	site->objRadarAnimation->eAnimationNextFrameMode = NEXT_FRAME_FORWARD;
	_poke_animation_thread(site); /* Wake the animation thread up so it animates right away */
}

void _on_pause_play_frame_btn_clicked(GtkButton* ipobjButton, RadarSite* site){
	if(site->objRadarAnimation->lIsAnimationPaused){
		_unpause_animation(site);
		site->objRadarAnimation->eAnimationNextFrameMode = NEXT_FRAME_FORWARD; /* Play the animation normally */
		_poke_animation_thread(site); /* restart playback right now instead of waiting for the animation thread to wake up. */
	} else {
		_pause_animation(site);
		site->objRadarAnimation->eAnimationNextFrameMode = NEXT_FRAME_UNCHANGED;
	}
}

gboolean _on_aweather_gui_key_press(GtkWidget *ipobjWidget, GdkEventKey *ipobjEvent, RadarSite *site){
	g_debug("radar.c _on_aweather_gui_key_press. key: %x, state: %x", ipobjEvent->keyval, ipobjEvent->state);
	/* Simulate pressing buttons when the user presses specific keys */
	switch(ipobjEvent->keyval){
		case GDK_KEY_period:
			_on_next_frame_btn_clicked(NULL, site);
			break;
		case GDK_KEY_comma:
			_on_previous_frame_btn_clicked(NULL, site);
			break;
		case GDK_KEY_slash:
			_on_pause_play_frame_btn_clicked(GTK_BUTTON(site->objRadarAnimation->objAnimationPausePlayBtn), site);
			break;
	}

	return false;
}


/* Attempts to locate the GtkWindow that contains this application. This can be used to attach event listeners to */
GtkWindow* _get_main_window_from_widget(GtkWidget *ipobjWidget){
	GtkWidget *objParent = ipobjWidget;

	while(objParent != NULL && !GTK_IS_WINDOW(objParent)){
		objParent = gtk_widget_get_parent(objParent);
	}
	return GTK_WINDOW(objParent);
}

/* Adds a keyboard event listener to the main window so we can add keyboard shortcuts for the animation */
void _setup_animation_keyboard_event_listeners(RadarSite* site){
	GtkWindow *objWindow = _get_main_window_from_widget(site->config);
	if(!objWindow){
		/* Log the error message and terminate the program */
		g_error("radar.c _setup_animation_keyboard_event_listeners failed. We were unable to locate the parent window to add a key press listener to. Keyboard shortcuts will not work.");
	}

	site->objRadarAnimation->iAnimationKeyboardEventSignalHandlerEventId = g_signal_connect(objWindow, "key-press-event", G_CALLBACK(_on_aweather_gui_key_press), site);
}

/* Removes the keyboard event listener from the main window. Call when the animation is finished */
void _remove_animation_keybaord_event_listeners(RadarSite* site){
	if(site->objRadarAnimation->iAnimationKeyboardEventSignalHandlerEventId != 0){
		GtkWindow *objWindow = _get_main_window_from_widget(site->config);
		if(!objWindow){
			/* Log the error message and terminate the program */
			g_error("radar.c _remove_animation_keybaord_event_listeners failed. We were unable to locate the parent window to add a key press listener to. Keyboard shortcuts will not be removed.");
		}

		g_signal_handler_disconnect(objWindow, site->objRadarAnimation->iAnimationKeyboardEventSignalHandlerEventId);
		site->objRadarAnimation->iAnimationKeyboardEventSignalHandlerEventId = 0;
	}
}

/* Sync animation UI with backend state. file, cur, total are optional parameters for showing the download status of files.
 * This must run on the main UI thread.
  */
void _animation_update_status_ui(gchar *file, goffset cur,
		goffset total, gpointer _site)
{
	RadarSite* site = _site;
	RadarAnimation* objRadarAnimation = site->objRadarAnimation;

	if(!objRadarAnimation->lIsAnimating){
		/* If we are not animating right now, then do not run the logic in this function. The logic in here requires specific UI elements
		 * to exist, which may not exist if the animation is not running.
		 * This could be called after the animation stops if there were many scheduled UI thread callbacks waiting to run.
		 */
		g_debug("_animation_update_status_ui: This function was called when no animation was running. Exiting the function now.");
		return;
	}

	/* Show toggle buttons to reflect the frames that are loaded.
	 * Start at the length of the frame selection buttons array and add any that are missing (iterate to the current number of loaded frames).
	 */
	g_debug("_animation_update_status_ui: objRadarAnimation->iAnimationFrameSelectionToggleButtonsLength: %i, objRadarAnimation->iAnimationFrames: %i, cur: %li, total: %li, objRadarAnimation->iAnimationCurrentFrame: %i", objRadarAnimation->iAnimationFrameSelectionToggleButtonsLength, objRadarAnimation->iAnimationFrames, cur, total, objRadarAnimation->iAnimationCurrentFrame);
	if(objRadarAnimation->iAnimationFrameSelectionToggleButtonsLength < objRadarAnimation->iAnimationFrames
		&& objRadarAnimation->iAnimationFrameSelectionToggleButtonsLength == 0){

		/* If we haven't added any frame control buttons to the UI yet, then add the frame selection message to the UI to describe the frame selection boxes. */
		GtkWidget* objFrameSelectionLabel = gtk_label_new("<b>Frame selection:</b>");
		gtk_label_set_use_markup(GTK_LABEL(objFrameSelectionLabel), true);
		gtk_widget_show(objFrameSelectionLabel);
		gtk_box_pack_start(GTK_BOX(objRadarAnimation->objAnimationFrameControlHbox), objFrameSelectionLabel, FALSE, FALSE, 10);

		/* Add the left, right arrows to the frame selection area and add a play / pause button */
		GtkWidget* objBtnPreviousFrame = gtk_button_new_with_label("<");
		gtk_widget_set_tooltip_text(objBtnPreviousFrame, "Jump to previous animation frame (,)");
		gtk_widget_set_size_request(objBtnPreviousFrame, 30, 30);
		g_signal_connect(objBtnPreviousFrame, "clicked", G_CALLBACK(_on_previous_frame_btn_clicked), site);
		gtk_widget_show(objBtnPreviousFrame);
		gtk_box_pack_start(GTK_BOX(objRadarAnimation->objAnimationFrameControlHbox), objBtnPreviousFrame, FALSE, FALSE, 0);

		GtkWidget* objBtnNextFrame = gtk_button_new_with_label(">");
		gtk_widget_set_tooltip_text(objBtnNextFrame, "Jump to next animation frame (.)");
		gtk_widget_set_size_request(objBtnNextFrame, 30, 30);
		g_signal_connect(objBtnNextFrame, "clicked", G_CALLBACK(_on_next_frame_btn_clicked), site);
		gtk_widget_show(objBtnNextFrame);
		gtk_box_pack_start(GTK_BOX(objRadarAnimation->objAnimationFrameControlHbox), objBtnNextFrame, FALSE, FALSE, 0);

		objRadarAnimation->objAnimationPausePlayBtn = gtk_button_new_with_label("");
		gtk_widget_set_tooltip_text(objRadarAnimation->objAnimationPausePlayBtn, "Play / pause the animation (/)");
		gtk_widget_set_size_request(objRadarAnimation->objAnimationPausePlayBtn, 30, 30);
		g_signal_connect(objRadarAnimation->objAnimationPausePlayBtn, "clicked", G_CALLBACK(_on_pause_play_frame_btn_clicked), site);
		gtk_box_pack_start(GTK_BOX(objRadarAnimation->objAnimationFrameControlHbox), objRadarAnimation->objAnimationPausePlayBtn, FALSE, FALSE, 0);
		gtk_widget_show(objRadarAnimation->objAnimationPausePlayBtn);
		_unpause_animation(site); /* Update the button text */
	}

	for(int iFrame = objRadarAnimation->iAnimationFrameSelectionToggleButtonsLength; iFrame < objRadarAnimation->iAnimationFrames; ++iFrame){
		/* Missing frame selection toggle button. Add it to the left side of the hbox, shift other boxes right. */
		objRadarAnimation->aAnimationFrameSelectionToggleButtons[iFrame] = gtk_toggle_button_new_with_label("");
		gtk_box_pack_start(GTK_BOX(objRadarAnimation->objAnimationFrameControlHbox), objRadarAnimation->aAnimationFrameSelectionToggleButtons[iFrame], FALSE, FALSE, 0);
		
		/* Move the button to the current front of the list. We load frames backwards, so we have to draw the UI backwards. Add button to the right of the '<' button. */
		gtk_box_reorder_child(GTK_BOX(objRadarAnimation->objAnimationFrameControlHbox), objRadarAnimation->aAnimationFrameSelectionToggleButtons[iFrame], 2);

		/* Set a fixed size for the button so they don't change size as the text in them changes */
		gtk_widget_set_size_request(objRadarAnimation->aAnimationFrameSelectionToggleButtons[iFrame], 30, 30);

		/* Make the toggle button visible */
		gtk_widget_show(objRadarAnimation->aAnimationFrameSelectionToggleButtons[iFrame]);
		
		/* Store the frame id in the button so when the button is toggled, we know which frame to enable or disable */
		g_object_set_data(G_OBJECT(objRadarAnimation->aAnimationFrameSelectionToggleButtons[iFrame]), "iFrameId", (gpointer)(guintptr) iFrame);

		/* Listen for the user to toggle this button */
		g_signal_connect(objRadarAnimation->aAnimationFrameSelectionToggleButtons[iFrame], "toggled", G_CALLBACK(_on_animation_frame_selection_toggle_button_toggle), site);

		objRadarAnimation->iAnimationFrameSelectionToggleButtonsLength++; /* Increment the number of buttons shown */
	} /* For each loaded frame */

	if(objRadarAnimation->lAnimationLoading){
		double dPercent = 0;
		if(file != NULL){
			/* If wa are active loading a file, display the file download status in the animate button */
			double dCurrentFrameDownloadPercent = (total == 0 ? 1 : ((double)(cur) / total) ); /* If the server told us the current file is zero bytes, then assume we downloaded the entire file already (set percent to 100). */
			dPercent = (objRadarAnimation->iAnimationFrames + dCurrentFrameDownloadPercent) / objRadarAnimation->iAnimationFrameLimit;
		} else {
			/* If we are not actively loading a file at the moment, show a general progress indicator */
			dPercent = (double) objRadarAnimation->iAnimationFrames / objRadarAnimation->iAnimationFrameLimit;
		}

		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(objRadarAnimation->objAnimateProgressBar), CLAMP(dPercent, 0, 1)); /* Gtk will throw an error if the percent is < 0 or > 1, hence we use CLAMP. */
		gchar *msg = g_strdup_printf("Loading %3.0f%%", (dPercent != dPercent /* if dPercent == NaN */ ? 0 : dPercent * 100));
		gtk_progress_bar_set_text(GTK_PROGRESS_BAR(objRadarAnimation->objAnimateProgressBar), msg);
		g_free(msg);
	} else if(objRadarAnimation->lIsAnimationCleanupInProgress){
		/* The animation has stopped. Update the button text */
		gtk_button_set_label(GTK_BUTTON(objRadarAnimation->objAnimateButton), "Animate");

		/* Clear out the progress bar */
		gtk_progress_bar_set_text(GTK_PROGRESS_BAR(objRadarAnimation->objAnimateProgressBar), "Stopped");
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(objRadarAnimation->objAnimateProgressBar), 0);
		
		/* If we need to run cleanup, then cleanup the UI and set the animation done flag */

		/* Cleanup the event listeners */
		_remove_animation_keybaord_event_listeners(site);

		/* Get all widgets in the frame control hbox and delete them */
		GList* objChildrenList = gtk_container_get_children(GTK_CONTAINER(objRadarAnimation->objAnimationFrameControlHbox));
		for(GList* objChild = objChildrenList; objChild != NULL; objChild = g_list_next(objChild)){
			gtk_widget_destroy(GTK_WIDGET(objChild->data));
		}
		g_list_free(objChildrenList);

		/* The buttons have been removed from the UI. The array can no be deleted */
		objRadarAnimation->iAnimationFrameSelectionToggleButtonsLength = 0;
		g_free(objRadarAnimation->aAnimationFrameSelectionToggleButtons);
		objRadarAnimation->aAnimationFrameSelectionToggleButtons = NULL;

		/* Cleanup the disabled frames array */
		g_free(objRadarAnimation->aAnimationFrameDisabled);
		objRadarAnimation->aAnimationFrameDisabled = NULL;

		/* Clear the GMutex and GCond as we are done with them */
		g_mutex_clear(&objRadarAnimation->objBtnPressedGMutex);
		g_cond_clear(&objRadarAnimation->objBtnPressedGCond);
		
		/* We are done animating. Flip the animating flag to false */
		objRadarAnimation->lIsAnimationCleanupInProgress = false;
		objRadarAnimation->lIsAnimating = false;
	} else if(objRadarAnimation->lUserWantsToAnimate){
		/* Set the progress bar to show the progress through the animation loop */
		gtk_progress_bar_set_text(GTK_PROGRESS_BAR(objRadarAnimation->objAnimateProgressBar), (objRadarAnimation->lIsAnimationPaused ? "Paused" : "Running"));
		double dRunningPercentage = 0;
		if(objRadarAnimation->iAnimationStartTime != -1
			&& objRadarAnimation->iAnimationFinishTime != -1
			&& objRadarAnimation->iAnimationCurrentFrameTime != -1){
			/* If we can, calculate the percentage of the way through the animation based on the oldest and newest frames in the animation. */
			dRunningPercentage = (double)(objRadarAnimation->iAnimationCurrentFrameTime - objRadarAnimation->iAnimationStartTime) / (objRadarAnimation->iAnimationFinishTime - objRadarAnimation->iAnimationStartTime);
		} else {
			dRunningPercentage = 1 - ((double) objRadarAnimation->iAnimationCurrentFrame / objRadarAnimation->iAnimationFrames);
		}
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(objRadarAnimation->objAnimateProgressBar), CLAMP(dRunningPercentage, 0, 1));

		gtk_button_set_label(GTK_BUTTON(objRadarAnimation->objAnimateButton), "Stop");
		
		/* If we changed frames, then update the button text */
		/* If the current frame is not disabled, then update the button text */
		if(objRadarAnimation->aAnimationFrameDisabled != NULL){ /* This shouldn't be NULL normally, but could be right before the animation starts loading */
			/* If the frame is not disabled, then update the button text */
			if(!objRadarAnimation->aAnimationFrameDisabled[objRadarAnimation->iAnimationPreviousFrameShownInUi])
				gtk_button_set_label(GTK_BUTTON(objRadarAnimation->aAnimationFrameSelectionToggleButtons[objRadarAnimation->iAnimationPreviousFrameShownInUi]), "");

			if(!objRadarAnimation->aAnimationFrameDisabled[objRadarAnimation->iAnimationCurrentFrame]){
				gchar* cBtnLabel = g_strdup_printf("%i", objRadarAnimation->iAnimationSubframeNbr + 1);
				gtk_button_set_label(GTK_BUTTON(objRadarAnimation->aAnimationFrameSelectionToggleButtons[objRadarAnimation->iAnimationCurrentFrame]), cBtnLabel);
				g_free(cBtnLabel);
			}
		}

		/* Update the previous frame number so we know which button to update when we move on to the next frame */
		objRadarAnimation->iAnimationPreviousFrameShownInUi = objRadarAnimation->iAnimationCurrentFrame;
	}

	/* Update the frame timestamp display if we have a frame timestamp to display and the label exists. */
	g_debug("_animation_update_status_ui: objRadarAnimation->cAnimationCurrentFrameTimestampMsg: %s, site->level2->date_label: %p", objRadarAnimation->cAnimationCurrentFrameTimestampMsg, site->level2->date_label);
	if(objRadarAnimation->cAnimationCurrentFrameTimestampMsg != NULL
		&& site->level2->date_label != NULL){
		gtk_label_set_markup(GTK_LABEL(site->level2->date_label), objRadarAnimation->cAnimationCurrentFrameTimestampMsg);


		/* Cleanup the timestamp string so it can be regenerated by the animation thread */
		g_free(objRadarAnimation->cAnimationCurrentFrameTimestampMsg);
		objRadarAnimation->cAnimationCurrentFrameTimestampMsg = NULL;
	} /* if there is an animation timestamp string to consumer */
}

/* Calls _animation_update_status_ui, but only requires a site pointer so it can be called from g_idle_add() */
gboolean _animation_update_status_ui_g_idle_add(gpointer _site){
	RadarSite* site = _site;
	_animation_update_status_ui(NULL, 0, 0, site);
	site->objRadarAnimation->iAnimationFrameChangeIdleSource = 0;
	return false;
}


/* Advances the animation to the next frame. Returns true if we hit the end of the loop or false if we are still in the middle of the loop */
bool _animation_goto_next_frame(RadarSite* site, NextFrameMode ipeNextFrameMode){
	RadarAnimation* objRadarAnimation = site->objRadarAnimation;

	/* Stores 1 if we are going forward in the animation loop or -1 if we are going backwards */
	int iAnimationFrameIncrementer = (ipeNextFrameMode == NEXT_FRAME_FORWARD ? 1 : -1);

	/* Stores the return value for this function */
	bool lDidWeHitTheEndOfTheAnimationLoop = false;

	objRadarAnimation->iAnimationSubframeNbr += iAnimationFrameIncrementer;

	if(objRadarAnimation->aAnimationCurrentFileSortedSubframes != NULL
		&& (!aweatherLevel2AreTheseElevationsTheSame(objRadarAnimation->aAnimationLevel2Frames[objRadarAnimation->iAnimationCurrentFrame]->dSelectedElevation, site->level2->dSelectedElevation) /* If the user changed the elevation */
			|| objRadarAnimation->aAnimationLevel2Frames[objRadarAnimation->iAnimationCurrentFrame]->iSelectedVolumeId != site->level2->iSelectedVolumeId) /* If the user changed the volume ("type") */
	){
		/* If the current subframe array is pointing to an outdated elevation or volume id, then force the regeneration of the subframe array.
		 * On 'normal' nexrad files, this won't do much as each file typically only contains one one scan for each volume at each elevation.
		 * However, some files contain multiple sweeps at the same elevation at different times. Hence, if the user changes the selected volume, or elevation
		 * we need to regenerate this array for the current frame so we don't keep showing the old elevation or volume for more frames.
		 */
		g_array_free(objRadarAnimation->aAnimationCurrentFileSortedSubframes, true);
		objRadarAnimation->aAnimationCurrentFileSortedSubframes = NULL;
	}

	/* Determine which subframe of the current file ee want to display */
	while(objRadarAnimation->aAnimationCurrentFileSortedSubframes == NULL
	  || objRadarAnimation->iAnimationSubframeNbr >= objRadarAnimation->aAnimationCurrentFileSortedSubframes->len /* If we went off the end of the array */
	  || objRadarAnimation->iAnimationSubframeNbr < 0 /* If we went off the front of the array */
	){
		/* We have used up all frames of the current level2 file - move
		 * on to the next file in the list.
		 * Start off at the last frame in the loop (oldest), then work down from there to 0 (newest frame).
		 * If the frame is disabled, then skip it. If we searched all frames and they are all disabled, then stay on the current frame.
		 */
		int iFrameSearchAttempts = 0;
		while((objRadarAnimation->aAnimationFrameDisabled[objRadarAnimation->iAnimationCurrentFrame]
			&& iFrameSearchAttempts < objRadarAnimation->iAnimationFrames)
			|| iFrameSearchAttempts == 0){ /* Force the loop to run at least once */
			objRadarAnimation->iAnimationCurrentFrame -= iAnimationFrameIncrementer;
			if(objRadarAnimation->iAnimationCurrentFrame < 0
				|| objRadarAnimation->iAnimationCurrentFrame >= objRadarAnimation->iAnimationFrames
			){
				objRadarAnimation->iAnimationCurrentFrame = (ipeNextFrameMode == NEXT_FRAME_FORWARD ? objRadarAnimation->iAnimationFrames - 1 : 0);

				/* End of loop hit. Return true */
				lDidWeHitTheEndOfTheAnimationLoop = true;
			}

			/* Increment the counter so we know if we got stuck in this loop (all frames disabled */
			++iFrameSearchAttempts;
		} /* While the level1 frame we are on is not disabled */

		/* Cleanup the old array if we are computing a new one */
		if(objRadarAnimation->aAnimationCurrentFileSortedSubframes != NULL) g_array_free(objRadarAnimation->aAnimationCurrentFileSortedSubframes, true);
		objRadarAnimation->aAnimationCurrentFileSortedSubframes = aweatherLevel2GetAllSweepsFromVolumeWithElevationSortedBySweepStartTime(objRadarAnimation->aAnimationLevel2Frames[objRadarAnimation->iAnimationCurrentFrame], site->level2->iSelectedVolumeId, site->level2->dSelectedElevation);
		objRadarAnimation->iAnimationSubframeNbr = (ipeNextFrameMode == NEXT_FRAME_FORWARD ? 0 : objRadarAnimation->aAnimationCurrentFileSortedSubframes->len - 1);

	}

	return lDidWeHitTheEndOfTheAnimationLoop;
}

/* Internal function used in _animation_update_thread to switch to the next frame. This could be run from the UI thread or animation thread. */
static void switchToNextFrame(gpointer _ipobjRadarAnimation){
	RadarAnimation* objRadarAnimation = (RadarAnimation*) _ipobjRadarAnimation;

	/* Hide the current frame */
	grits_object_hide(GRITS_OBJECT(objRadarAnimation->aAnimationLevel2Frames[objRadarAnimation->iPreviousLevel2FrameThatWasVisible]), true);

	/* After changing the frame number, show the current frame */
	grits_object_hide(GRITS_OBJECT(objRadarAnimation->aAnimationLevel2Frames[objRadarAnimation->iAnimationCurrentFrame]), false);

	/* Update the previous frame counter so we know which one to hide next */
	objRadarAnimation->iPreviousLevel2FrameThatWasVisible = objRadarAnimation->iAnimationCurrentFrame;
}

gpointer _animation_update_thread(gpointer _site)
{
	RadarSite* site = _site;
	RadarAnimation* objRadarAnimation = site->objRadarAnimation;

	g_debug("_animation_update_thread - %s", site->city->code);
	
	/* Set the loading flag to indicate that we are loading */
	objRadarAnimation->lAnimationLoading = true;
	
	/* Initialize computed animation start and finish times */
	objRadarAnimation->iAnimationStartTime = -1;
	objRadarAnimation->iAnimationFinishTime = -1;
	objRadarAnimation->iAnimationCurrentFrameTime = -1;

	/* Update the UI to show that we are loading the animation. */
	objRadarAnimation->iAnimationFrameChangeIdleSource = g_idle_add(_animation_update_status_ui_g_idle_add, site);

	gboolean offline = grits_viewer_get_offline(site->viewer);
	gchar *nexrad_url = grits_prefs_get_string(site->prefs,
			"aweather/nexrad_url", NULL);

	/* Find nearest volume (temporally) */
	g_debug("_animation_update_thread - find nearest - %s", site->city->code);
	gchar *dir_list = g_strconcat(nexrad_url, "/", site->city->code,
			"/", "dir.list", NULL);
	GList *files = grits_http_available(site->http,
			"^\\w{4}_\\d{8}_\\d{6}.bz2$", site->city->code,
			"\\d+ (.*)", (offline ? NULL : dir_list));
	g_free(dir_list);

	int iAnimationFrameIntervalMs = 0;
	int iAnimationEndFrameHoldMs = 0;
	/* Call to update the iAnimationFrameIntervalMs and iAnimationEndFrameHoldMs variables from the current user preferences object. */
	void updateIAnimationFrameIntervalMs(){
		iAnimationFrameIntervalMs = grits_prefs_get_integer(site->prefs, "aweather/animation_frame_interval_ms", NULL);
		iAnimationEndFrameHoldMs = grits_prefs_get_integer(site->prefs, "aweather/animation_end_frame_hold_ms", NULL);
	}
	/* Sync current user preference for animation frame interval ms setting to our local variable */
	updateIAnimationFrameIntervalMs();

	/* Copy the configured animation frames limit to our local state variable so that if the user changes it while we are loading, we don't go over an array limit */
	objRadarAnimation->iAnimationFrameLimit = grits_prefs_get_integer(site->prefs, "aweather/animation_frames", NULL);
	if(objRadarAnimation->iAnimationFrameLimit == 0){
		g_error("Warning! The animation frame count is set to 0. This is not supported. Please adjust the 'Animation Frames' setting to a larger value in the settings dialog.");
	}

	/* Allocate memory for the list of Level2 radar objects for this site. We may not fill this array fully, but we could use all of it depending on the files available on the server. */
	objRadarAnimation->aAnimationLevel2Frames = malloc(sizeof(AWeatherLevel2*) * objRadarAnimation->iAnimationFrameLimit);
	objRadarAnimation->iAnimationFrames = 0;
	g_debug("_animation_update_thread: iAnimationFrames set to 0");

	/* Ensure the frame selection buttons array is initialized */
	objRadarAnimation->aAnimationFrameSelectionToggleButtons = malloc(sizeof(GtkWidget*) * objRadarAnimation->iAnimationFrameLimit);
	objRadarAnimation->iAnimationFrameSelectionToggleButtonsLength = 0;

	objRadarAnimation->aAnimationFrameDisabled = calloc(sizeof(bool), objRadarAnimation->iAnimationFrameLimit);

	/* Reset the cleanup flag so it is in a known state */
	objRadarAnimation->lIsAnimationCleanupInProgress = false;

	/* Clear out the previous frame id */
	objRadarAnimation->iAnimationPreviousFrameShownInUi = 0;

	/* Star the animation in the forwards direction (when it finishes loading) */
	objRadarAnimation->eAnimationNextFrameMode = NEXT_FRAME_FORWARD;

	GList* objFilesListByTimeDesc = _find_nearest_return_GList_pointer(site->time, files, 5, true /* Sort the array so it is in a consistent order */);

	/* We want the file name that is closest to the current set time and the previous N files.
	 * Hence, we request the list element itself, then iterate forwards in the double-linked list to find the desired file names that are older than the starting file to build up our animation.
	 */
	for(GList *nearest = objFilesListByTimeDesc; objRadarAnimation->iAnimationFrames < objRadarAnimation->iAnimationFrameLimit && nearest != NULL; nearest = nearest->next){
		g_debug("_animation_update_thread: About to fetch frame, prev: %p, curr: '%s', next: %p", nearest->prev, (char*) nearest->data, nearest->next);
		/* Fetch new volume for the current file. */
		gchar *local = g_strconcat(site->city->code, "/", nearest->data, NULL);
		gchar *uri   = g_strconcat(nexrad_url, "/", local,   NULL);
		g_debug("_animation_update_thread: downloading from URI %s", uri);
		gchar *file  = grits_http_fetch(site->http, uri, local,
				offline ? GRITS_LOCAL : GRITS_UPDATE,
				_animation_update_status_ui, site);
		g_free(local);
		g_free(uri);
		if (file) {
			/* Load and add new volume to our array of level2 frames. Increment the frames counter so we know how many frames we have. */
			g_debug("_animation_update_thread - File is good. load - Site: %s, Frame number: %i", site->city->code, objRadarAnimation->iAnimationFrames);
			AWeatherLevel2* objLevel2 = aweather_level2_new_from_file(file, site->city->code, colormaps, site->prefs);
			g_debug("_animation_update_thread: parsing level2: %p", objLevel2);
			objRadarAnimation->aAnimationLevel2Frames[objRadarAnimation->iAnimationFrames] = objLevel2;

			if (!objLevel2) {
				g_debug("_animation_update_thread. We failed to load a level2 file. Skipping it. File: %s", file);
			} else {
				grits_object_hide(GRITS_OBJECT(objLevel2), true);
				g_debug("_animation_update_thread: After hide of frame.");
				grits_viewer_add(site->viewer, GRITS_OBJECT(objLevel2), GRITS_LEVEL_WORLD+3, TRUE);
				g_debug("_animation_update_thread: After add to viewer.");

				/* Frame successfully added. Increment the num frames counter */
				objRadarAnimation->iAnimationFrames++;
			}
			g_free(file);
		} /* If a file was returned from the server. */

		/* If there is not already a main thread request ongoing, then create a new request to update the GUI with the latest file. */
		if(!objRadarAnimation->iAnimationFrameChangeIdleSource){
			objRadarAnimation->iAnimationFrameChangeIdleSource = g_idle_add(_animation_update_status_ui_g_idle_add, site);
		}
	} /* for each file on the server starting at the selected time, walking backwards */
	g_debug("_animation_update_thread: Done loading level2 frames");

	/* Cleanup */
	g_free(nexrad_url);

	/* Cleanup the sorted list - contained strings are pointers to 'files' strings and don't need to be deleted. */
	g_list_free(objFilesListByTimeDesc);

	/* Cleanup the list */
	g_list_foreach(files, (GFunc)g_free, NULL);
	g_list_free(files);


	/* Hide the existing level2 radar scan so our animation frame is the only thing currently visible */
	grits_object_hide(GRITS_OBJECT(site->level2), true);
	
	/* Set the loading flag to indicate that we are done loading */
	objRadarAnimation->lAnimationLoading = false;
	objRadarAnimation->iAnimationCurrentFrame = 0;
	objRadarAnimation->iAnimationSubframeNbr = 0;

	/* Reset the prev frame store, as this stores the Level2 file the user was previously looking at, allowing us to hide it to show the next frame */
	objRadarAnimation->iPreviousLevel2FrameThatWasVisible = 0;

	/* Stores the times that the current loop of the animation starts and finishes at before we commit these values to the GUI */
	time_t iPreliminaryAnimationStartTime = -1;
	time_t iPreliminaryAnimationFinishTime = -1;

	/* Stores false unless the GUI thread pokes us while we are sleeping so we know to update which volume / sweep is displayed. */
	bool lDidWeGetPockedWhileSleeping = false;

	/* Loop through the frames until we are told to stop animating */
	while(objRadarAnimation->lUserWantsToAnimate){
		if(objRadarAnimation->lIsAnimationPaused){
			if(objRadarAnimation->eAnimationNextFrameMode != NEXT_FRAME_UNCHANGED)
				/* If the animation is paused and the user requested us to advance to the next frame, then advance one frame (or go back one frame) and clear the frame 'needs to advance flag'. */
				_animation_goto_next_frame(site, objRadarAnimation->eAnimationNextFrameMode);
			else {
				/* If no frame change is needed, then advance by one frame and go back by one frame to ensure we update the selected volume and elevation of the animation on the current frame */
				_animation_goto_next_frame(site, NEXT_FRAME_FORWARD);
				_animation_goto_next_frame(site, NEXT_FRAME_BACKWARDS);
			}
			objRadarAnimation->eAnimationNextFrameMode = NEXT_FRAME_UNCHANGED;
		} else {
			if(lDidWeGetPockedWhileSleeping){
				/* We got poked between frames - do not advance to the next frame automatically. Instead, switch to the next frame, then go backwards to refresh what is displayed so we
				 * keep the displayed sweep / volume in sync with what the user selected.
				 */
				_animation_goto_next_frame(site, NEXT_FRAME_FORWARD);
				_animation_goto_next_frame(site, NEXT_FRAME_BACKWARDS);
			} else if(_animation_goto_next_frame(site, objRadarAnimation->eAnimationNextFrameMode)){
				/* We hit the end of the loop. Run end of loop logic here */

				/* If we are starting over, then add an extra delay to let the user know we hit the end of the loop */
				if(!lDidWeGetPockedWhileSleeping){
					/* Sync current user preference for animation frame interval ms setting to our local variable when the animation hits the end frame. */
					updateIAnimationFrameIntervalMs();

					/* As long as we weren't poked during our frame transition sleep, then sleep some more for the end of loop delay */
					lDidWeGetPockedWhileSleeping = _animation_thread_usleep_or_wakeup_from_poke(site, iAnimationEndFrameHoldMs * 1000);
				}

				/* Loop finished - commit the start and finish times of this loop to the UI and clear out the variables so we can calculate them again in the next loop */
				objRadarAnimation->iAnimationStartTime = iPreliminaryAnimationStartTime;
				objRadarAnimation->iAnimationFinishTime = iPreliminaryAnimationFinishTime;
				iPreliminaryAnimationStartTime = -1;
				iPreliminaryAnimationFinishTime = -1;
			} /* If we hit the end of the loop */
		} /* If the animation is not paused */


		/* Switch to the correct sweep of the current level2 file if needed. */
		RslSweepDateTime objCurrentSweep = g_array_index(objRadarAnimation->aAnimationCurrentFileSortedSubframes, RslSweepDateTime, objRadarAnimation->iAnimationSubframeNbr);
		AWeatherLevel2* objCurrentLevel2 = objRadarAnimation->aAnimationLevel2Frames[objRadarAnimation->iAnimationCurrentFrame];

		/* If the sweep or volume needs to be changed before displaying this frame, then change it */
		if(objCurrentLevel2->iSelectedVolumeId != objCurrentSweep.iVolumeId
			|| objCurrentLevel2->iSelectedSweepId != objCurrentSweep.iSweepId){
			/* The sweep is changed asynchronously. Thus, we must run the hide / show of the next frame logic after that sweep swap is done by using the callback */
			objCurrentLevel2->objAfterSetSweepOneTimeCustomCallbackData = (gpointer) objRadarAnimation;
			objCurrentLevel2->fAfterSetSweepOneTimeCustomCallback = switchToNextFrame;
			aweather_level2_set_sweep(objCurrentLevel2, objCurrentSweep.iVolumeId, objCurrentSweep.iSweepId);
		} else {
			/* No sweep change is needed. Just swap the frames directly */
			switchToNextFrame((gpointer) objRadarAnimation);
		}

		/* If we need to set the iso on this volume (not in sync with the current site), then set it. This allows us to animate the 3D radar.
		 * If the volume is not defined on the level2 object, then this means that the iso has not yet been set.
		 */
		g_debug("_animation_update_thread: Checking if we should set level. objCurrentLevel2->volume: %p, site->level2->volume: %p", objCurrentLevel2->volume, site->level2->volume);
		if(site->level2->volume != NULL
			&& (objCurrentLevel2->volume == NULL || objCurrentLevel2->volume->level != site->level2->volume->level)){
			g_debug("_animation_update_thread: Setting level iso level to %f", site->level2->volume->level);
			aweather_level2_set_iso(objCurrentLevel2, site->level2->volume->level, false /* generate isosurface synchronously */);
		}

		/* If the timestamp string was consumed by the UI thread, then produce a new timestamp message for the UI */
		if(objRadarAnimation->cAnimationCurrentFrameTimestampMsg == NULL){
			objRadarAnimation->cAnimationCurrentFrameTimestampMsg = formatSweepStartAndEndTimeForDisplay(&objCurrentSweep.startDateTime, &objCurrentSweep.finishDateTime);
			g_debug("_animation_update_thread: Updating animation frame timestamp msg: %s, objRadarAnimation->iAnimationSubframeNbr: %i", objRadarAnimation->cAnimationCurrentFrameTimestampMsg, objRadarAnimation->iAnimationSubframeNbr);
		}

		objRadarAnimation->iAnimationCurrentFrameTime = getTimeTFromRslDateTime(&objCurrentSweep.startDateTime);
		iPreliminaryAnimationStartTime = (iPreliminaryAnimationStartTime == -1 ? objRadarAnimation->iAnimationCurrentFrameTime : MIN(iPreliminaryAnimationStartTime, objRadarAnimation->iAnimationCurrentFrameTime));
		iPreliminaryAnimationFinishTime = (iPreliminaryAnimationFinishTime == -1 ? objRadarAnimation->iAnimationCurrentFrameTime : MAX(iPreliminaryAnimationFinishTime, objRadarAnimation->iAnimationCurrentFrameTime));


		/* Trigger the radar images to be redrawn */
		grits_viewer_queue_draw(site->viewer);

		/* If there is not already a main thread request ongoing, then create a new request */
		if(!objRadarAnimation->iAnimationFrameChangeIdleSource){
			objRadarAnimation->iAnimationFrameChangeIdleSource = g_idle_add(_animation_update_status_ui_g_idle_add, site);
		}

		/* Calculate how long to sleep for in microseconds, then sleep for that long. */
		gint64 iSleepMilliseconds = iAnimationFrameIntervalMs;
		if(objRadarAnimation->lIsAnimationPaused == true
			&& objRadarAnimation->eAnimationNextFrameMode == NEXT_FRAME_UNCHANGED){
			/* If we are paused, set the sleep time to 10 minutes becuase there is no need to do anything here unless the user presses a button */
			iSleepMilliseconds = 1000 * 60 * 10;
		}
		lDidWeGetPockedWhileSleeping = _animation_thread_usleep_or_wakeup_from_poke(site, iSleepMilliseconds * 1000);
	}

	/* Show the existing static level2 radar scan and ensure our animation frames are hidden. */
	grits_object_hide(GRITS_OBJECT(objRadarAnimation->aAnimationLevel2Frames[objRadarAnimation->iAnimationCurrentFrame]), true);
	grits_object_hide(GRITS_OBJECT(site->level2), false);

	/* Cleanup */

	/* Wait for the set sweep callback we set above to finish running if it hasn't run yet. Since the function exists within the context of this function, it will be gone once this function exits.
	 * We must check each level2 frame.
	 */
	for(int iCurrentLevel2 = 0; iCurrentLevel2 < objRadarAnimation->iAnimationFrames; ++iCurrentLevel2){
		while(atomic_load(&objRadarAnimation->aAnimationLevel2Frames[iCurrentLevel2]->fAfterSetSweepOneTimeCustomCallback) != NULL){
			g_usleep(1000); /* Wait 1ms */
		}
	}

	if(objRadarAnimation->aAnimationCurrentFileSortedSubframes != NULL) g_array_free(objRadarAnimation->aAnimationCurrentFileSortedSubframes, true);
	objRadarAnimation->aAnimationCurrentFileSortedSubframes = NULL;

	for(int iFrame = 0; iFrame < objRadarAnimation->iAnimationFrames; ++iFrame){
		grits_object_destroy_pointer(&objRadarAnimation->aAnimationLevel2Frames[iFrame]);
	}
	g_free(objRadarAnimation->aAnimationLevel2Frames);
	objRadarAnimation->aAnimationLevel2Frames = NULL;

	objRadarAnimation->iAnimationCurrentFrame = 0;
	objRadarAnimation->iAnimationFrames = 0;
	objRadarAnimation->lIsAnimationCleanupInProgress = true;

	/* Update the UI one last time to reflect the new animation state. */
	objRadarAnimation->iAnimationFrameChangeIdleSource = g_idle_add(_animation_update_status_ui_g_idle_add, site);

	/* Trigger the original radar image to be redrawn. */
	grits_viewer_queue_draw(site->viewer);

	return NULL;
}

/* Starts the animation if site->objRadarAnimation->lUserWantsToAnimate is set to TRUE */
void _start_animation_if_user_requested_it_to_start(RadarSite* site){
	/* Start the background thread to load the animation frames and run the animation if the user wants to animate and we are not currently animating.
	 * Note that if there is no level2 for this current site (radar failed to load / process), then we cannot start the animation, as the container hbox will not exist. */
	if(site->objRadarAnimation->lUserWantsToAnimate
		&& !site->objRadarAnimation->lIsAnimating
		&& site->level2 != NULL
		/* If a sweep is not yet selected, then there is no sweep to select (a sweep is selected if possible on load). Thus, do not allow the animation to start as doing so could cause the animation loop to get stuck. */
		&& site->level2->iSelectedSweepId != AWEATHER_LEVEL2_SELECTED_SWEEP_ID_NONE){
		site->objRadarAnimation->lIsAnimating = true; /* Set to TRUE so the user cannot spawn a new background thread until the previous animation finished completely */

		/* If the previous thread pointer still exists, then clean it up before we start a new thread. */
		if(site->objRadarAnimation->objAnimationThread != NULL){
			g_thread_join(site->objRadarAnimation->objAnimationThread);
			site->objRadarAnimation->objAnimationThread = NULL;
		}


		/* Initialize the GMutex and GCond so we can interrupt the animation thread between frames */
		g_mutex_init(&site->objRadarAnimation->objBtnPressedGMutex);
		g_cond_init(&site->objRadarAnimation->objBtnPressedGCond);

		/* Add an event listener so the user can control the animation with their keyboard */
		_setup_animation_keyboard_event_listeners(site);

		site->objRadarAnimation->objAnimationThread = g_thread_new("animation-update-thread", _animation_update_thread, site);
	}
}

/* Sends a request to the animation thread, telling it to stop, then waits for it to finish all necessary cleanup. Safes the user's desired animation state to the site->objRadarAnimation->lUserWantsToAnimate variable. */
void _stop_animation_and_wait_for_animation_to_stop_save_user_choice(RadarSite* site){
	/* The user selected another time from the time selection list on the right. If the animation is running currently,
	 * we need to stop it, then restart it when the default layer finishes loading. Thus, the animation will now finish
	 * at the time the user selected.
	 */
	if(site->objRadarAnimation->lIsAnimating){
		bool lUserWantsToAnimateNow = site->objRadarAnimation->lUserWantsToAnimate;
		site->objRadarAnimation->lUserWantsToAnimate = false; /* Tell the animation thread to stop running */
		_poke_animation_thread(site); /* poke the animation thread so that if it is sleeping currently, it will wake up and cleanup the animation */

		/* Wait for the callback that the animation thread triggered to run, which will cleanup the remaining animation objects on the UI thread  */
		while(atomic_load(&site->objRadarAnimation->lIsAnimating)){
			gtk_main_iteration();
		}

		g_thread_join(site->objRadarAnimation->objAnimationThread); /* Wait for the animation process to finish - it should be done running now. */
		site->objRadarAnimation->objAnimationThread = NULL;

		/* Reset the user wants to animate flag so we can resume the animation later if needed */
		site->objRadarAnimation->lUserWantsToAnimate = lUserWantsToAnimateNow;
	}
}

void _on_animateButton_clicked(GtkButton *button, RadarSite* site){
	site->objRadarAnimation->lUserWantsToAnimate = !site->objRadarAnimation->lUserWantsToAnimate;
	if(site->objRadarAnimation->lUserWantsToAnimate){
		_start_animation_if_user_requested_it_to_start(site);
	} else {
		_poke_animation_thread(site); /* If we are trying to stop the animation, poke it so it wakes up and finishes right away. */
	}
}

GtkWidget* _getAnimateUi(RadarSite* site){
	/* Add the animation UI buttons to the scroll box - each button is 5 px apart. */
	GtkWidget *hbox = gtk_hbox_new(FALSE, 5);
	site->objRadarAnimation->objAnimateButton = gtk_button_new_with_label("Animate");
	gtk_widget_set_size_request(site->objRadarAnimation->objAnimateButton, 100, 30);
	gtk_box_pack_start(GTK_BOX(hbox), site->objRadarAnimation->objAnimateButton, FALSE, FALSE, 0);

	/* Add click event listener */
	g_signal_connect(site->objRadarAnimation->objAnimateButton, "clicked", G_CALLBACK(_on_animateButton_clicked), site);

	/* Add a progress bar to the right of the animate button */
	site->objRadarAnimation->objAnimateProgressBar = gtk_progress_bar_new();
	gtk_widget_set_size_request(site->objRadarAnimation->objAnimateProgressBar, 100, 30);
	gtk_box_pack_start(GTK_BOX(hbox), site->objRadarAnimation->objAnimateProgressBar, FALSE, FALSE, 0);

	/* Add an hbox to contain the frain selection toggle buttons */
	site->objRadarAnimation->objAnimationFrameControlHbox = gtk_hbox_new(FALSE, 5);
	gtk_box_pack_end(GTK_BOX(hbox), site->objRadarAnimation->objAnimationFrameControlHbox, TRUE, TRUE, 0);

	return hbox;
}






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
		/* UI design:
		 *  <scroll_window>
		 *    <vbox> <!-- vertical stack of widgets inside of the scroll window -->
		 *      <_getAnimateUi/>
		 *      <aweather_level2_get_config>
		 *    </vbox>
		 *  </scroll_window>
		 */

		/* Wrap the radar sweep / volume UI in a GtkScrolledWindow so that we don't use up too much screen space displaying all radar options on small screens.*/
		GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
		GtkWidget *vbox = gtk_vbox_new(FALSE, 5);
		GtkWidget *animateUi = _getAnimateUi(site);
		GtkWidget *sweepSelectionUiWidget = aweather_level2_get_config(site->level2, site->prefs);


		/* Automatically show the scrollbars */
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

		/* Add the radar animate UI and options UI to the vertical box */
		gtk_box_pack_start(GTK_BOX(vbox), animateUi, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(vbox), sweepSelectionUiWidget, FALSE, FALSE, 0);

		/* Add the vertical box to the scrolled window */
		gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scrolled_window), vbox);

		/* Add the scrolled window to the config bin */
		aweather_bin_set_child(GTK_BIN(site->config), scrolled_window);


		/* If the user had been running the animation prior to switching the finish time of the animation loop, then restart the animation automatically. */
		_start_animation_if_user_requested_it_to_start(site);
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
			file, site->city->code, colormaps, site->prefs);
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

	/* Stop the animation if it was running, save off the user's animation choice (running or stopped), then load the new loop end time.
	 * We start the animation back up automatically if the animation was running when we got here.
	 */
	_stop_animation_and_wait_for_animation_to_stop_save_user_choice(site);

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

	/* If we are animating right now, then don't delete the level2 object and don't cleanup here.
	 * If we try to cleanup during an animation, it will cause the program to crash.
	 * Instead, request the animation to stop. When this code runs again (the next time the user tries to move the map), we will allow the radar site to be cleaned up.
	 */
	if(site->objRadarAnimation->lIsAnimating){
		site->objRadarAnimation->lUserWantsToAnimate = false;
		return;
	}

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

	/* Cleanup the animation information */
	g_free(site->objRadarAnimation);

	site->status = STATUS_UNLOADED;
}

void radar_site_load(RadarSite *site)
{
	g_debug("RadarSite: load %s", site->city->code);

	/* Initialize animation object in case the user wants to run an animation */
	site->objRadarAnimation = g_malloc0(sizeof(RadarAnimation));

	/* Initialize animation fields */
	site->objRadarAnimation->cAnimationCurrentFrameTimestampMsg = NULL;
	site->objRadarAnimation->objAnimationThread = NULL;
	site->objRadarAnimation->lUserWantsToAnimate = false;
	site->objRadarAnimation->aAnimationCurrentFileSortedSubframes = NULL;

	/* Add tab page */
	site->config = gtk_alignment_new(0, 0, 1, 1);
	g_object_set_data(G_OBJECT(site->config), "site", site);
	GtkWidget* objTabLabel = gtk_label_new(site->city->name);
	gtk_widget_set_tooltip_text(objTabLabel, site->city->code);
	gtk_notebook_append_page(GTK_NOTEBOOK(site->pconfig), site->config, objTabLabel);
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
	else if (dist > 5*min_dist &&  site->status != STATUS_UNLOADED)
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

	/* If the animation was run, be sure to cleanup an old thread pointer */
	/* This is called when the window is closed. The thread doesn't like being unref-ed when the window is closed */
	//if(site->objRadarAnimation->objAnimationThread != NULL)
	//	g_object_unref(site->objRadarAnimation->objAnimationThread);

	g_free(site);
}


/**************
 * RadarConus *
 **************/
#define CONUS_NORTH       53.0
#define CONUS_WEST       -132.5
#define CONUS_WIDTH       4000.0
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
		if(colormap != NULL){
			/* Print the color table as long as we have a valid colormap */
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
		} else {
			/* If the level2 file contains no sweeps (perhaps it is a very new file with no data), then display a warning */
			g_warning("Warning! _draw_hud failed. This site has no valid colormap. We will not draw the colormap.");
		}
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
			/* If we are hiding the site while it is animating, then stop the animation and wait for it to finish, otherwise, start the animation back up if it was previously running.  */
			if(is_hidden)
				_stop_animation_and_wait_for_animation_to_stop_save_user_choice(site);
			else
				_start_animation_if_user_requested_it_to_start(site);

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

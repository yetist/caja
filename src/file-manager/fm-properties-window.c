/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/* fm-properties-window.c - window that lets user modify file properties

   Copyright (C) 2000 Eazel, Inc.

   The Mate Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Mate Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Mate Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
   Boston, MA 02110-1301, USA.

   Authors: Darin Adler <darin@bentspoon.com>
*/

#include <config.h>
#include <string.h>
#include <cairo.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <sys/stat.h>

#include <eel/eel-accessibility.h>
#include <eel/eel-gdk-pixbuf-extensions.h>
#include <eel/eel-glib-extensions.h>
#include <eel/eel-mate-extensions.h>
#include <eel/eel-gtk-extensions.h>
#include <eel/eel-labeled-image.h>
#include <eel/eel-stock-dialogs.h>
#include <eel/eel-vfs-extensions.h>
#include <eel/eel-wrap-table.h>

#include <libcaja-extension/caja-property-page-provider.h>

#include <libcaja-private/caja-mime-application-chooser.h>
#include <libcaja-private/caja-entry.h>
#include <libcaja-private/caja-extensions.h>
#include <libcaja-private/caja-file-attributes.h>
#include <libcaja-private/caja-file-operations.h>
#include <libcaja-private/caja-desktop-icon-file.h>
#include <libcaja-private/caja-global-preferences.h>
#include <libcaja-private/caja-emblem-utils.h>
#include <libcaja-private/caja-link.h>
#include <libcaja-private/caja-metadata.h>
#include <libcaja-private/caja-module.h>
#include <libcaja-private/caja-mime-actions.h>

#include "fm-properties-window.h"
#include "fm-ditem-page.h"
#include "fm-error-reporting.h"

#if HAVE_SYS_VFS_H
#include <sys/vfs.h>
#elif HAVE_SYS_MOUNT_H
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/mount.h>
#endif

#define USED_FILL_R  0.988235294
#define USED_FILL_G  0.91372549
#define USED_FILL_B  0.309803922

#define FREE_FILL_R  0.447058824
#define FREE_FILL_G  0.623529412
#define FREE_FILL_B  0.811764706

#define PREVIEW_IMAGE_WIDTH 96

#define ROW_PAD 6

static GHashTable *windows;
static GHashTable *pending_lists;

struct _FMPropertiesWindowPrivate {
	GList *original_files;
	GList *target_files;

	GtkNotebook *notebook;

	GtkGrid *basic_grid;

	GtkWidget *icon_button;
	GtkWidget *icon_image;
	GtkWidget *icon_chooser;

	GtkLabel *name_label;
	GtkWidget *name_field;
	char *pending_name;

	GtkLabel *directory_contents_title_field;
	GtkLabel *directory_contents_value_field;
	guint update_directory_contents_timeout_id;
	guint update_files_timeout_id;

	GList *emblem_buttons;
	GHashTable *initial_emblems;

	CajaFile *group_change_file;
	char         *group_change_group;
	unsigned int  group_change_timeout;
	CajaFile *owner_change_file;
	char         *owner_change_owner;
	unsigned int  owner_change_timeout;

	GList *permission_buttons;
	GList *permission_combos;
	GHashTable *initial_permissions;
	gboolean has_recursive_apply;

	GList *value_fields;

	GList *mime_list;

	gboolean deep_count_finished;

	guint total_count;
	goffset total_size;
	goffset total_size_on_disk;

	guint long_operation_underway;

 	GList *changed_files;

 	guint64 volume_capacity;
 	guint64 volume_free;

	GdkRGBA used_color;
	GdkRGBA free_color;
	GdkRGBA used_stroke_color;
	GdkRGBA free_stroke_color;
};

typedef enum {
	PERMISSIONS_CHECKBOXES_READ,
	PERMISSIONS_CHECKBOXES_WRITE,
	PERMISSIONS_CHECKBOXES_EXECUTE
} CheckboxType;

enum {
	TITLE_COLUMN,
	VALUE_COLUMN,
	COLUMN_COUNT
};

typedef struct {
	GList *original_files;
	GList *target_files;
	GtkWidget *parent_widget;
	char *pending_key;
	GHashTable *pending_files;
} StartupData;

/* drag and drop definitions */

enum {
	TARGET_URI_LIST,
	TARGET_MATE_URI_LIST,
	TARGET_RESET_BACKGROUND
};

static const GtkTargetEntry target_table[] = {
	{ "text/uri-list",  0, TARGET_URI_LIST },
	{ "x-special/mate-icon-list",  0, TARGET_MATE_URI_LIST },
	{ "x-special/mate-reset-background", 0, TARGET_RESET_BACKGROUND }
};

#define DIRECTORY_CONTENTS_UPDATE_INTERVAL	200 /* milliseconds */
#define FILES_UPDATE_INTERVAL			200 /* milliseconds */
#define STANDARD_EMBLEM_HEIGHT			52
#define EMBLEM_LABEL_SPACING			2

/*
 * A timeout before changes through the user/group combo box will be applied.
 * When quickly changing owner/groups (i.e. by keyboard or scroll wheel),
 * this ensures that the GUI doesn't end up unresponsive.
 *
 * Both combos react on changes by scheduling a new change and unscheduling
 * or cancelling old pending changes.
 */
#define CHOWN_CHGRP_TIMEOUT			300 /* milliseconds */

static void directory_contents_value_field_update (FMPropertiesWindow *window);
static void file_changed_callback                 (CajaFile       *file,
						   gpointer            user_data);
static void permission_button_update              (FMPropertiesWindow *window,
						   GtkToggleButton    *button);
static void permission_combo_update               (FMPropertiesWindow *window,
						   GtkComboBox        *combo);
static void value_field_update                    (FMPropertiesWindow *window,
						   GtkLabel           *field);
static void properties_window_update              (FMPropertiesWindow *window,
						   GList              *files);
static void is_directory_ready_callback           (CajaFile       *file,
						   gpointer            data);
static void cancel_group_change_callback          (FMPropertiesWindow *window);
static void cancel_owner_change_callback          (FMPropertiesWindow *window);
static void parent_widget_destroyed_callback      (GtkWidget          *widget,
						   gpointer            callback_data);
static void select_image_button_callback          (GtkWidget          *widget,
						   FMPropertiesWindow *properties_window);
static void set_icon                              (const char         *icon_path,
						   FMPropertiesWindow *properties_window);
static void remove_pending                        (StartupData        *data,
						   gboolean            cancel_call_when_ready,
						   gboolean            cancel_timed_wait,
						   gboolean            cancel_destroy_handler);
static void append_extension_pages                (FMPropertiesWindow *window);

static gboolean name_field_focus_out              (CajaEntry *name_field,
						   GdkEventFocus *event,
						   gpointer callback_data);
static void name_field_activate                   (CajaEntry *name_field,
						   gpointer callback_data);
static GtkLabel *attach_ellipsizing_value_label   (GtkGrid *grid,
                                                   GtkWidget *sibling,

						   const char *initial_text);

static GtkWidget* create_pie_widget 		  (FMPropertiesWindow *window);

G_DEFINE_TYPE_WITH_PRIVATE (FMPropertiesWindow, fm_properties_window, GTK_TYPE_DIALOG);

static gboolean
is_multi_file_window (FMPropertiesWindow *window)
{
	GList *l;
	int count;

	count = 0;

	for (l = window->details->original_files; l != NULL; l = l->next) {
		if (!caja_file_is_gone (CAJA_FILE (l->data))) {
			count++;
			if (count > 1) {
				return TRUE;
			}
		}
	}

	return FALSE;
}

static int
get_not_gone_original_file_count (FMPropertiesWindow *window)
{
	GList *l;
	int count;

	count = 0;

	for (l = window->details->original_files; l != NULL; l = l->next) {
		if (!caja_file_is_gone (CAJA_FILE (l->data))) {
			count++;
		}
	}

	return count;
}

static CajaFile *
get_original_file (FMPropertiesWindow *window)
{
	g_return_val_if_fail (!is_multi_file_window (window), NULL);

	if (window->details->original_files == NULL) {
		return NULL;
	}

	return CAJA_FILE (window->details->original_files->data);
}

static CajaFile *
get_target_file_for_original_file (CajaFile *file)
{
	CajaFile *target_file;

	target_file = NULL;
	if (CAJA_IS_DESKTOP_ICON_FILE (file)) {
		CajaDesktopLink *link;

		link = caja_desktop_icon_file_get_link (CAJA_DESKTOP_ICON_FILE (file));

		if (link != NULL) {
			GFile *location;

			/* map to linked URI for these types of links */
			location = caja_desktop_link_get_activation_location (link);

			if (location) {
				target_file = caja_file_get (location);
				g_object_unref (location);
			}

			g_object_unref (link);
		}
        } else {
		char *uri_to_display;

		uri_to_display = caja_file_get_activation_uri (file);

		if (uri_to_display != NULL) {
			target_file = caja_file_get_by_uri (uri_to_display);
			g_free (uri_to_display);
		}
	}

	if (target_file != NULL) {
		return target_file;
	}

	/* Ref passed-in file here since we've decided to use it. */
	caja_file_ref (file);
	return file;
}

static CajaFile *
get_target_file (FMPropertiesWindow *window)
{
	return CAJA_FILE (window->details->target_files->data);
}

static void
add_prompt (GtkWidget *vbox, const char *prompt_text, gboolean pack_at_start)
{
	GtkWidget *prompt;

	prompt = gtk_label_new (prompt_text);
   	gtk_label_set_justify (GTK_LABEL (prompt), GTK_JUSTIFY_LEFT);
	gtk_label_set_line_wrap (GTK_LABEL (prompt), TRUE);
	gtk_widget_show (prompt);
	if (pack_at_start) {
		gtk_box_pack_start (GTK_BOX (vbox), prompt, FALSE, FALSE, 0);
	} else {
		gtk_box_pack_end (GTK_BOX (vbox), prompt, FALSE, FALSE, 0);
	}
}

static void
add_prompt_and_separator (GtkWidget *vbox, const char *prompt_text)
{
	GtkWidget *separator_line;

	add_prompt (vbox, prompt_text, FALSE);

	separator_line = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

	gtk_widget_show (separator_line);
	gtk_box_pack_end (GTK_BOX (vbox), separator_line, TRUE, TRUE, 2*ROW_PAD);
}

static void
get_image_for_properties_window (FMPropertiesWindow *window,
				 char **icon_name,
				 char **display_name_,
				 GdkPixbuf **icon_pixbuf)
{
	CajaIconInfo *icon, *new_icon;
	GList *l;
	gint icon_scale;
	gchar *display_name = NULL;

	icon = NULL;
	icon_scale = gtk_widget_get_scale_factor (GTK_WIDGET (window->details->notebook));

	for (l = window->details->original_files; l != NULL; l = l->next) {
		CajaFile *file;

		file = CAJA_FILE (l->data);

		if (!icon) {
			icon = caja_file_get_icon (file, CAJA_ICON_SIZE_STANDARD, icon_scale,
						   CAJA_FILE_ICON_FLAGS_USE_THUMBNAILS |
						   CAJA_FILE_ICON_FLAGS_IGNORE_VISITING);
			display_name = caja_file_get_string_attribute (file, "type");
		} else {
			new_icon = caja_file_get_icon (file, CAJA_ICON_SIZE_STANDARD, icon_scale,
						       CAJA_FILE_ICON_FLAGS_USE_THUMBNAILS |
						       CAJA_FILE_ICON_FLAGS_IGNORE_VISITING);
			if (!new_icon || new_icon != icon) {
				g_object_unref (icon);
				g_object_unref (new_icon);
				icon = NULL;
				g_free (display_name);
				display_name = g_strdup (_("Multiple files"));
				break;
			}
			g_object_unref (new_icon);
		}
	}

	if (!icon) {
		icon = caja_icon_info_lookup_from_name ("text-x-generic",
							CAJA_ICON_SIZE_STANDARD,
							icon_scale);
	}

	if (icon_name != NULL) {
		*icon_name = g_strdup (caja_icon_info_get_used_name (icon));
	}

	if (display_name_ != NULL) {
		if (! display_name) {
			display_name = g_strdup (_("No files"));
		} else if (! caja_icon_info_get_used_name (icon)) {
			g_free (display_name);
			display_name = g_strdup (_("User-defined icon"));
		}
		*display_name_ = display_name;
		display_name = NULL;
	}

	if (icon_pixbuf != NULL) {
		*icon_pixbuf = caja_icon_info_get_pixbuf_at_size (icon, CAJA_ICON_SIZE_STANDARD);
	}

	g_object_unref (icon);
	g_free (display_name);
}

static void
update_properties_window_icon (FMPropertiesWindow *window)
{
	GdkPixbuf *pixbuf;
	cairo_surface_t *surface;
	char *name;
	char *display_name;

	get_image_for_properties_window (window, &name, &display_name, &pixbuf);

	if (name != NULL) {
		gtk_window_set_icon_name (GTK_WINDOW (window), name);
	} else {
		gtk_window_set_icon (GTK_WINDOW (window), pixbuf);
	}

	surface = gdk_cairo_surface_create_from_pixbuf (pixbuf, gtk_widget_get_scale_factor (GTK_WIDGET (window)),
							gtk_widget_get_window (GTK_WIDGET (window)));
	gtk_image_set_from_surface (GTK_IMAGE (window->details->icon_image), surface);

	atk_object_set_name (gtk_widget_get_accessible (window->details->icon_image), display_name);

	g_free (name);
	g_free (display_name);
	g_object_unref (pixbuf);
	cairo_surface_destroy (surface);
}

/* utility to test if a uri refers to a local image */
static gboolean
uri_is_local_image (const char *uri)
{
	GdkPixbuf *pixbuf;
	char *image_path;

	image_path = g_filename_from_uri (uri, NULL, NULL);
	if (image_path == NULL) {
		return FALSE;
	}

	pixbuf = gdk_pixbuf_new_from_file (image_path, NULL);
	g_free (image_path);

	if (pixbuf == NULL) {
		return FALSE;
	}
	g_object_unref (pixbuf);
	return TRUE;
}

static void
reset_icon (FMPropertiesWindow *properties_window)
{
	GList *l;

	for (l = properties_window->details->original_files; l != NULL; l = l->next) {
		CajaFile *file;

		file = CAJA_FILE (l->data);

		caja_file_set_metadata (file,
					    CAJA_METADATA_KEY_ICON_SCALE,
					    NULL, NULL);
		caja_file_set_metadata (file,
					    CAJA_METADATA_KEY_CUSTOM_ICON,
					    NULL, NULL);
	}
}

static void
fm_properties_window_drag_data_received (GtkWidget *widget, GdkDragContext *context,
					 int x, int y,
					 GtkSelectionData *selection_data,
					 guint info, guint time)
{
	char **uris;
	gboolean exactly_one;
	GtkImage *image;
 	GtkWindow *window;

	image = GTK_IMAGE (widget);
 	window = GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (image)));

	if (info == TARGET_RESET_BACKGROUND) {
		reset_icon (FM_PROPERTIES_WINDOW (window));

		return;
	}

	uris = g_strsplit (gtk_selection_data_get_data (selection_data), "\r\n", 0);
	exactly_one = uris[0] != NULL && (uris[1] == NULL || uris[1][0] == '\0');

	if (!exactly_one) {
		eel_show_error_dialog
			(_("You cannot assign more than one custom icon at a time!"),
			 _("Please drag just one image to set a custom icon."),
			 window);
	} else {
		if (uri_is_local_image (uris[0])) {
			set_icon (uris[0], FM_PROPERTIES_WINDOW (window));
		} else {
			GFile *f;

			f = g_file_new_for_uri (uris[0]);
			if (!g_file_is_native (f)) {
				eel_show_error_dialog
					(_("The file that you dropped is not local."),
					 _("You can only use local images as custom icons."),
					 window);

			} else {
				eel_show_error_dialog
					(_("The file that you dropped is not an image."),
					 _("You can only use local images as custom icons."),
					 window);
			}
			g_object_unref (f);
		}
	}
	g_strfreev (uris);
}

static GtkWidget *
create_image_widget (FMPropertiesWindow *window,
		     gboolean is_customizable)
{
 	GtkWidget *button;
	GtkWidget *image;

	image = gtk_image_new ();
	window->details->icon_image = image;

	update_properties_window_icon (window);
	gtk_widget_show (image);

	button = NULL;
	if (is_customizable) {
		button = gtk_button_new ();
		gtk_widget_set_tooltip_text (button, _("Change associated icon"));
		gtk_button_set_image (GTK_BUTTON (button), image);

		/* prepare the image to receive dropped objects to assign custom images */
		gtk_drag_dest_set (GTK_WIDGET (image),
				   GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_HIGHLIGHT | GTK_DEST_DEFAULT_DROP,
				   target_table, G_N_ELEMENTS (target_table),
				   GDK_ACTION_COPY | GDK_ACTION_MOVE);

		g_signal_connect (image, "drag_data_received",
				  G_CALLBACK (fm_properties_window_drag_data_received), NULL);
		g_signal_connect (button, "clicked",
				  G_CALLBACK (select_image_button_callback), window);
	}

	window->details->icon_button = button;

	return button != NULL ? button : image;
}

static void
set_name_field (FMPropertiesWindow *window,
                const gchar *original_name,
                const gchar *name)
{
	gboolean new_widget;
	gboolean use_label;

	/* There are four cases here:
	 * 1) Changing the text of a label
	 * 2) Changing the text of an entry
	 * 3) Creating label (potentially replacing entry)
	 * 4) Creating entry (potentially replacing label)
	 */
	use_label = is_multi_file_window (window) || !caja_file_can_rename (get_original_file (window));
	new_widget = !window->details->name_field || (use_label ? CAJA_IS_ENTRY (window->details->name_field) : GTK_IS_LABEL (window->details->name_field));

	if (new_widget) {
		if (window->details->name_field) {
			gtk_widget_destroy (window->details->name_field);
		}

		if (use_label) {
			window->details->name_field = GTK_WIDGET
				(attach_ellipsizing_value_label (window->details->basic_grid,
								 GTK_WIDGET (window->details->name_label),
								 name));

		} else {
			window->details->name_field = caja_entry_new ();
			gtk_entry_set_text (GTK_ENTRY (window->details->name_field), name);
			gtk_widget_show (window->details->name_field);
			gtk_grid_attach_next_to (window->details->basic_grid, window->details->name_field,
						 GTK_WIDGET (window->details->name_label),
						 GTK_POS_RIGHT, 1, 1);

			gtk_label_set_mnemonic_widget (GTK_LABEL (window->details->name_label), window->details->name_field);

			g_signal_connect_object (window->details->name_field, "focus_out_event",
						 G_CALLBACK (name_field_focus_out), window, 0);
			g_signal_connect_object (window->details->name_field, "activate",
						 G_CALLBACK (name_field_activate), window, 0);
		}

		gtk_widget_show (window->details->name_field);
	}
	/* Only replace text if the file's name has changed. */
	else if (original_name == NULL || strcmp (original_name, name) != 0) {

		if (use_label) {
			gtk_label_set_text (GTK_LABEL (window->details->name_field), name);
		} else {
			/* Only reset the text if it's different from what is
			 * currently showing. This causes minimal ripples (e.g.
			 * selection change).
			 */
			gchar *displayed_name = gtk_editable_get_chars (GTK_EDITABLE (window->details->name_field), 0, -1);
			if (strcmp (displayed_name, name) != 0) {
				gtk_entry_set_text (GTK_ENTRY (window->details->name_field), name);
			}
			g_free (displayed_name);
		}
	}
}

static void
update_name_field (FMPropertiesWindow *window)
{
	CajaFile *file;

	gtk_label_set_text_with_mnemonic (window->details->name_label,
					  ngettext ("_Name:", "_Names:",
						    get_not_gone_original_file_count (window)));

	if (is_multi_file_window (window)) {
		/* Multifile property dialog, show all names */
		GString *str;
		char *name;
		gboolean first;
		GList *l;

		str = g_string_new ("");

		first = TRUE;

		for (l = window->details->target_files; l != NULL; l = l->next) {
			file = CAJA_FILE (l->data);

			if (!caja_file_is_gone (file)) {
				if (!first) {
					g_string_append (str, ", ");
				}
				first = FALSE;

				name = caja_file_get_display_name (file);
				g_string_append (str, name);
				g_free (name);
			}
		}
		set_name_field (window, NULL, str->str);
		g_string_free (str, TRUE);
	} else {
		const char *original_name = NULL;
		char *current_name;

		file = get_original_file (window);

		if (file == NULL || caja_file_is_gone (file)) {
			current_name = g_strdup ("");
		} else {
			current_name = caja_file_get_display_name (file);
		}

		/* If the file name has changed since the original name was stored,
		 * update the text in the text field, possibly (deliberately) clobbering
		 * an edit in progress. If the name hasn't changed (but some other
		 * aspect of the file might have), then don't clobber changes.
		 */
		if (window->details->name_field) {
			original_name = (const char *) g_object_get_data (G_OBJECT (window->details->name_field), "original_name");
		}

		set_name_field (window, original_name, current_name);

		if (original_name == NULL ||
		    g_strcmp0 (original_name, current_name) != 0) {
			g_object_set_data_full (G_OBJECT (window->details->name_field),
						"original_name",
						current_name,
						g_free);
		} else {
			g_free (current_name);
		}
	}
}

static void
name_field_restore_original_name (CajaEntry *name_field)
{
	const char *original_name;
	char *displayed_name;

	original_name = (const char *) g_object_get_data (G_OBJECT (name_field),
							  "original_name");

	if (!original_name) {
		return;
	}

	displayed_name = gtk_editable_get_chars (GTK_EDITABLE (name_field), 0, -1);

	if (strcmp (original_name, displayed_name) != 0) {
		gtk_entry_set_text (GTK_ENTRY (name_field), original_name);
	}
	caja_entry_select_all (name_field);

	g_free (displayed_name);
}

static void
rename_callback (CajaFile *file, GFile *res_loc, GError *error, gpointer callback_data)
{
	FMPropertiesWindow *window;

	window = FM_PROPERTIES_WINDOW (callback_data);

	/* Complain to user if rename failed. */
	if (error != NULL) {
		fm_report_error_renaming_file (file,
					       window->details->pending_name,
					       error,
					       GTK_WINDOW (window));
		if (window->details->name_field != NULL) {
			name_field_restore_original_name (CAJA_ENTRY (window->details->name_field));
		}
	}

	g_object_unref (window);
}

static void
set_pending_name (FMPropertiesWindow *window, const char *name)
{
	g_free (window->details->pending_name);
	window->details->pending_name = g_strdup (name);
}

static void
name_field_done_editing (CajaEntry *name_field, FMPropertiesWindow *window)
{
	CajaFile *file;
	char *new_name;

	g_return_if_fail (CAJA_IS_ENTRY (name_field));

	/* Don't apply if the dialog has more than one file */
	if (is_multi_file_window (window)) {
		return;
	}

	file = get_original_file (window);

	/* This gets called when the window is closed, which might be
	 * caused by the file having been deleted.
	 */
	if (file == NULL || caja_file_is_gone  (file)) {
		return;
	}

	new_name = gtk_editable_get_chars (GTK_EDITABLE (name_field), 0, -1);

	/* Special case: silently revert text if new text is empty. */
	if (strlen (new_name) == 0) {
		name_field_restore_original_name (CAJA_ENTRY (name_field));
	} else {
		const char *original_name;

		original_name = (const char *) g_object_get_data (G_OBJECT (window->details->name_field),
								  "original_name");
		/* Don't rename if not changed since we read the display name.
		   This is needed so that we don't save the display name to the
		   file when nothing is changed */
		if (strcmp (new_name, original_name) != 0) {
			set_pending_name (window, new_name);
			g_object_ref (window);
			caja_file_rename (file, new_name,
					      rename_callback, window);
		}
	}

	g_free (new_name);
}

static gboolean
name_field_focus_out (CajaEntry *name_field,
		      GdkEventFocus *event,
		      gpointer callback_data)
{
	g_assert (FM_IS_PROPERTIES_WINDOW (callback_data));

	if (gtk_widget_get_sensitive (GTK_WIDGET (name_field))) {
		name_field_done_editing (name_field, FM_PROPERTIES_WINDOW (callback_data));
	}

	return FALSE;
}

static void
name_field_activate (CajaEntry *name_field, gpointer callback_data)
{
	g_assert (CAJA_IS_ENTRY (name_field));
	g_assert (FM_IS_PROPERTIES_WINDOW (callback_data));

	/* Accept changes. */
	name_field_done_editing (name_field, FM_PROPERTIES_WINDOW (callback_data));

	caja_entry_select_all_at_idle (name_field);
}

static gboolean
file_has_keyword (CajaFile *file, const char *keyword)
{
	GList *keywords, *word;

	keywords = caja_file_get_keywords (file);
	word = g_list_find_custom (keywords, keyword, (GCompareFunc) strcmp);
    	g_list_free_full (keywords, g_free);

	return (word != NULL);
}

static void
get_initial_emblem_state (FMPropertiesWindow *window,
			  const char *name,
			  GList **on,
			  GList **off)
{
	GList *l;

	*on = NULL;
	*off = NULL;

	for (l = window->details->original_files; l != NULL; l = l->next) {
		GList *initial_emblems;

		initial_emblems = g_hash_table_lookup (window->details->initial_emblems,
						       l->data);

		if (g_list_find_custom (initial_emblems, name, (GCompareFunc) strcmp)) {
			*on = g_list_prepend (*on, l->data);
		} else {
			*off = g_list_prepend (*off, l->data);
		}
	}
}

static void
emblem_button_toggled (GtkToggleButton *button,
		       FMPropertiesWindow *window)
{
	GList *l;
	GList *keywords;
	GList *word;
	char *name;
	GList *files_on;
	GList *files_off;

	name = g_object_get_data (G_OBJECT (button), "caja_emblem_name");

	files_on = NULL;
	files_off = NULL;
	if (gtk_toggle_button_get_active (button)
	    && !gtk_toggle_button_get_inconsistent (button)) {
		/* Go to the initial state unless the initial state was
		   consistent */
		get_initial_emblem_state (window, name,
					  &files_on, &files_off);

		if (!(files_on && files_off)) {
			g_list_free (files_on);
			g_list_free (files_off);
			files_on = g_list_copy (window->details->original_files);
			files_off = NULL;
		}
	} else if (gtk_toggle_button_get_inconsistent (button)
		   && !gtk_toggle_button_get_active (button)) {
		files_on = g_list_copy (window->details->original_files);
		files_off = NULL;
	} else {
		files_off = g_list_copy (window->details->original_files);
		files_on = NULL;
	}

	g_signal_handlers_block_by_func (G_OBJECT (button),
					 G_CALLBACK (emblem_button_toggled),
					 window);

	gtk_toggle_button_set_active (button, files_on != NULL);
	gtk_toggle_button_set_inconsistent (button, files_on && files_off);

	g_signal_handlers_unblock_by_func (G_OBJECT (button),
					   G_CALLBACK (emblem_button_toggled),
					   window);

	for (l = files_on; l != NULL; l = l->next) {
		CajaFile *file;

		file = CAJA_FILE (l->data);

		keywords = caja_file_get_keywords (file);

		word = g_list_find_custom (keywords, name,  (GCompareFunc)strcmp);
		if (!word) {
			keywords = g_list_prepend (keywords, g_strdup (name));
		}
		caja_file_set_keywords (file, keywords);
    		g_list_free_full (keywords, g_free);
	}

	for (l = files_off; l != NULL; l = l->next) {
		CajaFile *file;

		file = CAJA_FILE (l->data);

		keywords = caja_file_get_keywords (file);

		word = g_list_find_custom (keywords, name,  (GCompareFunc)strcmp);
		if (word) {
			keywords = g_list_remove_link (keywords, word);
    			g_list_free_full (word, g_free);
		}
		caja_file_set_keywords (file, keywords);
    		g_list_free_full (keywords, g_free);
	}

	g_list_free (files_on);
	g_list_free (files_off);
}

static void
emblem_button_update (FMPropertiesWindow *window,
			GtkToggleButton *button)
{
	GList *l;
	char *name;
	gboolean all_set;
	gboolean all_unset;

	name = g_object_get_data (G_OBJECT (button), "caja_emblem_name");

	all_set = TRUE;
	all_unset = TRUE;
	for (l = window->details->original_files; l != NULL; l = l->next) {
		gboolean has_keyword;
		CajaFile *file;

		file = CAJA_FILE (l->data);

		has_keyword = file_has_keyword (file, name);

		if (has_keyword) {
			all_unset = FALSE;
		} else {
			all_set = FALSE;
		}
	}

	g_signal_handlers_block_by_func (G_OBJECT (button),
					 G_CALLBACK (emblem_button_toggled),
					 window);

	gtk_toggle_button_set_active (button, !all_unset);
	gtk_toggle_button_set_inconsistent (button, !all_unset && !all_set);

	g_signal_handlers_unblock_by_func (G_OBJECT (button),
					   G_CALLBACK (emblem_button_toggled),
					   window);

}

static void
update_properties_window_title (FMPropertiesWindow *window)
{
	char *title;

	g_return_if_fail (GTK_IS_WINDOW (window));

	title = g_strdup_printf (_("Properties"));

	if (!is_multi_file_window (window)) {
		CajaFile *file;

		file = get_original_file (window);

		if (file != NULL) {
			char *name;

			g_free (title);
			name = caja_file_get_display_name (file);
			title = g_strdup_printf (_("%s Properties"), name);
			g_free (name);
		}
	}

  	gtk_window_set_title (GTK_WINDOW (window), title);

	g_free (title);
}

static void
clear_extension_pages (FMPropertiesWindow *window)
{
	int i;
	int num_pages;
	GtkWidget *page = NULL;

	num_pages = gtk_notebook_get_n_pages
				(GTK_NOTEBOOK (window->details->notebook));

	for (i = 0; i < num_pages; i++) {
		page = gtk_notebook_get_nth_page
				(GTK_NOTEBOOK (window->details->notebook), i);

		if (g_object_get_data (G_OBJECT (page), "is-extension-page")) {
			gtk_notebook_remove_page
				(GTK_NOTEBOOK (window->details->notebook), i);
			num_pages--;
			i--;
		}
	}
}

static void
refresh_extension_pages (FMPropertiesWindow *window)
{
	clear_extension_pages (window);
	append_extension_pages (window);
}

static void
remove_from_dialog (FMPropertiesWindow *window,
		    CajaFile *file)
{
	int index;
	GList *original_link;
	GList *target_link;
	CajaFile *original_file;
	CajaFile *target_file;

	index = g_list_index (window->details->target_files, file);
	if (index == -1) {
		index = g_list_index (window->details->original_files, file);
		g_return_if_fail (index != -1);
	}

	original_link = g_list_nth (window->details->original_files, index);
	target_link = g_list_nth (window->details->target_files, index);

	g_return_if_fail (original_link && target_link);

	original_file = CAJA_FILE (original_link->data);
	target_file = CAJA_FILE (target_link->data);

	window->details->original_files = g_list_remove_link (window->details->original_files, original_link);
	g_list_free (original_link);

	window->details->target_files = g_list_remove_link (window->details->target_files, target_link);
	g_list_free (target_link);

	g_hash_table_remove (window->details->initial_emblems, original_file);
	g_hash_table_remove (window->details->initial_permissions, target_file);

	g_signal_handlers_disconnect_by_func (original_file,
					      G_CALLBACK (file_changed_callback),
					      window);
	g_signal_handlers_disconnect_by_func (target_file,
					      G_CALLBACK (file_changed_callback),
					      window);

	caja_file_monitor_remove (original_file, &window->details->original_files);
	caja_file_monitor_remove (target_file, &window->details->target_files);

	caja_file_unref (original_file);
	caja_file_unref (target_file);

}

static gboolean
mime_list_equal (GList *a, GList *b)
{
	while (a && b) {
		if (strcmp (a->data, b->data)) {
			return FALSE;
		}
		a = a->next;
		b = b->next;
	}

	return (a == b);
}

static GList *
get_mime_list (FMPropertiesWindow *window)
{
	GList *ret;
	GList *l;

	ret = NULL;
	for (l = window->details->target_files; l != NULL; l = l->next) {
		ret = g_list_append (ret, caja_file_get_mime_type (CAJA_FILE (l->data)));
	}
	ret = g_list_reverse (ret);
	return ret;
}

static void
properties_window_update (FMPropertiesWindow *window,
			  GList *files)
{
	GList *l;
	GList *mime_list;
	GList *tmp;
	CajaFile *changed_file = NULL;
	gboolean dirty_original = FALSE;
	gboolean dirty_target = FALSE;

	if (files == NULL) {
		dirty_original = TRUE;
		dirty_target = TRUE;
	}

	for (tmp = files; tmp != NULL; tmp = tmp->next) {
		changed_file = CAJA_FILE (tmp->data);

		if (changed_file && caja_file_is_gone (changed_file)) {
			/* Remove the file from the property dialog */
			remove_from_dialog (window, changed_file);
			changed_file = NULL;

			if (window->details->original_files == NULL) {
				return;
			}
		}
		if (changed_file == NULL ||
		    g_list_find (window->details->original_files, changed_file)) {
			dirty_original = TRUE;
		}
		if (changed_file == NULL ||
		    g_list_find (window->details->target_files, changed_file)) {
			dirty_target = TRUE;
		}

	}

	if (dirty_original) {
		update_properties_window_title (window);
		update_properties_window_icon (window);
		update_name_field (window);

		for (l = window->details->emblem_buttons; l != NULL; l = l->next) {
			emblem_button_update (window, GTK_TOGGLE_BUTTON (l->data));
		}

		/* If any of the value fields start to depend on the original
		 * value, value_field_updates should be added here */
	}

	if (dirty_target) {
		for (l = window->details->permission_buttons; l != NULL; l = l->next) {
			permission_button_update (window, GTK_TOGGLE_BUTTON (l->data));
		}

		for (l = window->details->permission_combos; l != NULL; l = l->next) {
			permission_combo_update (window, GTK_COMBO_BOX (l->data));
		}

		for (l = window->details->value_fields; l != NULL; l = l->next) {
			value_field_update (window, GTK_LABEL (l->data));
		}
	}

	mime_list = get_mime_list (window);

	if (!window->details->mime_list) {
		window->details->mime_list = mime_list;
	} else {
		if (!mime_list_equal (window->details->mime_list, mime_list)) {
			refresh_extension_pages (window);
		}

	    	g_list_free_full (window->details->mime_list, g_free);
		window->details->mime_list = mime_list;
	}
}

static gboolean
update_files_callback (gpointer data)
{
 	FMPropertiesWindow *window;

 	window = FM_PROPERTIES_WINDOW (data);

	window->details->update_files_timeout_id = 0;

	properties_window_update (window, window->details->changed_files);

	if (window->details->original_files == NULL) {
		/* Close the window if no files are left */
		gtk_widget_destroy (GTK_WIDGET (window));
	} else {
		caja_file_list_free (window->details->changed_files);
		window->details->changed_files = NULL;
	}

 	return FALSE;
 }

static void
schedule_files_update (FMPropertiesWindow *window)
 {
 	g_assert (FM_IS_PROPERTIES_WINDOW (window));

	if (window->details->update_files_timeout_id == 0) {
		window->details->update_files_timeout_id
			= g_timeout_add (FILES_UPDATE_INTERVAL,
					 update_files_callback,
 					 window);
 	}
 }

static gboolean
file_list_attributes_identical (GList *file_list, const char *attribute_name)
{
	gboolean identical;
	char *first_attr;
	GList *l;

	first_attr = NULL;
	identical = TRUE;

	for (l = file_list; l != NULL; l = l->next) {
		CajaFile *file;

		file = CAJA_FILE (l->data);

		if (caja_file_is_gone (file)) {
			continue;
		}

		if (first_attr == NULL) {
			first_attr = caja_file_get_string_attribute_with_default (file, attribute_name);
		} else {
			char *attr;
			attr = caja_file_get_string_attribute_with_default (file, attribute_name);
			if (strcmp (attr, first_attr)) {
				identical = FALSE;
				g_free (attr);
				break;
			}
			g_free (attr);
		}
	}

	g_free (first_attr);
	return identical;
}

static char *
file_list_get_string_attribute (GList *file_list,
				const char *attribute_name,
				const char *inconsistent_value)
{
	if (file_list_attributes_identical (file_list, attribute_name)) {
		GList *l;

		for (l = file_list; l != NULL; l = l->next) {
			CajaFile *file;

			file = CAJA_FILE (l->data);
			if (!caja_file_is_gone (file)) {
				return caja_file_get_string_attribute_with_default
					(file,
					 attribute_name);
			}
		}
		return g_strdup (_("unknown"));
	} else {
		return g_strdup (inconsistent_value);
	}
}

static gboolean
file_list_all_directories (GList *file_list)
{
	GList *l;
	for (l = file_list; l != NULL; l = l->next) {
		if (!caja_file_is_directory (CAJA_FILE (l->data))) {
			return FALSE;
		}
	}
	return TRUE;
}

static void
value_field_update_internal (GtkLabel *label,
			     GList *file_list)
{
	const char *attribute_name;
	char *attribute_value;
	char *inconsistent_string;

	g_assert (GTK_IS_LABEL (label));

	attribute_name = g_object_get_data (G_OBJECT (label), "file_attribute");
	inconsistent_string = g_object_get_data (G_OBJECT (label), "inconsistent_string");
	attribute_value = file_list_get_string_attribute (file_list,
							  attribute_name,
							  inconsistent_string);
	if (!strcmp (attribute_name, "type") && strcmp (attribute_value, inconsistent_string)) {
		char *mime_type;

		mime_type = file_list_get_string_attribute (file_list,
							    "mime_type",
							    inconsistent_string);
		if (strcmp (mime_type, inconsistent_string)) {
			char *tmp;

			tmp = attribute_value;
			attribute_value = g_strdup_printf (C_("MIME type description (MIME type)", "%s (%s)"), attribute_value, mime_type);
			g_free (tmp);
		}
		g_free (mime_type);
	}

	gtk_label_set_text (label, attribute_value);
	g_free (attribute_value);
}

static void
value_field_update (FMPropertiesWindow *window, GtkLabel *label)
{
	gboolean use_original;

	use_original = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (label), "show_original"));

	value_field_update_internal (label,
				     (use_original ?
				      window->details->original_files :
				      window->details->target_files));
}

static GtkLabel *
attach_label (GtkGrid *grid,
              GtkWidget *sibling,
	      const char *initial_text,
	      gboolean ellipsize_text,
	      gboolean selectable,
	      gboolean mnemonic)
{
	GtkWidget *label_field;

	if (ellipsize_text) {
		label_field = gtk_label_new (initial_text);
                gtk_label_set_ellipsize (GTK_LABEL (label_field),
					 PANGO_ELLIPSIZE_END);
	} else if (mnemonic) {
		label_field = gtk_label_new_with_mnemonic (initial_text);
	} else {
		label_field = gtk_label_new (initial_text);
	}

	if (selectable) {
		gtk_label_set_selectable (GTK_LABEL (label_field), TRUE);
	}

	gtk_label_set_xalign (GTK_LABEL (label_field), 0);
	gtk_widget_show (label_field);
	if (ellipsize_text) {
		gtk_widget_set_hexpand (label_field, TRUE);
		gtk_label_set_max_width_chars (GTK_LABEL (label_field), 24);
	}

	if (sibling != NULL) {
		gtk_grid_attach_next_to (grid, label_field, sibling,
					 GTK_POS_RIGHT, 1, 1);
	} else {
		gtk_container_add (GTK_CONTAINER (grid), label_field);
	}

	return GTK_LABEL (label_field);
}

static GtkLabel *
attach_value_label (GtkGrid *grid,
                    GtkWidget *sibling,
                    const char *initial_text)
{
	return attach_label (grid, sibling, initial_text, FALSE, TRUE, FALSE);
}

static GtkLabel *
attach_ellipsizing_value_label (GtkGrid *grid,
                                GtkWidget *sibling,
                                const char *initial_text)
{
	return attach_label (grid, sibling, initial_text, TRUE, TRUE, FALSE);
}

static GtkWidget*
attach_value_field_internal (FMPropertiesWindow *window,
			     GtkGrid *grid,
			     GtkWidget *sibling,
			     const char *file_attribute_name,
			     const char *inconsistent_string,
			     gboolean show_original,
			     gboolean ellipsize_text)
{
	GtkLabel *value_field;

	if (ellipsize_text) {
		value_field = attach_ellipsizing_value_label (grid, sibling, "");
	} else {
		value_field = attach_value_label (grid, sibling, "");
	}

  	/* Stash a copy of the file attribute name in this field for the callback's sake. */
	g_object_set_data_full (G_OBJECT (value_field), "file_attribute",
				g_strdup (file_attribute_name), g_free);

	g_object_set_data_full (G_OBJECT (value_field), "inconsistent_string",
				g_strdup (inconsistent_string), g_free);

	g_object_set_data (G_OBJECT (value_field), "show_original", GINT_TO_POINTER (show_original));

	window->details->value_fields = g_list_prepend (window->details->value_fields,
							value_field);
	return GTK_WIDGET(value_field);
}

static GtkWidget*
attach_value_field (FMPropertiesWindow *window,
		    GtkGrid *grid,
		    GtkWidget *sibling,
		    const char *file_attribute_name,
		    const char *inconsistent_string,
		    gboolean show_original)
{
	return attach_value_field_internal (window,
					    grid, sibling,
					    file_attribute_name,
					    inconsistent_string,
					    show_original,
					    FALSE);
}

static GtkWidget*
attach_ellipsizing_value_field (FMPropertiesWindow *window,
				GtkGrid *grid,
				GtkWidget *sibling,
		    		const char *file_attribute_name,
				const char *inconsistent_string,
				gboolean show_original)
{
	return attach_value_field_internal (window,
					    grid, sibling,
					    file_attribute_name,
					    inconsistent_string,
					    show_original,
					    TRUE);
}

static void
group_change_callback (CajaFile *file,
		       GFile *res_loc,
		       GError *error,
		       FMPropertiesWindow *window)
{
	char *group;

	g_assert (FM_IS_PROPERTIES_WINDOW (window));
	g_assert (window->details->group_change_file == file);

	group = window->details->group_change_group;
	g_assert (group != NULL);

	/* Report the error if it's an error. */
	eel_timed_wait_stop ((EelCancelCallback) cancel_group_change_callback, window);
	fm_report_error_setting_group (file, error, GTK_WINDOW (window));

	caja_file_unref (file);
	g_free (group);

	window->details->group_change_file = NULL;
	window->details->group_change_group = NULL;
	g_object_unref (G_OBJECT (window));
}

static void
cancel_group_change_callback (FMPropertiesWindow *window)
{
	CajaFile *file;
	char *group;

	file = window->details->group_change_file;
	g_assert (CAJA_IS_FILE (file));

	group = window->details->group_change_group;
	g_assert (group != NULL);

	caja_file_cancel (file, (CajaFileOperationCallback) group_change_callback, window);

	g_free (group);
	caja_file_unref (file);

	window->details->group_change_file = NULL;
	window->details->group_change_group = NULL;
	g_object_unref (window);
}

static gboolean
schedule_group_change_timeout (FMPropertiesWindow *window)
{
	CajaFile *file;
	char *group;

	g_assert (FM_IS_PROPERTIES_WINDOW (window));

	file = window->details->group_change_file;
	g_assert (CAJA_IS_FILE (file));

	group = window->details->group_change_group;
	g_assert (group != NULL);

	eel_timed_wait_start
		((EelCancelCallback) cancel_group_change_callback,
		 window,
		 _("Cancel Group Change?"),
		 GTK_WINDOW (window));

	caja_file_set_group
		(file,  group,
		 (CajaFileOperationCallback) group_change_callback, window);

	window->details->group_change_timeout = 0;
	return FALSE;
}

static void
schedule_group_change (FMPropertiesWindow *window,
		       CajaFile       *file,
		       const char         *group)
{
	g_assert (FM_IS_PROPERTIES_WINDOW (window));
	g_assert (window->details->group_change_group == NULL);
	g_assert (window->details->group_change_file == NULL);
	g_assert (CAJA_IS_FILE (file));

	window->details->group_change_file = caja_file_ref (file);
	window->details->group_change_group = g_strdup (group);
	g_object_ref (G_OBJECT (window));
	window->details->group_change_timeout =
		g_timeout_add (CHOWN_CHGRP_TIMEOUT,
			       (GSourceFunc) schedule_group_change_timeout,
			       window);
}

static void
unschedule_or_cancel_group_change (FMPropertiesWindow *window)
{
	CajaFile *file;
	char *group;

	g_assert (FM_IS_PROPERTIES_WINDOW (window));

	file = window->details->group_change_file;
	group = window->details->group_change_group;

	g_assert ((file == NULL && group == NULL) ||
		  (file != NULL && group != NULL));

	if (file != NULL) {
		g_assert (CAJA_IS_FILE (file));

		if (window->details->group_change_timeout == 0) {
			caja_file_cancel (file,
					      (CajaFileOperationCallback) group_change_callback, window);
			eel_timed_wait_stop ((EelCancelCallback) cancel_group_change_callback, window);
		}

		caja_file_unref (file);
		g_free (group);

		window->details->group_change_file = NULL;
		window->details->group_change_group = NULL;
		g_object_unref (G_OBJECT (window));
	}

	if (window->details->group_change_timeout > 0) {
		g_assert (file != NULL);
		g_source_remove (window->details->group_change_timeout);
		window->details->group_change_timeout = 0;
	}
}

static void
changed_group_callback (GtkComboBox *combo_box, CajaFile *file)
{
	char *group;
	char *cur_group;

	g_assert (GTK_IS_COMBO_BOX (combo_box));
	g_assert (CAJA_IS_FILE (file));

	group = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (combo_box));
	cur_group = caja_file_get_group_name (file);

	if (group != NULL && strcmp (group, cur_group) != 0) {
		FMPropertiesWindow *window;

		/* Try to change file group. If this fails, complain to user. */
		window = FM_PROPERTIES_WINDOW (gtk_widget_get_ancestor (GTK_WIDGET (combo_box), GTK_TYPE_WINDOW));

		unschedule_or_cancel_group_change (window);
		schedule_group_change (window, file, group);
	}
	g_free (group);
	g_free (cur_group);
}

/* checks whether the given column at the first level
 * of model has the specified entries in the given order. */
static gboolean
tree_model_entries_equal (GtkTreeModel *model,
			  unsigned int  column,
			  GList        *entries)
{
	GtkTreeIter iter;
	gboolean empty_model;

	g_assert (GTK_IS_TREE_MODEL (model));
	g_assert (gtk_tree_model_get_column_type (model, column) == G_TYPE_STRING);

	empty_model = !gtk_tree_model_get_iter_first (model, &iter);

	if (!empty_model && entries != NULL) {
		GList *l;

		l = entries;

		do {
			char *val;

			gtk_tree_model_get (model, &iter,
					    column, &val,
					    -1);
			if ((val == NULL && l->data != NULL) ||
			    (val != NULL && l->data == NULL) ||
			    (val != NULL && strcmp (val, l->data))) {
				g_free (val);
				return FALSE;
			}

			g_free (val);
			l = l->next;
		} while (gtk_tree_model_iter_next (model, &iter));

		return l == NULL;
	} else {
		return (empty_model && entries == NULL) ||
		       (!empty_model && entries != NULL);
	}
}

static char *
combo_box_get_active_entry (GtkComboBox *combo_box,
			    unsigned int column)
{
	GtkTreeIter iter;
	char *val;

	g_assert (GTK_IS_COMBO_BOX (combo_box));

	if (gtk_combo_box_get_active_iter (GTK_COMBO_BOX (combo_box), &iter)) {
		GtkTreeModel *model;

		model = gtk_combo_box_get_model (combo_box);
		g_assert (GTK_IS_TREE_MODEL (model));

		gtk_tree_model_get (model, &iter,
				    column, &val,
				    -1);
		return val;
	}

	return NULL;
}

/* returns the index of the given entry in the the given column
 * at the first level of model. Returns -1 if entry can't be found
 * or entry is NULL.
 * */
static int
tree_model_get_entry_index (GtkTreeModel *model,
			    unsigned int  column,
			    const char   *entry)
{
	GtkTreeIter iter;
	gboolean empty_model;

	g_assert (GTK_IS_TREE_MODEL (model));
	g_assert (gtk_tree_model_get_column_type (model, column) == G_TYPE_STRING);

	empty_model = !gtk_tree_model_get_iter_first (model, &iter);
	if (!empty_model && entry != NULL) {
		int index;

		index = 0;

		do {
			char *val;

			gtk_tree_model_get (model, &iter,
					    column, &val,
					    -1);
			if (val != NULL && !strcmp (val, entry)) {
				g_free (val);
				return index;
			}

			g_free (val);
			index++;
		} while (gtk_tree_model_iter_next (model, &iter));
	}

	return -1;
}

static void
synch_groups_combo_box (GtkComboBox *combo_box, CajaFile *file)
{
	GList *groups;
	GList *node;
	GtkTreeModel *model;
	GtkListStore *store;
	char *current_group_name;
	int current_group_index;

	g_assert (GTK_IS_COMBO_BOX (combo_box));
	g_assert (CAJA_IS_FILE (file));

	if (caja_file_is_gone (file)) {
		return;
	}

	groups = caja_file_get_settable_group_names (file);

	model = gtk_combo_box_get_model (combo_box);
	store = GTK_LIST_STORE (model);
	g_assert (GTK_IS_LIST_STORE (model));

	if (!tree_model_entries_equal (model, 0, groups)) {
		int group_index;

		/* Clear the contents of ComboBox in a wacky way because there
		 * is no function to clear all items and also no function to obtain
		 * the number of items in a combobox.
		 */
		gtk_list_store_clear (store);

		for (node = groups, group_index = 0; node != NULL; node = node->next, ++group_index) {
			const char *group_name;

			group_name = (const char *)node->data;
			gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo_box), group_name);
		}
	}

	current_group_name = caja_file_get_group_name (file);
	current_group_index = tree_model_get_entry_index (model, 0, current_group_name);

	/* If current group wasn't in list, we prepend it (with a separator).
	 * This can happen if the current group is an id with no matching
	 * group in the groups file.
	 */
	if (current_group_index < 0 && current_group_name != NULL) {
		if (groups != NULL) {
			/* add separator */
			gtk_combo_box_text_prepend_text (GTK_COMBO_BOX_TEXT (combo_box), "-");
		}

		gtk_combo_box_text_prepend_text (GTK_COMBO_BOX_TEXT (combo_box), current_group_name);
		current_group_index = 0;
	}
	gtk_combo_box_set_active (combo_box, current_group_index);

	g_free (current_group_name);
    	g_list_free_full (groups, g_free);
}

static gboolean
combo_box_row_separator_func (GtkTreeModel *model,
			      GtkTreeIter  *iter,
			      gpointer      data)
{
  	gchar *text;
	gboolean ret;

  	gtk_tree_model_get (model, iter, 0, &text, -1);

	if (text == NULL) {
		return FALSE;
	}

  	if (strcmp (text, "-") == 0) {
    		ret = TRUE;
	} else {
		ret = FALSE;
	}

  	g_free (text);
  	return ret;
}

static GtkComboBox *
attach_combo_box (GtkGrid *grid,
                  GtkWidget *sibling,
                  gboolean two_columns)
{
	GtkWidget *combo_box;

	if (!two_columns) {
		combo_box = gtk_combo_box_text_new ();
	} else {
		GtkTreeModel *model;
		GtkCellRenderer *renderer;

		model = GTK_TREE_MODEL (gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING));
		combo_box = gtk_combo_box_new_with_model (model);
		g_object_unref (G_OBJECT (model));

		renderer = gtk_cell_renderer_text_new ();
		gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo_box), renderer, TRUE);
		gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (combo_box), renderer,
					       "text", 0);

	}

	gtk_widget_set_halign (combo_box, GTK_ALIGN_START);

	gtk_widget_show (combo_box);

  	gtk_combo_box_set_row_separator_func (GTK_COMBO_BOX (combo_box),
					      combo_box_row_separator_func,
					      NULL,
					      NULL);

	gtk_grid_attach_next_to (grid, combo_box, sibling,
				 GTK_POS_RIGHT, 1, 1);

	return GTK_COMBO_BOX (combo_box);
}

static GtkComboBox*
attach_group_combo_box (GtkGrid *grid,
                        GtkWidget *sibling,
                        CajaFile *file)
{
	GtkComboBox *combo_box;

	combo_box = attach_combo_box (grid, sibling, FALSE);

	synch_groups_combo_box (combo_box, file);

	/* Connect to signal to update menu when file changes. */
	g_signal_connect_object (file, "changed",
				 G_CALLBACK (synch_groups_combo_box),
				 combo_box, G_CONNECT_SWAPPED);
	g_signal_connect_data (combo_box, "changed",
			       G_CALLBACK (changed_group_callback),
			       caja_file_ref (file),
			       (GClosureNotify)caja_file_unref, 0);

	return combo_box;
}

static void
owner_change_callback (CajaFile *file,
                       GFile 	    *result_location,
		       GError        *error,
		       FMPropertiesWindow *window)
{
	char *owner;

	g_assert (FM_IS_PROPERTIES_WINDOW (window));
	g_assert (window->details->owner_change_file == file);

	owner = window->details->owner_change_owner;
	g_assert (owner != NULL);

	/* Report the error if it's an error. */
	eel_timed_wait_stop ((EelCancelCallback) cancel_owner_change_callback, window);
	fm_report_error_setting_owner (file, error, GTK_WINDOW (window));

	caja_file_unref (file);
	g_free (owner);

	window->details->owner_change_file = NULL;
	window->details->owner_change_owner = NULL;
	g_object_unref (G_OBJECT (window));
}

static void
cancel_owner_change_callback (FMPropertiesWindow *window)
{
	CajaFile *file;
	char *owner;

	file = window->details->owner_change_file;
	g_assert (CAJA_IS_FILE (file));

	owner = window->details->owner_change_owner;
	g_assert (owner != NULL);

	caja_file_cancel (file, (CajaFileOperationCallback) owner_change_callback, window);

	caja_file_unref (file);
	g_free (owner);

	window->details->owner_change_file = NULL;
	window->details->owner_change_owner = NULL;
	g_object_unref (window);
}

static gboolean
schedule_owner_change_timeout (FMPropertiesWindow *window)
{
	CajaFile *file;
	char *owner;

	g_assert (FM_IS_PROPERTIES_WINDOW (window));

	file = window->details->owner_change_file;
	g_assert (CAJA_IS_FILE (file));

	owner = window->details->owner_change_owner;
	g_assert (owner != NULL);

	eel_timed_wait_start
		((EelCancelCallback) cancel_owner_change_callback,
		 window,
		 _("Cancel Owner Change?"),
		 GTK_WINDOW (window));

	caja_file_set_owner
		(file,  owner,
		 (CajaFileOperationCallback) owner_change_callback, window);

	window->details->owner_change_timeout = 0;
	return FALSE;
}

static void
schedule_owner_change (FMPropertiesWindow *window,
		       CajaFile       *file,
		       const char         *owner)
{
	g_assert (FM_IS_PROPERTIES_WINDOW (window));
	g_assert (window->details->owner_change_owner == NULL);
	g_assert (window->details->owner_change_file == NULL);
	g_assert (CAJA_IS_FILE (file));

	window->details->owner_change_file = caja_file_ref (file);
	window->details->owner_change_owner = g_strdup (owner);
	g_object_ref (G_OBJECT (window));
	window->details->owner_change_timeout =
		g_timeout_add (CHOWN_CHGRP_TIMEOUT,
			       (GSourceFunc) schedule_owner_change_timeout,
			       window);
}

static void
unschedule_or_cancel_owner_change (FMPropertiesWindow *window)
{
	CajaFile *file;
	char *owner;

	g_assert (FM_IS_PROPERTIES_WINDOW (window));

	file = window->details->owner_change_file;
	owner = window->details->owner_change_owner;

	g_assert ((file == NULL && owner == NULL) ||
		  (file != NULL && owner != NULL));

	if (file != NULL) {
		g_assert (CAJA_IS_FILE (file));

		if (window->details->owner_change_timeout == 0) {
			caja_file_cancel (file,
					      (CajaFileOperationCallback) owner_change_callback, window);
			eel_timed_wait_stop ((EelCancelCallback) cancel_owner_change_callback, window);
		}

		caja_file_unref (file);
		g_free (owner);

		window->details->owner_change_file = NULL;
		window->details->owner_change_owner = NULL;
		g_object_unref (G_OBJECT (window));
	}

	if (window->details->owner_change_timeout > 0) {
		g_assert (file != NULL);
		g_source_remove (window->details->owner_change_timeout);
		window->details->owner_change_timeout = 0;
	}
}

static void
changed_owner_callback (GtkComboBox *combo_box, CajaFile* file)
{
	char *owner_text;
	char **name_array;
	char *new_owner;
	char *cur_owner;

	g_assert (GTK_IS_COMBO_BOX (combo_box));
	g_assert (CAJA_IS_FILE (file));

	owner_text = combo_box_get_active_entry (combo_box, 0);
        if (! owner_text)
	    return;
    	name_array = g_strsplit (owner_text, " - ", 2);
	new_owner = name_array[0];
	g_free (owner_text);
	cur_owner = caja_file_get_owner_name (file);

	if (strcmp (new_owner, cur_owner) != 0) {
		FMPropertiesWindow *window;

		/* Try to change file owner. If this fails, complain to user. */
		window = FM_PROPERTIES_WINDOW (gtk_widget_get_ancestor (GTK_WIDGET (combo_box), GTK_TYPE_WINDOW));

		unschedule_or_cancel_owner_change (window);
		schedule_owner_change (window, file, new_owner);
	}
	g_strfreev (name_array);
	g_free (cur_owner);
}

static void
synch_user_menu (GtkComboBox *combo_box, CajaFile *file)
{
	GList *users;
	GList *node;
	GtkTreeModel *model;
	GtkListStore *store;
	GtkTreeIter iter;
	char *user_name;
	char *owner_name;
	int owner_index;
	char **name_array;

	g_assert (GTK_IS_COMBO_BOX (combo_box));
	g_assert (CAJA_IS_FILE (file));

	if (caja_file_is_gone (file)) {
		return;
	}

	users = caja_get_user_names ();

	model = gtk_combo_box_get_model (combo_box);
	store = GTK_LIST_STORE (model);
	g_assert (GTK_IS_LIST_STORE (model));

	if (!tree_model_entries_equal (model, 1, users)) {
		int user_index;

		/* Clear the contents of ComboBox in a wacky way because there
		 * is no function to clear all items and also no function to obtain
		 * the number of items in a combobox.
		 */
		gtk_list_store_clear (store);

		for (node = users, user_index = 0; node != NULL; node = node->next, ++user_index) {
			char *combo_text;

			user_name = (char *)node->data;

			name_array = g_strsplit (user_name, "\n", 2);
			if (name_array[1] != NULL) {
				combo_text = g_strdup_printf ("%s - %s", name_array[0], name_array[1]);
			} else {
				combo_text = g_strdup (name_array[0]);
			}

			gtk_list_store_append (store, &iter);
			gtk_list_store_set (store, &iter,
					    0, combo_text,
					    1, user_name,
					    -1);

			g_strfreev (name_array);
			g_free (combo_text);
		}
	}

	owner_name = caja_file_get_string_attribute (file, "owner");
	owner_index = tree_model_get_entry_index (model, 0, owner_name);

	/* If owner wasn't in list, we prepend it (with a separator).
	 * This can happen if the owner is an id with no matching
	 * identifier in the passwords file.
	 */
	if (owner_index < 0 && owner_name != NULL) {
		if (users != NULL) {
			/* add separator */
			gtk_list_store_prepend (store, &iter);
			gtk_list_store_set (store, &iter,
					    0, "-",
					    1, NULL,
					    -1);
		}

		name_array = g_strsplit (owner_name, " - ", 2);
		if (name_array[1] != NULL) {
			user_name = g_strdup_printf ("%s\n%s", name_array[0], name_array[1]);
		} else {
			user_name = g_strdup (name_array[0]);
		}
		owner_index = 0;

		gtk_list_store_prepend (store, &iter);
		gtk_list_store_set (store, &iter,
				    0, owner_name,
				    1, user_name,
				    -1);

		g_free (user_name);
		g_strfreev (name_array);
	}

	gtk_combo_box_set_active (combo_box, owner_index);

	g_free (owner_name);
    	g_list_free_full (users, g_free);
}

static GtkComboBox*
attach_owner_combo_box (GtkGrid *grid,
                        GtkWidget *sibling,
                        CajaFile *file)
{
	GtkComboBox *combo_box;

	combo_box = attach_combo_box (grid, sibling, TRUE);

	synch_user_menu (combo_box, file);

	/* Connect to signal to update menu when file changes. */
	g_signal_connect_object (file, "changed",
				 G_CALLBACK (synch_user_menu),
				 combo_box, G_CONNECT_SWAPPED);
	g_signal_connect_data (combo_box, "changed",
			       G_CALLBACK (changed_owner_callback),
			       caja_file_ref (file),
			       (GClosureNotify)caja_file_unref, 0);

	return combo_box;
}

static gboolean
file_has_prefix (CajaFile *file,
		 GList *prefix_candidates)
{
	GList *p;
	GFile *location, *candidate_location;

	location = caja_file_get_location (file);

	for (p = prefix_candidates; p != NULL; p = p->next) {
		if (file == p->data) {
			continue;
		}

		candidate_location = caja_file_get_location (CAJA_FILE (p->data));
		if (g_file_has_prefix (location, candidate_location)) {
			g_object_unref (location);
			g_object_unref (candidate_location);
			return TRUE;
		}
		g_object_unref (candidate_location);
	}

	g_object_unref (location);

	return FALSE;
}

static void
directory_contents_value_field_update (FMPropertiesWindow *window)
{
	CajaRequestStatus file_status, status;
	char *text, *temp;
	guint directory_count;
	guint file_count;
	guint total_count;
	guint unreadable_directory_count;
	goffset total_size;
	goffset total_size_on_disk;
	gboolean used_two_lines;
	GList *l;
	guint file_unreadable;
	goffset file_size;
	goffset file_size_on_disk;
	CajaFile *file = NULL;

	g_assert (FM_IS_PROPERTIES_WINDOW (window));

	status = CAJA_REQUEST_DONE;
	file_status = CAJA_REQUEST_NOT_STARTED;
	total_count = window->details->total_count;
	total_size = window->details->total_size;
	total_size_on_disk = window->details->total_size_on_disk;
	unreadable_directory_count = FALSE;

	for (l = window->details->target_files; l; l = l->next) {
		file = CAJA_FILE (l->data);

		if (file_has_prefix (file, window->details->target_files)) {
			/* don't count nested files twice */
			continue;
		}

		if (caja_file_is_directory (file)) {
			file_status = caja_file_get_deep_counts (file,
					 &directory_count,
					 &file_count,
					 &file_unreadable,
					 &file_size,
					 &file_size_on_disk,
					 TRUE);
			total_count += (file_count + directory_count);
			total_size += file_size;
			total_size_on_disk += file_size_on_disk;

			if (file_unreadable) {
				unreadable_directory_count = TRUE;
			}

			if (file_status != CAJA_REQUEST_DONE) {
				status = file_status;
			}
		} else {
			++total_count;
			total_size += caja_file_get_size (file);
			total_size_on_disk += caja_file_get_size_on_disk (file);
		}
	}

	/* If we've already displayed the total once, don't do another visible
	 * count-up if the deep_count happens to get invalidated.
	 * But still display the new total, since it might have changed.
	 */
	if (window->details->deep_count_finished &&
	    status != CAJA_REQUEST_DONE) {
		return;
	}

	text = NULL;
	used_two_lines = FALSE;

	if (total_count == 0) {
		switch (status) {
		case CAJA_REQUEST_DONE:
			if (unreadable_directory_count == 0) {
				text = g_strdup (_("nothing"));
			} else {
				text = g_strdup (_("unreadable"));
			}

			break;
		default:
			text = g_strdup ("...");
		}
	} else {
		char *size_str;
		char *size_on_disk_str;

		if (g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_USE_IEC_UNITS)) {
			size_str = g_format_size_full (total_size, G_FORMAT_SIZE_IEC_UNITS);
			size_on_disk_str = g_format_size_full (total_size_on_disk, G_FORMAT_SIZE_IEC_UNITS);
		} else {
			size_str = g_format_size (total_size);
			size_on_disk_str = g_format_size (total_size_on_disk);
		}

		text = g_strdup_printf (ngettext("%'d item, with size %s (%s on disk)",
						 "%'d items, totalling %s (%s on disk)",
						 total_count),
					total_count, size_str, size_on_disk_str);
		g_free (size_str);
		g_free (size_on_disk_str);

		if (unreadable_directory_count != 0) {
			temp = text;
			text = g_strconcat (temp, "\n",
					    _("(some contents unreadable)"),
					    NULL);
			g_free (temp);
			used_two_lines = TRUE;
		}
	}

	gtk_label_set_text (window->details->directory_contents_value_field,
			    text);
	g_free (text);

	/* Also set the title field here, with a trailing carriage return &
	 * space if the value field has two lines. This is a hack to get the
	 * "Contents:" title to line up with the first line of the
	 * 2-line value. Maybe there's a better way to do this, but I
	 * couldn't think of one.
	 */
	text = g_strdup (_("Contents:"));
	if (used_two_lines) {
		temp = text;
		text = g_strconcat (temp, "\n ", NULL);
		g_free (temp);
	}
	gtk_label_set_text (window->details->directory_contents_title_field,
			    text);
	g_free (text);

	if (status == CAJA_REQUEST_DONE) {
		window->details->deep_count_finished = TRUE;
	}
}

static gboolean
update_directory_contents_callback (gpointer data)
{
	FMPropertiesWindow *window;

	window = FM_PROPERTIES_WINDOW (data);

	window->details->update_directory_contents_timeout_id = 0;
	directory_contents_value_field_update (window);

	return FALSE;
}

static void
schedule_directory_contents_update (FMPropertiesWindow *window)
{
	g_assert (FM_IS_PROPERTIES_WINDOW (window));

	if (window->details->update_directory_contents_timeout_id == 0) {
		window->details->update_directory_contents_timeout_id
			= g_timeout_add (DIRECTORY_CONTENTS_UPDATE_INTERVAL,
					 update_directory_contents_callback,
					 window);
	}
}

static GtkLabel *
attach_directory_contents_value_field (FMPropertiesWindow *window,
                                       GtkGrid *grid,
                                       GtkWidget *sibling)
{
	GtkLabel *value_field;
	GList *l;
	CajaFile *file = NULL;

	value_field = attach_value_label (grid, sibling, "");

	g_assert (window->details->directory_contents_value_field == NULL);
	window->details->directory_contents_value_field = value_field;

	gtk_label_set_line_wrap (value_field, TRUE);

	/* Fill in the initial value. */
	directory_contents_value_field_update (window);

	for (l = window->details->target_files; l; l = l->next) {
		file = CAJA_FILE (l->data);
		caja_file_recompute_deep_counts (file);

		g_signal_connect_object (file,
					 "updated_deep_count_in_progress",
					 G_CALLBACK (schedule_directory_contents_update),
					 window, G_CONNECT_SWAPPED);
	}

	return value_field;
}

static GtkLabel *
attach_title_field (GtkGrid *grid,
                    const char *title)
{
	return attach_label (grid, NULL, title, FALSE, FALSE, TRUE);
}

#define INCONSISTENT_STATE_STRING \
	"\xE2\x80\x92"

static void
append_title_value_pair (FMPropertiesWindow *window,
                         GtkGrid *grid,
                         const char *title,
                         const char *file_attribute_name,
                         const char *inconsistent_state,
                         gboolean show_original)
{
	GtkLabel *title_label;
	GtkWidget *value;

	title_label = attach_title_field (grid, title);
	value = attach_value_field (window, grid, GTK_WIDGET (title_label),
				    file_attribute_name,
				    inconsistent_state,
				    show_original);
	gtk_label_set_mnemonic_widget (title_label, value);
}

static void
append_title_and_ellipsizing_value (FMPropertiesWindow *window,
                                    GtkGrid *grid,
                                    const char *title,
                                    const char *file_attribute_name,
                                    const char *inconsistent_state,
                                    gboolean show_original)
{
	GtkLabel *title_label;
	GtkWidget *value;

	title_label = attach_title_field (grid, title);
	value = attach_ellipsizing_value_field (window, grid,
						GTK_WIDGET (title_label),
						file_attribute_name,
						inconsistent_state,
						show_original);
	gtk_label_set_mnemonic_widget (title_label, value);
}

static void
append_directory_contents_fields (FMPropertiesWindow *window,
                                  GtkGrid *grid)
{
	GtkLabel *title_field, *value_field;
	title_field = attach_title_field (grid, "");
	window->details->directory_contents_title_field = title_field;
	gtk_label_set_line_wrap (title_field, TRUE);

	value_field = attach_directory_contents_value_field
		(window, grid, GTK_WIDGET (title_field));

	gtk_label_set_mnemonic_widget (title_field, GTK_WIDGET(value_field));
}

static GtkWidget *
create_page_with_hbox (GtkNotebook *notebook,
		       const char *title)
{
	GtkWidget *hbox;

	g_assert (GTK_IS_NOTEBOOK (notebook));
	g_assert (title != NULL);

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_show (hbox);
	gtk_container_set_border_width (GTK_CONTAINER (hbox), 12);
	gtk_box_set_spacing (GTK_BOX (hbox), 12);
	gtk_notebook_append_page (notebook, hbox, gtk_label_new (title));

	return hbox;
}

static GtkWidget *
create_page_with_vbox (GtkNotebook *notebook,
		       const char *title)
{
	GtkWidget *vbox;

	g_assert (GTK_IS_NOTEBOOK (notebook));
	g_assert (title != NULL);

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_show (vbox);

	gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);
	gtk_notebook_append_page (notebook, vbox, gtk_label_new (title));

	return vbox;
}

static GtkWidget *
append_blank_row (GtkGrid *grid)
{
	return GTK_WIDGET (attach_title_field (grid, ""));
}

static void
append_blank_slim_row (GtkGrid *grid)
{
	GtkWidget *w;
	PangoAttribute *attribute;
	PangoAttrList *attr_list;

	attr_list = pango_attr_list_new ();
	attribute = pango_attr_scale_new (0.30);
	pango_attr_list_insert (attr_list, attribute);

	w = gtk_label_new (NULL);
	gtk_label_set_attributes (GTK_LABEL (w), attr_list);
	gtk_widget_show (w);

	pango_attr_list_unref (attr_list);

	gtk_container_add (GTK_CONTAINER (grid), w);
}

static GtkWidget *
create_grid_with_standard_properties (void)
{
	GtkWidget *grid;

	grid = gtk_grid_new ();
	gtk_container_set_border_width (GTK_CONTAINER (grid), 6);
	gtk_grid_set_row_spacing (GTK_GRID (grid), ROW_PAD);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	gtk_orientable_set_orientation (GTK_ORIENTABLE (grid), GTK_ORIENTATION_VERTICAL);
	gtk_widget_show (grid);

	return grid;
}

static gboolean
is_merged_trash_directory (CajaFile *file)
{
	char *file_uri;
	gboolean result;

	file_uri = caja_file_get_uri (file);
	result = strcmp (file_uri, "trash:///") == 0;
	g_free (file_uri);

	return result;
}

static gboolean
is_computer_directory (CajaFile *file)
{
	char *file_uri;
	gboolean result;

	file_uri = caja_file_get_uri (file);
	result = strcmp (file_uri, "computer:///") == 0;
	g_free (file_uri);

	return result;
}

static gboolean
is_network_directory (CajaFile *file)
{
	char *file_uri;
	gboolean result;

	file_uri = caja_file_get_uri (file);
	result = strcmp (file_uri, "network:///") == 0;
	g_free (file_uri);

	return result;
}

static gboolean
is_burn_directory (CajaFile *file)
{
	char *file_uri;
	gboolean result;

	file_uri = caja_file_get_uri (file);
	result = strcmp (file_uri, "burn:///") == 0;
	g_free (file_uri);

	return result;
}

static gboolean
should_show_custom_icon_buttons (FMPropertiesWindow *window)
{
	if (is_multi_file_window (window)) {
		return FALSE;
	}

	return TRUE;
}

static gboolean
should_show_file_type (FMPropertiesWindow *window)
{
	if (!is_multi_file_window (window)
	    && (is_merged_trash_directory (get_target_file (window)) ||
		is_computer_directory (get_target_file (window)) ||
		is_network_directory (get_target_file (window)) ||
		is_burn_directory (get_target_file (window)))) {
		return FALSE;
	}

	return TRUE;
}

static gboolean
should_show_location_info (FMPropertiesWindow *window)
{
	if (!is_multi_file_window (window)
	    && (is_merged_trash_directory (get_target_file (window)) ||
		is_computer_directory (get_target_file (window)) ||
		is_network_directory (get_target_file (window)) ||
		is_burn_directory (get_target_file (window)))) {
		return FALSE;
	}

	return TRUE;
}

static gboolean
should_show_accessed_date (FMPropertiesWindow *window)
{
	/* Accessed date for directory seems useless. If we some
	 * day decide that it is useful, we should separately
	 * consider whether it's useful for "trash:".
	 */
	if (file_list_all_directories (window->details->target_files)
	    || is_multi_file_window (window))
 	{
		return FALSE;
	}

	return TRUE;
}

static gboolean
should_show_modified_date (FMPropertiesWindow *window)
{
    CajaFile *file;

    if (is_multi_file_window (window))
        return FALSE;

    file = get_original_file (window);
    if ((file != NULL) && caja_file_can_unmount (file))
        return FALSE;

    return TRUE;
}

static gboolean
should_show_link_target (FMPropertiesWindow *window)
{
	if (!is_multi_file_window (window)
	    && caja_file_is_symbolic_link (get_target_file (window))) {
		return TRUE;
	}

	return FALSE;
}

static gboolean
should_show_free_space (FMPropertiesWindow *window)
{

	if (!is_multi_file_window (window)
	    && (is_merged_trash_directory (get_target_file (window)) ||
		is_computer_directory (get_target_file (window)) ||
		is_network_directory (get_target_file (window)) ||
		is_burn_directory (get_target_file (window)))) {
		return FALSE;
	}

	if (file_list_all_directories (window->details->target_files)) {
		return TRUE;
	}

	return FALSE;
}

static gboolean
should_show_volume_usage (FMPropertiesWindow *window)
{
	CajaFile 		*file;
	gboolean 		success = FALSE;

	if (is_multi_file_window (window)) {
		return FALSE;
	}

	file = get_original_file (window);

	if (file == NULL) {
		return FALSE;
	}

	if (caja_file_can_unmount (file)) {
		return TRUE;
	}

#ifdef TODO_GIO
	/* Look at is_mountpoint for activation uri */
#endif
	return success;
}

static void
paint_used_legend (GtkWidget *widget,
                   cairo_t *cr,
                   gpointer data)
{
	FMPropertiesWindow *window;
	gint width, height;
	GtkAllocation allocation;

	gtk_widget_get_allocation (widget, &allocation);

  	width  = allocation.width;
  	height = allocation.height;

	window = FM_PROPERTIES_WINDOW (data);

	cairo_rectangle  (cr,
			  2,
			  2,
			  width - 4,
			  height - 4);

	gdk_cairo_set_source_rgba (cr, &window->details->used_color);
	cairo_fill_preserve (cr);

	gdk_cairo_set_source_rgba (cr, &window->details->used_stroke_color);
	cairo_stroke (cr);
}

static void
paint_free_legend (GtkWidget *widget,
                   cairo_t *cr, gpointer data)
{
	FMPropertiesWindow *window;
	gint width, height;
	GtkAllocation allocation;

	window = FM_PROPERTIES_WINDOW (data);
	gtk_widget_get_allocation (widget, &allocation);

  	width  = allocation.width;
  	height = allocation.height;

	cairo_rectangle (cr,
			 2,
			 2,
			 width - 4,
			 height - 4);

	gdk_cairo_set_source_rgba (cr, &window->details->free_color);
	cairo_fill_preserve(cr);

	gdk_cairo_set_source_rgba (cr, &window->details->free_stroke_color);
	cairo_stroke (cr);
}

static void
paint_pie_chart (GtkWidget *widget,
                 cairo_t *cr,
                 gpointer data)
{

  	FMPropertiesWindow *window;
	gint width, height;
	double free, used;
	double angle1, angle2, split, xc, yc, radius;
	GtkAllocation allocation;

	window = FM_PROPERTIES_WINDOW (data);
	gtk_widget_get_allocation (widget, &allocation);

	width  = allocation.width;
  	height = allocation.height;

	free = (double)window->details->volume_free / (double)window->details->volume_capacity;
	used =  1.0 - free;

	angle1 = free * 2 * G_PI;
	angle2 = used * 2 * G_PI;
	split = (2 * G_PI - angle1) * .5;
	xc = width / 2;
	yc = height / 2;

	if (width < height) {
		radius = width / 2 - 8;
	} else {
		radius = height / 2 - 8;
	}

	if (angle1 != 2 * G_PI && angle1 != 0) {
		angle1 = angle1 + split;
	}

	if (angle2 != 2 * G_PI && angle2 != 0) {
		angle2 = angle2 - split;
	}

	if (used > 0) {
		if (free != 0) {
			cairo_move_to (cr,xc,yc);
		}

		cairo_arc (cr, xc, yc, radius, angle1, angle2);

		if (free != 0) {
			cairo_line_to (cr,xc,yc);
		}

		gdk_cairo_set_source_rgba (cr, &window->details->used_color);
		cairo_fill_preserve (cr);

		gdk_cairo_set_source_rgba (cr, &window->details->used_stroke_color);

		cairo_stroke (cr);
	}

	if (free > 0) {
		if (used != 0) {
			cairo_move_to (cr,xc,yc);
		}

		cairo_arc_negative (cr, xc, yc, radius, angle1, angle2);

		if (used != 0) {
			cairo_line_to (cr,xc,yc);
		}

		gdk_cairo_set_source_rgba (cr, &window->details->free_color);
		cairo_fill_preserve(cr);

		gdk_cairo_set_source_rgba (cr, &window->details->free_stroke_color);

		cairo_stroke (cr);
	}
}

/* Copied from gtk/gtkstyle.c */

static void
rgb_to_hls (gdouble *r,
            gdouble *g,
            gdouble *b)
{
  gdouble min;
  gdouble max;
  gdouble red;
  gdouble green;
  gdouble blue;
  gdouble h, l, s;
  gdouble delta;

  red = *r;
  green = *g;
  blue = *b;

  if (red > green)
    {
      if (red > blue)
        max = red;
      else
        max = blue;

      if (green < blue)
        min = green;
      else
        min = blue;
    }
  else
    {
      if (green > blue)
        max = green;
      else
        max = blue;

      if (red < blue)
        min = red;
      else
        min = blue;
    }

  l = (max + min) / 2;
  s = 0;
  h = 0;

  if (max != min)
    {
      if (l <= 0.5)
        s = (max - min) / (max + min);
      else
        s = (max - min) / (2 - max - min);

      delta = max -min;
      if (red == max)
        h = (green - blue) / delta;
      else if (green == max)
        h = 2 + (blue - red) / delta;
      else if (blue == max)
        h = 4 + (red - green) / delta;

      h *= 60;
      if (h < 0.0)
        h += 360;
    }

  *r = h;
  *g = l;
  *b = s;
}

static void
hls_to_rgb (gdouble *h,
            gdouble *l,
            gdouble *s)
{
  gdouble hue;
  gdouble lightness;
  gdouble saturation;
  gdouble m1, m2;
  gdouble r, g, b;

  lightness = *l;
  saturation = *s;

  if (lightness <= 0.5)
    m2 = lightness * (1 + saturation);
  else
    m2 = lightness + saturation - lightness * saturation;
  m1 = 2 * lightness - m2;

  if (saturation == 0)
    {
      *h = lightness;
      *l = lightness;
      *s = lightness;
    }
  else
    {
      hue = *h + 120;
      while (hue > 360)
        hue -= 360;
      while (hue < 0)
        hue += 360;

      if (hue < 60)
        r = m1 + (m2 - m1) * hue / 60;
      else if (hue < 180)
        r = m2;
      else if (hue < 240)
        r = m1 + (m2 - m1) * (240 - hue) / 60;
      else
        r = m1;

      hue = *h;
      while (hue > 360)
        hue -= 360;
      while (hue < 0)
        hue += 360;

      if (hue < 60)
        g = m1 + (m2 - m1) * hue / 60;
      else if (hue < 180)
        g = m2;
      else if (hue < 240)
        g = m1 + (m2 - m1) * (240 - hue) / 60;
      else
        g = m1;

      hue = *h - 120;
      while (hue > 360)
        hue -= 360;
      while (hue < 0)
        hue += 360;

      if (hue < 60)
        b = m1 + (m2 - m1) * hue / 60;
      else if (hue < 180)
        b = m2;
      else if (hue < 240)
        b = m1 + (m2 - m1) * (240 - hue) / 60;
      else
        b = m1;

      *h = r;
      *l = g;
      *s = b;
    }
}
static void
_pie_style_shade (GdkRGBA *a,
                  GdkRGBA *b,
                  gdouble   k)
{
  gdouble red;
  gdouble green;
  gdouble blue;

  red = a->red;
  green = a->green;
  blue = a->blue;

  rgb_to_hls (&red, &green, &blue);

  green *= k;
  if (green > 1.0)
    green = 1.0;
  else if (green < 0.0)
    green = 0.0;

  blue *= k;
  if (blue > 1.0)
    blue = 1.0;
  else if (blue < 0.0)
    blue = 0.0;

  hls_to_rgb (&red, &green, &blue);

  b->red = red;
  b->green = green;
  b->blue = blue;
  b->alpha = a->alpha;
}

static GtkWidget*
create_pie_widget (FMPropertiesWindow *window)
{
	CajaFile		*file;
	GtkGrid                 *grid;
	GtkStyleContext		*style;

	GtkWidget 		*pie_canvas;
	GtkWidget 		*used_canvas;
	GtkWidget 		*used_label;
	GtkWidget 		*free_canvas;
	GtkWidget 		*free_label;
	GtkWidget 		*capacity_label;
	GtkWidget 		*fstype_label;
	gchar			*capacity;
	gchar 			*used;
	gchar 			*free;
	gchar			*uri;
	gchar			*concat;
	GFile *location;
	GFileInfo *info;

	if (g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_USE_IEC_UNITS)) {
		capacity = g_format_size_full(window->details->volume_capacity, G_FORMAT_SIZE_IEC_UNITS);
		free = g_format_size_full(window->details->volume_free, G_FORMAT_SIZE_IEC_UNITS);
		used = g_format_size_full(window->details->volume_capacity - window->details->volume_free, G_FORMAT_SIZE_IEC_UNITS);
	}
	else {
		capacity = g_format_size(window->details->volume_capacity);
		free = g_format_size(window->details->volume_free);
		used = g_format_size(window->details->volume_capacity - window->details->volume_free);
	}

	file = get_original_file (window);

	uri = caja_file_get_activation_uri (file);

	grid = GTK_GRID (gtk_grid_new ());
	gtk_container_set_border_width (GTK_CONTAINER (grid), 5);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 5);
	style = gtk_widget_get_style_context (GTK_WIDGET (grid));

	if (!gtk_style_context_lookup_color (style, "chart_rgba_1", &window->details->used_color)) {

		window->details->used_color.red = USED_FILL_R;
		window->details->used_color.green = USED_FILL_G;
		window->details->used_color.blue = USED_FILL_B;
		window->details->used_color.alpha = 1;

	}

	if (!gtk_style_context_lookup_color (style, "chart_rgba_2", &window->details->free_color)) {
		window->details->free_color.red = FREE_FILL_R;
		window->details->free_color.green = FREE_FILL_G;
		window->details->free_color.blue = FREE_FILL_B;
		window->details->free_color.alpha = 1;

	}

	_pie_style_shade (&window->details->used_color, &window->details->used_stroke_color, 0.7);
	_pie_style_shade (&window->details->free_color, &window->details->free_stroke_color, 0.7);

	pie_canvas = gtk_drawing_area_new ();
	gtk_widget_set_size_request (pie_canvas, 200, 200);

	used_canvas = gtk_drawing_area_new ();

	gtk_widget_set_valign (used_canvas, GTK_ALIGN_CENTER);
	gtk_widget_set_halign (used_canvas, GTK_ALIGN_CENTER);

	gtk_widget_set_size_request (used_canvas, 20, 20);
	/* Translators: "used" refers to the capacity of the filesystem */
	concat = g_strconcat (used, " ", _("used"), NULL);
	used_label = gtk_label_new (concat);
	g_free (concat);

	free_canvas = gtk_drawing_area_new ();

	gtk_widget_set_valign (free_canvas, GTK_ALIGN_CENTER);
	gtk_widget_set_halign (free_canvas, GTK_ALIGN_CENTER);

	gtk_widget_set_size_request (free_canvas, 20, 20);
	/* Translators: "free" refers to the capacity of the filesystem */
	concat = g_strconcat (free, " ", _("free"), NULL);
	free_label = gtk_label_new (concat);
	g_free (concat);

	concat = g_strconcat (_("Total capacity:"), " ", capacity, NULL);
	capacity_label = gtk_label_new (concat);
	g_free (concat);
	fstype_label = gtk_label_new (NULL);

	location = g_file_new_for_uri (uri);
	info = g_file_query_filesystem_info (location, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
					     NULL, NULL);
	if (info) {
		const char *fs_type;

		fs_type = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE);

		if (fs_type != NULL) {
			concat = g_strconcat (_("Filesystem type:"), " ", fs_type, NULL);
			gtk_label_set_text (GTK_LABEL (fstype_label), concat);
			g_free (concat);
		}

		g_object_unref (info);
	}
	g_object_unref (location);

	g_free (uri);
	g_free (capacity);
	g_free (used);
	g_free (free);

	gtk_container_add_with_properties (GTK_CONTAINER (grid), pie_canvas,
					   "height", 4,
					   NULL);
	gtk_grid_attach_next_to (grid, used_canvas, pie_canvas,
				 GTK_POS_RIGHT, 1, 1);
	gtk_grid_attach_next_to (grid, used_label, used_canvas,
				 GTK_POS_RIGHT, 1, 1);

	gtk_grid_attach_next_to (grid, free_canvas, used_canvas,
				 GTK_POS_BOTTOM, 1, 1);
	gtk_grid_attach_next_to (grid, free_label, free_canvas,
				 GTK_POS_RIGHT, 1, 1);

	gtk_grid_attach_next_to (grid, capacity_label, free_canvas,
				 GTK_POS_BOTTOM, 2, 1);
	gtk_grid_attach_next_to (grid, fstype_label, capacity_label,
				 GTK_POS_BOTTOM, 2, 1);

	g_signal_connect (pie_canvas, "draw",
	                  G_CALLBACK (paint_pie_chart), window);
	g_signal_connect (used_canvas, "draw",
	                  G_CALLBACK (paint_used_legend), window);
	g_signal_connect (free_canvas, "draw",
	                  G_CALLBACK (paint_free_legend), window);

	return GTK_WIDGET (grid);

}

static GtkWidget*
create_volume_usage_widget (FMPropertiesWindow *window)
{
	GtkWidget *piewidget;
	gchar *uri;
	CajaFile *file;
	GFile *location;
	GFileInfo *info;

	file = get_original_file (window);

	uri = caja_file_get_activation_uri (file);

	location = g_file_new_for_uri (uri);
	info = g_file_query_filesystem_info (location, "filesystem::*", NULL, NULL);

	if (info) {
		window->details->volume_capacity = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
		window->details->volume_free = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);

		g_object_unref (info);
	} else {
		window->details->volume_capacity = 0;
		window->details->volume_free = 0;
	}

	g_object_unref (location);

	piewidget = create_pie_widget (window);

        gtk_widget_show_all (piewidget);

	return piewidget;
}

static void
create_basic_page (FMPropertiesWindow *window)
{
	GtkGrid *grid;
	GtkWidget *icon_pixmap_widget;
	GtkWidget *hbox, *vbox;

	hbox = create_page_with_hbox (window->details->notebook, _("Basic"));

	/* Icon pixmap */

	icon_pixmap_widget = create_image_widget (
		window, should_show_custom_icon_buttons (window));

	gtk_widget_set_halign (icon_pixmap_widget, GTK_ALIGN_END);
	gtk_widget_set_valign (icon_pixmap_widget, GTK_ALIGN_START);
	gtk_widget_show (icon_pixmap_widget);

	gtk_box_pack_start (GTK_BOX (hbox), icon_pixmap_widget, FALSE, FALSE, 0);

	window->details->icon_chooser = NULL;

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

	gtk_widget_show (vbox);
	gtk_container_add (GTK_CONTAINER (hbox), vbox);

	grid = GTK_GRID (create_grid_with_standard_properties ());
	gtk_box_pack_start (GTK_BOX (vbox), GTK_WIDGET (grid), FALSE, FALSE, 0);
	window->details->basic_grid = grid;

	/* Name label.  The text will be determined in update_name_field */
	window->details->name_label = attach_title_field (grid, NULL);

	/* Name field */
	window->details->name_field = NULL;
	update_name_field (window);

	/* Start with name field selected, if it's an entry. */
	if (CAJA_IS_ENTRY (window->details->name_field)) {
		caja_entry_select_all (CAJA_ENTRY (window->details->name_field));
		gtk_widget_grab_focus (GTK_WIDGET (window->details->name_field));
	}

	if (fm_ditem_page_should_show (window->details->target_files)) {
		GtkSizeGroup *label_size_group;
		GtkWidget *box;

		label_size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
		gtk_size_group_add_widget (label_size_group,
					   GTK_WIDGET (window->details->name_label));
		box = fm_ditem_page_make_box (label_size_group,
					      window->details->target_files);

		gtk_grid_attach_next_to (window->details->basic_grid, box,
					 GTK_WIDGET (window->details->name_label),
					 GTK_POS_BOTTOM, 2, 1);
	}

	if (should_show_file_type (window)) {
		append_title_value_pair (window,
					 grid, _("Type:"),
					 "type",
					 INCONSISTENT_STATE_STRING,
					 FALSE);
	}

	if (should_show_link_target (window)) {
		append_title_and_ellipsizing_value (window, grid,
						    _("Link target:"),
						    "link_target",
						    INCONSISTENT_STATE_STRING,
						    FALSE);
	}

	if (is_multi_file_window (window) ||
	    caja_file_is_directory (get_target_file (window))) {
		append_directory_contents_fields (window, grid);
	} else {
		append_title_value_pair (window, grid, _("Size:"),
					 "size_detail",
					 INCONSISTENT_STATE_STRING,
					 FALSE);
		append_title_value_pair (window, grid, _("Size on Disk:"),
					 "size_on_disk_detail",
					 INCONSISTENT_STATE_STRING,
					 FALSE);
	}

	append_blank_row (grid);

	if (should_show_location_info (window)) {
		append_title_and_ellipsizing_value (window, grid, _("Location:"),
						    "where",
						    INCONSISTENT_STATE_STRING,
						    TRUE);

		append_title_and_ellipsizing_value (window, grid,
						    _("Volume:"),
						    "volume",
						    INCONSISTENT_STATE_STRING,
						    FALSE);
	}

	if (should_show_accessed_date (window)
	   || should_show_modified_date (window))
	{
		append_blank_row (grid);
	}

	if (should_show_accessed_date (window))
	{
		append_title_value_pair (window, grid, _("Accessed:"),
					 "date_accessed",
					 INCONSISTENT_STATE_STRING,
					 FALSE);
	}

	if (should_show_modified_date (window))
	{
		append_title_value_pair (window, grid, _("Modified:"),
					 "date_modified",
					 INCONSISTENT_STATE_STRING,
					 FALSE);
		append_title_value_pair (window, grid, _("Created:"),
		                         "date_created",
		                         INCONSISTENT_STATE_STRING,
		                         FALSE);
	}

	if (should_show_free_space (window)) {
		append_blank_row (grid);

		append_title_value_pair (window, grid, _("Free space:"),
					 "free_space",
					 INCONSISTENT_STATE_STRING,
					 FALSE);
	}

	if (should_show_volume_usage (window)) {
		GtkWidget *volume_usage;

		volume_usage = create_volume_usage_widget (window);
		gtk_container_add_with_properties (GTK_CONTAINER (grid), volume_usage,
						   "width", 2,
						   NULL);
	}
}

static GHashTable *
get_initial_emblems (GList *files)
{
	GHashTable *ret;
	GList *l;

	ret = g_hash_table_new_full (g_direct_hash,
				     g_direct_equal,
				     NULL,
				     (GDestroyNotify) eel_g_list_free_deep);

	for (l = files; l != NULL; l = l->next) {
		CajaFile *file;
		GList *keywords;

		file = CAJA_FILE (l->data);

		keywords = caja_file_get_keywords (file);
		g_hash_table_insert (ret, file, keywords);
	}

	return ret;
}

static gboolean
files_has_directory (FMPropertiesWindow *window)
{
	GList *l;

	for (l = window->details->target_files; l != NULL; l = l->next) {
		CajaFile *file;
		file = CAJA_FILE (l->data);
		if (caja_file_is_directory (file)) {
			return TRUE;
		}

	}

	return FALSE;
}

static gboolean
files_has_changable_permissions_directory (FMPropertiesWindow *window)
{
	GList *l;

	for (l = window->details->target_files; l != NULL; l = l->next) {
		CajaFile *file;
		file = CAJA_FILE (l->data);
		if (caja_file_is_directory (file) &&
		    caja_file_can_get_permissions (file) &&
		    caja_file_can_set_permissions (file)) {
			return TRUE;
		}

	}

	return FALSE;
}

static gboolean
files_has_file (FMPropertiesWindow *window)
{
	GList *l;

	for (l = window->details->target_files; l != NULL; l = l->next) {
		CajaFile *file;
		file = CAJA_FILE (l->data);
		if (!caja_file_is_directory (file)) {
			return TRUE;
		}

	}

	return FALSE;
}

static void
create_emblems_page (FMPropertiesWindow *window)
{
	GtkWidget *emblems_table, *button, *scroller;
	GdkPixbuf *pixbuf;
	char *label;
	GList *icons, *l;
	CajaIconInfo *info;
	gint scale;

	/* The emblems wrapped table */
	scroller = eel_scrolled_wrap_table_new (TRUE, GTK_SHADOW_NONE, &emblems_table);

	gtk_container_set_border_width (GTK_CONTAINER (emblems_table), 12);

	/* stop GTK 3.22 builds from ballooning the properties dialog to full screen height */
	gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroller), 300);

	gtk_widget_show (scroller);

	gtk_notebook_append_page (window->details->notebook,
				  scroller, gtk_label_new (_("Emblems")));

	icons = caja_emblem_list_available ();
	scale = gtk_widget_get_scale_factor (scroller);

	window->details->initial_emblems = get_initial_emblems (window->details->original_files);

	l = icons;
	while (l != NULL) {
		char *emblem_name;

		emblem_name = l->data;
		l = l->next;

		if (!caja_emblem_should_show_in_list (emblem_name)) {
			continue;
		}

		info = caja_icon_info_lookup_from_name (emblem_name, CAJA_ICON_SIZE_SMALL, scale);
		pixbuf = caja_icon_info_get_pixbuf_nodefault_at_size (info, CAJA_ICON_SIZE_SMALL);

		if (pixbuf == NULL) {
			continue;
		}

		label = g_strdup (caja_icon_info_get_display_name (info));
		g_object_unref (info);

		if (label == NULL) {
			label = caja_emblem_get_keyword_from_icon_name (emblem_name);
		}

		button = eel_labeled_image_check_button_new (label, pixbuf);
		eel_labeled_image_set_fixed_image_height (EEL_LABELED_IMAGE (gtk_bin_get_child (GTK_BIN (button))), STANDARD_EMBLEM_HEIGHT * scale);
		eel_labeled_image_set_spacing (EEL_LABELED_IMAGE (gtk_bin_get_child (GTK_BIN (button))), EMBLEM_LABEL_SPACING * scale);

		g_free (label);
		g_object_unref (pixbuf);

		/* Attach parameters and signal handler. */
		g_object_set_data_full (G_OBJECT (button), "caja_emblem_name",
					caja_emblem_get_keyword_from_icon_name (emblem_name), g_free);

		window->details->emblem_buttons =
			g_list_append (window->details->emblem_buttons,
				       button);

		g_signal_connect_object (button, "toggled",
					 G_CALLBACK (emblem_button_toggled),
					 G_OBJECT (window),
					 0);

		gtk_container_add (GTK_CONTAINER (emblems_table), button);
	}
    	g_list_free_full (icons, g_free);
	gtk_widget_show_all (emblems_table);
}

static void
start_long_operation (FMPropertiesWindow *window)
{
	if (window->details->long_operation_underway == 0) {
		/* start long operation */
		GdkDisplay *display;
		GdkCursor * cursor;

		display = gtk_widget_get_display (GTK_WIDGET (window));
		cursor = gdk_cursor_new_for_display (display, GDK_WATCH);
		gdk_window_set_cursor (gtk_widget_get_window (GTK_WIDGET (window)), cursor);
		g_object_unref (cursor);
	}
	window->details->long_operation_underway ++;
}

static void
end_long_operation (FMPropertiesWindow *window)
{
	if (gtk_widget_get_window (GTK_WIDGET (window)) != NULL &&
	    window->details->long_operation_underway == 1) {
		/* finished !! */
		gdk_window_set_cursor (gtk_widget_get_window (GTK_WIDGET (window)), NULL);
	}
	window->details->long_operation_underway--;
}

static void
permission_change_callback (CajaFile *file,
			    GFile *res_loc,
			    GError *error,
			    gpointer callback_data)
{
	FMPropertiesWindow *window;
	g_assert (callback_data != NULL);

	window = FM_PROPERTIES_WINDOW (callback_data);
	end_long_operation (window);

	/* Report the error if it's an error. */
	fm_report_error_setting_permissions (file, error, NULL);

	g_object_unref (window);
}

static void
update_permissions (FMPropertiesWindow *window,
		    guint32 vfs_new_perm,
		    guint32 vfs_mask,
		    gboolean is_folder,
		    gboolean apply_to_both_folder_and_dir,
		    gboolean use_original)
{
	GList *l;

	for (l = window->details->target_files; l != NULL; l = l->next) {
		CajaFile *file;
		guint32 permissions;

		file = CAJA_FILE (l->data);

		if (!caja_file_can_get_permissions (file)) {
			continue;
		}

		if (!apply_to_both_folder_and_dir &&
		    ((caja_file_is_directory (file) && !is_folder) ||
		     (!caja_file_is_directory (file) && is_folder))) {
			continue;
		}

		permissions = caja_file_get_permissions (file);
		if (use_original) {
			gpointer ptr;
			if (g_hash_table_lookup_extended (window->details->initial_permissions,
							  file, NULL, &ptr)) {
				permissions = (permissions & ~vfs_mask) | (GPOINTER_TO_INT (ptr) & vfs_mask);
			}
		} else {
			permissions = (permissions & ~vfs_mask) | vfs_new_perm;
		}

		start_long_operation (window);
		g_object_ref (window);
		caja_file_set_permissions
			(file, permissions,
			 permission_change_callback,
			 window);
	}
}

static gboolean
initial_permission_state_consistent (FMPropertiesWindow *window,
				     guint32 mask,
				     gboolean is_folder,
				     gboolean both_folder_and_dir)
{
	GList *l;
	gboolean first;
	guint32 first_permissions;

	first = TRUE;
	first_permissions = 0;
	for (l = window->details->target_files; l != NULL; l = l->next) {
		CajaFile *file;
		guint32 permissions;

		file = l->data;

		if (!both_folder_and_dir &&
		    ((caja_file_is_directory (file) && !is_folder) ||
		     (!caja_file_is_directory (file) && is_folder))) {
			continue;
		}

		permissions = GPOINTER_TO_INT (g_hash_table_lookup (window->details->initial_permissions,
								    file));

		if (first) {
			if ((permissions & mask) != mask &&
			    (permissions & mask) != 0) {
				/* Not fully on or off -> inconsistent */
				return FALSE;
			}

			first_permissions = permissions;
			first = FALSE;

		} else if ((permissions & mask) != (first_permissions & mask)) {
			/* Not same permissions as first -> inconsistent */
			return FALSE;
		}
	}
	return TRUE;
}

static void
permission_button_toggled (GtkToggleButton *button,
			   FMPropertiesWindow *window)
{
	gboolean is_folder, is_special;
	guint32 permission_mask;
	gboolean inconsistent;
	gboolean on;

	permission_mask = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
							      "permission"));
	is_folder = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
							"is-folder"));
	is_special = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
							"is-special"));

	if (gtk_toggle_button_get_active (button)
	    && !gtk_toggle_button_get_inconsistent (button)) {
		/* Go to the initial state unless the initial state was
		   consistent, or we support recursive apply */
		inconsistent = TRUE;
		on = TRUE;

		if (!window->details->has_recursive_apply &&
		    initial_permission_state_consistent (window, permission_mask, is_folder, is_special)) {
			inconsistent = FALSE;
			on = TRUE;
		}
	} else if (gtk_toggle_button_get_inconsistent (button)
		   && !gtk_toggle_button_get_active (button)) {
		inconsistent = FALSE;
		on = TRUE;
	} else {
		inconsistent = FALSE;
		on = FALSE;
	}

	g_signal_handlers_block_by_func (G_OBJECT (button),
					 G_CALLBACK (permission_button_toggled),
					 window);

	gtk_toggle_button_set_active (button, on);
	gtk_toggle_button_set_inconsistent (button, inconsistent);

	g_signal_handlers_unblock_by_func (G_OBJECT (button),
					   G_CALLBACK (permission_button_toggled),
					   window);

	update_permissions (window,
			    on?permission_mask:0,
			    permission_mask,
			    is_folder,
			    is_special,
			    inconsistent);
}

static void
permission_button_update (FMPropertiesWindow *window,
			  GtkToggleButton *button)
{
	GList *l;
	gboolean all_set;
	gboolean all_unset;
	gboolean all_cannot_set;
	gboolean is_folder, is_special;
	gboolean no_match;
	gboolean sensitive;
	guint32 button_permission;

	if (gtk_toggle_button_get_inconsistent (button) &&
	    window->details->has_recursive_apply) {
		/* Never change from an inconsistent state if we have dirs, even
		 * if the current state is now consistent, because its a useful
		 * state for recursive apply.
		 */
		return;
	}

	button_permission = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
								"permission"));
	is_folder = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
							"is-folder"));
	is_special = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
							 "is-special"));

	all_set = TRUE;
	all_unset = TRUE;
	all_cannot_set = TRUE;
	no_match = TRUE;
	for (l = window->details->target_files; l != NULL; l = l->next) {
		CajaFile *file;
		guint32 file_permissions;

		file = CAJA_FILE (l->data);

		if (!caja_file_can_get_permissions (file)) {
			continue;
		}

		if (!is_special &&
		    ((caja_file_is_directory (file) && !is_folder) ||
		     (!caja_file_is_directory (file) && is_folder))) {
			continue;
		}

		no_match = FALSE;

		file_permissions = caja_file_get_permissions (file);

		if ((file_permissions & button_permission) == button_permission) {
			all_unset = FALSE;
		} else if ((file_permissions & button_permission) == 0) {
			all_set = FALSE;
		} else {
			all_unset = FALSE;
			all_set = FALSE;
		}

		if (caja_file_can_set_permissions (file)) {
			all_cannot_set = FALSE;
		}
	}

	sensitive = !all_cannot_set;
	if (!is_folder) {
		/* Don't insitive files when we have recursive apply */
		sensitive |= window->details->has_recursive_apply;
	}

	g_signal_handlers_block_by_func (G_OBJECT (button),
					 G_CALLBACK (permission_button_toggled),
					 window);

	gtk_toggle_button_set_active (button, !all_unset);
	/* if actually inconsistent, or default value for file buttons
	   if no files are selected. (useful for recursive apply) */
	gtk_toggle_button_set_inconsistent (button,
					    (!all_unset && !all_set) ||
					    (!is_folder && no_match));
	gtk_widget_set_sensitive (GTK_WIDGET (button), sensitive);

	g_signal_handlers_unblock_by_func (G_OBJECT (button),
					   G_CALLBACK (permission_button_toggled),
					   window);
}

static void
set_up_permissions_checkbox (FMPropertiesWindow *window,
			     GtkWidget *check_button,
			     guint32 permission,
			     gboolean is_folder)
{
	/* Load up the check_button with data we'll need when updating its state. */
        g_object_set_data (G_OBJECT (check_button), "permission",
			   GINT_TO_POINTER (permission));
        g_object_set_data (G_OBJECT (check_button), "properties_window",
			   window);
	g_object_set_data (G_OBJECT (check_button), "is-folder",
			   GINT_TO_POINTER (is_folder));

	window->details->permission_buttons =
		g_list_prepend (window->details->permission_buttons,
				check_button);

	g_signal_connect_object (check_button, "toggled",
				 G_CALLBACK (permission_button_toggled),
				 window,
				 0);
}

static GtkWidget *
add_permissions_checkbox_with_label (FMPropertiesWindow *window,
                                     GtkGrid *grid,
                                     GtkWidget *sibling,
                                     const char *label,
                                     guint32 permission_to_check,
                                     GtkLabel *label_for,
                                     gboolean is_folder)
{
	GtkWidget *check_button;
	gboolean a11y_enabled;

	check_button = gtk_check_button_new_with_mnemonic (label);
	gtk_widget_show (check_button);
	if (sibling) {
		gtk_grid_attach_next_to (grid, check_button, sibling,
					 GTK_POS_RIGHT, 1, 1);
	} else {
		gtk_container_add (GTK_CONTAINER (grid), check_button);
	}

	set_up_permissions_checkbox (window,
				     check_button,
				     permission_to_check,
				     is_folder);

	a11y_enabled = GTK_IS_ACCESSIBLE (gtk_widget_get_accessible (check_button));
	if (a11y_enabled && label_for != NULL) {
		eel_accessibility_set_up_label_widget_relation (GTK_WIDGET (label_for),
								check_button);
	}

	return check_button;
}

static GtkWidget *
add_permissions_checkbox (FMPropertiesWindow *window,
                          GtkGrid *grid,
                          GtkWidget *sibling,
                          CheckboxType type,
                          guint32 permission_to_check,
                          GtkLabel *label_for,
                          gboolean is_folder)
{
	const gchar *label;

	if (type == PERMISSIONS_CHECKBOXES_READ) {
		label = _("_Read");
	} else if (type == PERMISSIONS_CHECKBOXES_WRITE) {
		label = _("_Write");
	} else {
		label = _("E_xecute");
	}

	return add_permissions_checkbox_with_label (window, grid,
						    sibling,
						    label,
						    permission_to_check,
						    label_for,
						    is_folder);
}

enum {
	UNIX_PERM_SUID = S_ISUID,
	UNIX_PERM_SGID = S_ISGID,
	UNIX_PERM_STICKY = 01000,	/* S_ISVTX not defined on all systems */
	UNIX_PERM_USER_READ = S_IRUSR,
	UNIX_PERM_USER_WRITE = S_IWUSR,
	UNIX_PERM_USER_EXEC = S_IXUSR,
	UNIX_PERM_USER_ALL = S_IRUSR | S_IWUSR | S_IXUSR,
	UNIX_PERM_GROUP_READ = S_IRGRP,
	UNIX_PERM_GROUP_WRITE = S_IWGRP,
	UNIX_PERM_GROUP_EXEC = S_IXGRP,
	UNIX_PERM_GROUP_ALL = S_IRGRP | S_IWGRP | S_IXGRP,
	UNIX_PERM_OTHER_READ = S_IROTH,
	UNIX_PERM_OTHER_WRITE = S_IWOTH,
	UNIX_PERM_OTHER_EXEC = S_IXOTH,
	UNIX_PERM_OTHER_ALL = S_IROTH | S_IWOTH | S_IXOTH
};

typedef enum {
	PERMISSION_READ  = (1<<0),
	PERMISSION_WRITE = (1<<1),
	PERMISSION_EXEC  = (1<<2)
} PermissionValue;

typedef enum {
	PERMISSION_USER,
	PERMISSION_GROUP,
	PERMISSION_OTHER
} PermissionType;

static guint32 vfs_perms[3][3] = {
	{UNIX_PERM_USER_READ, UNIX_PERM_USER_WRITE, UNIX_PERM_USER_EXEC},
	{UNIX_PERM_GROUP_READ, UNIX_PERM_GROUP_WRITE, UNIX_PERM_GROUP_EXEC},
	{UNIX_PERM_OTHER_READ, UNIX_PERM_OTHER_WRITE, UNIX_PERM_OTHER_EXEC},
};

static guint32
permission_to_vfs (PermissionType type, PermissionValue perm)
{
	guint32 vfs_perm;
	g_assert (type >= 0 && type < 3);

	vfs_perm = 0;
	if (perm & PERMISSION_READ) {
		vfs_perm |= vfs_perms[type][0];
	}
	if (perm & PERMISSION_WRITE) {
		vfs_perm |= vfs_perms[type][1];
	}
	if (perm & PERMISSION_EXEC) {
		vfs_perm |= vfs_perms[type][2];
	}

	return vfs_perm;
}

static PermissionValue
permission_from_vfs (PermissionType type, guint32 vfs_perm)
{
	PermissionValue perm;
	g_assert (type >= 0 && type < 3);

	perm = 0;
	if (vfs_perm & vfs_perms[type][0]) {
		perm |= PERMISSION_READ;
	}
	if (vfs_perm & vfs_perms[type][1]) {
		perm |= PERMISSION_WRITE;
	}
	if (vfs_perm & vfs_perms[type][2]) {
		perm |= PERMISSION_EXEC;
	}

	return perm;
}

static void
permission_combo_changed (GtkWidget *combo, FMPropertiesWindow *window)
{
	GtkTreeIter iter;
	GtkTreeModel *model;
	gboolean is_folder, use_original;
	PermissionType type;
	int new_perm, mask;
	guint32 vfs_new_perm, vfs_mask;

	is_folder = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (combo), "is-folder"));
	type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (combo), "permission-type"));

	if (is_folder) {
		mask = PERMISSION_READ|PERMISSION_WRITE|PERMISSION_EXEC;
	} else {
		mask = PERMISSION_READ|PERMISSION_WRITE;
	}

	vfs_mask = permission_to_vfs (type, mask);

	model = gtk_combo_box_get_model (GTK_COMBO_BOX (combo));

	if (!gtk_combo_box_get_active_iter (GTK_COMBO_BOX (combo),  &iter)) {
		return;
	}
	gtk_tree_model_get (model, &iter, 1, &new_perm, 2, &use_original, -1);
	vfs_new_perm = permission_to_vfs (type, new_perm);

	update_permissions (window, vfs_new_perm, vfs_mask,
			    is_folder, FALSE, use_original);
}

static void
permission_combo_add_multiple_choice (GtkComboBox *combo, GtkTreeIter *iter)
{
	GtkTreeModel *model;
	GtkListStore *store;
	gboolean found;

	model = gtk_combo_box_get_model (combo);
	store = GTK_LIST_STORE (model);

	found = FALSE;
	gtk_tree_model_get_iter_first (model, iter);
	do {
		gboolean multi;
		gtk_tree_model_get (model, iter, 2, &multi, -1);

		if (multi) {
			found = TRUE;
			break;
		}
	} while (gtk_tree_model_iter_next (model, iter));

	if (!found) {
		gtk_list_store_append (store, iter);
		gtk_list_store_set (store, iter, 0, "---", 1, 0, 2, TRUE, -1);
	}
}

static void
permission_combo_update (FMPropertiesWindow *window,
			 GtkComboBox *combo)
{
	PermissionType type;
	PermissionValue perm, all_dir_perm, all_file_perm, all_perm;
	gboolean is_folder, no_files, no_dirs, all_file_same, all_dir_same, all_same;
	gboolean all_dir_cannot_set, all_file_cannot_set, sensitive;
	GtkTreeIter iter;
	int mask;
	GtkTreeModel *model;
	GtkListStore *store;
	GList *l;
	gboolean is_multi;

	model = gtk_combo_box_get_model (combo);

	is_folder = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (combo), "is-folder"));
	type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (combo), "permission-type"));

	is_multi = FALSE;
	if (gtk_combo_box_get_active_iter (GTK_COMBO_BOX (combo),  &iter)) {
		gtk_tree_model_get (model, &iter, 2, &is_multi, -1);
	}

	if (is_multi && window->details->has_recursive_apply) {
		/* Never change from an inconsistent state if we have dirs, even
		 * if the current state is now consistent, because its a useful
		 * state for recursive apply.
		 */
		return;
	}

	no_files = TRUE;
	no_dirs = TRUE;
	all_dir_same = TRUE;
	all_file_same = TRUE;
	all_dir_perm = 0;
	all_file_perm = 0;
	all_dir_cannot_set = TRUE;
	all_file_cannot_set = TRUE;

	for (l = window->details->target_files; l != NULL; l = l->next) {
		CajaFile *file;
		guint32 file_permissions;

		file = CAJA_FILE (l->data);

		if (!caja_file_can_get_permissions (file)) {
			continue;
		}

		if (caja_file_is_directory (file)) {
			mask = PERMISSION_READ|PERMISSION_WRITE|PERMISSION_EXEC;
		} else {
			mask = PERMISSION_READ|PERMISSION_WRITE;
		}

		file_permissions = caja_file_get_permissions (file);

		perm = permission_from_vfs (type, file_permissions) & mask;

		if (caja_file_is_directory (file)) {
			if (no_dirs) {
				all_dir_perm = perm;
				no_dirs = FALSE;
			} else if (perm != all_dir_perm) {
				all_dir_same = FALSE;
			}

			if (caja_file_can_set_permissions (file)) {
				all_dir_cannot_set = FALSE;
			}
		} else {
			if (no_files) {
				all_file_perm = perm;
				no_files = FALSE;
			} else if (perm != all_file_perm) {
				all_file_same = FALSE;
			}

			if (caja_file_can_set_permissions (file)) {
				all_file_cannot_set = FALSE;
			}
		}
	}

	if (is_folder) {
		all_same = all_dir_same;
		all_perm = all_dir_perm;
	} else {
		all_same = all_file_same && !no_files;
		all_perm = all_file_perm;
	}

	store = GTK_LIST_STORE (model);
	if (all_same) {
		gboolean found;

		found = FALSE;
		gtk_tree_model_get_iter_first (model, &iter);
		do {
			int current_perm;
			gtk_tree_model_get (model, &iter, 1, &current_perm, -1);

			if (((PermissionValue) current_perm) == all_perm) {
				found = TRUE;
				break;
			}
		} while (gtk_tree_model_iter_next (model, &iter));

		if (!found) {
			GString *str;
			str = g_string_new ("");

			if (!(all_perm & PERMISSION_READ)) {
				/* Translators: this gets concatenated to "no read",
				 * "no access", etc. (see following strings)
				 */
				g_string_append (str, _("no "));
			}
			if (is_folder) {
				g_string_append (str, _("list"));
			} else {
				g_string_append (str, _("read"));
			}

			g_string_append (str, ", ");

			if (!(all_perm & PERMISSION_WRITE)) {
				g_string_append (str, _("no "));
			}
			if (is_folder) {
				g_string_append (str, _("create/delete"));
			} else {
				g_string_append (str, _("write"));
			}

			if (is_folder) {
				g_string_append (str, ", ");

				if (!(all_perm & PERMISSION_EXEC)) {
					g_string_append (str, _("no "));
				}
				g_string_append (str, _("access"));
			}

			gtk_list_store_append (store, &iter);
			gtk_list_store_set (store, &iter,
					    0, str->str,
					    1, all_perm, -1);

			g_string_free (str, TRUE);
		}
	} else {
		permission_combo_add_multiple_choice (combo, &iter);
	}

	g_signal_handlers_block_by_func (G_OBJECT (combo),
					 G_CALLBACK (permission_combo_changed),
					 window);

	gtk_combo_box_set_active_iter (combo, &iter);

	/* Also enable if no files found (for recursive
	   file changes when only selecting folders) */
	if (is_folder) {
		sensitive = !all_dir_cannot_set;
	} else {
		sensitive = !all_file_cannot_set ||
			window->details->has_recursive_apply;
	}
	gtk_widget_set_sensitive (GTK_WIDGET (combo), sensitive);

	g_signal_handlers_unblock_by_func (G_OBJECT (combo),
					   G_CALLBACK (permission_combo_changed),
					   window);

}

static void
add_permissions_combo_box (FMPropertiesWindow *window, GtkGrid *grid,
			   PermissionType type, gboolean is_folder,
			   gboolean short_label)
{
	GtkWidget *combo;
	GtkLabel *label;
	GtkListStore *store;
	GtkCellRenderer *cell;
	GtkTreeIter iter;
	AtkObject *atk_object;
	static const gchar *const descriptions[4][3] = {
		{ N_("Access:"), N_("Folder access:"), N_("File access:") },
		/* As the UI lacks semantic grouping, provide more context for accessibility */
		{ N_("User access:"), N_("User folder access:"), N_("User file access:") },
		{ N_("Group access:"), N_("Group folder access:"), N_("Group file access:") },
		{ N_("Others access:"), N_("Others folder access:"), N_("Others file access:") }
	};
	const guint group = short_label ? 0 : is_folder ? 1 : 2;

	g_return_if_fail (type + 1 < G_N_ELEMENTS (descriptions));

	label = attach_title_field (grid, _(descriptions[0][group]));

	atk_object = gtk_widget_get_accessible (GTK_WIDGET (label));
	if (GTK_IS_ACCESSIBLE (atk_object)) {
		atk_object_set_name (atk_object, _(descriptions[type + 1][group]));
	}

	store = gtk_list_store_new (3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN);
	combo = gtk_combo_box_new_with_model (GTK_TREE_MODEL (store));

	g_object_set_data (G_OBJECT (combo), "is-folder", GINT_TO_POINTER (is_folder));
	g_object_set_data (G_OBJECT (combo), "permission-type", GINT_TO_POINTER (type));

	if (is_folder) {
		if (type != PERMISSION_USER) {
			gtk_list_store_append (store, &iter);
			/* Translators: this is referred to the permissions
			 * the user has in a directory.
			 */
			gtk_list_store_set (store, &iter, 0, _("None"), 1, 0, -1);
		}
		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter, 0, _("List files only"), 1, PERMISSION_READ, -1);
		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter, 0, _("Access files"), 1, PERMISSION_READ|PERMISSION_EXEC, -1);
		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter, 0, _("Create and delete files"), 1, PERMISSION_READ|PERMISSION_EXEC|PERMISSION_WRITE, -1);
	} else {
		if (type != PERMISSION_USER) {
			gtk_list_store_append (store, &iter);
			gtk_list_store_set (store, &iter, 0, _("None"), 1, 0, -1);
		}
		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter, 0, _("Read-only"), 1, PERMISSION_READ, -1);
		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter, 0, _("Read and write"), 1, PERMISSION_READ|PERMISSION_WRITE, -1);
	}
	if (window->details->has_recursive_apply) {
		permission_combo_add_multiple_choice (GTK_COMBO_BOX (combo), &iter);
	}

	g_object_unref (store);

	window->details->permission_combos =
		g_list_prepend (window->details->permission_combos,
				combo);

	g_signal_connect (combo, "changed", G_CALLBACK (permission_combo_changed), window);

	cell = gtk_cell_renderer_text_new ();
	gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), cell, TRUE);
	gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo), cell,
					"text", 0,
					NULL);

	gtk_label_set_mnemonic_widget (label, combo);
	gtk_widget_show (combo);

	gtk_grid_attach_next_to (grid, combo, GTK_WIDGET (label),
				 GTK_POS_RIGHT, 1, 1);
}

static GtkWidget *
append_special_execution_checkbox (FMPropertiesWindow *window,
				   GtkGrid *grid,
				   GtkWidget *sibling,
				   const char *label_text,
				   guint32 permission_to_check)
{
	GtkWidget *check_button;

	check_button = gtk_check_button_new_with_mnemonic (label_text);
	gtk_widget_show (check_button);

	if (sibling != NULL) {
		gtk_grid_attach_next_to (grid, check_button, sibling,
					 GTK_POS_RIGHT, 1, 1);
	} else {
		gtk_container_add_with_properties (GTK_CONTAINER (grid), check_button,
						   "left-attach", 1,
						   NULL);
	}

	set_up_permissions_checkbox (window,
				     check_button,
				     permission_to_check,
				     FALSE);
	g_object_set_data (G_OBJECT (check_button), "is-special",
			   GINT_TO_POINTER (TRUE));

	return check_button;
}

static void
append_special_execution_flags (FMPropertiesWindow *window, GtkGrid *grid)
{
	GtkWidget *title;

	append_blank_slim_row (grid);
	title = GTK_WIDGET (attach_title_field (grid, _("Special flags:")));

	append_special_execution_checkbox (window, grid, title, _("Set _user ID"), UNIX_PERM_SUID);
	append_special_execution_checkbox (window, grid, NULL, _("Set gro_up ID"), UNIX_PERM_SGID);
	append_special_execution_checkbox (window, grid, NULL, _("_Sticky"), UNIX_PERM_STICKY);
}

static gboolean
all_can_get_permissions (GList *file_list)
{
	GList *l;
	for (l = file_list; l != NULL; l = l->next) {
		CajaFile *file;

		file = CAJA_FILE (l->data);

		if (!caja_file_can_get_permissions (file)) {
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
all_can_set_permissions (GList *file_list)
{
	GList *l;
	for (l = file_list; l != NULL; l = l->next) {
		CajaFile *file;

		file = CAJA_FILE (l->data);

		if (!caja_file_can_set_permissions (file)) {
			return FALSE;
		}
	}

	return TRUE;
}

static GHashTable *
get_initial_permissions (GList *file_list)
{
	GHashTable *ret;
	GList *l;

	ret = g_hash_table_new (g_direct_hash,
				g_direct_equal);

	for (l = file_list; l != NULL; l = l->next) {
		guint32 permissions;
		CajaFile *file;

		file = CAJA_FILE (l->data);

		permissions = caja_file_get_permissions (file);
		g_hash_table_insert (ret, file,
				     GINT_TO_POINTER (permissions));
	}

	return ret;
}

static void
create_simple_permissions (FMPropertiesWindow *window, GtkGrid *page_grid)
{
	gboolean has_file, has_directory;
	GtkLabel *group_label;
	GtkLabel *owner_label;
	GtkLabel *execute_label;
	GtkWidget *value;

	has_file = files_has_file (window);
	has_directory = files_has_directory (window);

	if (!is_multi_file_window (window) && caja_file_can_set_owner (get_target_file (window))) {
		GtkComboBox *owner_combo_box;

		owner_label = attach_title_field (page_grid, _("_Owner:"));
		/* Combo box in this case. */
		owner_combo_box = attach_owner_combo_box (page_grid,
							  GTK_WIDGET (owner_label),
							  get_target_file (window));
		gtk_label_set_mnemonic_widget (owner_label,
					       GTK_WIDGET (owner_combo_box));
	} else {
		owner_label = attach_title_field (page_grid, _("Owner:"));
		/* Static text in this case. */
		value = attach_value_field (window,
					    page_grid, GTK_WIDGET (owner_label),
					    "owner",
					    INCONSISTENT_STATE_STRING,
					    FALSE);
		gtk_label_set_mnemonic_widget (owner_label, value);
	}

	if (has_directory) {
		add_permissions_combo_box (window, page_grid,
					   PERMISSION_USER, TRUE, FALSE);
	}
	if (has_file || window->details->has_recursive_apply) {
		add_permissions_combo_box (window, page_grid,
					   PERMISSION_USER, FALSE, !has_directory);
	}

	append_blank_slim_row (page_grid);

	if (!is_multi_file_window (window) && caja_file_can_set_group (get_target_file (window))) {
		GtkComboBox *group_combo_box;

		group_label = attach_title_field (page_grid, _("_Group:"));

		/* Combo box in this case. */
		group_combo_box = attach_group_combo_box (page_grid, GTK_WIDGET (group_label),
							  get_target_file (window));
		gtk_label_set_mnemonic_widget (group_label,
					       GTK_WIDGET (group_combo_box));
	} else {
		group_label = attach_title_field (page_grid, _("Group:"));

		/* Static text in this case. */
		value = attach_value_field (window, page_grid,
					    GTK_WIDGET (group_label),
					    "group",
					    INCONSISTENT_STATE_STRING,
					    FALSE);
		gtk_label_set_mnemonic_widget (group_label, value);
	}

	if (has_directory) {
		add_permissions_combo_box (window, page_grid,
					   PERMISSION_GROUP, TRUE,
					   FALSE);
	}
	if (has_file || window->details->has_recursive_apply) {
		add_permissions_combo_box (window, page_grid,
					   PERMISSION_GROUP, FALSE,
					   !has_directory);
	}

	append_blank_slim_row (page_grid);

	group_label = attach_title_field (page_grid, _("Others"));

	if (has_directory) {
		add_permissions_combo_box (window, page_grid,
					   PERMISSION_OTHER, TRUE,
					   FALSE);
	}
	if (has_file || window->details->has_recursive_apply) {
		add_permissions_combo_box (window, page_grid,
					   PERMISSION_OTHER, FALSE,
					   !has_directory);
	}

	append_blank_slim_row (page_grid);

	execute_label = attach_title_field (page_grid, _("Execute:"));
	add_permissions_checkbox_with_label (window, page_grid,
					     GTK_WIDGET (execute_label),
					     _("Allow _executing file as program"),
					     UNIX_PERM_USER_EXEC|UNIX_PERM_GROUP_EXEC|UNIX_PERM_OTHER_EXEC,
					     execute_label, FALSE);
}

static void
create_permission_checkboxes (FMPropertiesWindow *window,
			      GtkGrid *page_grid,
			      gboolean is_folder)
{
	GtkLabel *owner_perm_label;
	GtkLabel *group_perm_label;
	GtkLabel *other_perm_label;
	GtkGrid *check_button_grid;
	GtkWidget *w;

	owner_perm_label = attach_title_field (page_grid, _("Owner:"));
	group_perm_label = attach_title_field (page_grid, _("Group:"));
	other_perm_label = attach_title_field (page_grid, _("Others:"));

	check_button_grid = GTK_GRID (create_grid_with_standard_properties ());
	gtk_widget_show (GTK_WIDGET (check_button_grid));

	gtk_grid_attach_next_to (page_grid, GTK_WIDGET (check_button_grid),
				 GTK_WIDGET (owner_perm_label),
				 GTK_POS_RIGHT, 1, 3);

	w = add_permissions_checkbox (window,
				      check_button_grid,
				      NULL,
				      PERMISSIONS_CHECKBOXES_READ,
				      UNIX_PERM_USER_READ,
				      owner_perm_label,
				      is_folder);

	w = add_permissions_checkbox (window,
				      check_button_grid,
				      w,
				      PERMISSIONS_CHECKBOXES_WRITE,
				      UNIX_PERM_USER_WRITE,
				      owner_perm_label,
				      is_folder);

	w = add_permissions_checkbox (window,
				      check_button_grid,
				      w,
				      PERMISSIONS_CHECKBOXES_EXECUTE,
				      UNIX_PERM_USER_EXEC,
				      owner_perm_label,
				      is_folder);

	w = add_permissions_checkbox (window,
				      check_button_grid,
				      NULL,
				      PERMISSIONS_CHECKBOXES_READ,
				      UNIX_PERM_GROUP_READ,
				      group_perm_label,
				      is_folder);

	w = add_permissions_checkbox (window,
				      check_button_grid,
				      w,
				      PERMISSIONS_CHECKBOXES_WRITE,
				      UNIX_PERM_GROUP_WRITE,
				      group_perm_label,
				      is_folder);

	w = add_permissions_checkbox (window,
				      check_button_grid,
				      w,
				      PERMISSIONS_CHECKBOXES_EXECUTE,
				      UNIX_PERM_GROUP_EXEC,
				      group_perm_label,
				      is_folder);

	w = add_permissions_checkbox (window,
				      check_button_grid,
				      NULL,
				      PERMISSIONS_CHECKBOXES_READ,
				      UNIX_PERM_OTHER_READ,
				      other_perm_label,
				      is_folder);

	w = add_permissions_checkbox (window,
				      check_button_grid,
				      w,
				      PERMISSIONS_CHECKBOXES_WRITE,
				      UNIX_PERM_OTHER_WRITE,
				      other_perm_label,
				      is_folder);

	add_permissions_checkbox (window,
				  check_button_grid,
				  w,
				  PERMISSIONS_CHECKBOXES_EXECUTE,
				  UNIX_PERM_OTHER_EXEC,
				  other_perm_label,
				  is_folder);
}

static void
create_advanced_permissions (FMPropertiesWindow *window, GtkGrid *page_grid)
{
	GtkLabel *group_label;
	GtkLabel *owner_label;
	gboolean has_directory, has_file;

	if (!is_multi_file_window (window) && caja_file_can_set_owner (get_target_file (window))) {
		GtkComboBox *owner_combo_box;

		owner_label  = attach_title_field (page_grid, _("_Owner:"));
		/* Combo box in this case. */
		owner_combo_box = attach_owner_combo_box (page_grid,
							  GTK_WIDGET (owner_label),
							  get_target_file (window));
		gtk_label_set_mnemonic_widget (owner_label,
					       GTK_WIDGET (owner_combo_box));
	} else {
		GtkWidget *value;
		owner_label = attach_title_field (page_grid, _("Owner:"));

		/* Static text in this case. */
		value = attach_value_field (window,
					    page_grid,
					    GTK_WIDGET (owner_label),
					    "owner",
					    INCONSISTENT_STATE_STRING,
					    FALSE);
		gtk_label_set_mnemonic_widget (owner_label, value);
	}

	if (!is_multi_file_window (window) && caja_file_can_set_group (get_target_file (window))) {
		GtkComboBox *group_combo_box;

		group_label = attach_title_field (page_grid, _("_Group:"));

		/* Combo box in this case. */
		group_combo_box = attach_group_combo_box (page_grid, GTK_WIDGET (group_label),
							  get_target_file (window));
		gtk_label_set_mnemonic_widget (group_label,
					       GTK_WIDGET (group_combo_box));
	} else {
		group_label = attach_title_field (page_grid, _("Group:"));

		/* Static text in this case. */
		attach_value_field (window, page_grid, GTK_WIDGET (group_label),
				    "group",
				    INCONSISTENT_STATE_STRING,
				    FALSE);
	}

	append_blank_slim_row (page_grid);

	has_directory = files_has_directory (window);
	has_file = files_has_file (window);

	if (has_directory) {
		if (has_file || window->details->has_recursive_apply) {
			attach_title_field (page_grid, _("Folder Permissions:"));
		}
		create_permission_checkboxes (window, page_grid, TRUE);
	}

	if (has_file || window->details->has_recursive_apply) {
		if (has_directory) {
			attach_title_field (page_grid, _("File Permissions:"));
		}
		create_permission_checkboxes (window, page_grid, FALSE);
	}

	append_blank_slim_row (page_grid);
	append_special_execution_flags (window, page_grid);

	append_title_value_pair
		(window, page_grid, _("Text view:"),
		 "permissions", INCONSISTENT_STATE_STRING,
		 FALSE);
}

static void
set_recursive_permissions_done (gpointer callback_data)
{
	FMPropertiesWindow *window;

	window = FM_PROPERTIES_WINDOW (callback_data);
	end_long_operation (window);

	g_object_unref (window);
}

static void
apply_recursive_clicked (GtkWidget *recursive_button,
			 FMPropertiesWindow *window)
{
	guint32 file_permission, file_permission_mask;
	guint32 dir_permission, dir_permission_mask;
	guint32 vfs_mask, vfs_new_perm, p;
	gboolean active, is_folder, is_special, use_original;
	GList *l;
	GtkTreeModel *model;
	GtkTreeIter iter;
	PermissionType type;
	int new_perm, mask;
	GtkWidget *button = NULL;
	GtkWidget *combo = NULL;

	file_permission = 0;
	file_permission_mask = 0;
	dir_permission = 0;
	dir_permission_mask = 0;

	/* Advanced mode and execute checkbox: */
	for (l = window->details->permission_buttons; l != NULL; l = l->next) {
		button = l->data;

		if (gtk_toggle_button_get_inconsistent (GTK_TOGGLE_BUTTON (button))) {
			continue;
		}

		active = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button));
		p = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
							"permission"));
		is_folder = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
								"is-folder"));
		is_special = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
								 "is-special"));

		if (is_folder || is_special) {
			dir_permission_mask |= p;
			if (active) {
				dir_permission |= p;
			}
		}
		if (!is_folder || is_special) {
			file_permission_mask |= p;
			if (active) {
				file_permission |= p;
			}
		}
	}
	/* Simple mode, minus exec checkbox */
	for (l = window->details->permission_combos; l != NULL; l = l->next) {
		combo = l->data;

		if (!gtk_combo_box_get_active_iter (GTK_COMBO_BOX (combo),  &iter)) {
			continue;
		}

		type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (combo), "permission-type"));
		is_folder = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (combo),
								"is-folder"));

		model = gtk_combo_box_get_model (GTK_COMBO_BOX (combo));
		gtk_tree_model_get (model, &iter, 1, &new_perm, 2, &use_original, -1);
		if (use_original) {
			continue;
		}
		vfs_new_perm = permission_to_vfs (type, new_perm);

		if (is_folder) {
			mask = PERMISSION_READ|PERMISSION_WRITE|PERMISSION_EXEC;
		} else {
			mask = PERMISSION_READ|PERMISSION_WRITE;
		}
		vfs_mask = permission_to_vfs (type, mask);

		if (is_folder) {
			dir_permission_mask |= vfs_mask;
			dir_permission |= vfs_new_perm;
		} else {
			file_permission_mask |= vfs_mask;
			file_permission |= vfs_new_perm;
		}
	}

	for (l = window->details->target_files; l != NULL; l = l->next) {
		CajaFile *file;

		file = CAJA_FILE (l->data);

		if (caja_file_is_directory (file) &&
		    caja_file_can_set_permissions (file)) {
			char *uri;

			uri = caja_file_get_uri (file);
			start_long_operation (window);
			g_object_ref (window);
			caja_file_set_permissions_recursive (uri,
								 file_permission,
								 file_permission_mask,
								 dir_permission,
								 dir_permission_mask,
								 set_recursive_permissions_done,
								 window);
			g_free (uri);
		}
	}
}

static void
create_permissions_page (FMPropertiesWindow *window)
{
	GtkWidget *vbox;
	GList *file_list;

	vbox = create_page_with_vbox (window->details->notebook,
				      _("Permissions"));

	file_list = window->details->original_files;

	window->details->initial_permissions = NULL;

	if (all_can_get_permissions (file_list) && all_can_get_permissions (window->details->target_files)) {
		GtkGrid *page_grid;

		window->details->initial_permissions = get_initial_permissions (window->details->target_files);
		window->details->has_recursive_apply = files_has_changable_permissions_directory (window);

		if (!all_can_set_permissions (file_list)) {
			add_prompt_and_separator (
				vbox,
				_("You are not the owner, so you cannot change these permissions."));
		}

		page_grid = GTK_GRID (create_grid_with_standard_properties ());

		gtk_widget_show (GTK_WIDGET (page_grid));
		gtk_box_pack_start (GTK_BOX (vbox),
				    GTK_WIDGET (page_grid),
				    TRUE, TRUE, 0);

		if (g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_SHOW_ADVANCED_PERMISSIONS)) {
			create_advanced_permissions (window, page_grid);
		} else {
			create_simple_permissions (window, page_grid);
		}

		append_blank_slim_row (page_grid);

#ifdef HAVE_SELINUX
		append_title_value_pair
			(window, page_grid, _("SELinux context:"),
			 "selinux_context", INCONSISTENT_STATE_STRING,
			 FALSE);
#endif
		append_title_value_pair
			(window, page_grid, _("Last changed:"),
			 "date_permissions", INCONSISTENT_STATE_STRING,
			 FALSE);

		if (window->details->has_recursive_apply) {
			GtkWidget *button, *hbox;

			hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
			gtk_widget_show (hbox);
			gtk_container_add_with_properties (GTK_CONTAINER (page_grid), hbox,
							   "width", 2,
							   NULL);

			button = gtk_button_new_with_mnemonic (_("Apply Permissions to Enclosed Files"));
			gtk_widget_show (button);
			gtk_box_pack_start (GTK_BOX (hbox), button, FALSE, FALSE, 0);
			g_signal_connect (button, "clicked",
					  G_CALLBACK (apply_recursive_clicked),
					  window);
		}
	} else {
		char *prompt_text;

		if (!is_multi_file_window (window)) {
			char *file_name;

			file_name = caja_file_get_display_name (get_target_file (window));
			prompt_text = g_strdup_printf (_("The permissions of \"%s\" could not be determined."), file_name);
			g_free (file_name);
		} else {
			prompt_text = g_strdup (_("The permissions of the selected file could not be determined."));
		}

		add_prompt (vbox, prompt_text, TRUE);
		g_free (prompt_text);
	}
}

static void
append_extension_pages (FMPropertiesWindow *window)
{
	GList *providers;
	GList *module_providers;
	GList *p;

	providers = caja_extensions_get_for_type (CAJA_TYPE_PROPERTY_PAGE_PROVIDER);

	/* FIXME: we also need the property pages from two old modules that
	 * are not registered as proper extensions. This is going to work
	 * this way until some generic solution is introduced.
	 */
	module_providers = caja_module_get_extensions_for_type (CAJA_TYPE_PROPERTY_PAGE_PROVIDER);
	for (p = module_providers; p != NULL; p = p->next) {
		const gchar *type_name = G_OBJECT_TYPE_NAME (G_OBJECT (p->data));
		if (g_strcmp0 (type_name, "CajaNotesViewerProvider") == 0 ||
		    g_strcmp0 (type_name, "CajaImagePropertiesPageProvider") == 0) {
			providers = g_list_prepend (providers, p->data);
		}
	}

	for (p = providers; p != NULL; p = p->next) {
		CajaPropertyPageProvider *provider;
		GList *pages;
		GList *l;

		provider = CAJA_PROPERTY_PAGE_PROVIDER (p->data);

		pages = caja_property_page_provider_get_pages
			(provider, window->details->original_files);

		for (l = pages; l != NULL; l = l->next) {
			CajaPropertyPage *page;
			GtkWidget *page_widget;
			GtkWidget *label;

			page = CAJA_PROPERTY_PAGE (l->data);

			g_object_get (G_OBJECT (page),
				      "page", &page_widget, "label", &label,
				      NULL);

			gtk_notebook_append_page (window->details->notebook,
						  page_widget, label);

			g_object_set_data (G_OBJECT (page_widget),
					   "is-extension-page",
					   page);

			g_object_unref (page_widget);
			g_object_unref (label);

			g_object_unref (page);
		}

		g_list_free (pages);
	}

	caja_module_extension_list_free (providers);
}

static gboolean
should_show_emblems (FMPropertiesWindow *window)
{
	/* FIXME bugzilla.gnome.org 45643:
	 * Emblems aren't displayed on the the desktop Trash icon, so
	 * we shouldn't pretend that they work by showing them here.
	 * When bug 5643 is fixed we can remove this case.
	 */
	if (!is_multi_file_window (window)
	    && is_merged_trash_directory (get_target_file (window))) {
		return FALSE;
	}

	return TRUE;
}

static gboolean
should_show_permissions (FMPropertiesWindow *window)
{
	CajaFile *file;

	file = get_target_file (window);

	/* Don't show permissions for Trash and Computer since they're not
	 * really file system objects.
	 */
	if (!is_multi_file_window (window)
	    && (is_merged_trash_directory (file) ||
		is_computer_directory (file))) {
		return FALSE;
	}

	return TRUE;
}

static char *
get_pending_key (GList *file_list)
{
	GList *l;
	GList *uris;
	GString *key;
	char *ret;

	uris = NULL;
	for (l = file_list; l != NULL; l = l->next) {
		uris = g_list_prepend (uris, caja_file_get_uri (CAJA_FILE (l->data)));
	}
	uris = g_list_sort (uris, (GCompareFunc)strcmp);

	key = g_string_new ("");
	for (l = uris; l != NULL; l = l->next) {
		g_string_append (key, l->data);
		g_string_append (key, ";");
	}

    	g_list_free_full (uris, g_free);

	ret = key->str;
	g_string_free (key, FALSE);

	return ret;
}

static StartupData *
startup_data_new (GList *original_files,
		  GList *target_files,
		  const char *pending_key,
		  GtkWidget *parent_widget)
{
	StartupData *data;
	GList *l;

	data = g_new0 (StartupData, 1);
	data->original_files = caja_file_list_copy (original_files);
	data->target_files = caja_file_list_copy (target_files);
	data->parent_widget = parent_widget;
	data->pending_key = g_strdup (pending_key);
	data->pending_files = g_hash_table_new (g_direct_hash,
						g_direct_equal);

	for (l = data->target_files; l != NULL; l = l->next) {
		g_hash_table_insert (data->pending_files, l->data, l->data);
	}

	return data;
}

static void
startup_data_free (StartupData *data)
{
	caja_file_list_free (data->original_files);
	caja_file_list_free (data->target_files);
	g_hash_table_destroy (data->pending_files);
	g_free (data->pending_key);
	g_free (data);
}

static void
file_changed_callback (CajaFile *file, gpointer user_data)
{
	FMPropertiesWindow *window = FM_PROPERTIES_WINDOW (user_data);

	if (!g_list_find (window->details->changed_files, file)) {
		caja_file_ref (file);
		window->details->changed_files = g_list_prepend (window->details->changed_files, file);

		schedule_files_update (window);
	}
}

static gboolean
is_a_special_file (CajaFile *file)
{
	if (file == NULL ||
	    CAJA_IS_DESKTOP_ICON_FILE (file) ||
	    is_merged_trash_directory (file) ||
	    is_computer_directory (file)) {
		return TRUE;
	}
	return FALSE;
}

static gboolean
should_show_open_with (FMPropertiesWindow *window)
{
	CajaFile *file;

	/* Don't show open with tab for desktop special icons (trash, etc)
	 * We don't get the open-with menu for these anyway.
	 *
	 * Also don't show it for folders. Changing the default app for folders
	 * leads to all sort of hard to understand errors.
	 */

	if (is_multi_file_window (window)) {
		if (!file_list_attributes_identical (window->details->original_files,
						     "mime_type")) {
			return FALSE;
		} else {

			GList *l;

			for (l = window->details->original_files; l; l = l->next) {
				file = CAJA_FILE (l->data);
				if (caja_file_is_directory (file) ||
				    is_a_special_file (file)) {
					return FALSE;
				}
			}
		}
	} else {
		file = get_original_file (window);
		if (caja_file_is_directory (file) ||
		    is_a_special_file (file)) {
			return FALSE;
		}
	}
	return TRUE;
}

static void
create_open_with_page (FMPropertiesWindow *window)
{
	GtkWidget *vbox;
	char *mime_type;

	mime_type = caja_file_get_mime_type (get_target_file (window));

	if (!is_multi_file_window (window)) {
		char *uri;

		uri = caja_file_get_uri (get_target_file (window));

		if (uri == NULL) {
			return;
		}

		vbox = caja_mime_application_chooser_new (uri, mime_type);

		g_free (uri);
	} else {
		GList *uris;

		uris = window->details->original_files;
		if (uris == NULL) {
			return;
		}
		vbox = caja_mime_application_chooser_new_for_multiple_files (uris, mime_type);
	}

	gtk_widget_show (vbox);
	g_free (mime_type);

	gtk_notebook_append_page (window->details->notebook,
				  vbox, gtk_label_new (_("Open With")));
}

static FMPropertiesWindow *
create_properties_window (StartupData *startup_data)
{
	FMPropertiesWindow *window;
	GList *l;
	GtkWidget *action_area;

	window = FM_PROPERTIES_WINDOW (gtk_widget_new (fm_properties_window_get_type (), NULL));

	window->details->original_files = caja_file_list_copy (startup_data->original_files);

	window->details->target_files = caja_file_list_copy (startup_data->target_files);

	gtk_window_set_screen (GTK_WINDOW (window),
			       gtk_widget_get_screen (startup_data->parent_widget));

	gtk_window_set_type_hint (GTK_WINDOW (window), GDK_WINDOW_TYPE_HINT_DIALOG);

	/* Set initial window title */
	update_properties_window_title (window);

	/* Start monitoring the file attributes we display. Note that some
	 * of the attributes are for the original file, and some for the
	 * target files.
	 */

	for (l = window->details->original_files; l != NULL; l = l->next) {
		CajaFile *file;
		CajaFileAttributes attributes;

		file = CAJA_FILE (l->data);

		attributes =
			CAJA_FILE_ATTRIBUTES_FOR_ICON |
			CAJA_FILE_ATTRIBUTE_INFO |
			CAJA_FILE_ATTRIBUTE_LINK_INFO;

		caja_file_monitor_add (file,
					   &window->details->original_files,
					   attributes);
	}

	for (l = window->details->target_files; l != NULL; l = l->next) {
		CajaFile *file;
		CajaFileAttributes attributes;

		file = CAJA_FILE (l->data);

		attributes = 0;
		if (caja_file_is_directory (file)) {
			attributes |= CAJA_FILE_ATTRIBUTE_DEEP_COUNTS;
		}

		attributes |= CAJA_FILE_ATTRIBUTE_INFO;
		caja_file_monitor_add (file, &window->details->target_files, attributes);
	}

	for (l = window->details->target_files; l != NULL; l = l->next) {
		g_signal_connect_object (CAJA_FILE (l->data),
					 "changed",
					 G_CALLBACK (file_changed_callback),
					 G_OBJECT (window),
					 0);
	}

	for (l = window->details->original_files; l != NULL; l = l->next) {
		g_signal_connect_object (CAJA_FILE (l->data),
					 "changed",
					 G_CALLBACK (file_changed_callback),
					 G_OBJECT (window),
					 0);
	}

	/* Create the notebook tabs. */
	window->details->notebook = GTK_NOTEBOOK (gtk_notebook_new ());

        gtk_widget_add_events (GTK_WIDGET (window->details->notebook), GDK_SCROLL_MASK);
        g_signal_connect (window->details->notebook,
                          "scroll-event",
                          G_CALLBACK (eel_notebook_scroll_event_cb),
                          NULL);

	gtk_widget_show (GTK_WIDGET (window->details->notebook));
	gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (window))),
			    GTK_WIDGET (window->details->notebook),
			    TRUE, TRUE, 0);

	/* Create the pages. */
	create_basic_page (window);

	if (should_show_emblems (window)) {
		create_emblems_page (window);
	}

	if (should_show_permissions (window)) {
		create_permissions_page (window);
	}

	if (should_show_open_with (window)) {
		create_open_with_page (window);
	}

	/* append pages from available views */
	append_extension_pages (window);

        eel_dialog_add_button (GTK_DIALOG (window),
                               _("_Help"),
                               "help-browser",
                               GTK_RESPONSE_HELP);

        action_area = gtk_widget_get_parent (eel_dialog_add_button (GTK_DIALOG (window),
                                                                    _("_Close"),
                                                                    "window-close",
                                                                    GTK_RESPONSE_CLOSE));

	/* FIXME - HIGificiation, should be done inside GTK+ */
	gtk_container_set_border_width (GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (window))), 12);
	gtk_container_set_border_width (GTK_CONTAINER (action_area), 0);
	gtk_box_set_spacing (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (window))), 12);

	/* Update from initial state */
	properties_window_update (window, NULL);

	return window;
}

static GList *
get_target_file_list (GList *original_files)
{
	GList *ret;
	GList *l;

	ret = NULL;

	for (l = original_files; l != NULL; l = l->next) {
		CajaFile *target;

		target = get_target_file_for_original_file (CAJA_FILE (l->data));

		ret = g_list_prepend (ret, target);
	}

	ret = g_list_reverse (ret);

	return ret;
}

static void
add_window (FMPropertiesWindow *window)
{
	if (!is_multi_file_window (window)) {
		g_hash_table_insert (windows,
				     get_original_file (window),
				     window);
		g_object_set_data (G_OBJECT (window), "window_key",
				   get_original_file (window));
	}
}

static void
remove_window (FMPropertiesWindow *window)
{
	gpointer key;

	key = g_object_get_data (G_OBJECT (window), "window_key");
	if (key) {
		g_hash_table_remove (windows, key);
	}
}

static GtkWindow *
get_existing_window (GList *file_list)
{
	if (!file_list->next) {
		return g_hash_table_lookup (windows, file_list->data);
	}

	return NULL;
}

static void
cancel_create_properties_window_callback (gpointer callback_data)
{
	remove_pending ((StartupData *)callback_data, TRUE, FALSE, TRUE);
}

static void
parent_widget_destroyed_callback (GtkWidget *widget, gpointer callback_data)
{
	g_assert (widget == ((StartupData *)callback_data)->parent_widget);

	remove_pending ((StartupData *)callback_data, TRUE, TRUE, FALSE);
}

static void
cancel_call_when_ready_callback (gpointer key,
				 gpointer value,
				 gpointer user_data)
{
	caja_file_cancel_call_when_ready
		(CAJA_FILE (key),
		 is_directory_ready_callback,
		 user_data);
}

static void
remove_pending (StartupData *startup_data,
		gboolean cancel_call_when_ready,
		gboolean cancel_timed_wait,
		gboolean cancel_destroy_handler)
{
	if (cancel_call_when_ready) {
		g_hash_table_foreach (startup_data->pending_files,
				      cancel_call_when_ready_callback,
				      startup_data);

	}
	if (cancel_timed_wait) {
		eel_timed_wait_stop
			(cancel_create_properties_window_callback, startup_data);
	}
	if (cancel_destroy_handler) {
		g_signal_handlers_disconnect_by_func (startup_data->parent_widget,
						      G_CALLBACK (parent_widget_destroyed_callback),
						      startup_data);
	}

	g_hash_table_remove (pending_lists, startup_data->pending_key);

	startup_data_free (startup_data);
}

static void
is_directory_ready_callback (CajaFile *file,
			     gpointer data)
{
	StartupData *startup_data;

	startup_data = data;

	g_hash_table_remove (startup_data->pending_files, file);

	if (g_hash_table_size (startup_data->pending_files) == 0) {
		FMPropertiesWindow *new_window;

		new_window = create_properties_window (startup_data);

		add_window (new_window);

		remove_pending (startup_data, FALSE, TRUE, TRUE);

		gtk_window_present (GTK_WINDOW (new_window));
	}
}

void
fm_properties_window_present (GList *original_files,
			      GtkWidget *parent_widget)
{
	GList *l, *next;
	GtkWidget *parent_window;
	StartupData *startup_data;
	GList *target_files;
	GtkWindow *existing_window;
	char *pending_key;

	g_return_if_fail (original_files != NULL);
	g_return_if_fail (GTK_IS_WIDGET (parent_widget));

	/* Create the hash tables first time through. */
	if (windows == NULL) {
		windows = eel_g_hash_table_new_free_at_exit
			(NULL, NULL, "property windows");
	}

	if (pending_lists == NULL) {
		pending_lists = eel_g_hash_table_new_free_at_exit
			(g_str_hash, g_str_equal, "pending property window files");
	}

	/* Look to see if there's already a window for this file. */
	existing_window = get_existing_window (original_files);
	if (existing_window != NULL) {
		gtk_window_set_screen (existing_window,
				       gtk_widget_get_screen (parent_widget));
		gtk_window_present (existing_window);
		return;
	}

	pending_key = get_pending_key (original_files);

	/* Look to see if we're already waiting for a window for this file. */
	if (g_hash_table_lookup (pending_lists, pending_key) != NULL) {
		return;
	}

	target_files = get_target_file_list (original_files);

	startup_data = startup_data_new (original_files,
					 target_files,
					 pending_key,
					 parent_widget);

	caja_file_list_free (target_files);
	g_free(pending_key);

	/* Wait until we can tell whether it's a directory before showing, since
	 * some one-time layout decisions depend on that info.
	 */

	g_hash_table_insert (pending_lists, startup_data->pending_key, startup_data->pending_key);
	g_signal_connect (parent_widget, "destroy",
			  G_CALLBACK (parent_widget_destroyed_callback), startup_data);

	parent_window = gtk_widget_get_ancestor (parent_widget, GTK_TYPE_WINDOW);

	eel_timed_wait_start
		(cancel_create_properties_window_callback,
		 startup_data,
		 _("Creating Properties window."),
		 parent_window == NULL ? NULL : GTK_WINDOW (parent_window));

	for (l = startup_data->target_files; l != NULL; l = next) {
		next = l->next;
		caja_file_call_when_ready
			(CAJA_FILE (l->data),
			 CAJA_FILE_ATTRIBUTE_INFO,
			 is_directory_ready_callback,
			 startup_data);
	}
}

static void
real_response (GtkDialog *dialog,
	       int        response)
{
	GError *error = NULL;

	switch (response) {
	case GTK_RESPONSE_HELP:
		gtk_show_uri_on_window (GTK_WINDOW (dialog),
			                "help:mate-user-guide/goscaja-51",
			                gtk_get_current_event_time (),
			                &error);
		if (error != NULL) {
			eel_show_error_dialog (_("There was an error displaying help."), error->message,
					       GTK_WINDOW (dialog));
			g_error_free (error);
		}
		break;

	case GTK_RESPONSE_NONE:
	case GTK_RESPONSE_CLOSE:
	case GTK_RESPONSE_DELETE_EVENT:
		gtk_widget_destroy (GTK_WIDGET (dialog));
		break;

	default:
		g_assert_not_reached ();
		break;
	}
}

static void
real_destroy (GtkWidget *object)
{
	FMPropertiesWindow *window;
	GList *l;

	window = FM_PROPERTIES_WINDOW (object);

	remove_window (window);

	for (l = window->details->original_files; l != NULL; l = l->next) {
		caja_file_monitor_remove (CAJA_FILE (l->data), &window->details->original_files);
	}
	caja_file_list_free (window->details->original_files);
	window->details->original_files = NULL;

	for (l = window->details->target_files; l != NULL; l = l->next) {
		caja_file_monitor_remove (CAJA_FILE (l->data), &window->details->target_files);
	}
	caja_file_list_free (window->details->target_files);
	window->details->target_files = NULL;

	caja_file_list_free (window->details->changed_files);
	window->details->changed_files = NULL;

	window->details->name_field = NULL;

	g_list_free (window->details->emblem_buttons);
	window->details->emblem_buttons = NULL;

	if (window->details->initial_emblems) {
		g_hash_table_destroy (window->details->initial_emblems);
		window->details->initial_emblems = NULL;
	}

	g_list_free (window->details->permission_buttons);
	window->details->permission_buttons = NULL;

	g_list_free (window->details->permission_combos);
	window->details->permission_combos = NULL;

	if (window->details->initial_permissions) {
		g_hash_table_destroy (window->details->initial_permissions);
		window->details->initial_permissions = NULL;
	}

	g_list_free (window->details->value_fields);
	window->details->value_fields = NULL;

	if (window->details->update_directory_contents_timeout_id != 0) {
		g_source_remove (window->details->update_directory_contents_timeout_id);
		window->details->update_directory_contents_timeout_id = 0;
	}

	if (window->details->update_files_timeout_id != 0) {
		g_source_remove (window->details->update_files_timeout_id);
		window->details->update_files_timeout_id = 0;
	}

	GTK_WIDGET_CLASS (fm_properties_window_parent_class)->destroy (object);
}

static void
real_finalize (GObject *object)
{
	FMPropertiesWindow *window;

	window = FM_PROPERTIES_WINDOW (object);

    	g_list_free_full (window->details->mime_list, g_free);

	g_free (window->details->pending_name);

	G_OBJECT_CLASS (fm_properties_window_parent_class)->finalize (object);
}

/* converts
 *  file://foo/foobar/foofoo/bar
 * to
 *  foofoo/bar
 * if
 *  file://foo/foobar
 * is the parent
 *
 * It does not resolve any symlinks.
 * */
static char *
make_relative_uri_from_full (const char *uri,
			     const char *base_uri)
{
	g_assert (uri != NULL);
	g_assert (base_uri != NULL);

	if (g_str_has_prefix (uri, base_uri)) {
		uri += strlen (base_uri);
		if (*uri != '/') {
			return NULL;
		}

		while (*uri == '/') {
			uri++;
		}

		if (*uri != '\0') {
			return g_strdup (uri);
		}
	}

	return NULL;
}

/* icon selection callback to set the image of the file object to the selected file */
static void
set_icon (const char* icon_uri, FMPropertiesWindow *properties_window)
{
	char *icon_path;

	g_assert (icon_uri != NULL);
	g_assert (FM_IS_PROPERTIES_WINDOW (properties_window));

	icon_path = g_filename_from_uri (icon_uri, NULL, NULL);
	/* we don't allow remote URIs */
	if (icon_path != NULL) {
		GList *l;
		CajaFile *file = NULL;

		for (l = properties_window->details->original_files; l != NULL; l = l->next) {
			char *file_uri;

			file = CAJA_FILE (l->data);

			file_uri = caja_file_get_uri (file);

			if (caja_file_is_mime_type (file, "application/x-desktop")) {
				if (caja_link_local_set_icon (file_uri, icon_path)) {
					caja_file_invalidate_attributes (file,
									     CAJA_FILE_ATTRIBUTE_INFO |
									     CAJA_FILE_ATTRIBUTE_LINK_INFO);
				}
			} else {
				char *real_icon_uri;

				real_icon_uri = make_relative_uri_from_full (icon_uri, file_uri);

				if (real_icon_uri == NULL) {
					real_icon_uri = g_strdup (icon_uri);
				}

				caja_file_set_metadata (file, CAJA_METADATA_KEY_CUSTOM_ICON, NULL, real_icon_uri);
				caja_file_set_metadata (file, CAJA_METADATA_KEY_ICON_SCALE, NULL, NULL);

				g_free (real_icon_uri);
			}

			g_free (file_uri);
		}

		g_free (icon_path);
	}
}

static void
update_preview_callback (GtkFileChooser *icon_chooser,
			 FMPropertiesWindow *window)
{
	GdkPixbuf *pixbuf, *scaled_pixbuf;
	char *filename;

	pixbuf = NULL;

	filename = gtk_file_chooser_get_filename (icon_chooser);
	if (filename != NULL) {
		pixbuf = gdk_pixbuf_new_from_file (filename, NULL);
	}

	if (pixbuf != NULL) {
		GtkWidget *preview_widget;

		preview_widget = gtk_file_chooser_get_preview_widget (icon_chooser);
		gtk_file_chooser_set_preview_widget_active (icon_chooser, TRUE);

		if (gdk_pixbuf_get_width (pixbuf) > PREVIEW_IMAGE_WIDTH) {
			double scale;
			gint   height;
			gint   width;

			height = gdk_pixbuf_get_height (pixbuf);
			width = gdk_pixbuf_get_width (pixbuf);
			scale = ((double) height) / ((double) width);

			scaled_pixbuf = gdk_pixbuf_scale_simple
				(pixbuf,
				 PREVIEW_IMAGE_WIDTH,
				 (int) (scale * (double) PREVIEW_IMAGE_WIDTH),
				 GDK_INTERP_HYPER);
			g_object_unref (pixbuf);
			pixbuf = scaled_pixbuf;
		}

		gtk_image_set_from_pixbuf (GTK_IMAGE (preview_widget), pixbuf);
	} else {
		gtk_file_chooser_set_preview_widget_active (icon_chooser, FALSE);
	}

	g_free (filename);

	if (pixbuf != NULL) {
		g_object_unref (pixbuf);
	}
}

static void
custom_icon_file_chooser_response_cb (GtkDialog *dialog,
				      gint response,
				      FMPropertiesWindow *window)
{
	char *uri;

	switch (response) {
	case GTK_RESPONSE_NO:
		reset_icon (window);
		break;

	case GTK_RESPONSE_OK:
		uri = gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (dialog));
		set_icon (uri, window);
		g_free (uri);
		break;

	default:
		break;
	}

	gtk_widget_hide (GTK_WIDGET (dialog));
}

static void
select_image_button_callback (GtkWidget *widget,
			      FMPropertiesWindow *window)
{
	GtkWidget *dialog;
	GList *l;
	CajaFile *file;
	char *image_path;
	gboolean revert_is_sensitive;

	g_assert (FM_IS_PROPERTIES_WINDOW (window));

	dialog = window->details->icon_chooser;

	if (dialog == NULL) {
		GtkWidget *preview;
		GtkFileFilter *filter;

		dialog = eel_file_chooser_dialog_new (_("Select Custom Icon"), GTK_WINDOW (window),
						      GTK_FILE_CHOOSER_ACTION_OPEN,
						      "document-revert", GTK_RESPONSE_NO,
						      "process-stop", GTK_RESPONSE_CANCEL,
						      "document-open", GTK_RESPONSE_OK,
						      NULL);
		gtk_file_chooser_add_shortcut_folder (GTK_FILE_CHOOSER (dialog), ICONDIR, NULL);
		gtk_file_chooser_add_shortcut_folder (GTK_FILE_CHOOSER (dialog), PIXMAPDIR, NULL);
		gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);

		filter = gtk_file_filter_new ();
		gtk_file_filter_add_pixbuf_formats (filter);
		gtk_file_chooser_set_filter (GTK_FILE_CHOOSER (dialog), filter);

		preview = gtk_image_new ();
		gtk_widget_set_size_request (preview, PREVIEW_IMAGE_WIDTH, -1);
		gtk_file_chooser_set_preview_widget (GTK_FILE_CHOOSER (dialog), preview);
		gtk_file_chooser_set_use_preview_label (GTK_FILE_CHOOSER (dialog), FALSE);
		gtk_file_chooser_set_preview_widget_active (GTK_FILE_CHOOSER (dialog), FALSE);

		g_signal_connect (dialog, "update-preview",
				  G_CALLBACK (update_preview_callback), window);

		window->details->icon_chooser = dialog;

		g_object_add_weak_pointer (G_OBJECT (dialog),
					   (gpointer *) &window->details->icon_chooser);
	}

	/* it's likely that the user wants to pick an icon that is inside a local directory */
	if (g_list_length (window->details->original_files) == 1) {
		file = CAJA_FILE (window->details->original_files->data);

		if (caja_file_is_directory (file)) {
			char *uri;

			uri = caja_file_get_uri (file);

			image_path = g_filename_from_uri (uri, NULL, NULL);
			if (image_path != NULL) {
				gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), image_path);
				g_free (image_path);
			}

			g_free (uri);
		}
	}

	revert_is_sensitive = FALSE;
	for (l = window->details->original_files; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);
		image_path = caja_file_get_metadata (file, CAJA_METADATA_KEY_CUSTOM_ICON, NULL);
		revert_is_sensitive = (image_path != NULL);
		g_free (image_path);

		if (revert_is_sensitive) {
			break;
		}
	}
	gtk_dialog_set_response_sensitive (GTK_DIALOG (dialog), GTK_RESPONSE_NO, revert_is_sensitive);

	g_signal_connect (dialog, "response",
			  G_CALLBACK (custom_icon_file_chooser_response_cb), window);
	gtk_widget_show (dialog);
}

static void
fm_properties_window_class_init (FMPropertiesWindowClass *class)
{
	GtkBindingSet *binding_set;

	G_OBJECT_CLASS (class)->finalize = real_finalize;

	GTK_WIDGET_CLASS (class)->destroy = real_destroy;

	GTK_DIALOG_CLASS (class)->response = real_response;

	binding_set = gtk_binding_set_by_class (class);
	gtk_binding_entry_add_signal (binding_set, GDK_KEY_Escape, 0,
				      "close", 0);
}

static void
fm_properties_window_init (FMPropertiesWindow *window)
{
	window->details = fm_properties_window_get_instance_private (window);
}

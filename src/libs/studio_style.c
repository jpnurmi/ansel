/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Guillaume STUTIN.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
*/

/** Studio Capture: style module. Checked styles are auto-applied, in list
    order, to every image imported by the folder survey (first style replaces
    the fresh history, the rest stack on top). The order is editable and the
    selection can be re-applied manually to the displayed image. */

#include "common/darktable.h"
#include "common/debug.h"
#include "common/folder_survey.h"
#include "common/history_merge.h"
#include "common/image.h"
#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"

DT_MODULE(1)

typedef enum dt_studio_style_cols_t
{
  COL_ENABLED = 0,
  COL_NAME,
  NUM_COLS
} dt_studio_style_cols_t;

typedef struct dt_lib_studio_style_t
{
  GtkListStore *store;
  GtkWidget *treeview;
} dt_lib_studio_style_t;

const char *name(dt_lib_module_t *self)
{
  return _("Style");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "studio_capture", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 980;
}

/**
 * @brief Persist the checked style names, in list order, to conf.
 */
static void _studio_style_save(dt_lib_studio_style_t *d)
{
  GString *conf = g_string_new(NULL);
  GtkTreeModel *model = GTK_TREE_MODEL(d->store);
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  while(valid)
  {
    gboolean enabled = FALSE;
    gchar *style_name = NULL;
    gtk_tree_model_get(model, &iter, COL_ENABLED, &enabled, COL_NAME, &style_name, -1);
    if(enabled && !IS_NULL_PTR(style_name))
    {
      if(conf->len) g_string_append(conf, DT_FOLDER_SURVEY_STYLES_SEPARATOR);
      g_string_append(conf, style_name);
    }
    dt_free(style_name);
    valid = gtk_tree_model_iter_next(model, &iter);
  }

  dt_conf_set_string(DT_FOLDER_SURVEY_STYLES_CONF_KEY, conf->str);
  g_string_free(conf, TRUE);
}

/**
 * @brief Rebuild the list: conf-ordered enabled styles first, then the rest.
 */
static void _studio_style_populate(dt_lib_studio_style_t *d)
{
  gtk_list_store_clear(d->store);

  GList *all_styles = dt_styles_get_list("");
  GtkTreeIter iter;

  char *conf = dt_conf_get_string(DT_FOLDER_SURVEY_STYLES_CONF_KEY);
  gchar **selected = g_strsplit(conf && conf[0] ? conf : "", DT_FOLDER_SURVEY_STYLES_SEPARATOR, -1);
  dt_free(conf);

  // Enabled styles keep their persisted order; drop names whose style is gone.
  for(gchar **name = selected; *name; name++)
  {
    if((*name)[0] == '\0') continue;
    gboolean found = FALSE;
    for(GList *s = all_styles; s && !found; s = g_list_next(s))
      found = !g_strcmp0(((dt_style_t *)s->data)->name, *name);
    if(!found) continue;

    gtk_list_store_append(d->store, &iter);
    gtk_list_store_set(d->store, &iter, COL_ENABLED, TRUE, COL_NAME, *name, -1);
  }

  for(GList *s = all_styles; s; s = g_list_next(s))
  {
    const dt_style_t *style = (const dt_style_t *)s->data;
    gboolean already_listed = FALSE;
    for(gchar **name = selected; *name && !already_listed; name++)
      already_listed = !g_strcmp0(style->name, *name);
    if(already_listed) continue;

    gtk_list_store_append(d->store, &iter);
    gtk_list_store_set(d->store, &iter, COL_ENABLED, FALSE, COL_NAME, style->name, -1);
  }

  g_strfreev(selected);
  g_list_free_full(all_styles, dt_style_free);
}

static void _studio_style_toggled(GtkCellRendererToggle *renderer, gchar *path_str, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(d->store), &iter, path_str)) return;

  gboolean enabled = FALSE;
  gtk_tree_model_get(GTK_TREE_MODEL(d->store), &iter, COL_ENABLED, &enabled, -1);
  gtk_list_store_set(d->store, &iter, COL_ENABLED, !enabled, -1);
  _studio_style_save(d);
}

/**
 * @brief Move the selected row one step up or down and persist the new order.
 */
static void _studio_style_move(dt_lib_studio_style_t *d, const int direction)
{
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->treeview));
  GtkTreeModel *model = NULL;
  GtkTreeIter iter;
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  GtkTreeIter other = iter;
  gboolean has_other = (direction < 0) ? gtk_tree_model_iter_previous(model, &other)
                                       : gtk_tree_model_iter_next(model, &other);
  if(!has_other) return;

  gtk_list_store_swap(d->store, &iter, &other);
  _studio_style_save(d);
}

static void _studio_style_up_callback(GtkWidget *widget, gpointer user_data)
{
  _studio_style_move((dt_lib_studio_style_t *)user_data, -1);
}

static void _studio_style_down_callback(GtkWidget *widget, gpointer user_data)
{
  _studio_style_move((dt_lib_studio_style_t *)user_data, 1);
}

/**
 * @brief Re-apply the checked styles, in order, to the displayed image.
 */
static void _studio_style_apply_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;

  const int32_t imgid = dt_view_active_images_get_first();
  if(imgid <= UNKNOWN_IMAGE)
  {
    dt_control_log(_("No image is displayed."));
    return;
  }

  GtkTreeModel *model = GTK_TREE_MODEL(d->store);
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  dt_hm_batch_state_t batch = { 0 };
  gboolean first = TRUE;
  int applied = 0;

  while(valid)
  {
    gboolean enabled = FALSE;
    gchar *style_name = NULL;
    gtk_tree_model_get(model, &iter, COL_ENABLED, &enabled, COL_NAME, &style_name, -1);
    if(enabled && !IS_NULL_PTR(style_name))
    {
      const int32_t style_id = dt_styles_get_id_by_name(style_name);
      if(style_id > 0
         && !dt_styles_apply_to_image_merge(style_name, style_id, imgid,
                                            first ? DT_HISTORY_MERGE_REPLACE : DT_HISTORY_MERGE_APPEND, &batch))
      {
        first = FALSE;
        applied++;
      }
    }
    dt_free(style_name);
    valid = gtk_tree_model_iter_next(model, &iter);
  }
  dt_hm_batch_state_cleanup(&batch);

  if(applied == 0)
  {
    dt_control_log(_("No style is selected."));
    return;
  }

  // History went straight to DB: refresh cached metadata, mipmap and thumbnails,
  // then let listeners (studio center view) re-fetch their surfaces.
  dt_image_history_changed(imgid, TRUE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  dt_control_log(ngettext("Applied %d style.", "Applied %d styles.", applied), applied);
}

static void _studio_style_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;
  _studio_style_populate(d);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)g_malloc0(sizeof(dt_lib_studio_style_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  d->store = gtk_list_store_new(NUM_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING);
  d->treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->store));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->treeview), FALSE);
  gtk_widget_set_tooltip_text(d->treeview,
                              _("Checked styles are automatically applied, in this order, to every image "
                                "imported during the session. The first style replaces the history, the "
                                "following ones are stacked on top."));

  GtkCellRenderer *toggle_renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(toggle_renderer, "toggled", G_CALLBACK(_studio_style_toggled), d);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->treeview),
                              gtk_tree_view_column_new_with_attributes("", toggle_renderer, "active",
                                                                       COL_ENABLED, NULL));

  GtkCellRenderer *text_renderer = gtk_cell_renderer_text_new();
  g_object_set(text_renderer, "ellipsize", PANGO_ELLIPSIZE_END, (gchar *)0);
  GtkTreeViewColumn *name_column
      = gtk_tree_view_column_new_with_attributes("", text_renderer, "text", COL_NAME, NULL);
  gtk_tree_view_column_set_expand(name_column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->treeview), name_column);

  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(d->treeview, 100, "plugins/darkroom/studio_style/windowheight",
                                       DT_UI_RESIZE_DYNAMIC),
                     TRUE, TRUE, 0);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);

  GtkWidget *up = gtk_button_new_with_label(_("Up"));
  gtk_widget_set_tooltip_text(up, _("Apply the selected style earlier"));
  g_signal_connect(G_OBJECT(up), "clicked", G_CALLBACK(_studio_style_up_callback), d);
  gtk_box_pack_start(GTK_BOX(buttons), up, TRUE, TRUE, 0);

  GtkWidget *down = gtk_button_new_with_label(_("Down"));
  gtk_widget_set_tooltip_text(down, _("Apply the selected style later"));
  g_signal_connect(G_OBJECT(down), "clicked", G_CALLBACK(_studio_style_down_callback), d);
  gtk_box_pack_start(GTK_BOX(buttons), down, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), buttons, FALSE, FALSE, 0);

  GtkWidget *apply = gtk_button_new_with_label(_("Apply to the displayed image"));
  gtk_widget_set_tooltip_text(apply, _("Replace the history of the displayed image with the checked styles"));
  g_signal_connect(G_OBJECT(apply), "clicked", G_CALLBACK(_studio_style_apply_callback), d);
  gtk_box_pack_start(GTK_BOX(self->widget), apply, FALSE, FALSE, 0);

  _studio_style_populate(d);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_STYLE_CHANGED,
                                  G_CALLBACK(_studio_style_changed_callback), d);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_studio_style_changed_callback), self->data);
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

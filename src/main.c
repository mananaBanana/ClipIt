/* Copyright (C) 2010 by Cristian Henzel <oss@web-tm.com>
 *
 * forked from parcellite, which is
 * Copyright (C) 2007-2008 by Xyhthyx <xyhthyx@gmail.com>
 *
 * This file is part of ClipIt.
 *
 * ClipIt is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * ClipIt is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <stdlib.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <pthread.h>
#include "main.h"
#include "utils.h"
#include "history.h"
#include "keybinder.h"
#include "preferences.h"
#include "clipit-i18n.h"


static gchar* primary_text;
static gchar* clipboard_text;
static gchar* synchronized_text;
static GtkClipboard* primary;
static GtkClipboard* clipboard;
static GtkStatusIcon* status_icon;

static gboolean actions_lock = FALSE;

/* Init preferences structure */
prefs_t prefs = {DEF_USE_COPY,        DEF_USE_PRIMARY,      DEF_SYNCHRONIZE,
                 DEF_SAVE_HISTORY,    DEF_HISTORY_LIMIT,
                 DEF_SMALL_HISTORY,   DEF_HISTORY_SMALL,
                 DEF_HYPERLINKS_ONLY, DEF_CONFIRM_CLEAR,    DEF_FULL_HIST_BUTTON,
                 DEF_SINGLE_LINE,     DEF_REVERSE_HISTORY,  DEF_ITEM_LENGTH,
                 DEF_ELLIPSIZE,
                 INIT_HISTORY_KEY,    INIT_ACTIONS_KEY,     INIT_MENU_KEY,
                 INIT_SEARCH_KEY,     DEF_NO_ICON};

GtkListStore* search_list;
GtkWidget *search_entry;
GtkTreeSelection* search_selection;
GtkWidget* search_dialog;

/* Called every CHECK_INTERVAL seconds to check for new items */
static gboolean
item_check(gpointer data)
{
  /* Grab the current primary and clipboard text */
  gchar* primary_temp = gtk_clipboard_wait_for_text(primary);
  gchar* clipboard_temp = gtk_clipboard_wait_for_text(clipboard);
  
  /* What follows is an extremely confusing system of tests and crap... */
  
  /* Check if primary contents were lost */
  if ((primary_temp == NULL) && (primary_text != NULL))
  {
    /* Check contents */
    gint count;
    GdkAtom *targets;
    gboolean contents = gtk_clipboard_wait_for_targets(primary, &targets, &count);
    g_free(targets);
    /* Only recover lost contents if there isn't any other type of content in the clipboard */
    if (!contents)
    {
      gtk_clipboard_set_text(primary, primary_text, -1);
	/*
	 * in a future version...
	 * GSList* element = g_slist_nth(history, 0);
	 * gtk_clipboard_set_text(clipboard, (gchar*)element->data, -1);
	 * gtk_clipboard_set_text(primary, (gchar*)element->data, -1);
	 */
    }
  }
  else
  {
    GdkModifierType button_state;
    gdk_window_get_pointer(NULL, NULL, NULL, &button_state);
    /* Proceed if mouse button not being held */
    if ((primary_temp != NULL) && !(button_state & GDK_BUTTON1_MASK))
    {
      /* Check if primary is the same as the last entry */
      if (g_strcmp0(primary_temp, primary_text) != 0)
      {
        /* New primary entry */
        g_free(primary_text);
        primary_text = g_strdup(primary_temp);
        /* Check if primary option is enabled and if there's text to add */
        if (prefs.use_primary && primary_text)
        {
          /* Check contents before adding */
          if (prefs.hyperlinks_only && is_hyperlink(primary_text))
          {
            delete_duplicate(primary_text);
            append_item(primary_text);
          }
          else
          {
            delete_duplicate(primary_text);
            append_item(primary_text);
          }
        }
      }
    }
  }
  
  /* Check if clipboard contents were lost */
  if ((clipboard_temp == NULL) && (clipboard_text != NULL))
  {
    /* Check contents */
    gint count;
    GdkAtom *targets;
    gboolean contents = gtk_clipboard_wait_for_targets(primary, &targets, &count);
    g_free(targets);
		/* Only recover lost contents if there isn't any other type of content in the clipboard */
		if (!contents)
		{
      g_print("Clipboard is null, recovering ...\n");
      gtk_clipboard_set_text(clipboard, clipboard_text, -1);
    }
  }
  else
  {
    /* Check if clipboard is the same as the last entry */
    if (g_strcmp0(clipboard_temp, clipboard_text) != 0)
    {
      /* New clipboard entry */
      g_free(clipboard_text);
      clipboard_text = g_strdup(clipboard_temp);
      /* Check if clipboard option is enabled and if there's text to add */
      if (prefs.use_copy && clipboard_text)
      {
        /* Check contents before adding */
        if (prefs.hyperlinks_only && is_hyperlink(clipboard_text))
        {
          delete_duplicate(clipboard_text);
          append_item(clipboard_text);
        }
        else
        {
          delete_duplicate(clipboard_text);
          append_item(clipboard_text);
        }
      }
    }
  }
  
  /* Synchronization */
  if (prefs.synchronize)
  {
    if (g_strcmp0(synchronized_text, primary_text) != 0)
    {
      g_free(synchronized_text);
      synchronized_text = g_strdup(primary_text);
      gtk_clipboard_set_text(clipboard, primary_text, -1);
    }
    else if (g_strcmp0(synchronized_text, clipboard_text) != 0)
    {
      g_free(synchronized_text);
      synchronized_text = g_strdup(clipboard_text);
      gtk_clipboard_set_text(primary, clipboard_text, -1);
    }
  }
  
  /* Cleanup */
  g_free(primary_temp);
  g_free(clipboard_temp);
  
  return TRUE;
}

/* Thread function called for each action performed */
static void *
execute_action(void *command)
{
  /* Execute action */
  int sys_return;
  actions_lock = TRUE;
  if (!prefs.no_icon)
  {
  gtk_status_icon_set_from_stock((GtkStatusIcon*)status_icon, GTK_STOCK_EXECUTE);
  gtk_status_icon_set_tooltip((GtkStatusIcon*)status_icon, _("Executing action..."));
  }
  sys_return = system((gchar*)command);
  if (!prefs.no_icon)
  {
  gtk_status_icon_set_from_stock((GtkStatusIcon*)status_icon, GTK_STOCK_PASTE);
  gtk_status_icon_set_tooltip((GtkStatusIcon*)status_icon, _("Clipboard Manager"));
  }
  actions_lock = FALSE;
  g_free((gchar*)command);
  /* Exit this thread */
  pthread_exit(NULL);
}

/* Called when execution action exits */
static void
action_exit(GPid pid, gint status, gpointer data)
{
  g_spawn_close_pid(pid);
  if (!prefs.no_icon)
  {
    gtk_status_icon_set_from_stock((GtkStatusIcon*)status_icon, GTK_STOCK_PASTE);
    gtk_status_icon_set_tooltip((GtkStatusIcon*)status_icon, _("Clipboard Manager"));
  }
  actions_lock = FALSE;
}

/* Called when an action is selected from actions menu */
static void
action_selected(GtkButton *button, gpointer user_data)
{
  /* Change icon and enable lock */
  actions_lock = TRUE;
  if (!prefs.no_icon)
  {
    gtk_status_icon_set_from_stock((GtkStatusIcon*)status_icon, GTK_STOCK_EXECUTE);
    gtk_status_icon_set_tooltip((GtkStatusIcon*)status_icon, _("Executing action..."));
  }
  
  /* Insert clipboard into command (user_data), and prepare it for execution */
  gchar* clipboard_text = gtk_clipboard_wait_for_text(clipboard);
  gchar* command = g_markup_printf_escaped((gchar*)user_data, clipboard_text);
  g_free(clipboard_text);
  g_free(user_data);
  gchar* shell_command = g_shell_quote(command);
  g_free(command);
  gchar* cmd = g_strconcat("/bin/sh -c ", shell_command, NULL);
  g_free(shell_command);
  
  /* Execute action */
  GPid pid;
  gchar **argv;
  g_shell_parse_argv(cmd, NULL, &argv, NULL);
  g_free(cmd);
  g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, NULL);
  g_child_watch_add(pid, (GChildWatchFunc)action_exit, NULL);
  g_strfreev(argv);
}

/* Called when Edit Actions is selected from actions menu */
static void
edit_actions_selected(GtkButton *button, gpointer user_data)
{
  /* This helps prevent multiple instances */
  if (!gtk_grab_get_current())
    /* Show the preferences dialog on the actions tab */
    show_preferences(ACTIONS_TAB);
}

/* Called when Edit is selected from history menu */
static void
edit_selected(GtkMenuItem *menu_item, gpointer user_data)
{
  /* This helps prevent multiple instances */
  if (!gtk_grab_get_current())
  {
    /* Create clipboard buffer and set its text */
    GtkTextBuffer* clipboard_buffer = gtk_text_buffer_new(NULL);
    gchar* current_clipboard_text = gtk_clipboard_wait_for_text(clipboard);
    if (current_clipboard_text != NULL)
    {
      gtk_text_buffer_set_text(clipboard_buffer, current_clipboard_text, -1);
    }
    
    /* Create the dialog */
    GtkWidget* dialog = gtk_dialog_new_with_buttons(_("Editing Clipboard"), NULL,
                                                   (GTK_DIALOG_MODAL   +    GTK_DIALOG_NO_SEPARATOR),
                                                    GTK_STOCK_CANCEL,       GTK_RESPONSE_REJECT,
                                                    GTK_STOCK_OK,           GTK_RESPONSE_ACCEPT, NULL);
    
    gtk_window_set_default_size((GtkWindow*)dialog, 450, 300);
    gtk_window_set_icon((GtkWindow*)dialog, gtk_widget_render_icon(dialog, GTK_STOCK_EDIT, GTK_ICON_SIZE_MENU, NULL));
    
    /* Build the scrolled window with the text view */
    GtkWidget* scrolled_window = gtk_scrolled_window_new((GtkAdjustment*) gtk_adjustment_new(0, 0, 0, 0, 0, 0),
                                                         (GtkAdjustment*) gtk_adjustment_new(0, 0, 0, 0, 0, 0));
    
    gtk_scrolled_window_set_policy((GtkScrolledWindow*)scrolled_window,
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), scrolled_window, TRUE, TRUE, 2);
    GtkWidget* text_view = gtk_text_view_new_with_buffer(clipboard_buffer);
    gtk_text_view_set_left_margin((GtkTextView*)text_view, 2);
    gtk_text_view_set_right_margin((GtkTextView*)text_view, 2);
    gtk_container_add((GtkContainer*)scrolled_window, text_view);
    
    /* Run the dialog */
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run((GtkDialog*)dialog) == GTK_RESPONSE_ACCEPT)
    {
      /* Save changes done to the clipboard */
      GtkTextIter start, end;
      gtk_text_buffer_get_start_iter(clipboard_buffer, &start);
      gtk_text_buffer_get_end_iter(clipboard_buffer, &end);
      gchar* new_clipboard_text = gtk_text_buffer_get_text(clipboard_buffer, &start, &end, TRUE);
      gtk_clipboard_set_text(clipboard, new_clipboard_text, -1);
      g_free(new_clipboard_text);
    }
    gtk_widget_destroy(dialog);
    g_free(current_clipboard_text);
  }
}

/* Called when an item is selected from history menu */
static void
item_selected(GtkMenuItem *menu_item, gpointer user_data)
{
  /* Get the text from the right element and set as clipboard */
  GSList* element = g_slist_nth(history, (gint64)user_data);
  gtk_clipboard_set_text(clipboard, (gchar*)element->data, -1);
  gtk_clipboard_set_text(primary, (gchar*)element->data, -1);
}

/* Called when Clear is selected from history menu */
static void
clear_selected(GtkMenuItem *menu_item, gpointer user_data)
{
  /* Check for confirm clear option */
  if (prefs.confirm_clear)
  {
    GtkWidget* confirm_dialog = gtk_message_dialog_new(NULL,
                                                       GTK_DIALOG_MODAL,
                                                       GTK_MESSAGE_OTHER,
                                                       GTK_BUTTONS_OK_CANCEL,
                                                       _("Clear the history?"));
    
    if (gtk_dialog_run((GtkDialog*)confirm_dialog) == GTK_RESPONSE_OK)
    {
      /* Clear history and free history-related variables */
      g_slist_free(history);
      history = NULL;
      save_history();
      g_free(primary_text);
      g_free(clipboard_text);
      g_free(synchronized_text);
      primary_text = g_strdup("");
      clipboard_text = g_strdup("");
      synchronized_text = g_strdup("");
      gtk_clipboard_set_text(primary, "", -1);
      gtk_clipboard_set_text(clipboard, "", -1);
    }
    gtk_widget_destroy(confirm_dialog);
  }
  else
  {
    /* Clear history and free history-related variables */
    g_slist_free(history);
    history = NULL;
    save_history();
    g_free(primary_text);
    g_free(clipboard_text);
    g_free(synchronized_text);
    primary_text = g_strdup("");
    clipboard_text = g_strdup("");
    synchronized_text = g_strdup("");
    gtk_clipboard_set_text(primary, "", -1);
    gtk_clipboard_set_text(clipboard, "", -1);
  }
}

/* Called when About is selected from right-click menu */
static void
show_about_dialog(GtkMenuItem *menu_item, gpointer user_data)
{
  /* This helps prevent multiple instances */
  if (!gtk_grab_get_current())
  {
    const gchar* authors[] = {"Cristian Henzel <oss@web-tm.com>\n"
				"Gilberto \"Xyhthyx\" Miralla <xyhthyx@gmail.com>", NULL};
    const gchar* license =
      "This program is free software; you can redistribute it and/or modify\n"
      "it under the terms of the GNU General Public License as published by\n"
      "the Free Software Foundation; either version 3 of the License, or\n"
      "(at your option) any later version.\n"
      "\n"
      "This program is distributed in the hope that it will be useful,\n"
      "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
      "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
      "GNU General Public License for more details.\n"
      "\n"
      "You should have received a copy of the GNU General Public License\n"
      "along with this program.  If not, see <http://www.gnu.org/licenses/>.";
    
    /* Create the about dialog */
    GtkWidget* about_dialog = gtk_about_dialog_new();
    gtk_window_set_icon((GtkWindow*)about_dialog,
                        gtk_widget_render_icon(about_dialog, GTK_STOCK_ABOUT, GTK_ICON_SIZE_MENU, NULL));
    
    gtk_about_dialog_set_name((GtkAboutDialog*)about_dialog, "ClipIt");
    #ifdef HAVE_CONFIG_H
    gtk_about_dialog_set_version((GtkAboutDialog*)about_dialog, VERSION);
    #endif
    gtk_about_dialog_set_comments((GtkAboutDialog*)about_dialog,
                                _("Lightweight GTK+ clipboard manager."));
    
    gtk_about_dialog_set_website((GtkAboutDialog*)about_dialog,
                                 "COMING_SOON");
    
    gtk_about_dialog_set_copyright((GtkAboutDialog*)about_dialog, "Copyright (C) 2010 Cristian Henzel");
    gtk_about_dialog_set_authors((GtkAboutDialog*)about_dialog, authors);
    gtk_about_dialog_set_translator_credits ((GtkAboutDialog*)about_dialog,
                                             "Miloš Koutný <milos.koutny@gmail.com>\n"
                                             "Kim Jensen <reklamepost@energimail.dk>\n"
                                             "Eckhard M. Jäger <bart@neeneenee.de>\n"
                                             "Michael Stempin <mstempin@web.de>\n"
                                             "Benjamin 'sphax3d' Danon <sphax3d@gmail.com>\n"
                                             "Németh Tamás <ntomasz@vipmail.hu>\n"
                                             "Davide Truffa <davide@catoblepa.org>\n"
                                             "Jiro Kawada <jiro.kawada@gmail.com>\n"
                                             "Øyvind Sæther <oyvinds@everdot.org>\n"
                                             "pankamyk <pankamyk@o2.pl>\n"
                                             "Tomasz Rusek <tomek.rusek@gmail.com>\n"
                                             "Phantom X <megaphantomx@bol.com.br>\n"
                                             "Ovidiu D. Niţan <ov1d1u@sblug.ro>\n"
                                             "Alexander Kazancev <kazancas@mandriva.ru>\n"
                                             "Daniel Nylander <po@danielnylander.se>\n"
                                             "Hedef Türkçe <iletisim@hedefturkce.com>\n"
                                             "Lyman Li <lymanrb@gmail.com>\n"
                                             "Gilberto \"Xyhthyx\" Miralla <xyhthyx@gmail.com>");
    
    gtk_about_dialog_set_license((GtkAboutDialog*)about_dialog, license);
    gtk_about_dialog_set_logo_icon_name((GtkAboutDialog*)about_dialog, GTK_STOCK_PASTE);
    /* Run the about dialog */
    gtk_dialog_run((GtkDialog*)about_dialog);
    gtk_widget_destroy(about_dialog);
  }
}

static void
search_doubleclick()
{
  GtkTreeIter sel_iter;
  /* Check if selected */
  if (gtk_tree_selection_get_selected(search_selection, NULL, &sel_iter))
  {
    gchar *selected_item;
    gtk_tree_model_get((GtkTreeModel*)search_list, &sel_iter, 0, &selected_item, -1);
    GString* s_selected_item = g_string_new(selected_item);
    GtkClipboard* clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clip, (gchar*)selected_item, -1);
    gtk_widget_destroy(search_dialog);
    g_free(selected_item);
    g_string_free(s_selected_item, TRUE);
  }
}

gint
search_click(GtkWidget *widget, GdkEventButton *event, gpointer func_data)
{
  if(event->type==GDK_2BUTTON_PRESS || event->type==GDK_3BUTTON_PRESS)
  {
    search_doubleclick();
  }
  return FALSE;
}

/* Search through the history */
void
search_history()
{
  guint16 search_len = gtk_entry_get_text_length((GtkEntry*)search_entry);
  gchar* search_input = g_strdup(gtk_entry_get_text((GtkEntry*)search_entry));
  gchar* search_reg = g_regex_escape_string(search_input, search_len);
  gchar* search_regex = g_strconcat (".*", search_reg, ".*", NULL);
  GRegex* regex = g_regex_new(search_regex, G_REGEX_CASELESS, 0, NULL);
  GtkTreeIter search_iter;
  while(gtk_tree_model_get_iter_first((GtkTreeModel*)search_list, &search_iter))
    gtk_list_store_remove(search_list, &search_iter);
  // Test if there is text in the search box 
  if(search_len > 0)
  {
    if ((history != NULL) && (history->data != NULL))
    {
      // Declare some variables 
      GSList* element;
      gint element_number = 0;
      // Go through each element and adding each 
      for (element = history; element != NULL; element = element->next)
      {
        GString* string = g_string_new((gchar*)element->data);
	gchar* compare = string->str;
        gboolean result = g_regex_match(regex, compare, 0, NULL);
	//g_free(compare);
        if(result)
        {
          GtkTreeIter row_iter;
          gtk_list_store_append(search_list, &row_iter);
          gtk_list_store_set(search_list, &row_iter, 0, string->str, -1);
        }
        // Prepare for next item
        g_string_free(string, TRUE);
        element_number++;
      }
    }
    g_regex_unref(regex);
  }
  g_free(search_regex);
}

gint
search_key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  /* Check if [Return] key was pressed */
  if ((event->keyval == 0xff0d) || (event->keyval == 0xff8d))
    search_history();
  return FALSE;
}

/* Shows the search dialog */
void
show_search()
{
  /* Declare some variables */
  GtkWidget *frame,     *label,
            *alignment, *hbox,
            *vbox;

  GtkTreeViewColumn *tree_column;
  
  /* Create the dialog */
  search_dialog = gtk_dialog_new_with_buttons(_("Search"),     NULL,
                                                   (GTK_DIALOG_MODAL  + GTK_DIALOG_NO_SEPARATOR),
                                                    GTK_STOCK_CANCEL,   GTK_RESPONSE_CANCEL, NULL);

  gtk_window_set_icon((GtkWindow*)search_dialog, gtk_widget_render_icon(search_dialog, GTK_STOCK_FIND, GTK_ICON_SIZE_MENU, NULL));
  gtk_window_set_resizable((GtkWindow*)search_dialog, FALSE);

  GtkWidget* vbox_search = gtk_vbox_new(FALSE, 12);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area (GTK_DIALOG(search_dialog))), vbox_search, TRUE, TRUE, 2);
  gtk_widget_set_size_request((GtkWidget*)vbox_search, 400, 600);
  gtk_window_set_position((GtkWindow*)search_dialog, GTK_WIN_POS_CENTER);

  hbox = gtk_hbox_new(TRUE, 4);
  gtk_box_pack_start((GtkBox*)vbox_search, hbox, FALSE, FALSE, 0);
  search_entry = gtk_entry_new();
  gtk_box_pack_end((GtkBox*)hbox, search_entry, TRUE, TRUE, 0);
  GtkWidget* add_button_exclude = gtk_button_new_with_label(_("Search"));
  g_signal_connect((GObject*)search_entry, "key-press-event", (GCallback)search_key_pressed, NULL);
  //label = gtk_label_new(_("Search for:"));
  //gtk_label_set_line_wrap((GtkLabel*)label, TRUE);
  //gtk_misc_set_alignment((GtkMisc*)label, 0.0, 0.50);
  //gtk_label_set_max_width_chars((GtkMisc*)label, 12);
  //gtk_box_pack_start((GtkBox*)hbox, label, FALSE, FALSE, 0);

  /* Build the exclude treeview */
  GtkWidget* scrolled_window_search = gtk_scrolled_window_new(
                               (GtkAdjustment*)gtk_adjustment_new(0, 0, 0, 0, 0, 0),
                               (GtkAdjustment*)gtk_adjustment_new(0, 0, 0, 0, 0, 0));
  
  gtk_scrolled_window_set_policy((GtkScrolledWindow*)scrolled_window_search, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type((GtkScrolledWindow*)scrolled_window_search, GTK_SHADOW_ETCHED_OUT);
  GtkWidget* treeview_search = gtk_tree_view_new();
  gtk_tree_view_set_reorderable((GtkTreeView*)treeview_search, TRUE);
  gtk_tree_view_set_rules_hint((GtkTreeView*)treeview_search, TRUE);
  search_list = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
  gtk_tree_view_set_model((GtkTreeView*)treeview_search, (GtkTreeModel*)search_list);
  GtkCellRenderer* name_renderer_exclude = gtk_cell_renderer_text_new();
  g_object_set(name_renderer_exclude, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  g_object_set(name_renderer_exclude, "single-paragraph-mode", TRUE, NULL);
  tree_column = gtk_tree_view_column_new_with_attributes(_("Results"), name_renderer_exclude, "text", 0, NULL);
  gtk_tree_view_column_set_resizable(tree_column, TRUE);
  gtk_tree_view_append_column((GtkTreeView*)treeview_search, tree_column);
  gtk_container_add((GtkContainer*)scrolled_window_search, treeview_search);
  gtk_box_pack_start((GtkBox*)vbox_search, scrolled_window_search, TRUE, TRUE, 0);

  search_selection = gtk_tree_view_get_selection((GtkTreeView*)treeview_search);
  gtk_tree_selection_set_mode(search_selection, GTK_SELECTION_BROWSE);
  g_signal_connect((GObject*)treeview_search, "button_press_event", (GCallback)search_click, NULL);

  gtk_widget_show_all(search_dialog);

  /* Run the dialog */
  gtk_dialog_run((GtkDialog*)search_dialog);
  gtk_widget_destroy(search_dialog);
}

/* Called when Preferences is selected from right-click menu */
static void
preferences_selected(GtkMenuItem *menu_item, gpointer user_data)
{
  /* This helps prevent multiple instances */
  if (!gtk_grab_get_current())
    /* Show the preferences dialog */
    show_preferences(0);
}

/* Called when Quit is selected from right-click menu */
static void
quit_selected(GtkMenuItem *menu_item, gpointer user_data)
{
  /* Prevent quit with dialogs open */
  if (!gtk_grab_get_current())
    /* Quit the program */
    gtk_main_quit();
}

/* Called when status icon is control-clicked */
static gboolean
show_actions_menu(gpointer data)
{
  /* Declare some variables */
  GtkWidget *menu,       *menu_item,
            *menu_image, *item_label;
  
  /* Create menu */
  menu = gtk_menu_new();
  g_signal_connect((GObject*)menu, "selection-done", (GCallback)gtk_widget_destroy, NULL);
  /* Actions using: */
  menu_item = gtk_image_menu_item_new_with_label("Actions using:");
  menu_image = gtk_image_new_from_stock(GTK_STOCK_EXECUTE, GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image((GtkImageMenuItem*)menu_item, menu_image);
  g_signal_connect((GObject*)menu_item, "select", (GCallback)gtk_menu_item_deselect, NULL);
  gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  /* Clipboard contents */
  gchar* text = gtk_clipboard_wait_for_text(clipboard);
  if (text != NULL)
  {
    menu_item = gtk_menu_item_new_with_label("None");
    /* Modify menu item label properties */
    item_label = gtk_bin_get_child((GtkBin*)menu_item);
    gtk_label_set_single_line_mode((GtkLabel*)item_label, TRUE);
    gtk_label_set_ellipsize((GtkLabel*)item_label, prefs.ellipsize);
    gtk_label_set_width_chars((GtkLabel*)item_label, 30);
    /* Making bold... */
    gchar* bold_text = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup((GtkLabel*)item_label, bold_text);
    g_free(bold_text);
    /* Append menu item */
    g_signal_connect((GObject*)menu_item, "select", (GCallback)gtk_menu_item_deselect, NULL);
    gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  }
  else
  {
    /* Create menu item for empty clipboard contents */
    menu_item = gtk_menu_item_new_with_label("None");
    /* Modify menu item label properties */
    item_label = gtk_bin_get_child((GtkBin*)menu_item);
    gtk_label_set_markup((GtkLabel*)item_label, _("<b>None</b>"));
    /* Append menu item */
    g_signal_connect((GObject*)menu_item, "select", (GCallback)gtk_menu_item_deselect, NULL);
    
    gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  }
  /* -------------------- */
  gtk_menu_shell_append((GtkMenuShell*)menu, gtk_separator_menu_item_new());
  /* Actions */
  gchar* path = g_build_filename(g_get_home_dir(), ACTIONS_FILE, NULL);
  FILE* actions_file = fopen(path, "rb");
  g_free(path);
  /* Check that it opened and begin read */
  if (actions_file)
  {
    gint size;
    size_t fread_return;
    fread_return = fread(&size, 4, 1, actions_file);
    /* Check if actions file is empty */
    if (!size)
    {
      /* File contained no actions so adding empty */
      menu_item = gtk_menu_item_new_with_label(_("Empty"));
      gtk_widget_set_sensitive(menu_item, FALSE);
      gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
    }
    /* Continue reading items until size is 0 */
    while (size)
    {
      /* Read name */
      gchar* name = (gchar*)g_malloc(size + 1);
      fread_return = fread(name, size, 1, actions_file);
      name[size] = '\0';
      menu_item = gtk_menu_item_new_with_label(name);
      g_free(name);
      fread_return = fread(&size, 4, 1, actions_file);
      /* Read command */
      gchar* command = (gchar*)g_malloc(size + 1);
      fread_return = fread(command, size, 1, actions_file);
      command[size] = '\0';
      fread_return = fread(&size, 4, 1, actions_file);
      /* Append the action */
      gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
      g_signal_connect((GObject*)menu_item,        "activate",
                       (GCallback)action_selected, (gpointer)command);      
    }
    fclose(actions_file);
  }
  else
  {
    /* File did not open so adding empty */
    menu_item = gtk_menu_item_new_with_label(_("Empty"));
    gtk_widget_set_sensitive(menu_item, FALSE);
    gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  }
  /* -------------------- */
  gtk_menu_shell_append((GtkMenuShell*)menu, gtk_separator_menu_item_new());
  /* Edit actions */
  menu_item = gtk_image_menu_item_new_with_mnemonic(_("_Edit actions"));
  menu_image = gtk_image_new_from_stock(GTK_STOCK_EDIT, GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image((GtkImageMenuItem*)menu_item, menu_image);
  g_signal_connect((GObject*)menu_item, "activate", (GCallback)edit_actions_selected, NULL);
  gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  /* Popup the menu... */
  gtk_widget_show_all(menu);
  gtk_menu_popup((GtkMenu*)menu, NULL, NULL, NULL, NULL, 1, gtk_get_current_event_time());
  /* Return false so the g_timeout_add() function is called only once */
  return FALSE;
}

static gboolean
show_history_menu_full(gpointer data)
{
  /* Declare some variables */
  GtkWidget *menu,       *menu_item,
            *menu_image, *item_label;
  
  /* Create the menu */
  menu = gtk_menu_new();
  g_signal_connect((GObject*)menu, "selection-done", (GCallback)gtk_widget_destroy, NULL);
  /* Edit clipboard */
  menu_item = gtk_image_menu_item_new_with_mnemonic(_("_Edit Clipboard"));
  menu_image = gtk_image_new_from_stock(GTK_STOCK_EDIT, GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image((GtkImageMenuItem*)menu_item, menu_image);
  g_signal_connect((GObject*)menu_item, "activate", (GCallback)edit_selected, NULL);
  gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  /* -------------------- */
  gtk_menu_shell_append((GtkMenuShell*)menu, gtk_separator_menu_item_new());
  /* Items */
  if ((history != NULL) && (history->data != NULL))
  {
    /* Declare some variables */
    GSList* element;
    gint element_number = 0;
    gchar* primary_temp = gtk_clipboard_wait_for_text(primary);
    gchar* clipboard_temp = gtk_clipboard_wait_for_text(clipboard);
    /* Reverse history if enabled */
    if (prefs.reverse_history)
    {
      history = g_slist_reverse(history);
      element_number = g_slist_length(history) - 1;
    }
    /* Go through each element and adding each */
    for (element = history; element != NULL; element = element->next)
    {
      GString* string = g_string_new((gchar*)element->data);
      /* Remove control characters */
      int i = 0;
      while (i < string->len)
      {
        if (string->str[i] == '\n')
          g_string_erase(string, i, 1);
        else
          i++;
      }
      /* Ellipsize text */
      if (string->len > prefs.item_length)
      {
        switch (prefs.ellipsize)
        {
          case PANGO_ELLIPSIZE_START:
            string = g_string_erase(string, 0, string->len-(prefs.item_length));
            string = g_string_prepend(string, "...");
            break;
          case PANGO_ELLIPSIZE_MIDDLE:
            string = g_string_erase(string, (prefs.item_length/2), string->len-(prefs.item_length));
            string = g_string_insert(string, (string->len/2), "...");
            break;
          case PANGO_ELLIPSIZE_END:
            string = g_string_truncate(string, prefs.item_length);
            string = g_string_append(string, "...");
            break;
        }
      }
      /* Make new item with ellipsized text */
      menu_item = gtk_menu_item_new_with_label(string->str);
      g_signal_connect((GObject*)menu_item,      "activate",
                       (GCallback)item_selected, (gpointer)(gint64)element_number);
      
      /* Modify menu item label properties */
      item_label = gtk_bin_get_child((GtkBin*)menu_item);
      gtk_label_set_single_line_mode((GtkLabel*)item_label, prefs.single_line);
      
      /* Check if item is also clipboard text and make bold */
      if ((clipboard_temp) && (g_strcmp0((gchar*)element->data, clipboard_temp) == 0))
      {
        gchar* bold_text = g_markup_printf_escaped("<b>%s</b>", string->str);
        gtk_label_set_markup((GtkLabel*)item_label, bold_text);
        g_free(bold_text);
      }
      else if ((primary_temp) && (g_strcmp0((gchar*)element->data, primary_temp) == 0))
      {
        gchar* italic_text = g_markup_printf_escaped("<i>%s</i>", string->str);
        gtk_label_set_markup((GtkLabel*)item_label, italic_text);
        g_free(italic_text);
      }
      /* Append item */
      gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
      /* Prepare for next item */
      g_string_free(string, TRUE);
      if (prefs.reverse_history)
        element_number--;
      else
        element_number++;
    }
    /* Cleanup */
    g_free(primary_temp);
    g_free(clipboard_temp);
    /* Return history to normal if reversed */
    if (prefs.reverse_history)
      history = g_slist_reverse(history);
  }
  else
  {
    /* Nothing in history so adding empty */
    menu_item = gtk_menu_item_new_with_label(_("Empty"));
    gtk_widget_set_sensitive(menu_item, FALSE);
    gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  }
  /* -------------------- */
  gtk_menu_shell_append((GtkMenuShell*)menu, gtk_separator_menu_item_new());
  /* Clear */
  menu_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_CLEAR, NULL);
  g_signal_connect((GObject*)menu_item, "activate", (GCallback)clear_selected, NULL);
  gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  /* Popup the menu... */
  gtk_widget_show_all(menu);
  gtk_menu_popup((GtkMenu*)menu, NULL, NULL, NULL, NULL, 1, gtk_get_current_event_time());
  /* Return FALSE so the g_timeout_add() function is called only once */
  return FALSE;
}

/* Generates the small history menu */
static gboolean
show_history_menu_small(gpointer data)
{
  /* Declare some variables */
  GtkWidget *menu,       *menu_item,
            *menu_image, *item_label;
  
  /* Create the menu */
  menu = gtk_menu_new();
  g_signal_connect((GObject*)menu, "selection-done", (GCallback)gtk_widget_destroy, NULL);
  /* Edit clipboard
  menu_item = gtk_image_menu_item_new_with_mnemonic(_("_Edit Clipboard"));
  menu_image = gtk_image_new_from_stock(GTK_STOCK_EDIT, GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image((GtkImageMenuItem*)menu_item, menu_image);
  g_signal_connect((GObject*)menu_item, "activate", (GCallback)edit_selected, NULL);
  gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  -------------------- */
  // gtk_menu_shell_append((GtkMenuShell*)menu, gtk_separator_menu_item_new());
  /* Items */
  if ((history != NULL) && (history->data != NULL))
  {
    /* Declare some variables */
    GSList* element;
    gint element_number = 0;
    gint element_number_small = 0;
    gchar* primary_temp = gtk_clipboard_wait_for_text(primary);
    gchar* clipboard_temp = gtk_clipboard_wait_for_text(clipboard);
    /* Reverse history if enabled */
    if (prefs.reverse_history)
    {
      history = g_slist_reverse(history);
      element_number = g_slist_length(history) - 1;
    }
    /* Go through each element and adding each */
    for (element = history; (element != NULL) && (element_number_small < prefs.history_small); element = element->next)
    {
      GString* string = g_string_new((gchar*)element->data);
      /* Remove control characters */
      int i = 0;
      while (i < string->len)
      {
        if (string->str[i] == '\n')
          g_string_erase(string, i, 1);
        else
          i++;
      }
      /* Ellipsize text */
      if (string->len > prefs.item_length)
      {
        switch (prefs.ellipsize)
        {
          case PANGO_ELLIPSIZE_START:
            string = g_string_erase(string, 0, string->len-(prefs.item_length));
            string = g_string_prepend(string, "...");
            break;
          case PANGO_ELLIPSIZE_MIDDLE:
            string = g_string_erase(string, (prefs.item_length/2), string->len-(prefs.item_length));
            string = g_string_insert(string, (string->len/2), "...");
            break;
          case PANGO_ELLIPSIZE_END:
            string = g_string_truncate(string, prefs.item_length);
            string = g_string_append(string, "...");
            break;
        }
      }
      /* Make new item with ellipsized text */
      menu_item = gtk_menu_item_new_with_label(string->str);
      g_signal_connect((GObject*)menu_item,      "activate",
                       (GCallback)item_selected, (gpointer)(gint64)element_number);
      
      /* Modify menu item label properties */
      item_label = gtk_bin_get_child((GtkBin*)menu_item);
      gtk_label_set_single_line_mode((GtkLabel*)item_label, prefs.single_line);
      
      /* Check if item is also clipboard text and make bold */
      if ((clipboard_temp) && (g_strcmp0((gchar*)element->data, clipboard_temp) == 0))
      {
        gchar* bold_text = g_markup_printf_escaped("<b>%s</b>", string->str);
        gtk_label_set_markup((GtkLabel*)item_label, bold_text);
        g_free(bold_text);
      }
      else if ((primary_temp) && (g_strcmp0((gchar*)element->data, primary_temp) == 0))
      {
        gchar* italic_text = g_markup_printf_escaped("<i>%s</i>", string->str);
        gtk_label_set_markup((GtkLabel*)item_label, italic_text);
        g_free(italic_text);
      }
      /* Append item */
      gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
      /* Prepare for next item */
      g_string_free(string, TRUE);
      if (prefs.reverse_history)
        element_number--;
      else
        element_number++;
			element_number_small++;
    }
    /* Cleanup */
    g_free(primary_temp);
    g_free(clipboard_temp);
    /* Return history to normal if reversed */
    if (prefs.reverse_history)
      history = g_slist_reverse(history);
  }
  else
  {
    /* Nothing in history so adding empty */
    menu_item = gtk_menu_item_new_with_label(_("Empty"));
    gtk_widget_set_sensitive(menu_item, FALSE);
    gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  }
  /* -------------------- */
  /* Show full history (if enabled) */
  if (!prefs.full_hist_button)
  {
    gtk_menu_shell_append((GtkMenuShell*)menu, gtk_separator_menu_item_new());
    menu_item = gtk_image_menu_item_new_with_mnemonic(_("_Show full history"));
    menu_image = gtk_image_new_from_stock(GTK_STOCK_ZOOM_IN, GTK_ICON_SIZE_MENU);
    gtk_image_menu_item_set_image((GtkImageMenuItem*)menu_item, menu_image);
    g_signal_connect((GObject*)menu_item, "activate", (GCallback)show_history_menu_full, NULL);
    gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  }
  /* Popup the menu... */
  gtk_widget_show_all(menu);
  gtk_menu_popup((GtkMenu*)menu, NULL, NULL, NULL, NULL, 1, gtk_get_current_event_time());
  /* Return FALSE so the g_timeout_add() function is called only once */
  return FALSE;
}

/* Called when status icon is left-clicked */
static gboolean
show_history_menu(gpointer data)
{
	if (prefs.small_history)
		g_timeout_add(POPUP_DELAY, show_history_menu_small, NULL);
	else
		g_timeout_add(POPUP_DELAY, show_history_menu_full, NULL);
  /* Return FALSE so the g_timeout_add() function is called only once */
  return FALSE;
}

/* Called when status icon is right-clicked */
static void
show_clipit_menu(GtkStatusIcon *status_icon, guint button,
                     guint activate_time,        gpointer user_data)
{
  /* Declare some variables */
  GtkWidget *menu,       *menu_item,
            *menu_image, *item_label;
  
  /* Create the menu */
  menu = gtk_menu_new();
  g_signal_connect((GObject*)menu, "selection-done", (GCallback)gtk_widget_destroy, NULL);
  /* About */
  menu_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_ABOUT, NULL);
  g_signal_connect((GObject*)menu_item, "activate", (GCallback)show_about_dialog, NULL);
  gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  /* Full history button (if enabled) */
  if (prefs.full_hist_button && prefs.small_history)
  {
    menu_item = gtk_image_menu_item_new_with_mnemonic(_("_Show full history"));
    menu_image = gtk_image_new_from_stock(GTK_STOCK_ZOOM_IN, GTK_ICON_SIZE_MENU);
    gtk_image_menu_item_set_image((GtkImageMenuItem*)menu_item, menu_image);
    g_signal_connect((GObject*)menu_item, "activate", (GCallback)show_history_menu_full, NULL);
    gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  }
  /* Search */
  menu_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_FIND, NULL);
  g_signal_connect((GObject*)menu_item, "activate", (GCallback)show_search, NULL);
  gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  /* Preferences */
  menu_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_PREFERENCES, NULL);
  g_signal_connect((GObject*)menu_item, "activate", (GCallback)preferences_selected, NULL);
  gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  /* -------------------- */
  gtk_menu_shell_append((GtkMenuShell*)menu, gtk_separator_menu_item_new());
  /* Quit */
  menu_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_QUIT, NULL);
  g_signal_connect((GObject*)menu_item, "activate", (GCallback)quit_selected, NULL);
  gtk_menu_shell_append((GtkMenuShell*)menu, menu_item);
  /* Popup the menu... */
  gtk_widget_show_all(menu);
  gtk_menu_popup((GtkMenu*)menu, NULL, NULL, NULL, user_data, button, activate_time);
}

/* Called when status icon is clicked */
/* (checks type of click and calls correct function */
static void
status_icon_clicked(GtkStatusIcon *status_icon, gpointer user_data)
{
  /* Check what type of click was recieved */
  GdkModifierType state;
  gtk_get_current_event_state(&state);
  /* Control click */
  if (state == GDK_MOD2_MASK+GDK_CONTROL_MASK || state == GDK_CONTROL_MASK)
  {
    if (actions_lock == FALSE)
    {
      g_timeout_add(POPUP_DELAY, show_actions_menu, NULL);
    }
  }
  /* Normal click */
  else
  {
    g_timeout_add(POPUP_DELAY, show_history_menu, NULL);
  }
}

/* Called when history global hotkey is pressed */
void
history_hotkey(char *keystring, gpointer user_data)
{
  g_timeout_add(POPUP_DELAY, show_history_menu, NULL);
}

/* Called when actions global hotkey is pressed */
void
actions_hotkey(char *keystring, gpointer user_data)
{
  g_timeout_add(POPUP_DELAY, show_actions_menu, NULL);
}

/* Called when actions global hotkey is pressed */
void
menu_hotkey(char *keystring, gpointer user_data)
{
  show_clipit_menu(status_icon, 0, 0, NULL);
}

/* Called when actions global hotkey is pressed */
void
search_hotkey(char *keystring, gpointer user_data)
{
  show_search();
}

/* Startup calls and initializations */
static void
clipit_init()
{
  /* Create clipboard */
  primary = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
  clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  g_timeout_add(CHECK_INTERVAL, item_check, NULL);
  
  /* Read preferences */
  read_preferences();
  
  /* Read history */
  if (prefs.save_history)
    read_history();
  
  /* Bind global keys */
  keybinder_init();
  keybinder_bind(prefs.history_key, history_hotkey, NULL);
  keybinder_bind(prefs.actions_key, actions_hotkey, NULL);
  keybinder_bind(prefs.menu_key, menu_hotkey, NULL);
  keybinder_bind(prefs.search_key, search_hotkey, NULL);
  
  /* Create status icon */
  if (!prefs.no_icon)
  {
    status_icon = gtk_status_icon_new_from_stock(GTK_STOCK_PASTE);
    gtk_status_icon_set_tooltip((GtkStatusIcon*)status_icon, _("Clipboard Manager"));
    g_signal_connect((GObject*)status_icon, "activate", (GCallback)status_icon_clicked, NULL);
    g_signal_connect((GObject*)status_icon, "popup-menu", (GCallback)show_clipit_menu, NULL);
  }
}

/* This is Sparta! */
int
main(int argc, char *argv[])
{
  bindtextdomain(GETTEXT_PACKAGE, CLIPITLOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);
  
  /* Initiate GTK+ */
  gtk_init(&argc, &argv);
  
  /* Parse options and exit if returns TRUE */
  if (argc > 1)
  {
    if (parse_options(argc, argv))
      return 0;
  }
  /* Read input from stdin (if any) */
  else
  {
    /* Check if stdin is piped */
    if (!isatty(fileno(stdin)))
    {
      GString* piped_string = g_string_new(NULL);
      /* Append stdin to string */
      while (1)
      {
        gchar* buffer = (gchar*)g_malloc(256);
        if (fgets(buffer, 256, stdin) == NULL)
        {
          g_free(buffer);
          break;
        }
        g_string_append(piped_string, buffer);
        g_free(buffer);
      }
      /* Check if anything was piped in */
      if (piped_string->len > 0)
      {
        /* Truncate new line character */
        /* g_string_truncate(piped_string, (piped_string->len - 1)); */
        /* Copy to clipboard */
        GtkClipboard* clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clip, piped_string->str, -1);
        gtk_clipboard_store(clip);
        /* Exit */
        return 0;
      }
      g_string_free(piped_string, TRUE);
    }
  }
  
  /* Init ClipIt */
  clipit_init();
  
  /* Run GTK+ loop */
  gtk_main();
  
  /* Unbind keys */
  keybinder_unbind(prefs.history_key, history_hotkey);
  keybinder_unbind(prefs.actions_key, actions_hotkey);
  keybinder_unbind(prefs.menu_key, menu_hotkey);
  keybinder_unbind(prefs.search_key, search_hotkey);
  /* Cleanup */
  g_free(prefs.history_key);
  g_free(prefs.actions_key);
  g_free(prefs.menu_key);
  g_free(prefs.search_key);
  g_slist_free(history);
  g_free(primary_text);
  g_free(clipboard_text);
  g_free(synchronized_text);
  
  /* Exit */
  return 0;
}

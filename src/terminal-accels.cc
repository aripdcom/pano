/*
 * Copyright © 2001, 2002 Havoc Pennington, Red Hat Inc.
 * Copyright © 2008, 2011, 2012, 2013 Christian Persch
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

#include "config.h"

#include <string.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "terminal-accels.hh"
#include "terminal-accel-row.hh"
#include "terminal-app.hh"
#include "terminal-debug.hh"
#include "terminal-schemas.hh"
#include "terminal-intl.hh"
#include "terminal-util.hh"
#include "terminal-libgsystem.hh"

/* NOTES
 *
 * There are two sources of keybindings changes, from GSettings and from
 * the accel map (happens with in-place menu editing).
 *
 * When a keybinding gsettings key changes, we propagate that into the
 * accel map.
 * When the accel map changes, we queue a sync to GSettings.
 *
 * To avoid infinite loops, we short-circuit in both directions
 * if the value is unchanged from last known.
 *
 * In the keybinding editor, when editing or clearing an accel, we write
 * the change directly to GSettings and rely on the callback to
 * actually apply the change to the accel map.
 */

#define KEY_CLOSE_TAB           "close-tab"
#define KEY_CLOSE_WINDOW        "close-window"
#define KEY_COPY                "copy"
#define KEY_COPY_HTML           "copy-html"
#define KEY_DETACH_TAB          "detach-tab"
#define KEY_FIND                "find"
#define KEY_FIND_CLEAR          "find-clear"
#define KEY_FIND_PREV           "find-previous"
#define KEY_FIND_NEXT           "find-next"
#define KEY_FULL_SCREEN         "full-screen"
#define KEY_HEADER_MENU         "header-menu"
#define KEY_HELP                "help"
#define KEY_MOVE_TAB_LEFT       "move-tab-left"
#define KEY_MOVE_TAB_RIGHT      "move-tab-right"
#define KEY_MOVE_TAB_START      "move-tab-start"
#define KEY_MOVE_TAB_END        "move-tab-end"
#define KEY_NEW_TAB             "new-tab"
#define KEY_NEW_WINDOW          "new-window"
#define KEY_NEXT_TAB            "next-tab"
#define KEY_PASTE               "paste"
#define KEY_PREFERENCES         "preferences"
#define KEY_PREV_TAB            "prev-tab"
#define KEY_READ_ONLY           "read-only"
#define KEY_RESET_AND_CLEAR     "reset-and-clear"
#define KEY_RESET               "reset"
#define KEY_SAVE_CONTENTS       "save-contents"
#define KEY_SELECT_ALL          "select-all"
#define KEY_SHOW_TABS_OVERVIEW  "show-tabs-overview"
#define KEY_ZOOM_IN             "zoom-in"
#define KEY_ZOOM_NORMAL         "zoom-normal"
#define KEY_ZOOM_OUT            "zoom-out"
#define KEY_SWITCH_TAB_PREFIX   "switch-to-tab-"
#define KEY_PREV_PROMPT         "previous-prompt"
#define KEY_NEXT_PROMPT         "next-prompt"
#define KEY_COPY_OUTPUT         "copy-output"
#define KEY_FOCUS_TAB_LIST      "focus-tab-list"
#define KEY_REOPEN_TAB          "reopen-closed-tab"
#define KEY_CLOSE_OTHER_TABS    "close-other-tabs"
#define KEY_SET_TITLE           "set-terminal-title"
#define KEY_COMMAND_PALETTE     "command-palette"
#define KEY_QUICK_SELECT        "quick-select"
#define KEY_BROADCAST_TAB       "broadcast-tab"
#define KEY_BROADCAST_WINDOW    "broadcast-window"
#define KEY_SPLIT_RIGHT         "split-right"
#define KEY_SPLIT_DOWN          "split-down"
#define KEY_CLOSE_PANE          "close-pane"
#define KEY_ZOOM_PANE           "zoom-pane"
#define KEY_EQUALIZE_PANES      "equalize-panes"
#define KEY_MOVE_PANE_TO_TAB    "move-pane-to-tab"
#define KEY_FOCUS_PANE_LEFT     "focus-pane-left"
#define KEY_FOCUS_PANE_RIGHT    "focus-pane-right"
#define KEY_FOCUS_PANE_UP       "focus-pane-up"
#define KEY_FOCUS_PANE_DOWN     "focus-pane-down"
#define KEY_RESIZE_PANE_LEFT    "resize-pane-left"
#define KEY_RESIZE_PANE_RIGHT   "resize-pane-right"
#define KEY_RESIZE_PANE_UP      "resize-pane-up"
#define KEY_RESIZE_PANE_DOWN    "resize-pane-down"
#define KEY_SWAP_PANE_LEFT      "swap-pane-left"
#define KEY_SWAP_PANE_RIGHT     "swap-pane-right"
#define KEY_SWAP_PANE_UP        "swap-pane-up"
#define KEY_SWAP_PANE_DOWN      "swap-pane-down"

typedef struct
{
  const char *user_visible_name;
  const char *settings_key;
  const char *action_name;
  const GVariantType *action_parameter_type;
  const char *action_parameter;
  GVariant *parameter;
  const char *shadow_action_name;
} KeyEntry;

typedef struct
{
  KeyEntry *key_entry;
  guint n_elements;
  const char *user_visible_name;
  gboolean headerbar_only;
} KeyEntryList;

#define ENTRY_FULL(name, key, action, type, parameter, shadow_name) \
  { name, key, "win." action, (const GVariantType *) type, parameter, nullptr, shadow_name }
#define ENTRY(name, key, action, type, parameter) \
  ENTRY_FULL (name, key, action, type, parameter, "win.shadow")
#define ENTRY_MDI(name, key, action, type, parameter) \
  ENTRY_FULL (name, key, action, type, parameter, "win.shadow-mdi")
/* Only consumed while the shell reports its prompts */
#define ENTRY_PROMPT(name, key, action, type, parameter) \
  ENTRY_FULL (name, key, action, type, parameter, "win.shadow-prompt")
/* Only consumed while the active tab is split */
#define ENTRY_PANE(name, key, action, type, parameter) \
  ENTRY_FULL (name, key, action, type, parameter, "win.shadow-pane")

static KeyEntry file_entries[] = {
  ENTRY (N_("New Tab"),       KEY_NEW_TAB,       "new-terminal",  "(ss)",  "('tab','current')"   ),
  ENTRY (N_("New Window"),    KEY_NEW_WINDOW,    "new-terminal",  "(ss)",  "('window','current')"),
  ENTRY (N_("Save Contents"), KEY_SAVE_CONTENTS, "save-contents", nullptr,    nullptr                  ),
  ENTRY (N_("Close Tab"),     KEY_CLOSE_TAB,     "close",         "s",     "'tab'"               ),
  ENTRY (N_("Close Other Tabs"), KEY_CLOSE_OTHER_TABS, "close",   "s",     "'other-tabs'"        ),
  ENTRY (N_("Reopen Closed Tab"), KEY_REOPEN_TAB, "reopen-tab",     nullptr, nullptr               ),
  ENTRY (N_("Close Window"),  KEY_CLOSE_WINDOW,  "close",         "s",     "'window'"            ),
};

static KeyEntry edit_entries[] = {
  ENTRY (N_("Copy"),                KEY_COPY,                "copy",         "s", "'text'"   ),
  ENTRY (N_("Copy as HTML"),        KEY_COPY_HTML,           "copy",         "s", "'html'"   ),
  ENTRY (N_("Paste"),               KEY_PASTE,               "paste-text",   nullptr, nullptr      ),
  ENTRY (N_("Select All"),          KEY_SELECT_ALL,          "select-all",   nullptr, nullptr      ),
  ENTRY (N_("Preferences"),         KEY_PREFERENCES,         "edit-preferences",  nullptr, nullptr      ),
};

static KeyEntry search_entries[] = {
  ENTRY (N_("Find"),            KEY_FIND,       "find",          nullptr, nullptr),
  ENTRY (N_("Find Next"),       KEY_FIND_NEXT,  "find-forward",  nullptr, nullptr),
  ENTRY (N_("Find Previous"),   KEY_FIND_PREV,  "find-backward", nullptr, nullptr),
  ENTRY (N_("Clear Highlight"), KEY_FIND_CLEAR, "find-clear",    nullptr, nullptr)
};

static KeyEntry view_entries[] = {
  ENTRY (N_("Full Screen"),           KEY_FULL_SCREEN,    "fullscreen",      nullptr, nullptr),
  ENTRY (N_("Zoom In"),               KEY_ZOOM_IN,        "zoom-in",         nullptr, nullptr),
  ENTRY (N_("Zoom Out"),              KEY_ZOOM_OUT,       "zoom-out",        nullptr, nullptr),
  ENTRY (N_("Normal Size"),           KEY_ZOOM_NORMAL,    "zoom-normal",     nullptr, nullptr),
  ENTRY (N_("Command Palette"),       KEY_COMMAND_PALETTE, "command-palette", nullptr, nullptr),
  ENTRY (N_("Quick Select"),          KEY_QUICK_SELECT,   "quick-select",    nullptr, nullptr),
};

static KeyEntry terminal_entries[] = {
  ENTRY (N_("Read-Only"),       KEY_READ_ONLY,          "read-only", nullptr, nullptr   ),
  ENTRY (N_("Set Title"),       KEY_SET_TITLE,          "set-title", nullptr, nullptr   ),
  ENTRY (N_("Reset"),           KEY_RESET,              "reset",     "b",  "false"),
  ENTRY (N_("Reset and Clear"), KEY_RESET_AND_CLEAR,    "reset",     "b",  "true" ),
  ENTRY_PROMPT (N_("Scroll to Previous Prompt"),    KEY_PREV_PROMPT, "scroll-to-prompt", "s", "'previous'"),
  ENTRY_PROMPT (N_("Scroll to Next Prompt"),        KEY_NEXT_PROMPT, "scroll-to-prompt", "s", "'next'"),
  ENTRY_PROMPT (N_("Copy Output of Last Command"),  KEY_COPY_OUTPUT, "copy-output",      nullptr, nullptr),
};

static KeyEntry tabs_entries[] = {
  ENTRY_MDI (N_("Switch to Previous Tab"), KEY_PREV_TAB,       "tab-switch-left",  nullptr, nullptr),
  ENTRY_MDI (N_("Switch to Next Tab"),     KEY_NEXT_TAB,       "tab-switch-right", nullptr, nullptr),
  ENTRY_MDI (N_("Move Tab to the Left"),   KEY_MOVE_TAB_LEFT,  "tab-move-left",    nullptr, nullptr),
  ENTRY_MDI (N_("Move Tab to the Right"),  KEY_MOVE_TAB_RIGHT, "tab-move-right",   nullptr, nullptr),
  ENTRY_MDI (N_("Move Tab to the Start"),  KEY_MOVE_TAB_START, "tab-move-start",   nullptr, nullptr),
  ENTRY_MDI (N_("Move Tab to the End"),    KEY_MOVE_TAB_END,   "tab-move-end",     nullptr, nullptr),
  ENTRY_MDI (N_("Detach Tab"),             KEY_DETACH_TAB,     "tab-detach",       nullptr, nullptr),
  ENTRY_MDI (N_("Show Tabs Overview"), KEY_SHOW_TABS_OVERVIEW, "tab-overview",     nullptr, nullptr),
  ENTRY_FULL (N_("Focus the Tab List"), KEY_FOCUS_TAB_LIST, "focus-tab-list", nullptr, nullptr, "win.shadow-tablist"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "1", "active-tab", "i", "0"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "2", "active-tab", "i", "1"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "3", "active-tab", "i", "2"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "4", "active-tab", "i", "3"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "5", "active-tab", "i", "4"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "6", "active-tab", "i", "5"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "7", "active-tab", "i", "6"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "8", "active-tab", "i", "7"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "9", "active-tab", "i", "8"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "10", "active-tab", "i", "9"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "11", "active-tab", "i", "10"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "12", "active-tab", "i", "11"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "13", "active-tab", "i", "12"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "14", "active-tab", "i", "13"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "15", "active-tab", "i", "14"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "16", "active-tab", "i", "15"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "17", "active-tab", "i", "16"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "18", "active-tab", "i", "17"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "19", "active-tab", "i", "18"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "20", "active-tab", "i", "19"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "21", "active-tab", "i", "20"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "22", "active-tab", "i", "21"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "23", "active-tab", "i", "22"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "24", "active-tab", "i", "23"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "25", "active-tab", "i", "24"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "26", "active-tab", "i", "25"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "27", "active-tab", "i", "26"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "28", "active-tab", "i", "27"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "29", "active-tab", "i", "28"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "30", "active-tab", "i", "29"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "31", "active-tab", "i", "30"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "32", "active-tab", "i", "31"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "33", "active-tab", "i", "32"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "34", "active-tab", "i", "33"),
  ENTRY_MDI (nullptr, KEY_SWITCH_TAB_PREFIX "35", "active-tab", "i", "34"),
  ENTRY_MDI (N_("Switch to Last Tab"), KEY_SWITCH_TAB_PREFIX "last", "active-tab", "i", "-1"),
};

static KeyEntry panes_entries[] = {
  ENTRY      (N_("Split Right"),             KEY_SPLIT_RIGHT,       "split",       "s", "'right'"),
  ENTRY      (N_("Split Down"),              KEY_SPLIT_DOWN,        "split",       "s", "'down'" ),
  ENTRY_PANE (N_("Close Pane"),              KEY_CLOSE_PANE,        "close-pane",  nullptr, nullptr),
  ENTRY_PANE (N_("Zoom Pane"),               KEY_ZOOM_PANE,         "zoom-pane",   nullptr, nullptr),
  ENTRY_PANE (N_("Make Panes the Same Size"), KEY_EQUALIZE_PANES,   "equalize-panes", nullptr, nullptr),
  ENTRY_PANE (N_("Move Pane to a New Tab"),  KEY_MOVE_PANE_TO_TAB,  "move-pane",   "s", "'tab'"  ),
  ENTRY_PANE (N_("Focus the Pane on the Left"),  KEY_FOCUS_PANE_LEFT,  "pane-focus",  "s", "'left'" ),
  ENTRY_PANE (N_("Focus the Pane on the Right"), KEY_FOCUS_PANE_RIGHT, "pane-focus",  "s", "'right'"),
  ENTRY_PANE (N_("Focus the Pane Above"),    KEY_FOCUS_PANE_UP,     "pane-focus",  "s", "'up'"   ),
  ENTRY_PANE (N_("Focus the Pane Below"),    KEY_FOCUS_PANE_DOWN,   "pane-focus",  "s", "'down'" ),
  ENTRY_PANE (N_("Move Divider Left"),       KEY_RESIZE_PANE_LEFT,  "pane-resize", "s", "'left'" ),
  ENTRY_PANE (N_("Move Divider Right"),      KEY_RESIZE_PANE_RIGHT, "pane-resize", "s", "'right'"),
  ENTRY_PANE (N_("Move Divider Up"),         KEY_RESIZE_PANE_UP,    "pane-resize", "s", "'up'"   ),
  ENTRY_PANE (N_("Move Divider Down"),       KEY_RESIZE_PANE_DOWN,  "pane-resize", "s", "'down'" ),
  ENTRY_PANE (N_("Swap With the Pane on the Left"),  KEY_SWAP_PANE_LEFT,  "swap-pane", "s", "'left'" ),
  ENTRY_PANE (N_("Swap With the Pane on the Right"), KEY_SWAP_PANE_RIGHT, "swap-pane", "s", "'right'"),
  ENTRY_PANE (N_("Swap With the Pane Above"),  KEY_SWAP_PANE_UP,     "swap-pane", "s", "'up'"   ),
  ENTRY_PANE (N_("Swap With the Pane Below"),  KEY_SWAP_PANE_DOWN,   "swap-pane", "s", "'down'" ),
  ENTRY      (N_("Broadcast Input to This Tab"),    KEY_BROADCAST_TAB,    "broadcast", "s", "'tab'"   ),
  ENTRY      (N_("Broadcast Input to This Window"), KEY_BROADCAST_WINDOW, "broadcast", "s", "'window'"),
};

static KeyEntry help_entries[] = {
  ENTRY (N_("Contents"), KEY_HELP, "help", nullptr, nullptr)
};

static KeyEntry global_entries[] = {
  ENTRY (N_("Show Primary Menu"), KEY_HEADER_MENU, "header-menu", nullptr, nullptr),
};

#undef ENTRY_FULL
#undef ENTRY
#undef ENTRY_MDI
#undef ENTRY_PANE
#undef ENTRY_PROMPT

static KeyEntryList all_entries[] =
{
  { file_entries,     G_N_ELEMENTS (file_entries),     N_("File"),     FALSE },
  { edit_entries,     G_N_ELEMENTS (edit_entries),     N_("Edit"),     FALSE },
  { view_entries,     G_N_ELEMENTS (view_entries),     N_("View"),     FALSE },
  { search_entries,   G_N_ELEMENTS (search_entries),   N_("Search"),   FALSE },
  { terminal_entries, G_N_ELEMENTS (terminal_entries), N_("Terminal"), FALSE },
  { tabs_entries,     G_N_ELEMENTS (tabs_entries),     N_("Tabs"),     FALSE },
  { panes_entries,    G_N_ELEMENTS (panes_entries),    N_("Panes"),    FALSE },
  { help_entries,     G_N_ELEMENTS (help_entries),     N_("Help"),     FALSE },
  { global_entries,   G_N_ELEMENTS (global_entries),   N_("Global"),   TRUE  },
};

enum
{
  ACTION_COLUMN,
  KEYVAL_COLUMN,
  N_COLUMNS
};

static GHashTable *settings_key_to_entry;
static GSettings *keybinding_settings = nullptr;
static GSettings *global_settings = nullptr;

GS_DEFINE_CLEANUP_FUNCTION(GtkTreePath*, _terminal_local_free_tree_path, gtk_tree_path_free)
#define terminal_free_tree_path __attribute__((__cleanup__(_terminal_local_free_tree_path)))

static void
key_changed_cb (GSettings *settings,
                const char *settings_key,
                gpointer user_data)
{
  GtkApplication *application = (GtkApplication*)user_data;
  const gchar *accels[3] = { nullptr, nullptr, nullptr };

  _terminal_debug_print (TERMINAL_DEBUG_ACCELS,
                         "key %s changed\n",
                         settings_key);

  KeyEntry *key_entry = reinterpret_cast<KeyEntry*>
    (g_hash_table_lookup (settings_key_to_entry, settings_key));
  if (!key_entry)
    {
      /* shouldn't really happen, but let's be safe */
      _terminal_debug_print (TERMINAL_DEBUG_ACCELS,
                             "  WARNING: KeyEntry for changed key not found, bailing out\n");
      return;
    }

  gs_free char *value = g_settings_get_string (settings, settings_key);

  gs_free char *detailed = g_action_print_detailed_name (key_entry->action_name,
                                                         key_entry->parameter);
  gs_unref_variant GVariant *shadow_parameter = g_variant_new_string (detailed);
  gs_free char *shadow_detailed = g_action_print_detailed_name (key_entry->shadow_action_name,
                                                                shadow_parameter);

  /* We want to always consume the action's accelerators, even if the corresponding
   * action is insensitive, so the corresponding shortcut key escape code isn't sent
   * to the terminal. See bug #453193, bug #138609, and bug #559728.
   * Since GtkApplication's accelerators don't use GtkAccelGroup, we have no way
   * to intercept/chain on its activation. The only way to do this that I found
   * was to install an extra action with the same accelerator that shadows
   * the real action and gets activated when the shadowed action is disabled.
   */

  auto n_accels = 0;
  if (!g_str_equal (value, "disabled"))
    accels[n_accels++] = value;

  /* F10 opens the main menu too, unless turned off in the preferences */
  if (g_str_equal (settings_key, KEY_HEADER_MENU) &&
      global_settings &&
      g_settings_get_boolean (global_settings, TERMINAL_SETTING_ENABLE_MENU_BAR_ACCEL_KEY))
    accels[n_accels++] = "F10";

  gtk_application_set_accels_for_action (application,
                                         detailed,
                                         accels);
  gtk_application_set_accels_for_action (application,
                                         shadow_detailed,
                                         accels);
}

static void
add_accel_entries (GApplication*application,
                   KeyEntry *entries,
                   guint n_entries)
{
  guint j;

  for (j = 0; j < n_entries; ++j) {
    KeyEntry *key_entry = &entries[j];
    if (key_entry->action_parameter) {
      GError *err = nullptr;
      key_entry->parameter = g_variant_parse (key_entry->action_parameter_type,
                                              key_entry->action_parameter,
                                              nullptr, nullptr, &err);
      terminal_assert_no_error (err);

      g_assert (key_entry->parameter != nullptr);
    }

    g_hash_table_insert (settings_key_to_entry,
                         (gpointer) key_entry->settings_key,
                         key_entry);

    key_changed_cb (keybinding_settings, key_entry->settings_key, application);
  }
}

static void
menu_accel_changed_cb (GSettings *settings,
                       const char *key,
                       gpointer user_data)
{
  key_changed_cb (keybinding_settings, KEY_HEADER_MENU, user_data);
}

void
terminal_accels_init (GApplication *application,
                      GSettings *settings,
                      GSettings *app_settings,
                      gboolean use_headerbar)
{
  guint i, j;

  keybinding_settings = (GSettings*)g_object_ref (settings);
  global_settings = (GSettings*)g_object_ref (app_settings);

  settings_key_to_entry = g_hash_table_new (g_str_hash, g_str_equal);

  /* Initialise names of tab_switch_entries */
  j = 1;
  for (i = 0; i < G_N_ELEMENTS (tabs_entries); i++)
    {
      gs_free char *name = nullptr;

      if (tabs_entries[i].user_visible_name != nullptr)
        continue;

      name = g_strdup_printf (_("Switch to Tab %u"), j++);
      tabs_entries[i].user_visible_name = g_intern_string (name);
    }

  for (i = 0; i < G_N_ELEMENTS (all_entries); ++i) {
    if (!use_headerbar && all_entries[i].headerbar_only)
      continue;

    add_accel_entries (application, all_entries[i].key_entry, all_entries[i].n_elements);
  }

  g_signal_connect (keybinding_settings, "changed", G_CALLBACK (key_changed_cb), application);
  g_signal_connect (global_settings,
                    "changed::" TERMINAL_SETTING_ENABLE_MENU_BAR_ACCEL_KEY,
                    G_CALLBACK (menu_accel_changed_cb), application);
}

/**
 * terminal_accels_foreach_action:
 * @func: called for each action
 * @user_data: data for @func
 *
 * Calls @func with the translated name, the action (such as "win.split"),
 * its target and its accelerator (or %NULL) of each action that can have a
 * keyboard shortcut, except switching to a tab by its number.
 */
void
terminal_accels_foreach_action (TerminalAccelsForeachFunc func,
                                void* user_data)
{
  for (auto i = 0u; i < G_N_ELEMENTS (all_entries); ++i) {
    for (auto j = 0u; j < all_entries[i].n_elements; ++j) {
      auto const entry = &all_entries[i].key_entry[j];
      if (!entry->user_visible_name ||
          (g_str_has_prefix (entry->settings_key, KEY_SWITCH_TAB_PREFIX) &&
           !g_str_equal (entry->settings_key, KEY_SWITCH_TAB_PREFIX "last")))
        continue;

      gs_free char *accel = keybinding_settings
        ? g_settings_get_string (keybinding_settings, entry->settings_key)
        : nullptr;
      func (_(entry->user_visible_name),
            entry->action_name,
            entry->parameter,
            accel && !g_str_equal (accel, "disabled") ? accel : nullptr,
            user_data);
    }
  }
}

void
terminal_accels_shutdown (void)
{
  guint i, j;

  for (i = 0; i < G_N_ELEMENTS (all_entries); ++i) {
    for (j = 0; j < all_entries[i].n_elements; ++j) {
      KeyEntry *key_entry;

      key_entry = &(all_entries[i].key_entry[j]);
      if (key_entry->parameter)
        g_variant_unref (key_entry->parameter);
    }
  }

  g_signal_handlers_disconnect_by_func (keybinding_settings,
                                        (void*)key_changed_cb,
                                        g_application_get_default ());

  g_signal_handlers_disconnect_by_func (global_settings,
                                        (void*)menu_accel_changed_cb,
                                        g_application_get_default ());

  g_clear_pointer (&settings_key_to_entry, (GDestroyNotify) g_hash_table_unref);
  g_clear_object (&keybinding_settings);
  g_clear_object (&global_settings);
}

GSettings *
terminal_accels_get_settings (void)
{
  g_return_val_if_fail (keybinding_settings != nullptr, nullptr);

  return keybinding_settings;
}

#ifdef TERMINAL_PREFERENCES
void
terminal_accels_populate_preferences (AdwPreferencesPage *page)
{
  g_return_if_fail (ADW_IS_PREFERENCES_PAGE (page));

  for (guint i = 0; i < G_N_ELEMENTS (all_entries); i++)
    {
      AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

      adw_preferences_group_set_title (group, all_entries[i].user_visible_name);

      for (guint j = 0; j < all_entries[i].n_elements; ++j)
        {
          const KeyEntry *key_entry = &(all_entries[i].key_entry[j]);
          TerminalAccelRow *row;

          row = (TerminalAccelRow *)g_object_new (TERMINAL_TYPE_ACCEL_ROW,
                                                  "key", key_entry->settings_key,
                                                  "title", key_entry->user_visible_name,
                                                  nullptr);
          adw_preferences_group_add (group, GTK_WIDGET (row));
        }

      adw_preferences_page_add (page, group);
    }
}
#endif

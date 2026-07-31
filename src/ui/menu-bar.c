#include "menu-bar.h"

#include "editor-panel.h"
#include "project-actions.h"

#include "core/markup-c.h"
#include "core/wordsmith-core-c.h"

struct MenuBar {
    GtkApplication* app; /* borrowed */
};

/* ── action handlers ─────────────────────────────────────────────────────── */

/* Menu items whose feature does not exist yet. Named actions keep the model
 * and the accelerators honest while the panels fill in. */
static void on_stub_action(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) param;
    (void) user;

    const char* name = g_action_get_name(G_ACTION(action));
    g_message("wordsmith: action '%s' is not implemented yet", name);
}

static void on_new_project(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    project_actions_new_dialog(user);
}

static void on_open_project(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    project_actions_open_dialog(user);
}

static void on_close_project(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    project_actions_close(user);
}

static void on_save(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    project_actions_save(user);
}

static void on_new_text(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    project_actions_new_text(user);
}

static void on_new_folder(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    project_actions_new_folder(user);
}

/* The binder's context menu carries its target folder, or the item to gather
 * up, in the action's parameter. */
static void on_new_text_in(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    project_actions_new_text_in(user, g_variant_get_string(param, NULL));
}

static void on_new_folder_in(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    project_actions_new_folder_in(user, g_variant_get_string(param, NULL));
}

static void on_new_folder_with_selection(GSimpleAction* action, GVariant* param,
                                         gpointer user)
{
    (void) action;
    project_actions_new_folder_with_selection(user,
                                              g_variant_get_string(param, NULL));
}

static void on_undo(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    editor_panel_undo(((WordsmithUiState*) user)->editor);
}

static void on_redo(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    editor_panel_redo(((WordsmithUiState*) user)->editor);
}

static void on_cut(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    editor_panel_cut(((WordsmithUiState*) user)->editor);
}

static void on_copy(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    editor_panel_copy(((WordsmithUiState*) user)->editor);
}

static void on_paste(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    editor_panel_paste(((WordsmithUiState*) user)->editor);
}

static void toggle_style(gpointer user, uint32_t span_flag)
{
    WordsmithUiState* state = user;
    editor_panel_toggle_style(state->editor, span_flag);
}

static void on_format_bold(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    toggle_style(user, WORDSMITH_MARKUP_SPAN_STRONG);
}

static void on_format_italic(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    toggle_style(user, WORDSMITH_MARKUP_SPAN_EMPHASIS);
}

static void on_format_underline(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;
    toggle_style(user, WORDSMITH_MARKUP_SPAN_UNDERLINE);
}

static void on_about(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;

    WordsmithUiState* state = user;
    const char* authors[] = { "David Luco", NULL };

    gtk_show_about_dialog(
        state != NULL ? state->window : NULL,
        "program-name", "Wordsmith",
        "version", wordsmith_version(),
        "comments", "A novel editor.",
        "authors", authors,
        NULL);
}

static void on_quit(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;

    WordsmithUiState* state = user;
    if (state != NULL && state->window != NULL) {
        gtk_window_close(state->window);
    }
}

/* ── action table ────────────────────────────────────────────────────────── */

typedef struct ActionSpec {
    const char* name;
    GCallback   callback;
    const char* accel; /* NULL when the action has no keyboard shortcut */
} ActionSpec;

static const ActionSpec ACTIONS[] = {
    /* File */
    { "new-project",      G_CALLBACK(on_new_project),      "<Control>n"        },
    { "open-project",     G_CALLBACK(on_open_project),     "<Control>o"        },
    { "save",             G_CALLBACK(on_save),             "<Control>s"        },
    { "save-as",          G_CALLBACK(on_stub_action),      "<Control><Shift>s" },
    { "compile",          G_CALLBACK(on_stub_action),      "<Control><Shift>e" },
    { "close-project",    G_CALLBACK(on_close_project),    "<Control>w"        },
    { "quit",             G_CALLBACK(on_quit),             "<Control>q"        },

    /* Edit */
    { "undo",             G_CALLBACK(on_undo),             "<Control>z"        },
    { "redo",             G_CALLBACK(on_redo),             "<Control><Shift>z" },
    { "cut",              G_CALLBACK(on_cut),              "<Control>x"        },
    { "copy",             G_CALLBACK(on_copy),             "<Control>c"        },
    { "paste",            G_CALLBACK(on_paste),            "<Control>v"        },
    { "find",             G_CALLBACK(on_stub_action),      "<Control>f"        },
    { "find-in-project",  G_CALLBACK(on_stub_action),      "<Control><Shift>f" },

    /* Insert */
    { "new-text",         G_CALLBACK(on_new_text),         "<Control>t"        },
    { "new-folder",       G_CALLBACK(on_new_folder),       NULL                },
    { "insert-comment",   G_CALLBACK(on_stub_action),      NULL                },
    { "insert-footnote",  G_CALLBACK(on_stub_action),      NULL                },

    /* View */
    { "view-document",    G_CALLBACK(on_stub_action),      NULL                },
    { "view-corkboard",   G_CALLBACK(on_stub_action),      NULL                },
    { "view-outliner",    G_CALLBACK(on_stub_action),      NULL                },
    { "composition-mode", G_CALLBACK(on_stub_action),      "F11"               },

    /* Project */
    { "project-targets",    G_CALLBACK(on_stub_action),    NULL                },
    { "project-statistics", G_CALLBACK(on_stub_action),    NULL                },
    { "project-settings",   G_CALLBACK(on_stub_action),    NULL                },

    /* Format */
    { "format-bold",      G_CALLBACK(on_format_bold),      "<Control>b"        },
    { "format-italic",    G_CALLBACK(on_format_italic),    "<Control>i"        },
    { "format-underline", G_CALLBACK(on_format_underline), "<Control>u"        },

    /* Help */
    { "about",            G_CALLBACK(on_about),            NULL                },
};

/* Actions taking a path. They have no menu-bar entry and no accelerator: the
 * binder's context menu raises them with the row it was opened over. */
static const ActionSpec TARGETED_ACTIONS[] = {
    { "new-text-in",                G_CALLBACK(on_new_text_in),                NULL },
    { "new-folder-in",              G_CALLBACK(on_new_folder_in),              NULL },
    { "new-folder-with-selection",  G_CALLBACK(on_new_folder_with_selection),  NULL },
};

/* Stateful toggles for the two side panes. The window reads these back when
 * it wires up pane visibility. */
static const char* const TOGGLE_ACTIONS[] = {
    "show-binder",
    "show-inspector",
};

static void install_actions(WordsmithUiState* state, GtkApplication* app)
{
    for (gsize index = 0; index < G_N_ELEMENTS(ACTIONS); index++) {
        const ActionSpec* spec = &ACTIONS[index];

        GSimpleAction* action = g_simple_action_new(spec->name, NULL);
        g_signal_connect(action, "activate", spec->callback, state);
        g_action_map_add_action(G_ACTION_MAP(state->window), G_ACTION(action));
        g_object_unref(action);

        if (spec->accel != NULL) {
            char* detailed = g_strconcat("win.", spec->name, NULL);
            const char* accels[] = { spec->accel, NULL };
            gtk_application_set_accels_for_action(app, detailed, accels);
            g_free(detailed);
        }
    }

    for (gsize index = 0; index < G_N_ELEMENTS(TARGETED_ACTIONS); index++) {
        const ActionSpec* spec = &TARGETED_ACTIONS[index];

        GSimpleAction* action = g_simple_action_new(spec->name, G_VARIANT_TYPE_STRING);
        g_signal_connect(action, "activate", spec->callback, state);
        g_action_map_add_action(G_ACTION_MAP(state->window), G_ACTION(action));
        g_object_unref(action);
    }

    for (gsize index = 0; index < G_N_ELEMENTS(TOGGLE_ACTIONS); index++) {
        GSimpleAction* action = g_simple_action_new_stateful(
            TOGGLE_ACTIONS[index], NULL, g_variant_new_boolean(TRUE));
        g_signal_connect(action, "activate", G_CALLBACK(on_stub_action), state);
        g_action_map_add_action(G_ACTION_MAP(state->window), G_ACTION(action));
        g_object_unref(action);
    }
}

/* ── menu model ──────────────────────────────────────────────────────────── */

static GMenuModel* build_menu_model(void)
{
    GMenu* menubar = g_menu_new();

    GMenu* file_menu = g_menu_new();
    GMenu* file_project_section = g_menu_new();
    g_menu_append(file_project_section, "New Project...", "win.new-project");
    g_menu_append(file_project_section, "Open Project...", "win.open-project");
    g_menu_append_section(file_menu, NULL, G_MENU_MODEL(file_project_section));
    g_object_unref(file_project_section);

    GMenu* file_save_section = g_menu_new();
    g_menu_append(file_save_section, "Save", "win.save");
    g_menu_append(file_save_section, "Save As...", "win.save-as");
    g_menu_append(file_save_section, "Compile...", "win.compile");
    g_menu_append_section(file_menu, NULL, G_MENU_MODEL(file_save_section));
    g_object_unref(file_save_section);

    GMenu* file_close_section = g_menu_new();
    g_menu_append(file_close_section, "Close Project", "win.close-project");
    g_menu_append(file_close_section, "Quit", "win.quit");
    g_menu_append_section(file_menu, NULL, G_MENU_MODEL(file_close_section));
    g_object_unref(file_close_section);

    g_menu_append_submenu(menubar, "_File", G_MENU_MODEL(file_menu));
    g_object_unref(file_menu);

    GMenu* edit_menu = g_menu_new();
    GMenu* undo_section = g_menu_new();
    g_menu_append(undo_section, "Undo", "win.undo");
    g_menu_append(undo_section, "Redo", "win.redo");
    g_menu_append_section(edit_menu, NULL, G_MENU_MODEL(undo_section));
    g_object_unref(undo_section);

    GMenu* clipboard_section = g_menu_new();
    g_menu_append(clipboard_section, "Cut", "win.cut");
    g_menu_append(clipboard_section, "Copy", "win.copy");
    g_menu_append(clipboard_section, "Paste", "win.paste");
    g_menu_append_section(edit_menu, NULL, G_MENU_MODEL(clipboard_section));
    g_object_unref(clipboard_section);

    GMenu* find_section = g_menu_new();
    g_menu_append(find_section, "Find...", "win.find");
    g_menu_append(find_section, "Find in Project...", "win.find-in-project");
    g_menu_append_section(edit_menu, NULL, G_MENU_MODEL(find_section));
    g_object_unref(find_section);

    g_menu_append_submenu(menubar, "_Edit", G_MENU_MODEL(edit_menu));
    g_object_unref(edit_menu);

    GMenu* insert_menu = g_menu_new();
    GMenu* insert_doc_section = g_menu_new();
    g_menu_append(insert_doc_section, "New Text", "win.new-text");
    g_menu_append(insert_doc_section, "New Folder", "win.new-folder");
    g_menu_append_section(insert_menu, NULL, G_MENU_MODEL(insert_doc_section));
    g_object_unref(insert_doc_section);

    GMenu* insert_note_section = g_menu_new();
    g_menu_append(insert_note_section, "Comment", "win.insert-comment");
    g_menu_append(insert_note_section, "Footnote", "win.insert-footnote");
    g_menu_append_section(insert_menu, NULL, G_MENU_MODEL(insert_note_section));
    g_object_unref(insert_note_section);

    g_menu_append_submenu(menubar, "_Insert", G_MENU_MODEL(insert_menu));
    g_object_unref(insert_menu);

    GMenu* view_menu = g_menu_new();
    GMenu* view_mode_section = g_menu_new();
    g_menu_append(view_mode_section, "Document", "win.view-document");
    g_menu_append(view_mode_section, "Corkboard", "win.view-corkboard");
    g_menu_append(view_mode_section, "Outliner", "win.view-outliner");
    g_menu_append_section(view_menu, NULL, G_MENU_MODEL(view_mode_section));
    g_object_unref(view_mode_section);

    GMenu* view_pane_section = g_menu_new();
    g_menu_append(view_pane_section, "Show Binder", "win.show-binder");
    g_menu_append(view_pane_section, "Show Inspector", "win.show-inspector");
    g_menu_append_section(view_menu, NULL, G_MENU_MODEL(view_pane_section));
    g_object_unref(view_pane_section);

    GMenu* view_mode_full_section = g_menu_new();
    g_menu_append(view_mode_full_section, "Composition Mode", "win.composition-mode");
    g_menu_append_section(view_menu, NULL, G_MENU_MODEL(view_mode_full_section));
    g_object_unref(view_mode_full_section);

    g_menu_append_submenu(menubar, "_View", G_MENU_MODEL(view_menu));
    g_object_unref(view_menu);

    GMenu* project_menu = g_menu_new();
    g_menu_append(project_menu, "Project Targets...", "win.project-targets");
    g_menu_append(project_menu, "Project Statistics...", "win.project-statistics");
    g_menu_append(project_menu, "Project Settings...", "win.project-settings");
    g_menu_append_submenu(menubar, "_Project", G_MENU_MODEL(project_menu));
    g_object_unref(project_menu);

    GMenu* format_menu = g_menu_new();
    g_menu_append(format_menu, "Bold", "win.format-bold");
    g_menu_append(format_menu, "Italic", "win.format-italic");
    g_menu_append(format_menu, "Underline", "win.format-underline");
    g_menu_append_submenu(menubar, "F_ormat", G_MENU_MODEL(format_menu));
    g_object_unref(format_menu);

    GMenu* help_menu = g_menu_new();
    g_menu_append(help_menu, "About Wordsmith", "win.about");
    g_menu_append_submenu(menubar, "_Help", G_MENU_MODEL(help_menu));
    g_object_unref(help_menu);

    return G_MENU_MODEL(menubar);
}

MenuBar* menu_bar_new(WordsmithUiState* state, GtkApplication* app)
{
    MenuBar* menu_bar = g_new0(MenuBar, 1);
    menu_bar->app = app;

    install_actions(state, app);

    GMenuModel* model = build_menu_model();
    gtk_application_set_menubar(app, model);
    g_object_unref(model);

    return menu_bar;
}

void menu_bar_free(MenuBar* menu_bar)
{
    if (menu_bar == NULL) {
        return;
    }
    g_free(menu_bar);
}

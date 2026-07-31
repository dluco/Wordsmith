#include "menu-bar.h"

#include "core/wordsworth-core-c.h"

struct MenuBar {
    GtkApplication* app; /* borrowed */
};

/* Every menu item routes here until its feature exists. Named actions keep
 * the accelerators and the menu model honest while the panels fill in. */
static void on_stub_action(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) param;
    (void) user;

    const char* name = g_action_get_name(G_ACTION(action));
    g_message("wordsworth: action '%s' is not implemented yet", name);
}

static void on_about(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;

    WordsworthUiState* state = user;
    const char* authors[] = { "David Luco", NULL };

    gtk_show_about_dialog(
        state != NULL ? state->window : NULL,
        "program-name", "Wordsworth",
        "version", wordsworth_version(),
        "comments", "A novel editor.",
        "authors", authors,
        NULL);
}

static void on_quit(GSimpleAction* action, GVariant* param, gpointer user)
{
    (void) action;
    (void) param;

    WordsworthUiState* state = user;
    if (state != NULL && state->window != NULL) {
        gtk_window_close(state->window);
    }
}

typedef struct ActionSpec {
    const char* name;
    GCallback   callback;
    const char* accel; /* NULL when the action has no keyboard shortcut */
} ActionSpec;

static const ActionSpec ACTIONS[] = {
    /* File */
    { "new-project",      G_CALLBACK(on_stub_action), "<Control>n"       },
    { "open-project",     G_CALLBACK(on_stub_action), "<Control>o"       },
    { "save",             G_CALLBACK(on_stub_action), "<Control>s"       },
    { "save-as",          G_CALLBACK(on_stub_action), "<Control><Shift>s" },
    { "compile",          G_CALLBACK(on_stub_action), "<Control><Shift>e" },
    { "close-project",    G_CALLBACK(on_stub_action), "<Control>w"       },
    { "quit",             G_CALLBACK(on_quit),        "<Control>q"       },

    /* Edit */
    { "undo",             G_CALLBACK(on_stub_action), "<Control>z"       },
    { "redo",             G_CALLBACK(on_stub_action), "<Control><Shift>z" },
    { "cut",              G_CALLBACK(on_stub_action), "<Control>x"       },
    { "copy",             G_CALLBACK(on_stub_action), "<Control>c"       },
    { "paste",            G_CALLBACK(on_stub_action), "<Control>v"       },
    { "find",             G_CALLBACK(on_stub_action), "<Control>f"       },
    { "find-in-project",  G_CALLBACK(on_stub_action), "<Control><Shift>f" },

    /* Insert */
    { "new-text",         G_CALLBACK(on_stub_action), "<Control>t"       },
    { "new-folder",       G_CALLBACK(on_stub_action), NULL               },
    { "insert-comment",   G_CALLBACK(on_stub_action), NULL               },
    { "insert-footnote",  G_CALLBACK(on_stub_action), NULL               },

    /* View */
    { "view-document",    G_CALLBACK(on_stub_action), NULL               },
    { "view-corkboard",   G_CALLBACK(on_stub_action), NULL               },
    { "view-outliner",    G_CALLBACK(on_stub_action), NULL               },
    { "composition-mode", G_CALLBACK(on_stub_action), "F11"              },

    /* Project */
    { "project-targets",  G_CALLBACK(on_stub_action), NULL               },
    { "project-statistics", G_CALLBACK(on_stub_action), NULL             },
    { "project-settings", G_CALLBACK(on_stub_action), NULL               },

    /* Format */
    { "format-bold",      G_CALLBACK(on_stub_action), "<Control>b"       },
    { "format-italic",    G_CALLBACK(on_stub_action), "<Control>i"       },
    { "format-underline", G_CALLBACK(on_stub_action), "<Control>u"       },

    /* Help */
    { "about",            G_CALLBACK(on_about),       NULL               },
};

/* Stateful toggles for the two side panes. The window reads these back when
 * it wires up pane visibility. */
static const char* const TOGGLE_ACTIONS[] = {
    "show-binder",
    "show-inspector",
};

static void install_actions(WordsworthUiState* state, GtkApplication* app)
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

    for (gsize index = 0; index < G_N_ELEMENTS(TOGGLE_ACTIONS); index++) {
        GSimpleAction* action = g_simple_action_new_stateful(
            TOGGLE_ACTIONS[index], NULL, g_variant_new_boolean(TRUE));
        g_signal_connect(action, "activate", G_CALLBACK(on_stub_action), state);
        g_action_map_add_action(G_ACTION_MAP(state->window), G_ACTION(action));
        g_object_unref(action);
    }
}

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
    g_menu_append(help_menu, "About Wordsworth", "win.about");
    g_menu_append_submenu(menubar, "_Help", G_MENU_MODEL(help_menu));
    g_object_unref(help_menu);

    return G_MENU_MODEL(menubar);
}

MenuBar* menu_bar_new(WordsworthUiState* state, GtkApplication* app)
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

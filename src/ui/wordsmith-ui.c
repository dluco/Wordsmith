#include "wordsmith-ui.h"

#include "main-window.h"
#include "text-scale.h"

#include <gtk/gtk.h>

#define WORDSMITH_APP_ID       "io.github.dluco.Wordsmith"
#define WORDSMITH_RESOURCE_DIR "/io/github/dluco/Wordsmith"

/* Defined in the generated wordsmith-resources.c (GResource bundle, built
 * with --manual-register). Registers the stylesheet and any bundled icons. */
void wordsmith_resources_register_resource(void);

static void load_display_assets(void)
{
    GdkDisplay* display = gdk_display_get_default();
    if (display == NULL) {
        return;
    }

    /* The bundle carries an icon theme's worth of directories, so the status
     * marks resolve by name wherever a GtkImage asks for one. */
    gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(display),
                                     WORDSMITH_RESOURCE_DIR "/icons");

    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(provider, WORDSMITH_RESOURCE_DIR "/style.css");
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void on_app_startup(GApplication* app, gpointer user_data)
{
    (void) app;
    (void) user_data;

    wordsmith_resources_register_resource();
    load_display_assets();

    /* After the stylesheet, so the saved text size overrides it rather than
     * being overridden. */
    text_scale_init();
}

static void on_app_activate(GApplication* app, gpointer user_data)
{
    (void) user_data;

    main_window_present(GTK_APPLICATION(app), NULL);
}

/* `wordsmith <project-directory>`. GApplication routes command-line paths
 * here rather than to activate, one window per path. */
static void on_app_open(GApplication* app, GFile** files, gint file_count,
                        const gchar* hint, gpointer user_data)
{
    (void) hint;
    (void) user_data;

    for (gint index = 0; index < file_count; index++) {
        char* path = g_file_get_path(files[index]);
        main_window_present(GTK_APPLICATION(app), path);
        g_free(path);
    }
}

void wordsmith_ui_init(void)
{
    /* GtkApplication calls gtk_init() during startup. */
}

int wordsmith_ui_main(int argc, char* argv[])
{
    GtkApplication* app = gtk_application_new(
        WORDSMITH_APP_ID, G_APPLICATION_HANDLES_OPEN);

    g_signal_connect(app, "startup", G_CALLBACK(on_app_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_app_open), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

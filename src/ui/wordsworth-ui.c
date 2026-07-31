#include "wordsworth-ui.h"

#include "main-window.h"

#include <gtk/gtk.h>

#define WORDSWORTH_APP_ID       "io.github.dluco.Wordsworth"
#define WORDSWORTH_RESOURCE_DIR "/io/github/dluco/Wordsworth"

/* Defined in the generated wordsworth-resources.c (GResource bundle, built
 * with --manual-register). Registers the stylesheet and any bundled icons. */
void wordsworth_resources_register_resource(void);

static void load_stylesheet(void)
{
    GdkDisplay* display = gdk_display_get_default();
    if (display == NULL) {
        return;
    }

    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(provider, WORDSWORTH_RESOURCE_DIR "/style.css");
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void on_app_startup(GApplication* app, gpointer user_data)
{
    (void) app;
    (void) user_data;

    wordsworth_resources_register_resource();
    load_stylesheet();
}

static void on_app_activate(GApplication* app, gpointer user_data)
{
    (void) user_data;

    main_window_present(GTK_APPLICATION(app), NULL);
}

/* `wordsworth <project-directory>`. GApplication routes command-line paths
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

void wordsworth_ui_init(void)
{
    /* GtkApplication calls gtk_init() during startup. */
}

int wordsworth_ui_main(int argc, char* argv[])
{
    GtkApplication* app = gtk_application_new(
        WORDSWORTH_APP_ID, G_APPLICATION_HANDLES_OPEN);

    g_signal_connect(app, "startup", G_CALLBACK(on_app_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_app_open), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

#include "inspector-panel.h"

#include "core/frontmatter-c.h"
#include "core/markup-c.h"   /* wordsmith_free_string */
#include "core/project-c.h"

#include <string.h>

/* Fields the inspector knows by name, in the order it shows them. Everything
 * else in the frontmatter still appears, under "Other", spelled as the file
 * spells it — a key we do not recognise is not a key we should hide. */
static const struct {
    const char* key;
    const char* label;
} SCHEMA_FIELDS[] = {
    { "title",    "Title" },
    { "synopsis", "Synopsis" },
    { "status",   "Status" },
    { "tags",     "Tags" },
    { "keywords", "Keywords" },
    { "created",  "Created" },
    { "modified", "Modified" },
};

#define SCHEMA_FIELD_COUNT (int) (sizeof(SCHEMA_FIELDS) / sizeof(SCHEMA_FIELDS[0]))

struct InspectorPanel {
    GtkWidget* root;         /* borrowed once parented into the window */
    GtkWidget* content;      /* the box rows are added to */
    GtkWidget* placeholder;  /* shown when there is nothing to show */
};

static gboolean is_schema_key(const char* key)
{
    for (int i = 0; i < SCHEMA_FIELD_COUNT; i++) {
        if (g_strcmp0(SCHEMA_FIELDS[i].key, key) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* ── rows ────────────────────────────────────────────────────────────────── */

static void add_row(InspectorPanel* inspector, const char* label, const char* value)
{
    GtkWidget* name = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_widget_add_css_class(name, "inspector-field-name");

    GtkWidget* text = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);
    gtk_widget_add_css_class(text, "inspector-field-value");

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(row), name);
    gtk_box_append(GTK_BOX(row), text);
    gtk_box_append(GTK_BOX(inspector->content), row);
}

static void add_heading(InspectorPanel* inspector, const char* text)
{
    GtkWidget* heading = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_widget_add_css_class(heading, "inspector-heading");
    gtk_box_append(GTK_BOX(inspector->content), heading);
}

/* A key's value as one displayable string: sequences joined, mappings named
 * rather than flattened into something that reads like a value. */
static char* value_text(const WordsmithFrontmatter* frontmatter, const char* key)
{
    switch (wordsmith_frontmatter_value_kind(frontmatter, key)) {
    case WORDSMITH_FRONTMATTER_SCALAR: {
        const char* scalar = wordsmith_frontmatter_string(frontmatter, key);
        return g_strdup(scalar != NULL ? scalar : "");
    }
    case WORDSMITH_FRONTMATTER_SEQUENCE: {
        const size_t count = wordsmith_frontmatter_sequence_count(frontmatter, key);
        if (count == 0) {
            return g_strdup("");
        }
        GString* joined = g_string_new(NULL);
        for (size_t i = 0; i < count; i++) {
            const char* item = wordsmith_frontmatter_sequence_at(frontmatter, key, i);
            if (i > 0) {
                g_string_append(joined, ", ");
            }
            g_string_append(joined, item != NULL ? item : "");
        }
        return g_string_free(joined, FALSE);
    }
    case WORDSMITH_FRONTMATTER_MAP:
        return g_strdup("(nested fields)");
    case WORDSMITH_FRONTMATTER_MISSING:
        break;
    }
    return NULL;
}

static void clear_content(InspectorPanel* inspector)
{
    GtkWidget* child = gtk_widget_get_first_child(inspector->content);
    while (child != NULL) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(inspector->content), child);
        child = next;
    }
}

static void show_placeholder(InspectorPanel* inspector, const char* text)
{
    gtk_label_set_text(GTK_LABEL(inspector->placeholder), text);
    gtk_widget_set_visible(inspector->placeholder, TRUE);
    gtk_widget_set_visible(inspector->content, FALSE);
}

/* ── panel ───────────────────────────────────────────────────────────────── */

InspectorPanel* inspector_panel_new(void)
{
    InspectorPanel* inspector = g_new0(InspectorPanel, 1);

    inspector->placeholder = gtk_label_new("Inspector");
    gtk_widget_add_css_class(inspector->placeholder, "pane-placeholder");
    gtk_widget_set_vexpand(inspector->placeholder, TRUE);
    gtk_widget_set_hexpand(inspector->placeholder, TRUE);

    inspector->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(inspector->content, "inspector-fields");
    gtk_widget_set_hexpand(inspector->content, TRUE);
    gtk_widget_set_visible(inspector->content, FALSE);

    GtkWidget* stack = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(stack), inspector->placeholder);
    gtk_box_append(GTK_BOX(stack), inspector->content);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), stack);

    inspector->root = scroller;
    return inspector;
}

void inspector_panel_free(InspectorPanel* inspector)
{
    if (inspector == NULL) {
        return;
    }
    g_free(inspector);
}

GtkWidget* inspector_panel_widget(InspectorPanel* inspector)
{
    return inspector != NULL ? inspector->root : NULL;
}

void inspector_panel_clear(InspectorPanel* inspector)
{
    if (inspector == NULL) {
        return;
    }
    clear_content(inspector);
    show_placeholder(inspector, "Inspector");
}

void inspector_panel_set_document(InspectorPanel* inspector, const char* path)
{
    if (inspector == NULL) {
        return;
    }
    if (path == NULL) {
        inspector_panel_clear(inspector);
        return;
    }

    char* error = NULL;
    char* text  = wordsmith_document_read(path, &error);
    if (text == NULL) {
        wordsmith_free_string(error);
        inspector_panel_clear(inspector);
        return;
    }

    WordsmithFrontmatter* frontmatter = wordsmith_frontmatter_parse(text);
    wordsmith_free_string(text);
    if (frontmatter == NULL) {
        inspector_panel_clear(inspector);
        return;
    }

    clear_content(inspector);

    const size_t key_count = wordsmith_frontmatter_key_count(frontmatter);
    if (key_count == 0) {
        /* An empty block is still a block, and saying so is more useful than
         * an inspector that looks broken. */
        show_placeholder(inspector,
                         wordsmith_frontmatter_present(frontmatter)
                             ? "No fields in this document's frontmatter"
                             : "This document has no frontmatter");
        wordsmith_frontmatter_free(frontmatter);
        return;
    }

    for (int i = 0; i < SCHEMA_FIELD_COUNT; i++) {
        char* value = value_text(frontmatter, SCHEMA_FIELDS[i].key);
        if (value != NULL) {
            add_row(inspector, SCHEMA_FIELDS[i].label, value);
            g_free(value);
        }
    }

    gboolean heading_shown = FALSE;
    for (size_t i = 0; i < key_count; i++) {
        const char* key = wordsmith_frontmatter_key_at(frontmatter, i);
        if (key == NULL || is_schema_key(key)) {
            continue;
        }
        if (!heading_shown) {
            add_heading(inspector, "Other");
            heading_shown = TRUE;
        }
        char* value = value_text(frontmatter, key);
        add_row(inspector, key, value != NULL ? value : "");
        g_free(value);
    }

    const size_t diagnostic_count = wordsmith_frontmatter_diagnostic_count(frontmatter);
    if (diagnostic_count > 0) {
        add_heading(inspector, "Frontmatter problems");
        for (size_t i = 0; i < diagnostic_count; i++) {
            size_t      line    = 0;
            const char* message = wordsmith_frontmatter_diagnostic_at(frontmatter, i, &line);
            char*       where   = g_strdup_printf("Line %zu", line);
            add_row(inspector, where, message != NULL ? message : "");
            g_free(where);
        }
    }

    wordsmith_frontmatter_free(frontmatter);

    gtk_widget_set_visible(inspector->placeholder, FALSE);
    gtk_widget_set_visible(inspector->content, TRUE);
}

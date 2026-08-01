#include "inspector-panel.h"

#include "core/frontmatter-c.h"
#include "core/markup-c.h"   /* wordsmith_free_string */
#include "core/project-c.h"

#include <string.h>

/* How a field is typed, which is also how it is written back. */
typedef enum FieldShape {
    FIELD_LINE,       /* one line */
    FIELD_PARAGRAPH,  /* several */
    FIELD_LIST,       /* typed comma-separated, written as a block sequence */
    FIELD_STATUS,     /* chosen from a list, with a mark for each */
} FieldShape;

/* The statuses offered, and the mark shown against each.
 *
 * The vocabulary is Scrivener's, which is as close to a convention as this
 * corner of the world has: a manuscript goes to do, in progress, drafted,
 * revised, final, done. The marks follow it round — a ring that fills as the
 * work does, amber while it is being written, blue once it is being revised,
 * and a struck circle at the end.
 *
 * The list is not a closed set. Frontmatter is a text file an author may have
 * written by hand or shared from another tool, so a status this build has never
 * heard of is kept, shown under its own mark, and offered back: the file is
 * where the truth lives, and a control that silently dropped a value it did not
 * recognise would be a control that loses work.
 *
 * A NULL `value` is the absence of a status, which on disk is the absence of
 * the key rather than a status spelled "No Status". */
static const struct {
    const char* value;
    const char* label;
    const char* icon;
} STATUSES[] = {
    { NULL,            "No Status",     "wordsmith-status-none" },
    { "To Do",         "To Do",         "wordsmith-status-todo" },
    { "In Progress",   "In Progress",   "wordsmith-status-in-progress" },
    { "First Draft",   "First Draft",   "wordsmith-status-first-draft" },
    { "Revised Draft", "Revised Draft", "wordsmith-status-revised-draft" },
    { "Final Draft",   "Final Draft",   "wordsmith-status-final-draft" },
    { "Done",          "Done",          "wordsmith-status-done" },
};

#define STATUS_COUNT (int) (sizeof(STATUSES) / sizeof(STATUSES[0]))

/* The mark for a status the list does not have. */
#define STATUS_OTHER_ICON "wordsmith-status-other"

/* The fields the inspector can write, in the order it shows them. These are
 * always on show, whether or not the file has them: the pane is where metadata
 * gets added, so an empty one still has to offer somewhere to type. */
static const struct {
    const char* key;
    const char* label;
    FieldShape  shape;
    const char* hint;
} EDITABLE_FIELDS[] = {
    { "title",    "Title",    FIELD_LINE,      "Untitled" },
    { "synopsis", "Synopsis", FIELD_PARAGRAPH, NULL },
    { "status",   "Status",   FIELD_STATUS,    NULL },
    { "tags",     "Tags",     FIELD_LIST,      "Comma-separated" },
};

#define EDITABLE_FIELD_COUNT (int) (sizeof(EDITABLE_FIELDS) / sizeof(EDITABLE_FIELDS[0]))

/* Known fields the pane shows but does not write. The timestamps are the
 * project's to maintain, not the author's to retype. */
static const struct {
    const char* key;
    const char* label;
} READ_ONLY_FIELDS[] = {
    { "keywords", "Keywords" },
    { "created",  "Created" },
    { "modified", "Modified" },
};

#define READ_ONLY_FIELD_COUNT (int) (sizeof(READ_ONLY_FIELDS) / sizeof(READ_ONLY_FIELDS[0]))

struct InspectorPanel {
    GtkWidget* root;         /* borrowed once parented into the window */
    GtkWidget* content;      /* the box rows are added to */
    GtkWidget* placeholder;  /* shown when there is nothing to show */

    char*    path;       /* owned; NULL when the pane is empty */
    gboolean is_folder;

    /* Filling the fields in sets their text, and that must not read back as
     * the author having typed something. */
    gboolean loading;
    guint    reload_source;

    InspectorCommitFn commit_callback;
    void*             commit_user_data;
};

/* Structure, not description: the binder already draws the child order, and
 * repeating it here as a list of filenames would be noise. */
#define CHILD_ORDER_KEY "children"

/* What a field's widget needs to know to write itself back. */
typedef struct FieldEditor {
    InspectorPanel* inspector;
    const char*     key;      /* static, from EDITABLE_FIELDS */
    FieldShape      shape;
    GtkWidget*      widget;   /* GtkEntry, or GtkTextView for a paragraph */
    char*           loaded;   /* what the file said, to tell an edit from a visit */
} FieldEditor;

static void render_path(InspectorPanel* inspector);

static gboolean is_known_key(const char* key)
{
    for (int i = 0; i < EDITABLE_FIELD_COUNT; i++) {
        if (g_strcmp0(EDITABLE_FIELDS[i].key, key) == 0) {
            return TRUE;
        }
    }
    for (int i = 0; i < READ_ONLY_FIELD_COUNT; i++) {
        if (g_strcmp0(READ_ONLY_FIELDS[i].key, key) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* ── reading values ──────────────────────────────────────────────────────── */

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

/* A nested mapping is not something one line of text can hold, so a field
 * carrying one is shown rather than offered for editing — typing over it would
 * replace the whole of it, and not visibly. */
static gboolean is_typeable(const WordsmithFrontmatter* frontmatter, const char* key)
{
    return wordsmith_frontmatter_value_kind(frontmatter, key)
           != WORDSMITH_FRONTMATTER_MAP;
}

/* ── the status control ──────────────────────────────────────────────────── */

/* Which of STATUSES `value` is, or -1 for one the list does not have. Matching
 * ignores case, so a hand-written `done` lands on the same row as `Done`
 * without the file being rewritten to say so. */
static int status_index_of(const char* value)
{
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    for (int i = 0; i < STATUS_COUNT; i++) {
        /* Row 0 has no value of its own, so it answers to its label: a file
         * that does spell out "No Status" means the same thing by it. */
        const char* known =
            STATUSES[i].value != NULL ? STATUSES[i].value : STATUSES[i].label;
        if (g_ascii_strcasecmp(known, value) == 0) {
            return i;
        }
    }
    return -1;
}

static const char* status_icon_for_label(const char* label)
{
    for (int i = 0; i < STATUS_COUNT; i++) {
        if (g_strcmp0(STATUSES[i].label, label) == 0) {
            return STATUSES[i].icon;
        }
    }
    return STATUS_OTHER_ICON;
}

static void on_status_setup(GtkSignalListItemFactory* factory, GtkListItem* item,
                            gpointer user_data)
{
    (void) factory;
    (void) user_data;

    GtkWidget* row   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_append(GTK_BOX(row), gtk_image_new());
    gtk_box_append(GTK_BOX(row), label);
    gtk_list_item_set_child(item, row);
}

static void on_status_bind(GtkSignalListItemFactory* factory, GtkListItem* item,
                           gpointer user_data)
{
    (void) factory;
    (void) user_data;

    GtkStringObject* entry = gtk_list_item_get_item(item);
    const char*      label = gtk_string_object_get_string(entry);

    GtkWidget* row  = gtk_list_item_get_child(item);
    GtkWidget* icon = gtk_widget_get_first_child(row);
    GtkWidget* text = gtk_widget_get_next_sibling(icon);

    gtk_image_set_from_icon_name(GTK_IMAGE(icon), status_icon_for_label(label));
    gtk_label_set_text(GTK_LABEL(text), label);
}

/* The rows offered are the standard list, plus whatever the file says when that
 * is something else — kept at the end under its own mark rather than dropped. */
static GtkWidget* status_dropdown_new(const char* value)
{
    GtkStringList* labels = gtk_string_list_new(NULL);
    for (int i = 0; i < STATUS_COUNT; i++) {
        gtk_string_list_append(labels, STATUSES[i].label);
    }

    int selected = status_index_of(value);
    if (selected < 0) {
        gtk_string_list_append(labels, value);
        selected = STATUS_COUNT;
    }

    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_status_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_status_bind), NULL);

    GtkWidget* dropdown = gtk_drop_down_new(G_LIST_MODEL(labels), NULL);
    gtk_drop_down_set_factory(GTK_DROP_DOWN(dropdown), factory);
    g_object_unref(factory);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), (guint) selected);
    return dropdown;
}

/* ── writing values ──────────────────────────────────────────────────────── */

static char* field_text(FieldEditor* field)
{
    if (field->shape == FIELD_STATUS) {
        GtkStringObject* chosen =
            gtk_drop_down_get_selected_item(GTK_DROP_DOWN(field->widget));
        if (chosen == NULL) {
            return g_strdup("");
        }
        const char* label = gtk_string_object_get_string(chosen);
        const int   index = status_index_of(label);
        if (index < 0) {
            /* The row that came from the file writes back what the file said. */
            return g_strdup(label);
        }
        return g_strdup(STATUSES[index].value != NULL ? STATUSES[index].value : "");
    }

    if (field->shape == FIELD_PARAGRAPH) {
        GtkTextBuffer* buffer =
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(field->widget));
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        char* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        return text != NULL ? text : g_strdup("");
    }

    const char* text = gtk_editable_get_text(GTK_EDITABLE(field->widget));
    return g_strdup(text != NULL ? text : "");
}

/* Split what was typed into a comma-separated field. Blanks fall out, so
 * "one,, two ," is two tags and "," is none at all. NULL-terminated. */
static GStrv split_list(const char* text)
{
    GPtrArray* items = g_ptr_array_new();
    GStrv      parts = g_strsplit(text, ",", -1);

    for (GStrv part = parts; *part != NULL; part++) {
        char* item = g_strstrip(g_strdup(*part));
        if (item[0] != '\0') {
            g_ptr_array_add(items, item);
        } else {
            g_free(item);
        }
    }
    g_strfreev(parts);

    g_ptr_array_add(items, NULL);
    return (GStrv) g_ptr_array_free(items, FALSE);
}

/* Report what the author typed, if it differs from what the file said. */
static void field_commit(FieldEditor* field)
{
    InspectorPanel* inspector = field->inspector;
    if (inspector->loading || inspector->path == NULL
        || inspector->commit_callback == NULL) {
        return;
    }

    char* text = g_strstrip(field_text(field));
    if (g_strcmp0(text, field->loaded) == 0) {
        g_free(text);
        return;
    }

    /* Remembered before the write rather than after it: if the write fails the
     * pane reloads from disk anyway, and this keeps a field that failed from
     * asking again on every visit. */
    g_free(field->loaded);
    field->loaded = g_strdup(text);

    InspectorEdit edit = {
        .path       = inspector->path,
        .is_folder  = inspector->is_folder,
        .key        = field->key,
        .value      = text[0] != '\0' ? text : NULL,
        .items      = NULL,
        .item_count = 0,
    };

    GStrv items = NULL;
    if (field->shape == FIELD_LIST && edit.value != NULL) {
        items = split_list(text);
        edit.value      = NULL;
        edit.items      = items[0] != NULL ? (const char* const*) items : NULL;
        edit.item_count = g_strv_length(items);
    }

    inspector->commit_callback(&edit, inspector->commit_user_data);

    g_strfreev(items);
    g_free(text);
}

static void on_field_activate(GtkEntry* entry, gpointer user_data)
{
    (void) entry;
    field_commit(user_data);
}

/* Leaving the field is the commit for everything that has no Enter to press,
 * and a second, harmless one for everything that does. */
static void on_field_focus_leave(GtkEventControllerFocus* controller,
                                 gpointer user_data)
{
    (void) controller;
    field_commit(user_data);
}

/* Choosing is the whole gesture for a status: there is nothing further to
 * confirm, so the choice is the commit. */
static void on_status_selected(GObject* dropdown, GParamSpec* spec,
                               gpointer user_data)
{
    (void) dropdown;
    (void) spec;
    field_commit(user_data);
}

static void field_editor_free(gpointer data)
{
    FieldEditor* field = data;
    g_free(field->loaded);
    g_free(field);
}

/* ── rows ────────────────────────────────────────────────────────────────── */

static GtkWidget* field_name_label(const char* label)
{
    GtkWidget* name = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_widget_add_css_class(name, "inspector-field-name");
    return name;
}

static void add_row(InspectorPanel* inspector, const char* label, const char* value)
{
    GtkWidget* text = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);
    gtk_widget_add_css_class(text, "inspector-field-value");

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(row), field_name_label(label));
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

/* A field the author can type in. `value` is what the file holds, "" when it
 * holds nothing — an absent field and an empty one look the same here, which is
 * the point: both are somewhere to start writing. */
static void add_field_row(InspectorPanel* inspector, int index, const char* value)
{
    FieldEditor* field = g_new0(FieldEditor, 1);
    field->inspector = inspector;
    field->key       = EDITABLE_FIELDS[index].key;
    field->shape     = EDITABLE_FIELDS[index].shape;
    field->loaded    = g_strdup(value);

    if (field->shape == FIELD_STATUS) {
        field->widget = status_dropdown_new(value);
        gtk_widget_add_css_class(field->widget, "inspector-field-status");
        /* Connected after the row is picked, so setting it up is not a choice. */
        g_signal_connect(field->widget, "notify::selected",
                         G_CALLBACK(on_status_selected), field);
    } else if (field->shape == FIELD_PARAGRAPH) {
        GtkWidget* view = gtk_text_view_new();
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 4);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 4);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 4);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 4);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                                 value, -1);
        /* Tall enough to read as somewhere a few sentences go. */
        gtk_widget_set_size_request(view, -1, 72);
        gtk_widget_add_css_class(view, "inspector-field-paragraph");
        field->widget = view;
    } else {
        GtkWidget* entry = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(entry), value);
        if (EDITABLE_FIELDS[index].hint != NULL) {
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                           EDITABLE_FIELDS[index].hint);
        }
        gtk_widget_add_css_class(entry, "inspector-field-entry");
        g_signal_connect(entry, "activate", G_CALLBACK(on_field_activate), field);
        field->widget = entry;
    }

    if (field->shape != FIELD_STATUS) {
        GtkEventController* focus = gtk_event_controller_focus_new();
        g_signal_connect(focus, "leave", G_CALLBACK(on_field_focus_leave), field);
        gtk_widget_add_controller(field->widget, focus);
    }

    g_object_set_data_full(G_OBJECT(field->widget), "inspector-field", field,
                           field_editor_free);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(row), field_name_label(EDITABLE_FIELDS[index].label));
    gtk_box_append(GTK_BOX(row), field->widget);
    gtk_box_append(GTK_BOX(inspector->content), row);
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
    if (inspector->reload_source != 0) {
        g_source_remove(inspector->reload_source);
    }
    g_free(inspector->path);
    g_free(inspector);
}

GtkWidget* inspector_panel_widget(InspectorPanel* inspector)
{
    return inspector != NULL ? inspector->root : NULL;
}

void inspector_panel_set_commit_callback(InspectorPanel* inspector,
                                         InspectorCommitFn callback,
                                         void* user_data)
{
    if (inspector == NULL) {
        return;
    }
    inspector->commit_callback  = callback;
    inspector->commit_user_data = user_data;
}

void inspector_panel_clear(InspectorPanel* inspector)
{
    if (inspector == NULL) {
        return;
    }
    inspector->loading = TRUE;
    clear_content(inspector);
    inspector->loading = FALSE;

    g_clear_pointer(&inspector->path, g_free);
    inspector->is_folder = FALSE;
    show_placeholder(inspector, "Inspector");
}

/* Fill the pane from parsed metadata, whether it came from a document's
 * frontmatter or a folder's sidecar. Takes ownership of `frontmatter`. */
static void render_metadata(InspectorPanel* inspector,
                            WordsmithFrontmatter* frontmatter,
                            const char* problems_heading)
{
    if (frontmatter == NULL) {
        inspector_panel_clear(inspector);
        return;
    }

    /* Nothing built here counts as the author typing, and the fields are set
     * from the file as they are built. */
    inspector->loading = TRUE;
    clear_content(inspector);

    for (int i = 0; i < EDITABLE_FIELD_COUNT; i++) {
        const char* key   = EDITABLE_FIELDS[i].key;
        char*       value = value_text(frontmatter, key);
        if (is_typeable(frontmatter, key)) {
            add_field_row(inspector, i, value != NULL ? value : "");
        } else {
            add_row(inspector, EDITABLE_FIELDS[i].label, value != NULL ? value : "");
        }
        g_free(value);
    }

    for (int i = 0; i < READ_ONLY_FIELD_COUNT; i++) {
        char* value = value_text(frontmatter, READ_ONLY_FIELDS[i].key);
        if (value != NULL) {
            add_row(inspector, READ_ONLY_FIELDS[i].label, value);
            g_free(value);
        }
    }

    const size_t key_count = wordsmith_frontmatter_key_count(frontmatter);
    gboolean     heading_shown = FALSE;
    for (size_t i = 0; i < key_count; i++) {
        const char* key = wordsmith_frontmatter_key_at(frontmatter, i);
        if (key == NULL || is_known_key(key)
            || g_strcmp0(key, CHILD_ORDER_KEY) == 0) {
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
        add_heading(inspector, problems_heading);
        for (size_t i = 0; i < diagnostic_count; i++) {
            size_t      line    = 0;
            const char* message = wordsmith_frontmatter_diagnostic_at(frontmatter, i, &line);
            char*       where   = g_strdup_printf("Line %zu", line);
            add_row(inspector, where, message != NULL ? message : "");
            g_free(where);
        }
    }

    wordsmith_frontmatter_free(frontmatter);
    inspector->loading = FALSE;

    gtk_widget_set_visible(inspector->placeholder, FALSE);
    gtk_widget_set_visible(inspector->content, TRUE);
}

/* A document keeps its fields in its own frontmatter. */
static void render_document(InspectorPanel* inspector)
{
    char* error = NULL;
    char* text  = wordsmith_document_read(inspector->path, &error);
    if (text == NULL) {
        wordsmith_free_string(error);
        inspector_panel_clear(inspector);
        return;
    }

    WordsmithFrontmatter* frontmatter = wordsmith_frontmatter_parse(text);
    wordsmith_free_string(text);

    render_metadata(inspector, frontmatter, "Frontmatter problems");
}

/* A folder has no file of its own to carry frontmatter, so its fields live in a
 * sidecar beside its contents. Most folders will not have one yet, and that
 * reads the same as an empty one: writing any field creates it. */
static void render_folder(InspectorPanel* inspector)
{
    char* sidecar = wordsmith_folder_metadata_path(inspector->path);
    if (sidecar == NULL) {
        inspector_panel_clear(inspector);
        return;
    }

    char* error = NULL;
    char* text  = wordsmith_document_read(sidecar, &error);
    wordsmith_free_string(sidecar);
    wordsmith_free_string(error);

    render_metadata(inspector,
                    wordsmith_frontmatter_parse_yaml(text != NULL ? text : ""),
                    "Metadata problems");
    wordsmith_free_string(text);
}

static void render_path(InspectorPanel* inspector)
{
    if (inspector->path == NULL) {
        inspector_panel_clear(inspector);
        return;
    }
    if (inspector->is_folder) {
        render_folder(inspector);
    } else {
        render_document(inspector);
    }
}

static void set_target(InspectorPanel* inspector, const char* path, gboolean is_folder)
{
    char* copy = g_strdup(path);
    g_free(inspector->path);
    inspector->path      = copy;
    inspector->is_folder = is_folder;
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
    set_target(inspector, path, FALSE);
    render_path(inspector);
}

void inspector_panel_set_folder(InspectorPanel* inspector, const char* path)
{
    if (inspector == NULL) {
        return;
    }
    if (path == NULL) {
        inspector_panel_clear(inspector);
        return;
    }
    set_target(inspector, path, TRUE);
    render_path(inspector);
}

/* Rebuilding the fields from inside one of their own handlers would take the
 * widget out from under the controller that is still emitting, so the refresh
 * waits for the main loop to come back round. */
static gboolean on_reload_idle(gpointer user_data)
{
    InspectorPanel* inspector = user_data;
    inspector->reload_source = 0;
    render_path(inspector);
    return G_SOURCE_REMOVE;
}

void inspector_panel_reload(InspectorPanel* inspector)
{
    if (inspector == NULL || inspector->path == NULL
        || inspector->reload_source != 0) {
        return;
    }
    inspector->reload_source = g_idle_add(on_reload_idle, inspector);
}

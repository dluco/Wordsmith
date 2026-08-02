#include "binder-panel.h"

#include <string.h>

/* ── model item ──────────────────────────────────────────────────────────── */

/* One row in the binder. Holds a borrowed core node handle, which stays valid
 * only until the project is reloaded; the panel rebuilds the whole model on
 * reload, so no item outlives its handle. */
#define BINDER_TYPE_ITEM (binder_item_get_type())
G_DECLARE_FINAL_TYPE(BinderItem, binder_item, BINDER, ITEM, GObject)

struct _BinderItem {
    GObject parent_instance;

    char*    name;
    char*    path;
    gboolean is_folder;

    const WordsmithBinderNode* node;
};

G_DEFINE_FINAL_TYPE(BinderItem, binder_item, G_TYPE_OBJECT)

static void binder_item_finalize(GObject* object)
{
    BinderItem* item = BINDER_ITEM(object);
    g_free(item->name);
    g_free(item->path);
    G_OBJECT_CLASS(binder_item_parent_class)->finalize(object);
}

static void binder_item_class_init(BinderItemClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = binder_item_finalize;
}

static void binder_item_init(BinderItem* item)
{
    (void) item;
}

static BinderItem* binder_item_new(const WordsmithBinderNode* node)
{
    BinderItem* item = g_object_new(BINDER_TYPE_ITEM, NULL);
    item->node      = node;
    item->name      = g_strdup(wordsmith_binder_node_name(node));
    item->path      = g_strdup(wordsmith_binder_node_path(node));
    item->is_folder = wordsmith_binder_node_is_folder(node) != 0;
    return item;
}

/* ── panel ───────────────────────────────────────────────────────────────── */

/* Set on each row widget in `setup`, so a right-click or a drop can find which
 * row it landed on by walking up from the picked widget. The GtkListItem
 * outlives the items bound into it, which is what makes it safe to stash. */
#define ROW_LIST_ITEM_KEY "binder-list-item"

/* Drag feedback: a line along the edge the item would land on, or a filled row
 * when it would go inside a folder. */
#define DROP_BEFORE_CLASS "binder-drop-above"
#define DROP_INTO_CLASS   "binder-drop-into"
#define DROP_AFTER_CLASS  "binder-drop-below"

/* A row's name is a GtkStack of the two ways to show it: a label, and an entry
 * while it is being renamed.
 *
 * GtkEditableLabel is this widget already, and is not used for two reasons. It
 * offers no way to ellipsize the label it shows, so a long chapter name would
 * make the whole binder scroll sideways; and it starts editing on a double
 * click, which is not the gesture wanted here — a slow second click on a row
 * that is already selected is. Both are in the way of behaviour this pane has
 * to get right, and neither is reachable from outside the widget. */
#define NAME_PAGE_DISPLAY "display"
#define NAME_PAGE_EDIT    "edit"

struct BinderPanel {
    GtkWidget*        root;   /* borrowed once parented into the window */
    GtkListView*      list_view;
    GtkTreeListModel* tree_model;
    GtkSingleSelection* selection;
    GtkWidget*        context_menu;  /* GtkPopoverMenu, parented to the list view */

    WordsmithProject* project;   /* borrowed; NULL when no project is open */

    BinderSelectFn select_callback;
    void*          select_user_data;

    BinderMoveFn   move_callback;
    void*          move_user_data;

    BinderExpandFn expand_callback;
    void*          expand_user_data;

    BinderRenameFn rename_callback;
    void*          rename_user_data;

    /* The edit in progress, or all NULL. The entry is borrowed and is what says
     * *which* row is being renamed: rows are recycled as the list scrolls, so a
     * path alone could not tell whether the widget still belongs to the item the
     * author started editing. The two strings are what the edit is about, kept
     * because the row they came from may be gone by the time it is accepted. */
    GtkWidget*     renaming_entry;
    char*          renaming_path;
    char*          renaming_name;

    /* Set while the panel is opening folders itself, so putting an expansion
     * back does not read as the author having asked for it. */
    gboolean       applying_expansion;
};

/* ── rows ────────────────────────────────────────────────────────────────── */

/* The BinderItem shown by a row widget — the expander built in `setup` — or
 * NULL if it is between bindings. Owned by the caller. */
static BinderItem* row_item(GtkWidget* row)
{
    GtkListItem* list_item = g_object_get_data(G_OBJECT(row), ROW_LIST_ITEM_KEY);
    if (list_item == NULL) {
        return NULL;
    }
    GtkTreeListRow* tree_row = gtk_list_item_get_item(list_item);
    return tree_row != NULL ? gtk_tree_list_row_get_item(tree_row) : NULL;
}

/* The row widget `inner` sits inside, or NULL if it is not in one. Borrowed. */
static GtkWidget* row_widget_from(GtkWidget* inner)
{
    while (inner != NULL) {
        if (g_object_get_data(G_OBJECT(inner), ROW_LIST_ITEM_KEY) != NULL) {
            return inner;
        }
        inner = gtk_widget_get_parent(inner);
    }
    return NULL;
}

/* The row widget under (x, y) in list-view coordinates, or NULL over the empty
 * space below the tree. Borrowed. */
static GtkWidget* row_widget_at(BinderPanel* binder, double x, double y)
{
    GtkWidget* picked = gtk_widget_pick(GTK_WIDGET(binder->list_view), x, y,
                                        GTK_PICK_DEFAULT);
    return row_widget_from(picked);
}

/* The three parts of a row's name, from the expander built in `setup`. */

static GtkWidget* row_name_stack(GtkWidget* row)
{
    GtkWidget* box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(row));
    return box != NULL ? gtk_widget_get_last_child(box) : NULL;
}

static GtkWidget* row_name_label(GtkWidget* row)
{
    GtkWidget* stack = row_name_stack(row);
    return stack != NULL
               ? gtk_stack_get_child_by_name(GTK_STACK(stack), NAME_PAGE_DISPLAY)
               : NULL;
}

static GtkWidget* row_name_entry(GtkWidget* row)
{
    GtkWidget* stack = row_name_stack(row);
    return stack != NULL
               ? gtk_stack_get_child_by_name(GTK_STACK(stack), NAME_PAGE_EDIT)
               : NULL;
}

/* The BinderItem under (x, y) in list-view coordinates. Owned by the caller. */
static BinderItem* row_item_at(BinderPanel* binder, double x, double y)
{
    GtkWidget* row = row_widget_at(binder, x, y);
    return row != NULL ? row_item(row) : NULL;
}

/* ── drag and drop ───────────────────────────────────────────────────────── */

/* The dragged row travels as a BinderItem rather than as its path in a string.
 * Drags are within the one process, so the object goes across intact — and a
 * private type means no stray text drag from another window can look like a
 * binder row, and nothing outside can be handed one of ours. */

/* Whether `candidate` is `ancestor` or sits beneath it. Both paths come from
 * the core, spelled the same way, so a prefix test is enough here; the core
 * checks properly again before it moves anything. */
static gboolean path_is_within(const char* ancestor, const char* candidate)
{
    if (g_strcmp0(ancestor, candidate) == 0) {
        return TRUE;
    }
    char* prefix = g_strconcat(ancestor, G_DIR_SEPARATOR_S, NULL);
    const gboolean within = g_str_has_prefix(candidate, prefix);
    g_free(prefix);
    return within;
}

static BinderDropZone zone_at(GtkWidget* row, double y, gboolean is_folder)
{
    const double height = gtk_widget_get_height(row);
    if (height <= 0.0) {
        return BINDER_DROP_BEFORE;
    }
    /* A document has no inside to offer, so it is split down the middle. A
     * folder keeps its middle half for dropping things in, leaving a quarter at
     * each end to slot alongside it. */
    if (!is_folder) {
        return y < height / 2.0 ? BINDER_DROP_BEFORE : BINDER_DROP_AFTER;
    }
    if (y < height * 0.25) {
        return BINDER_DROP_BEFORE;
    }
    if (y > height * 0.75) {
        return BINDER_DROP_AFTER;
    }
    return BINDER_DROP_INTO;
}

static void clear_drop_feedback(GtkWidget* widget)
{
    gtk_widget_remove_css_class(widget, DROP_BEFORE_CLASS);
    gtk_widget_remove_css_class(widget, DROP_INTO_CLASS);
    gtk_widget_remove_css_class(widget, DROP_AFTER_CLASS);
}

static void show_drop_feedback(GtkWidget* widget, BinderDropZone zone)
{
    clear_drop_feedback(widget);
    gtk_widget_add_css_class(widget,
                             zone == BINDER_DROP_BEFORE ? DROP_BEFORE_CLASS
                                 : zone == BINDER_DROP_AFTER ? DROP_AFTER_CLASS
                                                             : DROP_INTO_CLASS);
}

/* The BinderItem being dragged, or NULL if the drag is carrying something else
 * or has not offered its data yet. Borrowed. */
static BinderItem* dragged_item(const GValue* value)
{
    if (value == NULL || !G_VALUE_HOLDS(value, BINDER_TYPE_ITEM)) {
        return NULL;
    }
    return g_value_get_object(value);
}

/* Refused drops are the ones the core would reject anyway, caught here so the
 * pointer says no rather than the drop raising an error dialog. */
static gboolean drop_is_allowed(BinderItem* dragged, BinderItem* onto)
{
    if (dragged == NULL || onto == NULL) {
        return FALSE;
    }
    if (g_strcmp0(dragged->path, onto->path) == 0) {
        return FALSE;   /* onto itself */
    }
    /* A folder cannot be dropped anywhere inside its own subtree, whether that
     * means into a descendant folder or beside a document within it. */
    return !(dragged->is_folder && path_is_within(dragged->path, onto->path));
}

/* Performing the move rebuilds the whole binder, which destroys the very row
 * whose drop handler asked for it. So the request is queued and run once the
 * drop has finished and the widget tree is nobody's business but ours. */
typedef struct PendingMove {
    BinderPanel*   binder;
    char*          source;
    char*          target;
    BinderDropZone zone;
} PendingMove;

static gboolean run_pending_move(gpointer user_data)
{
    PendingMove* move = user_data;

    if (move->binder->move_callback != NULL) {
        move->binder->move_callback(move->source, move->target, move->zone,
                                    move->binder->move_user_data);
    }

    g_free(move->source);
    g_free(move->target);
    g_free(move);
    return G_SOURCE_REMOVE;
}

static void queue_move(BinderPanel* binder, const char* source, const char* target,
                       BinderDropZone zone)
{
    PendingMove* move = g_new0(PendingMove, 1);
    move->binder = binder;
    move->source = g_strdup(source);
    move->target = g_strdup(target);
    move->zone   = zone;
    g_idle_add(run_pending_move, move);
}

static GdkContentProvider* on_drag_prepare(GtkDragSource* source, double x, double y,
                                           gpointer user_data)
{
    (void) x;
    (void) y;
    (void) user_data;

    GtkWidget* row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
    BinderItem* item = row_item(row);
    if (item == NULL) {
        return NULL;
    }

    GdkContentProvider* provider =
        gdk_content_provider_new_typed(BINDER_TYPE_ITEM, item);
    g_object_unref(item);
    return provider;
}

static void on_drag_begin(GtkDragSource* source, GdkDrag* drag, gpointer user_data)
{
    (void) drag;
    (void) user_data;

    /* Drag the row's own likeness, so what is being moved is never in doubt. */
    GtkWidget* row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
    GdkPaintable* likeness = gtk_widget_paintable_new(row);
    gtk_drag_source_set_icon(source, likeness, 0, 0);
    g_object_unref(likeness);
}

static GdkDragAction on_row_drop_motion(GtkDropTarget* target, double x, double y,
                                        gpointer user_data)
{
    (void) x;
    (void) user_data;

    GtkWidget* row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    BinderItem* onto = row_item(row);
    if (onto == NULL) {
        return 0;
    }

    GdkDragAction action = 0;
    if (drop_is_allowed(dragged_item(gtk_drop_target_get_value(target)), onto)) {
        show_drop_feedback(row, zone_at(row, y, onto->is_folder));
        action = GDK_ACTION_MOVE;
    } else {
        clear_drop_feedback(row);
    }

    g_object_unref(onto);
    return action;
}

static void on_row_drop_leave(GtkDropTarget* target, gpointer user_data)
{
    (void) user_data;
    clear_drop_feedback(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)));
}

static gboolean on_row_drop(GtkDropTarget* target, const GValue* value, double x,
                            double y, gpointer user_data)
{
    (void) x;

    BinderPanel* binder = user_data;
    GtkWidget* row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    clear_drop_feedback(row);

    BinderItem* onto = row_item(row);
    BinderItem* dragged = dragged_item(value);
    if (!drop_is_allowed(dragged, onto)) {
        g_clear_object(&onto);
        return FALSE;
    }

    queue_move(binder, dragged->path, onto->path,
               zone_at(row, y, onto->is_folder));

    g_object_unref(onto);
    return TRUE;
}

/* The empty space below the tree stands for the manuscript folder itself, so a
 * drop there lifts an item back out to the top level. Rows have their own drop
 * targets and answer first; this one only has to make sure it is not quietly
 * catching a drop a row already refused. */
static GdkDragAction on_empty_drop_motion(GtkDropTarget* target, double x, double y,
                                          gpointer user_data)
{
    BinderPanel* binder = user_data;
    if (binder->project == NULL || row_widget_at(binder, x, y) != NULL) {
        return 0;
    }
    if (dragged_item(gtk_drop_target_get_value(target)) == NULL) {
        return 0;
    }
    return GDK_ACTION_MOVE;
}

static gboolean on_empty_drop(GtkDropTarget* target, const GValue* value, double x,
                              double y, gpointer user_data)
{
    (void) target;

    BinderPanel* binder = user_data;
    BinderItem* dragged = dragged_item(value);
    if (binder->project == NULL || dragged == NULL
        || row_widget_at(binder, x, y) != NULL) {
        return FALSE;
    }

    queue_move(binder, dragged->path,
               wordsmith_project_manuscript_path(binder->project),
               BINDER_DROP_INTO);
    return TRUE;
}

/* ── renaming in place ───────────────────────────────────────────────────── */

/* Accepting a rename rebuilds the whole binder, which destroys the row the
 * entry being read is sitting in. Queued for the same reason a drop is. */
typedef struct PendingRename {
    BinderPanel* binder;
    char*        path;
    char*        name;
} PendingRename;

static gboolean run_pending_rename(gpointer user_data)
{
    PendingRename* rename = user_data;

    if (rename->binder->rename_callback != NULL) {
        rename->binder->rename_callback(rename->path, rename->name,
                                        rename->binder->rename_user_data);
    }

    g_free(rename->path);
    g_free(rename->name);
    g_free(rename);
    return G_SOURCE_REMOVE;
}

/* Close the edit in progress, keeping what was typed or throwing it away.
 *
 * The panel's record of the edit is cleared *first*, before anything that could
 * fire another signal: putting the label back takes the focus off the entry, and
 * the focus handler lands here too. Clearing first is what makes the second call
 * a no-op rather than a second rename. */
static void finish_rename(BinderPanel* binder, gboolean commit)
{
    if (binder->renaming_entry == NULL) {
        return;
    }

    GtkWidget* entry = binder->renaming_entry;
    char*      path  = binder->renaming_path;
    char*      was   = binder->renaming_name;

    binder->renaming_entry = NULL;
    binder->renaming_path  = NULL;
    binder->renaming_name  = NULL;

    char* typed = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));

    GtkWidget* row = row_widget_from(entry);
    if (row != NULL) {
        gtk_stack_set_visible_child_name(GTK_STACK(row_name_stack(row)),
                                         NAME_PAGE_DISPLAY);
    }

    /* An empty entry is a cancelled edit, not a request to be called nothing.
     * The name it already had is not a rename either, and asking for one would
     * cost a rebuild of the binder to arrive back where it started. */
    if (commit && typed[0] != '\0' && g_strcmp0(typed, was) != 0) {
        PendingRename* rename = g_new0(PendingRename, 1);
        rename->binder = binder;
        rename->path   = g_strdup(path);
        rename->name   = g_strdup(typed);
        g_idle_add(run_pending_rename, rename);
    }

    g_free(typed);
    g_free(path);
    g_free(was);
}

static void start_rename(BinderPanel* binder, GtkWidget* row, BinderItem* item)
{
    GtkWidget* stack = row_name_stack(row);
    GtkWidget* entry = row_name_entry(row);
    if (stack == NULL || entry == NULL) {
        return;
    }

    /* Whatever was being renamed before, this is what is being renamed now. */
    finish_rename(binder, TRUE);

    binder->renaming_entry = entry;
    binder->renaming_path  = g_strdup(item->path);
    binder->renaming_name  = g_strdup(item->name);

    gtk_editable_set_text(GTK_EDITABLE(entry), item->name);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), NAME_PAGE_EDIT);

    /* Selected whole, the way every rename in a file manager starts: the common
     * case is replacing the name, and the author who wanted to amend it only has
     * to press an arrow key first. */
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    gtk_widget_grab_focus(entry);
}

static void on_name_entry_activate(GtkEntry* entry, gpointer user_data)
{
    (void) entry;
    finish_rename(user_data, TRUE);
}

static gboolean on_name_entry_key(GtkEventControllerKey* controller, guint keyval,
                                  guint keycode, GdkModifierType modifiers,
                                  gpointer user_data)
{
    (void) controller;
    (void) keycode;
    (void) modifiers;

    if (keyval != GDK_KEY_Escape) {
        return GDK_EVENT_PROPAGATE;
    }
    finish_rename(user_data, FALSE);
    return GDK_EVENT_STOP;
}

/* Clicking away keeps what was typed, which is what every list that renames in
 * place does, and what an author who has stopped looking at the entry means. */
static void on_name_entry_focus_leave(GtkEventControllerFocus* controller,
                                      gpointer user_data)
{
    BinderPanel* binder = user_data;
    if (binder->renaming_entry
        == gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller))) {
        finish_rename(binder, TRUE);
    }
}

/* A click on the name of a row that was *already* selected opens the entry, the
 * way it does in the Finder.
 *
 * The gesture is on the label rather than on the row, so the icon and the
 * expander's twist arrow are still ordinary places to click. It runs in the
 * bubble phase, from the label outwards, which puts it ahead of the list view's
 * own handling of the press — so "already selected" means as of before this
 * click, which is exactly the question being asked. */
static void on_name_click(GtkGestureClick* gesture, int n_press, double x, double y,
                          gpointer user_data)
{
    (void) x;
    (void) y;

    BinderPanel* binder = user_data;
    if (n_press != 1 || binder->project == NULL) {
        return;
    }

    GtkWidget* label = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GtkWidget* row   = row_widget_from(label);
    BinderItem* item = row != NULL ? row_item(row) : NULL;
    if (item == NULL) {
        return;
    }

    char* selected = binder_panel_selected_path(binder);
    if (g_strcmp0(selected, item->path) == 0) {
        start_rename(binder, row, item);
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }

    g_free(selected);
    g_object_unref(item);
}

void binder_panel_begin_rename(BinderPanel* binder, const char* path)
{
    if (binder == NULL || path == NULL || binder->project == NULL) {
        return;
    }

    for (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(binder->list_view));
         child != NULL; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget* row = row_widget_from(gtk_widget_get_first_child(child));
        BinderItem* item = row != NULL ? row_item(row) : NULL;
        if (item == NULL) {
            continue;
        }

        const gboolean matched = g_strcmp0(item->path, path) == 0;
        if (matched) {
            start_rename(binder, row, item);
        }
        g_object_unref(item);

        if (matched) {
            return;
        }
    }
}

/* ── list model ─────────────────────────────────────────────────────────── */

/* Children of `item`, or NULL for a document. GtkTreeListModel takes the
 * reference it is handed and uses a NULL return to mean "not expandable". */
static GListModel* create_child_model(gpointer item, gpointer user_data)
{
    (void) user_data;

    BinderItem* parent = item;
    if (!parent->is_folder) {
        return NULL;
    }

    GListStore* store = g_list_store_new(BINDER_TYPE_ITEM);
    const size_t count = wordsmith_binder_node_child_count(parent->node);
    for (size_t index = 0; index < count; index++) {
        const WordsmithBinderNode* child =
            wordsmith_binder_node_child(parent->node, index);
        BinderItem* child_item = binder_item_new(child);
        g_list_store_append(store, child_item);
        g_object_unref(child_item);
    }
    return G_LIST_MODEL(store);
}

static void on_setup_row(GtkSignalListItemFactory* factory, GtkListItem* list_item,
                         gpointer user_data)
{
    (void) factory;

    GtkWidget* expander = gtk_tree_expander_new();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* icon = gtk_image_new();
    GtkWidget* label = gtk_label_new(NULL);
    GtkWidget* entry = gtk_entry_new();
    GtkWidget* name = gtk_stack_new();

    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);

    /* The entry asks for no width of its own, so opening it over a name does not
     * shove the row wider than the pane it is in. */
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), 0);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(entry), 0);
    gtk_widget_set_hexpand(entry, TRUE);

    gtk_stack_add_named(GTK_STACK(name), label, NAME_PAGE_DISPLAY);
    gtk_stack_add_named(GTK_STACK(name), entry, NAME_PAGE_EDIT);
    gtk_widget_set_hexpand(name, TRUE);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), name);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    g_object_set_data(G_OBJECT(expander), ROW_LIST_ITEM_KEY, list_item);

    /* Renaming: the click that opens the entry, and the three ways out of it.
     * All of them are on the row's own widgets and read the panel's record of
     * the edit rather than anything captured here, since rows are recycled. */
    GtkGesture* name_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(name_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(name_click, "pressed", G_CALLBACK(on_name_click), user_data);
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(name_click));

    g_signal_connect(entry, "activate", G_CALLBACK(on_name_entry_activate), user_data);

    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_name_entry_key), user_data);
    gtk_widget_add_controller(entry, keys);

    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_name_entry_focus_leave), user_data);
    gtk_widget_add_controller(entry, focus);

    /* Both controllers live on the row for as long as the list item does, and
     * read the item they are acting on out of it at the moment they fire —
     * rows are recycled as the list scrolls, so nothing may be captured here. */
    GtkDragSource* drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_signal_connect(drag, "prepare", G_CALLBACK(on_drag_prepare), NULL);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(drag));

    GtkDropTarget* drop = gtk_drop_target_new(BINDER_TYPE_ITEM, GDK_ACTION_MOVE);
    /* Preloaded, so "motion" can see what is being dragged and refuse a drop
     * before the pointer is released rather than after. */
    gtk_drop_target_set_preload(drop, TRUE);
    g_signal_connect(drop, "motion", G_CALLBACK(on_row_drop_motion), user_data);
    g_signal_connect(drop, "leave", G_CALLBACK(on_row_drop_leave), user_data);
    g_signal_connect(drop, "drop", G_CALLBACK(on_row_drop), user_data);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(drop));

    gtk_list_item_set_child(list_item, expander);
}

static void on_bind_row(GtkSignalListItemFactory* factory, GtkListItem* list_item,
                        gpointer user_data)
{
    (void) factory;
    (void) user_data;

    GtkTreeListRow* row = gtk_list_item_get_item(list_item);
    BinderItem* item = gtk_tree_list_row_get_item(row);

    GtkWidget* expander = gtk_list_item_get_child(list_item);
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);

    GtkWidget* box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
    GtkWidget* icon = gtk_widget_get_first_child(box);

    gtk_image_set_from_icon_name(GTK_IMAGE(icon),
                                 item->is_folder ? "folder-symbolic"
                                                 : "text-x-generic-symbolic");
    gtk_label_set_text(GTK_LABEL(row_name_label(expander)), item->name);

    g_object_unref(item);
}

/* A row scrolling out of the tree takes its widgets away to show something else
 * with them. An edit still open in one of them is over the moment that happens:
 * the entry the author is typing in is about to be handed to another chapter. */
static void on_unbind_row(GtkSignalListItemFactory* factory, GtkListItem* list_item,
                          gpointer user_data)
{
    (void) factory;

    BinderPanel* binder = user_data;
    GtkWidget* expander = gtk_list_item_get_child(list_item);
    if (expander != NULL && binder->renaming_entry == row_name_entry(expander)) {
        finish_rename(binder, TRUE);
    }
}

static void on_selection_changed(GObject* object, GParamSpec* spec,
                                 gpointer user_data)
{
    (void) object;
    (void) spec;

    BinderPanel* binder = user_data;
    if (binder->select_callback == NULL) {
        return;
    }

    GtkTreeListRow* row = gtk_single_selection_get_selected_item(binder->selection);
    if (row == NULL) {
        binder->select_callback(NULL, 0, binder->select_user_data);
        return;
    }

    BinderItem* item = gtk_tree_list_row_get_item(row);
    binder->select_callback(item->path, item->is_folder ? 1 : 0,
                            binder->select_user_data);
    g_object_unref(item);
}

/* Every twist of an expander adds or removes the folder's children, so the flat
 * model changing is the signal that the expansion did. There is no per-row
 * signal to use instead: rows are created and dropped as folders open and
 * close, so there would be nothing lasting to connect to. */
static void on_tree_items_changed(GListModel* model, guint position, guint removed,
                                  guint added, gpointer user_data)
{
    (void) model;
    (void) position;
    (void) removed;
    (void) added;

    BinderPanel* binder = user_data;
    if (binder->applying_expansion || binder->expand_callback == NULL) {
        return;
    }
    binder->expand_callback(binder->expand_user_data);
}

/* ── context menu ────────────────────────────────────────────────────────── */

static void append_targeted(GMenu* section, const char* label, const char* action,
                            const char* target)
{
    GMenuItem* item = g_menu_item_new(label, NULL);
    /* The path travels in the action's target rather than in state kept on the
     * panel: the name prompt is modal and answered long after the popover has
     * closed, so anything the binder remembered would be stale by then. */
    g_menu_item_set_action_and_target_value(item, action, g_variant_new_string(target));
    g_menu_append_item(section, item);
    g_object_unref(item);
}

static void show_context_menu(BinderPanel* binder, double x, double y,
                              const char* target_folder, const char* item_path)
{
    GMenu* model = g_menu_new();

    GMenu* create = g_menu_new();
    append_targeted(create, "New Text", "win.new-text-in", target_folder);
    append_targeted(create, "New Folder", "win.new-folder-in", target_folder);
    g_menu_append_section(model, NULL, G_MENU_MODEL(create));
    g_object_unref(create);

    /* Only over a row: there is nothing to gather up, or to rename, out on the
     * empty space below the tree. */
    if (item_path != NULL) {
        GMenu* group = g_menu_new();
        append_targeted(group, "New Folder with Selection",
                        "win.new-folder-with-selection", item_path);
        g_menu_append_section(model, NULL, G_MENU_MODEL(group));
        g_object_unref(group);

        GMenu* item_section = g_menu_new();
        append_targeted(item_section, "Rename", "win.rename-item", item_path);
        append_targeted(item_section, "Move to Trash", "win.trash-item", item_path);
        g_menu_append_section(model, NULL, G_MENU_MODEL(item_section));
        g_object_unref(item_section);
    }

    gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(binder->context_menu),
                                    G_MENU_MODEL(model));
    g_object_unref(model);

    const GdkRectangle at = { (int) x, (int) y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(binder->context_menu), &at);
    gtk_popover_popup(GTK_POPOVER(binder->context_menu));
}

/* Right-clicking deliberately leaves the selection alone. Selection is what
 * opens a document here, so selecting the row under the pointer would mean
 * asking for a menu quietly swapped the document being edited. The popover
 * points at the row instead, which is what says who the menu is about. */
static void on_secondary_click(GtkGestureClick* gesture, int n_press, double x,
                               double y, gpointer user_data)
{
    (void) n_press;

    BinderPanel* binder = user_data;
    if (binder->project == NULL) {
        return;
    }

    BinderItem* item = row_item_at(binder, x, y);
    char* item_path = NULL;
    char* target = NULL;

    if (item != NULL) {
        item_path = g_strdup(item->path);
        /* New items go inside a folder, or beside a document. */
        target = item->is_folder ? g_strdup(item->path)
                                 : g_path_get_dirname(item->path);
        g_object_unref(item);
    } else {
        target = g_strdup(wordsmith_project_manuscript_path(binder->project));
    }

    show_context_menu(binder, x, y, target, item_path);

    g_free(item_path);
    g_free(target);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* ── tree construction ───────────────────────────────────────────────────── */

/* The top level shows the manuscript folder's children rather than the folder
 * itself, so the binder does not waste a row on a root the user cannot act on. */
static GListModel* build_root_model(BinderPanel* binder)
{
    GListStore* store = g_list_store_new(BINDER_TYPE_ITEM);
    if (binder->project == NULL) {
        return G_LIST_MODEL(store);
    }

    const WordsmithBinderNode* root =
        wordsmith_project_binder_root(binder->project);
    if (root == NULL) {
        return G_LIST_MODEL(store);
    }

    const size_t count = wordsmith_binder_node_child_count(root);
    for (size_t index = 0; index < count; index++) {
        BinderItem* item = binder_item_new(wordsmith_binder_node_child(root, index));
        g_list_store_append(store, item);
        g_object_unref(item);
    }
    return G_LIST_MODEL(store);
}

void binder_panel_reload(BinderPanel* binder)
{
    /* Whatever the rebuild is for, it is not this edit: the rows about to be
     * dropped include the one being typed into. Thrown away rather than kept,
     * because a reload the author did not ask for — a project opening, a
     * document being created — is not their answer to the entry. Explicitly
     * here rather than left to the unbinding of each row, which would read a
     * discarded edit as an accepted one. */
    finish_rename(binder, FALSE);

    /* The rebuild drops every row and with it every expander's state, so the
     * open folders are carried across by hand. Without this, creating a
     * document at the bottom of a book would fold the whole binder shut. */
    char** was_expanded = binder_panel_expanded_paths(binder);

    GListModel* root_model = build_root_model(binder);

    /* Ownership runs in a chain: the tree model consumes the root model, the
     * selection consumes the tree model, and the list view takes a reference
     * on the selection. Dropping our own reference at the end leaves the view
     * as sole owner, so the panel's pointers are borrowed from it. Replacing
     * the model drops every row, which is why node handles never outlive a
     * reload. */
    GtkTreeListModel* tree_model = gtk_tree_list_model_new(
        root_model, FALSE, FALSE, create_child_model, binder, NULL);
    GtkSingleSelection* selection =
        gtk_single_selection_new(G_LIST_MODEL(tree_model));

    gtk_single_selection_set_autoselect(selection, FALSE);
    gtk_single_selection_set_can_unselect(selection, TRUE);
    g_signal_connect(selection, "notify::selected-item",
                     G_CALLBACK(on_selection_changed), binder);
    g_signal_connect(tree_model, "items-changed",
                     G_CALLBACK(on_tree_items_changed), binder);

    binder->tree_model = tree_model;
    binder->selection  = selection;

    gtk_list_view_set_model(binder->list_view, GTK_SELECTION_MODEL(selection));
    g_object_unref(selection);

    binder_panel_set_expanded_paths(binder, (const char* const*) was_expanded);
    g_strfreev(was_expanded);
}

void binder_panel_set_project(BinderPanel* binder, WordsmithProject* project)
{
    binder->project = project;
    binder_panel_reload(binder);
}

/* ── selection helpers ───────────────────────────────────────────────────── */

char* binder_panel_selected_path(BinderPanel* binder)
{
    if (binder == NULL || binder->project == NULL) {
        return NULL;
    }

    GtkTreeListRow* row = gtk_single_selection_get_selected_item(binder->selection);
    if (row == NULL) {
        return NULL;
    }

    BinderItem* item = gtk_tree_list_row_get_item(row);
    char* path = g_strdup(item->path);
    g_object_unref(item);

    return path;
}

char* binder_panel_target_folder(BinderPanel* binder)
{
    if (binder->project == NULL) {
        return NULL;
    }

    const char* manuscript = wordsmith_project_manuscript_path(binder->project);
    GtkTreeListRow* row = gtk_single_selection_get_selected_item(binder->selection);
    if (row == NULL) {
        return g_strdup(manuscript);
    }

    BinderItem* item = gtk_tree_list_row_get_item(row);
    char* target = NULL;
    if (item->is_folder) {
        target = g_strdup(item->path);
    } else {
        target = g_path_get_dirname(item->path);
    }
    g_object_unref(item);

    return target;
}

void binder_panel_select_path(BinderPanel* binder, const char* path)
{
    if (path == NULL) {
        return;
    }

    /* Walk the flattened tree, expanding folders as we go so that rows deeper
     * than the current expansion state come into existence. The model grows
     * beneath us as rows expand, so the count is re-read each pass. */
    for (guint index = 0; index < g_list_model_get_n_items(
             G_LIST_MODEL(binder->tree_model)); index++) {
        GtkTreeListRow* row =
            g_list_model_get_item(G_LIST_MODEL(binder->tree_model), index);
        BinderItem* item = gtk_tree_list_row_get_item(row);

        const gboolean matched = strcmp(item->path, path) == 0;
        const gboolean is_ancestor = item->is_folder
            && g_str_has_prefix(path, item->path);

        if (matched) {
            gtk_single_selection_set_selected(binder->selection, index);
            g_object_unref(item);
            g_object_unref(row);
            return;
        }
        if (is_ancestor) {
            gtk_tree_list_row_set_expanded(row, TRUE);
        }

        g_object_unref(item);
        g_object_unref(row);
    }
}

/* ── expansion ───────────────────────────────────────────────────────────── */

/* Both of these walk the flattened tree, which holds only the rows that are on
 * screen: a folder inside a collapsed one has no row at all. That is the right
 * scope for both directions, since what is being carried is what the author can
 * see. */

char** binder_panel_expanded_paths(BinderPanel* binder)
{
    GPtrArray* paths = g_ptr_array_new();

    if (binder != NULL && binder->tree_model != NULL) {
        const guint count = g_list_model_get_n_items(G_LIST_MODEL(binder->tree_model));
        for (guint index = 0; index < count; index++) {
            GtkTreeListRow* row =
                g_list_model_get_item(G_LIST_MODEL(binder->tree_model), index);
            if (gtk_tree_list_row_get_expanded(row)) {
                BinderItem* item = gtk_tree_list_row_get_item(row);
                g_ptr_array_add(paths, g_strdup(item->path));
                g_object_unref(item);
            }
            g_object_unref(row);
        }
    }

    g_ptr_array_add(paths, NULL);
    return (char**) g_ptr_array_free(paths, FALSE);
}

void binder_panel_set_expanded_paths(BinderPanel* binder, const char* const* paths)
{
    if (binder == NULL || binder->tree_model == NULL || paths == NULL) {
        return;
    }

    GHashTable* wanted = g_hash_table_new(g_str_hash, g_str_equal);
    for (size_t index = 0; paths[index] != NULL; index++) {
        g_hash_table_add(wanted, (gpointer) paths[index]);
    }

    /* Opening a folder inserts its children directly after it, so a single
     * forward pass reaches every depth: a child is always still ahead of the
     * cursor when its parent opens. The model grows underneath, which is why
     * the count is re-read each time round. */
    binder->applying_expansion = TRUE;
    for (guint index = 0;
         index < g_list_model_get_n_items(G_LIST_MODEL(binder->tree_model)); index++) {
        GtkTreeListRow* row =
            g_list_model_get_item(G_LIST_MODEL(binder->tree_model), index);
        BinderItem* item = gtk_tree_list_row_get_item(row);

        if (item->is_folder && g_hash_table_contains(wanted, item->path)) {
            gtk_tree_list_row_set_expanded(row, TRUE);
        }

        g_object_unref(item);
        g_object_unref(row);
    }
    binder->applying_expansion = FALSE;

    g_hash_table_destroy(wanted);
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */

BinderPanel* binder_panel_new(void)
{
    BinderPanel* binder = g_new0(BinderPanel, 1);

    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_setup_row), binder);
    g_signal_connect(factory, "bind", G_CALLBACK(on_bind_row), NULL);
    g_signal_connect(factory, "unbind", G_CALLBACK(on_unbind_row), binder);

    GtkWidget* list_view = gtk_list_view_new(NULL, factory);
    binder->list_view = GTK_LIST_VIEW(list_view);
    gtk_widget_add_css_class(list_view, "navigation-sidebar");

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list_view);

    /* The popover is parented to the list view by hand, so it has to be
     * unparented by hand too; nothing else knows it is there. */
    binder->context_menu = gtk_popover_menu_new_from_model(NULL);
    gtk_popover_set_has_arrow(GTK_POPOVER(binder->context_menu), FALSE);
    gtk_widget_set_halign(binder->context_menu, GTK_ALIGN_START);
    gtk_widget_set_parent(binder->context_menu, list_view);
    g_signal_connect_swapped(list_view, "destroy", G_CALLBACK(gtk_widget_unparent),
                             binder->context_menu);

    /* Capture phase: the rows would otherwise take the press first. */
    GtkGesture* secondary = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(secondary),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(secondary, "pressed", G_CALLBACK(on_secondary_click), binder);
    gtk_widget_add_controller(list_view, GTK_EVENT_CONTROLLER(secondary));

    /* Catches the drops the rows do not: the empty space under the tree. */
    GtkDropTarget* to_root = gtk_drop_target_new(BINDER_TYPE_ITEM, GDK_ACTION_MOVE);
    gtk_drop_target_set_preload(to_root, TRUE);
    g_signal_connect(to_root, "motion", G_CALLBACK(on_empty_drop_motion), binder);
    g_signal_connect(to_root, "drop", G_CALLBACK(on_empty_drop), binder);
    gtk_widget_add_controller(list_view, GTK_EVENT_CONTROLLER(to_root));

    binder->root = scroller;

    /* Start with an empty model so the view is valid before a project opens. */
    binder_panel_reload(binder);
    return binder;
}

void binder_panel_free(BinderPanel* binder)
{
    if (binder == NULL) {
        return;
    }
    g_free(binder->renaming_path);
    g_free(binder->renaming_name);
    g_free(binder);
}

GtkWidget* binder_panel_widget(BinderPanel* binder)
{
    return binder != NULL ? binder->root : NULL;
}

void binder_panel_set_visible(BinderPanel* binder, gboolean visible)
{
    if (binder == NULL) {
        return;
    }
    gtk_widget_set_visible(binder->root, visible);
}

gboolean binder_panel_is_visible(BinderPanel* binder)
{
    return binder != NULL && gtk_widget_get_visible(binder->root);
}

void binder_panel_set_select_callback(BinderPanel* binder,
                                      BinderSelectFn callback,
                                      void* user_data)
{
    binder->select_callback  = callback;
    binder->select_user_data = user_data;
}

void binder_panel_set_move_callback(BinderPanel* binder, BinderMoveFn callback,
                                    void* user_data)
{
    binder->move_callback  = callback;
    binder->move_user_data = user_data;
}

void binder_panel_set_expand_callback(BinderPanel* binder, BinderExpandFn callback,
                                      void* user_data)
{
    binder->expand_callback  = callback;
    binder->expand_user_data = user_data;
}

void binder_panel_set_rename_callback(BinderPanel* binder, BinderRenameFn callback,
                                      void* user_data)
{
    binder->rename_callback  = callback;
    binder->rename_user_data = user_data;
}

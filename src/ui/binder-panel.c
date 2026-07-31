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

/* Set on each row widget in `setup`, so a right-click can find which row it
 * landed on by walking up from the picked widget. The GtkListItem outlives the
 * items bound into it, which is what makes it safe to stash. */
#define ROW_LIST_ITEM_KEY "binder-list-item"

struct BinderPanel {
    GtkWidget*        root;   /* borrowed once parented into the window */
    GtkListView*      list_view;
    GtkTreeListModel* tree_model;
    GtkSingleSelection* selection;
    GtkWidget*        context_menu;  /* GtkPopoverMenu, parented to the list view */

    WordsmithProject* project;   /* borrowed; NULL when no project is open */

    BinderSelectFn select_callback;
    void*          select_user_data;
};

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
    (void) user_data;

    GtkWidget* expander = gtk_tree_expander_new();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* icon = gtk_image_new();
    GtkWidget* label = gtk_label_new(NULL);

    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    g_object_set_data(G_OBJECT(expander), ROW_LIST_ITEM_KEY, list_item);

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
    GtkWidget* label = gtk_widget_get_last_child(box);

    gtk_image_set_from_icon_name(GTK_IMAGE(icon),
                                 item->is_folder ? "folder-symbolic"
                                                 : "text-x-generic-symbolic");
    gtk_label_set_text(GTK_LABEL(label), item->name);

    g_object_unref(item);
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

/* ── context menu ────────────────────────────────────────────────────────── */

/* The BinderItem under (x, y) in list-view coordinates, or NULL over empty
 * space. Owned by the caller. */
static BinderItem* row_item_at(BinderPanel* binder, double x, double y)
{
    GtkWidget* picked = gtk_widget_pick(GTK_WIDGET(binder->list_view), x, y,
                                        GTK_PICK_DEFAULT);

    while (picked != NULL && picked != GTK_WIDGET(binder->list_view)) {
        GtkListItem* list_item = g_object_get_data(G_OBJECT(picked), ROW_LIST_ITEM_KEY);
        if (list_item != NULL) {
            GtkTreeListRow* row = gtk_list_item_get_item(list_item);
            return row != NULL ? gtk_tree_list_row_get_item(row) : NULL;
        }
        picked = gtk_widget_get_parent(picked);
    }
    return NULL;
}

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

    /* Only over a row: there is nothing to gather up out on the empty space
     * below the tree. */
    if (item_path != NULL) {
        GMenu* group = g_menu_new();
        append_targeted(group, "New Folder with Selection",
                        "win.new-folder-with-selection", item_path);
        g_menu_append_section(model, NULL, G_MENU_MODEL(group));
        g_object_unref(group);
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

    binder->tree_model = tree_model;
    binder->selection  = selection;

    gtk_list_view_set_model(binder->list_view, GTK_SELECTION_MODEL(selection));
    g_object_unref(selection);
}

void binder_panel_set_project(BinderPanel* binder, WordsmithProject* project)
{
    binder->project = project;
    binder_panel_reload(binder);
}

/* ── selection helpers ───────────────────────────────────────────────────── */

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

/* ── lifecycle ───────────────────────────────────────────────────────────── */

BinderPanel* binder_panel_new(void)
{
    BinderPanel* binder = g_new0(BinderPanel, 1);

    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_setup_row), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_bind_row), NULL);

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
    g_free(binder);
}

GtkWidget* binder_panel_widget(BinderPanel* binder)
{
    return binder != NULL ? binder->root : NULL;
}

void binder_panel_set_select_callback(BinderPanel* binder,
                                      BinderSelectFn callback,
                                      void* user_data)
{
    binder->select_callback  = callback;
    binder->select_user_data = user_data;
}

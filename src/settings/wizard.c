#include <gconf/gconf-client.h>
#include <hildon/hildon.h>
#include <osso-log.h>
#include <connui/connui.h>

#include <libintl.h>
#include <string.h>

#include "mapper.h"
#include "wizard.h"
#include "stage.h"

struct iap_wizzard_advanced
{
  int gap0;
  int dialog;
  int field_8;
  int field_C;
  GHashTable *widgets;
  int field_14;
  int field_18;
  int import_mode;
  int field_20;
};

struct iap_wizard
{
  gpointer user_data;
  GtkWidget *dialog;
  GtkWidget *button_next;
  GtkWidget *button_finish;
  GtkWindow *parent;
  guint response_id;
  GtkNotebook *notebook;
  gchar **page_ids;
  GHashTable *widgets;
  GHashTable *pages;
  GSList *plugins;
  GSList *plugin_modules;
  struct stage_widget *stage_widgets;
  struct stage *stage;
  int unk1;
  struct iap_wizzard_advanced *advanced;
  int import_mode;
  int unk2;
  gchar *iap_id;
  int current_page;
  int page_index[8];
  gboolean in_progress;
};

struct iap_wizard_page
{
  gchar *id;
  gchar *msgid;
  GtkWidget * (*create)(struct iap_wizard *iw);
  gchar * (*next)(struct iap_wizard *iw, guint current);
  void (*finish)(struct iap_wizard *iw);
  gchar *unk1;
  gchar *next_page;
  gchar *unk2;
  gpointer priv;
};

struct iap_wizard_plugin
{
  const gchar *name;
  guint prio;
  struct iap_wizard_page *pages;
  GHashTable *widgets;
  struct stage_widget *stage_widgets;
  gpointer priv;
  int field_18;
  int get_page;
  int field_20;
  int field_24;
  int field_28;
  int field_2C;
  gboolean advanced_closed;
};


GtkWidget *
iap_wizard_get_dialog(struct iap_wizard *iw)
{
  return iw->dialog;
}

struct stage *
iap_wizard_get_active_stage(struct iap_wizard *iw)
{
  return iw->stage;
}

gchar *
iap_wizard_get_current_page(struct iap_wizard *iw)
{
  gint idx = gtk_notebook_get_current_page(iw->notebook);

  if (idx == -1)
    return NULL;

  return iw->page_ids[idx];
}

gchar *
iap_wizard_get_iap_id(struct iap_wizard *iw)
{
  if (iw)
    return g_strdup(iw->iap_id);

  return NULL;
}

void
iap_wizard_select_plugin_label(struct iap_wizard *iw, gchar *name, guint idx)
{
  gchar *label = g_strdup_printf("%s%d", name, idx);

  if (label)
  {
    GtkWidget *widget = g_hash_table_lookup(iw->widgets, label);

    g_free(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(GTK_WIDGET(widget)), TRUE);
  }
}

void
iap_wizard_set_completed(struct iap_wizard *iw, gboolean completed)
{
  iw->in_progress = !completed;
  iap_wizard_validate_finish_button(iw);
}

static gchar *
iap_wizard_get_next_page(struct iap_wizard *iw, const void *page_name,
                         guint current)
{
  struct iap_wizard_page *wizzard_page;

  if (!page_name)
    page_name = iap_wizard_get_current_page(iw);

  wizzard_page =
      (struct iap_wizard_page *)g_hash_table_lookup(iw->pages, page_name);

  if (wizzard_page)
  {
    if (wizzard_page->next)
      return wizzard_page->next(wizzard_page->priv, current);
    else
      return wizzard_page->next_page;
  }
  else
    ULOG_ERR("IAP Wizard page %s not found!", page_name);

  return NULL;
}

void
iap_wizard_validate_finish_button(struct iap_wizard *iw)
{
  if (!iw->import_mode)
  {
    gboolean sensitive = FALSE;

    hildon_helper_set_insensitive_message(iw->button_finish,
                                          dgettext("osso-connectivity-ui",
                                                   "conn_ib_compl_all"));

    if (!iw->in_progress)
    {
      const gchar *name_text = gtk_entry_get_text(
            GTK_ENTRY(GTK_WIDGET(g_hash_table_lookup(iw->widgets, "NAME"))));
      gchar *page_name = iap_wizard_get_current_page(iw);

      if (!page_name)
        page_name = "WELCOME";

      if (!strcmp(page_name, "NAME_AND_TYPE") && name_text && *name_text)
      {
        hildon_helper_set_insensitive_message(
              iw->button_finish, dgettext("osso-connectivity-ui",
                                          "conn_ib_conn_name_in_use"));
      }

      while (!g_str_has_suffix(page_name, "COMPLETE"))
      {
        page_name = iap_wizard_get_next_page(iw, page_name, 0);

        if (!page_name)
          break;
      }

      if (page_name)
        sensitive = TRUE;
    }

    gtk_dialog_set_response_sensitive(GTK_DIALOG(iw->dialog), 0, sensitive);
  }
}

static void
iap_wizard_load_plugins(struct iap_wizard *iw)
{
  gboolean (*plugin_init)(struct iap_wizard *, struct iap_wizard_plugin *);
  GDir *dir = g_dir_open("/usr/lib/iapsettings", 0, NULL);
  GSList *l;
  const gchar *module_name;

  plugin_init = NULL;

  if (!dir)
    return;

  while ((module_name = g_dir_read_name(dir)))
  {
    gchar *module_path =
        g_module_build_path("/usr/lib/iapsettings", module_name);
    GModule *module = g_module_open(module_path, G_MODULE_BIND_LOCAL);

    ULOG_INFO("Opening module %s: %p", module_path, module);

    if (module &&
        g_module_symbol(module, "iap_wizard_plugin_init",
                        (gpointer *)&plugin_init))
    {
      struct iap_wizard_plugin *plugin = g_new0(struct iap_wizard_plugin, 1);

      if (plugin && plugin_init(iw, plugin))
      {
        ULOG_INFO("Using IAP settings module %s", module_name);
        iw->plugin_modules = g_slist_append(iw->plugin_modules, module);
        iw->plugins = g_slist_append(iw->plugins, plugin);
      }
      else
      {
        g_module_close(module);
        ULOG_ERR("Unable to initialize module %s", module_name);
      }
    }
    else
      ULOG_ERR("Unable to use module %s: %s", module_name, g_module_error());

    g_free(module_path);
  }

  for (l = iw->plugins; l; l = l->next)
  {
    struct iap_wizard_plugin *plugin = l->data;
    struct iap_wizard_page *page;

    iap_wizard_init_stage_widgets(iw, plugin->stage_widgets);
    page = (struct iap_wizard_page *)plugin->pages;

    while (page->id)
    {
      page->priv = plugin->priv;

      g_hash_table_insert(iw->pages, page->id, page);
      page++;
    }
  }

  g_dir_close(dir);
}

static GtkWidget *
iap_wizard_welcome_page_create(struct iap_wizard *iw)
{
  GdkPixbuf *pixbuf;
  GtkWidget *hbox;
  GtkWidget *image;
  GtkWidget *vbox;
  GtkWidget *label;

  hbox = gtk_hbox_new(FALSE, 0);
  pixbuf = connui_pixbuf_load("widgets_wizard", 50);
  image = gtk_image_new_from_pixbuf(pixbuf);
  connui_pixbuf_unref(pixbuf);

  gtk_misc_set_alignment(GTK_MISC(image), 0.0, 0.0);
  gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, FALSE, 0);

  vbox = gtk_vbox_new(FALSE, 0);

  label = gtk_label_new(dgettext("osso-connectivity-ui",
                                 "conn_set_iap_fi_welcome_text"));

  g_object_set(G_OBJECT(label), "wrap", TRUE, NULL);
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.0);

  gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);


  label = gtk_label_new(dgettext("osso-connectivity-ui",
                                 "conn_set_iap_fi_tap_next"));

  g_object_set(G_OBJECT(label), "wrap", TRUE, NULL);
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.0);
  gtk_box_pack_end(GTK_BOX(vbox), label, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

  return GTK_WIDGET(hbox);
}

static void
iap_wizard_complete_page_advanced_clicked_cb(HildonButton *button,
                                             struct iap_wizard *iw)
{
  gtk_dialog_response(GTK_DIALOG(iw->dialog), 4);
}

static GtkWidget *
iap_wizard_complete_page_create(struct iap_wizard *iw)
{
  GtkWidget *vbox;
  GtkWidget *hbox;
  GdkPixbuf *pixbuf;
  GtkWidget *image;
  GtkWidget *label;
  GtkWidget *adv_button;

  vbox = gtk_vbox_new(FALSE, 0);
  hbox = gtk_hbox_new(FALSE, 0);
  pixbuf = connui_pixbuf_load("widgets_wizard", 50);
  image = gtk_image_new_from_pixbuf(pixbuf);
  connui_pixbuf_unref(pixbuf);

  gtk_misc_set_alignment(GTK_MISC(image), 0.0, 0.0);
  gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, FALSE, 0);

  label = gtk_label_new(dgettext("osso-connectivity-ui",
                                 "conn_set_iap_fi_finish_text"));
  g_hash_table_insert(iw->widgets, g_strdup("FINISH_LABEL"), label);
  g_object_set(G_OBJECT(label), "wrap", TRUE, NULL);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.0);
  gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);
  hbox = gtk_hbox_new(FALSE, 0);

  adv_button =
      hildon_button_new_with_text(HILDON_SIZE_FINGER_HEIGHT,
                                  HILDON_BUTTON_ARRANGEMENT_VERTICAL,
                                  dgettext("osso-connectivity-ui",
                                           "conn_set_iap_bd_advanced"),
                                  NULL);
  g_hash_table_insert(iw->widgets, g_strdup("ADVANCED_BUTTON"), adv_button);
  g_signal_connect(GTK_OBJECT(adv_button), "clicked",
                   G_CALLBACK(iap_wizard_complete_page_advanced_clicked_cb),
                   iw);

  gtk_box_pack_end(GTK_BOX(hbox), adv_button, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

  return GTK_WIDGET(vbox);
}

static void
iap_wizard_complete_page_finish(struct iap_wizard *iw)
{
  iap_wizard_set_completed(iw, TRUE);
}

static struct iap_wizard_page iap_wizard_pages[] =
{
  {"WELCOME",
   "conn_set_iap_ti_welcome",
   iap_wizard_welcome_page_create,
   NULL,
   NULL,
   NULL,
   "NAME_AND_TYPE",
   "Connectivity_Internetsettings_connectiondialog",
   NULL},
  {
    "NAME_AND_TYPE",
    "conn_set_iap_ti_name_type",
    iap_wizard_name_and_type_page_create,
    iap_wizard_name_and_type_page_next,
    iap_wizard_name_and_type_page_finish,
    NULL,
    NULL,
    "Connectivity_Internetsettings_connectiondialog",
    NULL
  },
  {
    "COMPLETE",
    "conn_set_iap_ti_finish",
    iap_wizard_complete_page_create,
    NULL,
    iap_wizard_complete_page_finish,
    NULL,
    NULL,
    "Connectivity_Internetsettings_IAPsetupfinish",
    NULL
  },
  {0}
};

static struct stage_widget iap_wizard_widgets[] =
{
  {NULL, NULL, "NAME", "name", NULL, &mapper_entry2string, NULL},
  {NULL, NULL, "IPV4_HTTP_HOST", "proxy_http", NULL, &mapper_entry2string, NULL},
  {NULL, NULL, "IPV4_HTTP_PORT", "proxy_http_port", NULL, &mapper_numbereditor2int, NULL},
  {NULL, NULL, "IPV4_HTTPS_HOST", "proxy_https", NULL, &mapper_entry2string, NULL},
  {NULL, NULL, "IPV4_HTTPS_PORT", "proxy_https_port", NULL, &mapper_numbereditor2int, NULL},
  {NULL, NULL, "IPV4_FTP_HOST", "proxy_ftp", NULL, &mapper_entry2string, NULL},
  {NULL, NULL, "IPV4_FTP_PORT", "proxy_ftp_port", NULL, &mapper_numbereditor2int, NULL},
  {NULL, NULL, "IPV4_RTSP_HOST", "proxy_rtsp", NULL, &mapper_entry2string, NULL},
  {NULL, NULL, "IPV4_RTSP_PORT", "proxy_rtsp_port", NULL, &mapper_numbereditor2int, NULL},
  {NULL, NULL, "IPV4_OMIT_PROXY", "omit_proxy", NULL, &mapper_entry2stringlist, ", "},
  {NULL, NULL, "IPV4_PROXY_URL", "autoconf_url", NULL, &mapper_entry2string, NULL},
  {NULL, NULL, "IPV4_ADDRESS", "ipv4_address", NULL, &mapper_entry2string, NULL},
  {NULL, NULL, "IPV4_NETMASK", "ipv4_netmask", NULL, &mapper_entry2string, NULL},
  {NULL, NULL, "IPV4_GATEWAY", "ipv4_gateway", NULL, &mapper_entry2string, NULL},
  {NULL, NULL, "IPV4_AUTO_DNS", "ipv4_autodns", NULL, &mapper_toggle2bool, NULL},
  {NULL, NULL, "IPV4_DNS1", "ipv4_dns1", NULL, &mapper_entry2string, NULL},
  {NULL, NULL, "IPV4_DNS2", "ipv4_dns2", NULL, &mapper_entry2string, NULL},
  {0, }
};

static void
iap_wizzard_notebook_switch_page_cb(GtkNotebook *notebook, gpointer arg1,
                                    guint idx, struct iap_wizard *iw)
{
  GtkDialog *dialog = GTK_DIALOG(iw->dialog);
  gchar *id = iw->page_ids[idx];

  if (gtk_notebook_get_current_page(notebook) != -1)
  {
    struct iap_wizard_page *page =
        (struct iap_wizard_page *)g_hash_table_lookup(iw->pages, id);

    if (page)
    {
      gboolean sensitive;

      gtk_window_set_title(GTK_WINDOW(dialog), dgettext("osso-connectivity-ui",
                                                        page->msgid));
      sensitive = strcmp(id, "WELCOME");

      if (sensitive)
        sensitive = strcmp(id, "NAME_AND_TYPE");

      gtk_dialog_set_response_sensitive(dialog, 1, sensitive);

      if (g_str_has_suffix(id, "COMPLETE"))
        sensitive = FALSE;
      else if (page->next)
        sensitive = TRUE;
      else if (page->next_page)
        sensitive = TRUE;
      else
        sensitive = FALSE;

      gtk_dialog_set_response_sensitive(dialog, 2, sensitive);

      if (page->finish)
        page->finish(page->priv);
    }
    else
      ULOG_ERR("Unable to find page %s!", id);
  }
}

struct iap_wizard *
iap_wizard_create(gpointer user_data, GtkWindow *parent)
{
  struct iap_wizard *iw = g_new0(struct iap_wizard, 1);
  GtkDialogFlags dialog_flags = GTK_DIALOG_NO_SEPARATOR;
  struct iap_wizard_page *page;
  GtkDialog *dialog;

  iw->user_data = user_data;
  iw->pages = g_hash_table_new(g_str_hash, g_str_equal);
  iw->widgets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  if (parent)
    dialog_flags |= (GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL);

  iw->parent = parent;
  iw->in_progress = TRUE;

  iw->dialog = gtk_dialog_new_with_buttons(NULL, parent, dialog_flags, NULL);
  dialog = GTK_DIALOG(iw->dialog);

  iw->button_finish = gtk_dialog_add_button(dialog,
                                            dgettext("hildon-libs",
                                                     "wdgt_bd_finish"),
                                            0);
  gtk_dialog_add_button(dialog,
                        dgettext("hildon-libs",
                                 "wdgt_bd_previous"),
                        1);
  iw->button_next = gtk_dialog_add_button(dialog,
                                          dgettext("hildon-libs",
                                                   "wdgt_bd_next"),
                                          2);
  iap_common_set_close_response(iw->dialog, 3);

  iw->notebook = GTK_NOTEBOOK(gtk_notebook_new());

  gtk_notebook_set_show_tabs(iw->notebook, FALSE);
  gtk_notebook_set_show_border(iw->notebook, FALSE);

  gtk_container_add(GTK_CONTAINER(dialog->vbox), GTK_WIDGET(iw->notebook));
  iap_wizard_init_stage_widgets(iw, iap_wizard_widgets);

  page = iap_wizard_pages;

  while(page->id)
  {
    page->priv = iw;
    g_hash_table_insert(iw->pages, page->id, page);
  }

  iap_wizard_load_plugins(iw);

  iw->page_ids = g_new0(gchar *, g_hash_table_size(iw->pages) + 1);
  g_hash_table_foreach(iw->pages, (GHFunc)iap_wizzard_create_page, iw);
  g_signal_connect(GTK_OBJECT(iw->notebook), "switch-page",
                   G_CALLBACK(iap_wizzard_notebook_switch_page_cb), iw);
  g_signal_connect(GTK_OBJECT(iw->dialog), "response",
                   G_CALLBACK(iap_wizard_dialog_response_cb), iw);

  return iw;
}

void
iap_wizard_destroy(struct iap_wizard *iw)
{
  GSList *m;
  GSList *l;

  gtk_widget_destroy(iw->dialog);
  for (l = iw->plugins, m = iw->plugin_modules; l && m;
       l = l->next, m = m->next)
  {
    void (*iap_wizard_plugin_destroy)(struct iap_wizard *,
                                      struct iap_wizard_plugin *);

    if (g_module_symbol((GModule *)m->data, "iap_wizard_plugin_destroy",
                        (gpointer *)&iap_wizard_plugin_destroy) )
    {
      if (iap_wizard_plugin_destroy)
        iap_wizard_plugin_destroy(iw, (struct iap_wizard_plugin *)l->data);
    }

    g_module_close((GModule *)m->data);
    g_free(l->data);
  }

  g_slist_free(iw->plugins);
  g_slist_free(iw->plugin_modules);
  g_hash_table_destroy(iw->pages);
  g_hash_table_destroy(iw->widgets);
  g_strfreev(iw->page_ids);
  g_free(iw->stage_widgets);
  g_free(iw->iap_id);
  g_free(iw);
}

GtkWidget *
iap_wizard_get_widget(struct iap_wizard *iw, const gchar *id)
{
  gpointer widget;
  GSList *l;

  if ((widget = g_hash_table_lookup(iw->widgets, id)))
    return GTK_WIDGET(widget);

  for (l = iw->plugins; l; l = l->next )
  {
    struct iap_wizard_plugin *plugin = (struct iap_wizard_plugin *)l->data;

    if ((widget = GTK_WIDGET(g_hash_table_lookup(plugin->widgets, id))))
      return GTK_WIDGET(widget);
  }

  if (iw->advanced && (widget = g_hash_table_lookup(iw->advanced->widgets, id)))
    return GTK_WIDGET(widget);

  return NULL;
}

void
iap_wizzard_create_page(const gchar *id, struct iap_wizard_page *wp,
                        struct iap_wizard *iw)
{
  GtkWidget *page;
  GtkWidget *viewport;
  GtkWidget *scrolled_window;
  gint idx;
  GtkRequisition requisition;

  page = wp->create(wp->priv);
  viewport = gtk_viewport_new(NULL, NULL);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE);
  gtk_container_add(GTK_CONTAINER(viewport), GTK_WIDGET(page));

  scrolled_window = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled_window),
                                      GTK_SHADOW_NONE);
  gtk_container_add(GTK_CONTAINER(scrolled_window), viewport);

  if (GTK_IS_CONTAINER(page))
  {
    gtk_container_set_focus_vadjustment(
          GTK_CONTAINER(page), gtk_scrolled_window_get_vadjustment(
                                         GTK_SCROLLED_WINDOW(scrolled_window)));
  }

  idx = gtk_notebook_append_page(iw->notebook, scrolled_window, NULL);

  if (idx >= 0)
    iw->page_ids[idx] = g_strdup(wp->id);

  gtk_widget_show_all(page);
  gtk_widget_size_request(page, &requisition);
  gtk_widget_set_size_request(scrolled_window, -1, requisition.height);
}

void
iap_wizard_set_start_page(struct iap_wizard *iw, const gchar *page_id)
{
  const gchar *id = iw->page_ids[0];
  int idx = 0;

  if (!id)
    return;

  while (strcmp(id, page_id))
  {
    id = iw->page_ids[++idx];

    if (!id)
      return;
  }

  iw->page_index[0] = idx;
}

void
iap_wizard_set_empty_values(struct iap_wizard *iw)
{
  int idx = 1;
  gpointer widget;
  char iap[100];

  do
  {
    const char *s = dgettext("osso-connectivity-ui",
                             "conn_set_iap_fi_conn_name_default");

    sprintf(iap, s, idx++);
  }
  while (strcmp(iap, "conn_set_iap_fi_conn_name_default") &&
         iap_settings_iap_exists(iap, NULL));

  widget = g_hash_table_lookup(iw->widgets, "NAME");
  gtk_entry_set_text(GTK_ENTRY(GTK_WIDGET(widget)), iap);

  iap_wizard_set_start_page(iw, "WELCOME");
  iap_wizard_validate_finish_button(iw);
}

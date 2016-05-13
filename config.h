#ifndef __PS_CONFIG_H__
#define __PS_CONFIG_H__

#include <glib.h>

typedef struct ps_config_item {
	/*! \brief Name of the item */
	const char *name;
	/*! \brief Value of the item */
	const char *value;
} ps_config_item;

/*! \brief Configuration category ([category]) */
typedef struct ps_config_category {
	/*! \brief Name of the category */
	const char *name;
	/*! \brief Linked list of items */
	GList *items;
} ps_config_category;

/*! \brief Configuration container */
typedef struct ps_config {
	/*! \brief Name of the configuration */
	const char *name;
	/*! \brief Linked list of uncategorized items */
	GList *items;
	/*! \brief Linked list of categories category */
	GList *categories;
} ps_config;


ps_config *ps_config_parse(const char *config_file);
 
ps_config *ps_config_create(const char *name);
 
GList *ps_config_get_categories(ps_config *config);

ps_config_category *ps_config_get_category(ps_config *config, const char *name);

GList *ps_config_get_items(ps_config_category *category);

ps_config_item *ps_config_get_item(ps_config_category *category, const char *name);
 
ps_config_item *ps_config_get_item_drilldown(ps_config *config, const char *category, const char *name);
 
ps_config_category *ps_config_add_category(ps_config *config, const char *category);

int ps_config_remove_category(ps_config *config, const char *category);

ps_config_item *ps_config_add_item(ps_config *config, const char *category, const char *name, const char *value);

int ps_config_remove_item(ps_config *config, const char *category, const char *name);

void ps_config_print(ps_config *config);

int ps_config_save(ps_config *config, const char *folder, const char *filename);

void ps_config_destroy(ps_config *config);


#endif

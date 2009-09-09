/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2009, Nokia
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <sys/param.h>
#include <stdlib.h>
#include <time.h>
#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>

#include <libtracker-client/tracker.h>
#include <libtracker-common/tracker-common.h>

static gchar	*file;
static gchar	*query;
static gboolean	 update;
static gboolean  list_classes;
static gboolean  list_class_prefixes;
static gchar    *list_properties;

static GOptionEntry   entries[] = {
	{ "file", 'f', 0, G_OPTION_ARG_FILENAME, &file,
	  N_("Path to use to run a query or update from file"),
	  N_("FILE"),
	},
	{ "query", 'q', 0, G_OPTION_ARG_STRING, &query,
	  N_("SPARQL query"),
	  N_("SPARQL"),
	},
	{ "update", 'u', 0, G_OPTION_ARG_NONE, &update,
	  N_("This is used with --query and for database updates only."),
	  NULL,
	},
	{ "list-classes", 'c', 0, G_OPTION_ARG_NONE, &list_classes,
	  N_("Retrieve classes"),
	  NULL,
	},
	{ "list-class-prefixes", 'x', 0, G_OPTION_ARG_NONE, &list_class_prefixes,
	  N_("Retrieve class prefixes"),
	  NULL,
	},
	{ "list-properties", 'p', 0, G_OPTION_ARG_STRING, &list_properties,
	  N_("Retrieve properties for a class, prefixes can be used too (e.g. rdfs#Resource)"),
	  N_("CLASS"),
	},
	{ NULL }
};

static gchar *
get_class_from_prefix (TrackerClient *client,
		       const gchar   *prefix)
{
	GError *error = NULL;
	GPtrArray *results;
	const gchar *query;
	gchar *found;
	gint i;
	
	query = "SELECT ?prefix ?ns "
		"WHERE {"
		"  ?ns a tracker:Namespace ;"
		"  tracker:prefix ?prefix "
		"}";
	
	/* We have namespace prefix, get full name */
	results = tracker_resources_sparql_query (client, query, &error);
	
	if (error) {
		g_printerr ("%s, %s\n",
			    _("Could not get namespace prefixes"),
			    error->message);
		g_error_free (error);

		return NULL;
	}
	
	if (!results) {
		g_printerr ("%s\n",
			    _("No namespace prefixes were found"));
		
		return NULL;
	}

	for (i = 0, found = NULL; i < results->len && !found; i++) {
		gchar **data;
		gchar *class_prefix, *class_name;

		data = g_ptr_array_index (results, i);
		class_prefix = data[0];
		class_name = data[1];

		if (strcmp (class_prefix, prefix) == 0) {
			found = g_strdup (class_name);
		}
	}

	g_ptr_array_foreach (results, (GFunc) g_strfreev, NULL);
	g_ptr_array_free (results, TRUE);

	return found;
}

static void
results_foreach (gpointer value,
		 gpointer user_data)
{
	gchar **data;
	gchar **p;
	gint i;

	data = value;

	for (p = data, i = 0; *p; p++, i++) {
		if (i == 0) {
			g_print ("  %s", *p);
		} else {
			g_print (", %s", *p);
		}
	}

	g_print ("\n");
}

int
main (int argc, char **argv)
{
	TrackerClient *client;
	GOptionContext *context;
	GError *error = NULL;
	GPtrArray *results;
	gboolean success;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new (_("- Query or update using SPARQL"));

	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, NULL);

	if (!list_classes && !list_class_prefixes && !list_properties &&
	    ((!file && !query) || (file && query))) {
		gchar *help;

		g_printerr ("%s\n\n",
			    _("Either a file or query needs to be specified"));

		help = g_option_context_get_help (context, TRUE, NULL);
		g_option_context_free (context);
		g_printerr ("%s", help);
		g_free (help);

		return EXIT_FAILURE;
	}

	g_option_context_free (context);

	client = tracker_connect (FALSE, G_MAXINT);

	if (!client) {
		g_printerr ("%s\n",
			    _("Could not establish a DBus connection to Tracker"));
		return EXIT_FAILURE;
	}

	if (list_classes) {
		const gchar *query;

		query = "SELECT ?cl WHERE { ?cl a rdfs:Class }";

		results = tracker_resources_sparql_query (client, query, &error);

		if (error) {
			g_printerr ("%s, %s\n",
				    _("Could not list classes"),
				    error->message);
			g_error_free (error);
			tracker_disconnect (client);

			return EXIT_FAILURE;
		}

		if (!results) {
			g_print ("%s\n",
				 _("No classes were found"));
		} else {
			g_print (tracker_dngettext (NULL,
						    _("Class: %d"), 
						    _("Classes: %d"),
						    results->len),
				 results->len);
			g_print ("\n");

			g_ptr_array_foreach (results, results_foreach, NULL);
			g_ptr_array_foreach (results, (GFunc) g_strfreev, NULL);
			g_ptr_array_free (results, TRUE);
		}
	}

	if (list_class_prefixes) {
		const gchar *query;

		query = "SELECT ?prefix ?ns "
			"WHERE {"
			"  ?ns a tracker:Namespace ;"
			"  tracker:prefix ?prefix "
			"}";

		results = tracker_resources_sparql_query (client, query, &error);

		if (error) {
			g_printerr ("%s, %s\n",
				    _("Could not list class prefixes"),
				    error->message);
			g_error_free (error);
			tracker_disconnect (client);

			return EXIT_FAILURE;
		}

		if (!results) {
			g_print ("%s\n",
				 _("No class prefixes were found"));
		} else {
			g_print (tracker_dngettext (NULL,
						    _("Prefix: %d"), 
						    _("Prefixes: %d"),
						    results->len),
				 results->len);
			g_print ("\n");

			g_ptr_array_foreach (results, results_foreach, NULL);
			g_ptr_array_foreach (results, (GFunc) g_strfreev, NULL);
			g_ptr_array_free (results, TRUE);
		}
	}

	if (list_properties) {
		gchar *query;
		gchar *class_name;

		if (g_str_has_prefix (list_properties, "http://")) {
			/* We have full class name */
			class_name = g_strdup (list_properties);
		} else {
			gchar *p;
			gchar *prefix, *property;
			gchar *class_name_no_property;

			prefix = g_strdup (list_properties);
			p = strchr (prefix, '#');
			
			if (!p) {
				g_printerr ("%s\n", 
					    _("Could not find property for class prefix, "
					      "e.g. #Resource in 'rdfs#Resource'"));
				g_free (prefix);
				tracker_disconnect (client);
				return EXIT_FAILURE;
			}

			property = g_strdup (p + 1);
			*p = '\0';

			class_name_no_property = get_class_from_prefix (client, prefix);
			g_free (prefix);

			if (!class_name_no_property) {
				g_free (property);
				tracker_disconnect (client);
				return EXIT_FAILURE;
			}

			class_name = g_strconcat (class_name_no_property, property, NULL);
			g_free (class_name_no_property);
			g_free (property);
		}
	
		query = g_strdup_printf ("SELECT ?prop "
					 "WHERE {"
					 "  ?prop a rdf:Property ;"
					 "  rdfs:domain <%s>"
					 "}",
					 class_name);

		results = tracker_resources_sparql_query (client, query, &error);
		g_free (query);
		g_free (class_name);

		if (error) {
			g_printerr ("%s, %s\n",
				    _("Could not list properties"),
				    error->message);
			g_error_free (error);
			tracker_disconnect (client);

			return EXIT_FAILURE;
		}

		if (!results) {
			g_print ("%s\n",
				 _("No properties were found"));
		} else {
			g_print (tracker_dngettext (NULL,
						    _("Property: %d"), 
						    _("Properties: %d"),
						    results->len),
				 results->len);
			g_print ("\n");

			g_ptr_array_foreach (results, results_foreach, NULL);
			g_ptr_array_foreach (results, (GFunc) g_strfreev, NULL);
			g_ptr_array_free (results, TRUE);
		}
	}

	if (file) {
		gchar *path_in_utf8;
		gsize size;

		path_in_utf8 = g_filename_to_utf8 (file, -1, NULL, NULL, &error);
		if (error) {
			g_printerr ("%s:'%s', %s\n",
				    _("Could not get UTF-8 path from path"),
				    file,
				    error->message);
			g_error_free (error);
			tracker_disconnect (client);

			return EXIT_FAILURE;
		}

		g_file_get_contents (path_in_utf8, &query, &size, &error);
		if (error) {
			g_printerr ("%s:'%s', %s\n",
				    _("Could not read file"),
				    path_in_utf8,
				    error->message);
			g_error_free (error);
			g_free (path_in_utf8);
			tracker_disconnect (client);

			return EXIT_FAILURE;
		}

		g_free (path_in_utf8);
	}

	if (query) {
		if (G_UNLIKELY (update)) {
			tracker_resources_sparql_update (client, query, &error);

			if (error) {
				g_printerr ("%s, %s\n",
					    _("Could not run update"),
					    error->message);
				g_error_free (error);
				
				return FALSE;
			}
		} else {
			results = tracker_resources_sparql_query (client, query, &error);

			if (error) {
				g_printerr ("%s, %s\n",
					    _("Could not run query"),
					    error->message);
				g_error_free (error);
				
				return FALSE;
			}
			
			if (!results) {
				g_print ("%s\n",
					 _("No results found matching your query"));
			} else {
				g_print (tracker_dngettext (NULL,
							    _("Result: %d"), 
							    _("Results: %d"),
							    results->len),
					 results->len);
				g_print ("\n");
				
				g_ptr_array_foreach (results, results_foreach, NULL);
				g_ptr_array_foreach (results, (GFunc) g_strfreev, NULL);
				g_ptr_array_free (results, TRUE);
			}
		}
	}

	tracker_disconnect (client);

	return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

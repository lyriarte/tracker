/*
 * Copyright (C) 2010, Codeminded BVBA <abustany@gnome.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */
#include <glib.h>
#include <libtracker-client/tracker.h>
#include <libtracker-common/tracker-common.h>
#include <stdlib.h>
#include <string.h>

/* This MUST be larger than TRACKER_STEROIDS_BUFFER_SIZE */
#define LONG_NAME_SIZE 128*1024*sizeof(char)

static TrackerClient *client;

static void
insert_test_data ()
{
	GError *error = NULL;
	const char *delete_query = "DELETE { "
                               "<urn:testdata1> a rdfs:Resource ."
                               "<urn:testdata2> a rdfs:Resource ."
                               "<urn:testdata3> a rdfs:Resource ."
                               "<urn:testdata4> a rdfs:Resource ."
                               "}";
	const char *query = "INSERT {"
	                    "    <urn:testdata1> a nfo:FileDataObject ; nie:url \"/foo/bar\" ."
	                    "    <urn:testdata2> a nfo:FileDataObject ; nie:url \"/plop/coin\" ."
	                    "    <urn:testdata3> a nmm:Artist ; nmm:artistName \"testArtist\" ."
                        "    <urn:testdata4> a nmm:Photo ; nao:identifier \"%s\" ."
	                    "}";
	char *longName = g_malloc (LONG_NAME_SIZE);
	char *filled_query;

	memset (longName, 'a', LONG_NAME_SIZE);

	longName[LONG_NAME_SIZE - 1] = '\0';

	filled_query = g_strdup_printf (query, longName);

	tracker_resources_sparql_update (client, delete_query, NULL);
	tracker_resources_sparql_update (client, filled_query, &error);

	g_free (filled_query);

	g_assert (!error);
}

static void
free_hash_table (GHashTable *hash, gpointer user_data)
{
	g_hash_table_unref (hash);
}

static void
free_results_inner (GPtrArray *array, gpointer user_data)
{
	g_ptr_array_foreach (array, (GFunc)free_hash_table, NULL);
	g_ptr_array_free (array, TRUE);
}

static void
free_results (GPtrArray *results)
{
	g_ptr_array_foreach (results, (GFunc)free_results_inner, NULL);
	g_ptr_array_free (results, TRUE);
}

/*
 * I comment that part out because I don't know how anonymous node hashing
 * works, but if we know two SparqlUpdate calls are going to return the same
 * urns we could use those functions to compare the results between the normal
 * and fast method. I wrote them before realizing I wouldn't know how to use
 * them.
 */
/*
static gboolean
compare_hash_tables (GHashTable *h1, GHashTable *h2)
{
	GHashTableIter i1, i2;
	gpointer k1, v1, k2, v2;

	if (g_hash_table_size (h1) != g_hash_table_size (h2)) {
		return FALSE;
	}

	g_hash_table_iter_init (&i1, h1);
	g_hash_table_iter_init (&i2, h2);

	while (g_hash_table_iter_next (&i1, &k1, &v1)) {
		g_hash_table_iter_next (&i2, &k2, &v2);

		if (g_strcmp0 (k1, k2)) {
			return FALSE;
		}

		if (g_strcmp0 (v1, v2)) {
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
compare_results (GPtrArray *r1, GPtrArray *r2)
{
	int i, j;

	if (!r1 || !r2) {
		return FALSE;
	}

	if (r1->len != r2->len) {
		return FALSE;
	}

	for (i = 0; i < r1->len; i++) {
		GPtrArray *inner1, *inner2;

		inner1 = g_ptr_array_index (r1, i);
		inner2 = g_ptr_array_index (r2, i);

		if (inner1->len != inner2->len) {
			return FALSE;
		}

		for (j = 0; j < inner1->len; j++) {
			GHashTable *h1, *h2;

			h1 = g_ptr_array_index (inner1, j);
			h2 = g_ptr_array_index (inner2, j);

			if (!compare_hash_tables (h1, h2)) {
				return FALSE;
			}
		}
	}

	return TRUE;
}
*/

/* Runs the same query using the iterate and traditional interface, and compare
 * the results */
static void
test_tracker_sparql_query_iterate ()
{
	GPtrArray *r1;
	TrackerResultIterator *iterator;
	GError *error = NULL;
	const gchar *query = "SELECT ?r nie:url(?r) WHERE {?r a nfo:FileDataObject}";
	guint i = 0;
	int n_rows = 0;

	r1 = tracker_resources_sparql_query (client, query, &error);

	g_assert (!error);

	iterator = tracker_resources_sparql_query_iterate (client, query, &error);

	g_assert (!error);

	while (tracker_result_iterator_has_next (iterator)) {
		GStrv row;

		g_assert (i < r1->len);

		n_rows ++;

		tracker_result_iterator_next (iterator);
		row = g_ptr_array_index (r1, i++);

		g_assert (!g_strcmp0 (tracker_result_iterator_value (iterator, 0), row[0]));
	}

	g_assert (n_rows == r1->len);

	tracker_result_iterator_free (iterator);
	tracker_dbus_results_ptr_array_free (&r1);
}

/* Runs the same query using the iterate and traditional interface, and compare
 * the results */
static void
test_tracker_sparql_query_iterate_largerow ()
{
	GPtrArray *r1;
	TrackerResultIterator *iterator;
	GError *error = NULL;
	const gchar *query = "SELECT nao:identifier(?r) WHERE {?r a nmm:Photo}";
	guint i = 0;
	int n_rows = 0;

	r1 = tracker_resources_sparql_query (client, query, &error);

	g_assert (!error);

	iterator = tracker_resources_sparql_query_iterate (client, query, &error);

	g_assert (!error);

	while (tracker_result_iterator_has_next (iterator)) {
		GStrv row;

		g_assert (i < r1->len);

		n_rows ++;

		tracker_result_iterator_next (iterator);
		row = g_ptr_array_index (r1, i++);

		g_assert (!g_strcmp0 (tracker_result_iterator_value (iterator, 0), row[0]));
	}

	g_assert (n_rows == r1->len);

	tracker_result_iterator_free (iterator);
	tracker_dbus_results_ptr_array_free (&r1);
}

/* Runs an invalid query */
static void
test_tracker_sparql_query_iterate_error ()
{
	TrackerResultIterator *iterator;
	GError *error = NULL;
	const gchar *query = "bork bork bork";

	iterator = tracker_resources_sparql_query_iterate (client, query, &error);

	/* tracker_sparql_query_iterate should return null on error */
	g_assert (!iterator);
	/* error should be set, along with its message */
	g_assert (error && error->message);

	g_error_free (error);
}

/* Runs a query returning an empty set */
static void
test_tracker_sparql_query_iterate_empty ()
{
	TrackerResultIterator *iterator;
	GError *error = NULL;
	const gchar *query = "SELECT ?r WHERE {?r a nfo:FileDataObject; nao:identifier \"thisannotationdoesnotexist\"}";

	iterator = tracker_resources_sparql_query_iterate (client, query, &error);

	g_assert (iterator);
	g_assert (!error);

	g_assert (!tracker_result_iterator_has_next (iterator));
	g_assert (!tracker_result_iterator_n_columns (iterator));
	if (g_test_trap_fork (0, G_TEST_TRAP_SILENCE_STDOUT | G_TEST_TRAP_SILENCE_STDERR)) {
		tracker_result_iterator_value (iterator, 0);
		exit (0);
	}
	g_test_trap_assert_failed ();
	/* And just to make coverage happy... */
	tracker_result_iterator_next (iterator);
}

static void
test_tracker_sparql_query_iterate_sigpipe ()
{
	TrackerResultIterator *iterator;
	GError *error = NULL;
	const gchar *query = "SELECT ?r WHERE {?r a nfo:FileDataObject}";

	iterator = tracker_resources_sparql_query_iterate (client, query, &error);

	g_assert (iterator);
	g_assert (!error);

	tracker_result_iterator_next (iterator);

	return;
}

static void
test_tracker_sparql_update_fast_small ()
{
	GError *error = NULL;
	const gchar *query = "INSERT { _:x a nmo:Message }";

	tracker_resources_sparql_update (client, query, &error);

	g_assert (!error);

	return;
}

static void
test_tracker_sparql_update_fast_large ()
{
	GError *error = NULL;
	const gchar *query_template = "INSERT { _:x a nmo:Message; nao:identifier \"%s\" }";
	gchar *lotsOfA;
	gchar *query;

	lotsOfA = g_malloc (LONG_NAME_SIZE);
	memset (lotsOfA, 'a', LONG_NAME_SIZE);
	lotsOfA[LONG_NAME_SIZE-1] = '\0';

	query = g_strdup_printf (query_template, lotsOfA);

	tracker_resources_sparql_update (client, query, &error);

	g_free (lotsOfA);
	g_free (query);

	g_assert (!error);

	return;
}

static void
test_tracker_sparql_update_fast_error ()
{
	GError *error = NULL;
	const gchar *query = "blork blork blork";

	tracker_resources_sparql_update (client, query, &error);

	g_assert (error);

	return;
}

static void
test_tracker_sparql_update_blank_fast_small ()
{
	GError *error = NULL;
	const gchar *query = "INSERT { _:x a nmo:Message }";
	GPtrArray *results;

	results = tracker_resources_sparql_update_blank (client, query, &error);

	g_assert (!error);
	g_assert (results);

	free_results (results);

	return;
}

static void
test_tracker_sparql_update_blank_fast_large ()
{
	GError *error = NULL;
	const gchar *query_template = "INSERT { _:x a nmo:Message; nao:identifier \"%s\" }";
	gchar *lotsOfA;
	gchar *query;
	GPtrArray *results;

	lotsOfA = g_malloc (LONG_NAME_SIZE);
	memset (lotsOfA, 'a', LONG_NAME_SIZE);
	lotsOfA[LONG_NAME_SIZE-1] = '\0';

	query = g_strdup_printf (query_template, lotsOfA);

	results = tracker_resources_sparql_update_blank (client, query, &error);

	g_free (lotsOfA);
	g_free (query);

	g_assert (!error);
	g_assert (results);

	free_results (results);

	return;
}

static void
test_tracker_sparql_update_blank_fast_error ()
{
	GError *error = NULL;
	const gchar *query = "blork blork blork";
	GPtrArray *results;

	results = tracker_resources_sparql_update_blank (client, query, &error);

	g_assert (error);
	g_assert (!results);

	return;
}

static void
test_tracker_sparql_update_blank_fast_no_blanks ()
{
	GError *error = NULL;
	const gchar *query = "INSERT { <urn:not_blank> a nmo:Message }";
	GPtrArray *results;

	results = tracker_resources_sparql_update_blank (client, query, &error);

	g_assert (!error);
	g_assert (results);

	free_results (results);

	return;
}

static void
test_tracker_batch_sparql_update_fast ()
{
	GError *error = NULL;
	const gchar *query = "INSERT { _:x a nmo:Message }";

	tracker_resources_batch_sparql_update (client, query, &error);

	g_assert (!error);

	return;
}

gint
main (gint argc, gchar **argv)
{
        g_type_init ();
        g_test_init (&argc, &argv, NULL);

		client = tracker_client_new (0, -1);

		insert_test_data ();

        g_test_add_func ("/steroids/tracker/tracker_sparql_query_iterate", test_tracker_sparql_query_iterate);
        g_test_add_func ("/steroids/tracker/tracker_sparql_query_iterate_largerow", test_tracker_sparql_query_iterate_largerow);
        g_test_add_func ("/steroids/tracker/tracker_sparql_query_iterate_error", test_tracker_sparql_query_iterate_error);
        g_test_add_func ("/steroids/tracker/tracker_sparql_query_iterate_empty", test_tracker_sparql_query_iterate_empty);
        g_test_add_func ("/steroids/tracker/tracker_sparql_query_iterate_sigpipe", test_tracker_sparql_query_iterate_sigpipe);
        g_test_add_func ("/steroids/tracker/tracker_sparql_update_fast_small", test_tracker_sparql_update_fast_small);
        g_test_add_func ("/steroids/tracker/tracker_sparql_update_fast_large", test_tracker_sparql_update_fast_large);
        g_test_add_func ("/steroids/tracker/tracker_sparql_update_fast_error", test_tracker_sparql_update_fast_error);
        g_test_add_func ("/steroids/tracker/tracker_sparql_update_blank_fast_small", test_tracker_sparql_update_blank_fast_small);
        g_test_add_func ("/steroids/tracker/tracker_sparql_update_blank_fast_large", test_tracker_sparql_update_blank_fast_large);
        g_test_add_func ("/steroids/tracker/tracker_sparql_update_blank_fast_error", test_tracker_sparql_update_blank_fast_error);
        g_test_add_func ("/steroids/tracker/tracker_sparql_update_blank_fast_no_blanks", test_tracker_sparql_update_blank_fast_no_blanks);
        g_test_add_func ("/steroids/tracker/tracker_batch_sparql_update_fast", test_tracker_batch_sparql_update_fast);

		/* client is leaked */

        return g_test_run ();
}

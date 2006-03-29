/* Tracker
 * Copyright (C) 2005, Mr Jamie McCracken (jamiemcc@gnome.org)	
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#define DBUS_API_SUBJECT_TO_CHANGE

#include "config.h"
#include <mysql/mysql.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <glib/gstdio.h>


#ifdef HAVE_INOTIFY
#include "tracker-inotify.h"
#else
#ifdef HAVE_FAM	
#include "tracker-fam.h"
#endif
#endif

#ifndef HAVE_INOTIFY
#ifndef HAVE_FAM
#define POLL_ONLY
#include "tracker-db.h" 
#endif
#endif


#include "tracker-metadata.h"
#include "tracker-dbus-methods.h"
 
/* 
 *   The workflow to process files and notified file change events are as follows:
 *
 *   1) File scan or file notification change (as reported by FAM/iNotify). 
 *   2) File Scheduler (we wait until a file's changes have stabilised  (NB not neccesary with inotify))
 *   3) We process a file's basic metadata (stat) and determine what needs doing in a seperate thread.
 *   4)	We extract embedded metadata/text/thumbnail in another thread and save changes to the DB
 *
 *  The HashTable file_scheduler maintains notified file changes and holds a counter value for 
 *  each file so that we wait a certain period of time for a file's changes to settle down 
 *  before we process them. The counter is increased further if a file undergoes further 
 *  change whilst in this "pause" phase. If using inotify we can just use the 
 *  "writable file close" event instead of this.
 *
 *  Three threads are used to fully process a file event. Files or events to be processed are placed on 
 *  asynchronous queues where another thread takes over the work. 
 *
 *  The main thread is very lightweight and no cpu intensive or file I/O or DB access is permitted 
 *  here after initialisation of the daemon. This ensures the main thread can handle events and DBUS
 *  requests in a timely low latency manner. 
 *
 *  The File Process thread is for moderate CPU intensive load and I/O and involves calls to stat()
 *  and simple fast queries on the DB. The main thread queues files to be processed by this thread
 *  via the file_process async queue. As no heavily CPU intensive activity occurs here, we can quickly
 *  keep the DB representation of the watched file system up to date. Once a file has been processed
 *  here it is then placed on the file metadata queue which is handled by the File Metadata thread.
 *
 *  The File Metadata thread is a low priority thread to handle the highly CPU intensive parts.
 *  During this phase, embedded metadata is extracted from files and if a text filter and/or 
 *  thumbnailer is available for the mime type of the file then these will be spawned synchronously.
 *  Finally all metadata (including file's text contents and path to thumbnails) is saved to the DB.
 *
 *  All responses including user initiated requests are queued by the main thread onto an 
 *  asynchronous queue where multiple threads are waiting to process them. 
 */

#define	FILE_POLL_PERIOD 15 * 60 * 1000



/* global config options variables */

extern	GSList 		*watch_directory_roots_list = NULL;
extern	GSList 		*no_watch_directory_list = NULL;
extern	gboolean	index_text_files = TRUE;
extern	gboolean	index_documents = TRUE;
extern	gboolean	index_source_code = TRUE;
extern	gboolean	index_scripts = FALSE;
extern	gboolean	index_html = TRUE;
extern	gboolean	index_pdf = TRUE;
extern	gboolean	index_application_help_files = TRUE;
extern	gboolean	index_desktop_files = TRUE;
extern	gboolean	index_epiphany_bookmarks = TRUE;
extern	gboolean	index_epiphany_history = TRUE;
extern	gboolean	index_firefox_bookmarks = TRUE;
extern	gboolean	index_firefox_history = TRUE;
extern	gboolean	store_text_file_contents_in_db = FALSE;
extern	char		*db_buffer_memory_limit = "1M";


/* list to store all directories to poll  */
#ifdef POLL_ONLY
static 	GAsyncQueue 	*file_pending_queue;
static 	GAsyncQueue 	*file_process_queue;
#else
extern 	GAsyncQueue 	*file_pending_queue;
extern 	GAsyncQueue 	*file_process_queue;
#endif

extern 	GAsyncQueue 	*user_request_queue;
extern 	GSList 		*poll_list;
extern  DBConnection	*main_thread_db_con = NULL;

static 	GAsyncQueue 	*file_metadata_queue = NULL;

extern 	GMutex 		*log_access_mutex;
extern 	char 		*log_file; 

extern	gboolean	use_nfs_safe_locking;

static 	DBusConnection  *main_connection;
static 	GHashTable  	*file_scheduler = NULL;
static 	GMutex 		*scheduler_mutex = NULL;

static 	gboolean 	is_running = FALSE;

static 	GMutex 		*process_thread_mutex = NULL;
static 	GMutex 		*metadata_thread_mutex = NULL;
static 	GMutex 		*user_thread1_mutex = NULL;
static 	GMutex 		*user_thread2_mutex = NULL;
static 	GMutex 		*poll_thread_mutex = NULL;

static	GThread 	*file_poll_thread = NULL;

static void schedule_file_check (const char * uri);

static void delete_directory (DBConnection *db_con, FileInfo *info);

static void delete_file (DBConnection *db_con, FileInfo *info);

static void scan_directory (FileInfo *info);


#ifdef POLL_ONLY

#define MAX_FILE_WATCHES -1

 gboolean 	tracker_start_watching 		(void){return TRUE;}
 void     	tracker_end_watching 		(void){return;}

 gboolean 	tracker_add_watch_dir 		(const char *dir, DBConnection	*db_con){return FALSE;}
 void     	tracker_remove_watch_dir 	(const char *dir, gboolean delete_subdirs, DBConnection	*db_con) {return;}

 gboolean 	tracker_is_directory_watched 	(const char * dir, DBConnection	*db_con) {return FALSE;}
 int		tracker_count_watch_dirs 	(void) {return 0;}
#endif


static void
do_cleanup ()
{

	
	tracker_print_object_allocations ();

	/* stop threads from further processing of events if possible */
	is_running = FALSE;

	/* acquire thread mutexes - we wait until all threads have been stopped and DB connections closed */

	g_mutex_lock (process_thread_mutex);
	g_mutex_lock (metadata_thread_mutex);
	g_mutex_lock (user_thread1_mutex);
	g_mutex_lock (user_thread2_mutex);
	
	tracker_end_watching ();

	/* This must be called after all other mysql functions */
	mysql_server_end ();
	
}

static void
poll_dir (const char *uri, DBConnection *db_con)
{
	FileInfo 	*info_dir;
	char 		**files, **files_p, *str;
	int 		i;
	

	

	/* check for any deletions*/
	files = tracker_db_get_files_in_folder (db_con, uri);

	for (files_p = files; *files_p; files_p++) {
		if (*files_p) {
			FileInfo 	*info;
				
			str = *files_p;
			tracker_log ("checking %s", str);

	     		if (!tracker_file_is_valid  (str)) {
				info = tracker_create_file_info (str, 1, 0, 0);
				info = tracker_db_get_file_info (db_con, info);

				if (!info->is_directory) {
					delete_file (db_con, info);
				} else {
					delete_directory (db_con, info);
				}
				info = tracker_free_file_info (info); 
			}
		}
	}

	g_strfreev (files);

	/* check for new subfolder additions */
	GSList *file_list = NULL, *tmp;

	file_list = tracker_get_files (uri, TRUE);

	tmp = file_list;
	while (tmp != NULL) {
		FileInfo 	*info;
		
		str = (char *)tmp->data;
		
		info = tracker_create_file_info (str, TRACKER_ACTION_DIRECTORY_CREATED, 0, 0);
		info = tracker_db_get_file_info (db_con, info);	
		if (info->file_id == -1) {
			g_async_queue_push (file_pending_queue, info);
		} else {
			info = tracker_free_file_info (info); 
		}
		tmp = tmp->next;
	}


	g_slist_foreach (file_list, (GFunc) g_free, NULL); 
	g_slist_free (file_list);
	


	/* scan dir for changes in all other files */
	info_dir = tracker_create_file_info (uri, 1, 0, 0);	
	scan_directory (info_dir);
	info_dir = tracker_free_file_info (info_dir); 

}


static void
poll_directories (gpointer db_con)
{	

	g_return_if_fail (db_con);
	tracker_log ("polling dirs");
	if (g_slist_length (poll_list) > 0) { 
		g_slist_foreach (poll_list, (GFunc) poll_dir, db_con);
	}

}





static void
poll_files_thread () 
{
	DBConnection db_con;

	/* set thread safe DB connection */
	mysql_thread_init ();

	db_con.db = tracker_db_connect ();

	tracker_db_prepare_queries (&db_con);

	while (is_running) {

		/* we only poll if we cannot lock mutex */
		if (g_mutex_trylock (poll_thread_mutex)) {
			g_mutex_unlock (poll_thread_mutex);
			g_usleep (1000000);
			continue;
		}			
		
		poll_directories (&db_con);
		g_mutex_unlock (poll_thread_mutex);
		
	}

	mysql_close (db_con.db);
	mysql_thread_end ();

}

static gboolean
start_poll (void)
{
	if (!file_poll_thread && g_slist_length (poll_list) > 0) {
		file_poll_thread = g_thread_create ((GThreadFunc) poll_files_thread, NULL, FALSE, NULL); 
		tracker_log ("started polling");
	}

	g_mutex_trylock (poll_thread_mutex);
	return TRUE;
}


static void 
add_dirs_to_watch_list (GSList *dir_list, gboolean check_dirs, DBConnection *db_con)
{
	GSList *file_list = NULL;
	gboolean start_polling = FALSE;
	
	g_return_if_fail (dir_list != NULL);

	/* add sub directories breadth first recursively to avoid runnig out of file handles */
	while (g_slist_length (dir_list) > 0) {
		
		if (!start_polling && ((tracker_count_watch_dirs () + g_slist_length (dir_list)) < MAX_FILE_WATCHES )) {
			char *str;
			GSList *tmp = dir_list;

			while (tmp != NULL) {
			
				str = (char *)tmp->data;

				/* use polling if FAM or Inotify fails */ 
				if (!tracker_add_watch_dir (str, db_con) && tracker_is_directory (str) && !tracker_is_dir_polled (str)) {
					tracker_add_poll_dir (str);
				}
				tmp = tmp->next;
			}

		} else {
			g_slist_foreach (dir_list, (GFunc) tracker_add_poll_dir, NULL); 
		}

		g_slist_foreach (dir_list, (GFunc) tracker_get_dirs, &file_list);
		
		if (check_dirs) {
			g_slist_foreach (file_list, (GFunc) schedule_file_check, NULL);
		}

		g_slist_foreach (dir_list, (GFunc) g_free, NULL); 
		g_slist_free (dir_list);

		if (tracker_count_watch_dirs () > MAX_FILE_WATCHES) {
			start_polling = TRUE;
		}

		dir_list = file_list;
	
		file_list = NULL;
	}
} 

static gboolean
watch_dir (const char* dir, DBConnection *db_con)
{
	char *dir_utf8 = NULL;
	GSList *mylist = NULL;

	g_assert (dir);

	if (!g_utf8_validate (dir, -1, NULL)) {

		dir_utf8 = g_filename_to_utf8 (dir, -1, NULL,NULL,NULL);
		if (!dir_utf8) {
			tracker_log ("******ERROR**** watch_dir could not be converted to utf8 format");
			return FALSE;
		}
	} else {
		dir_utf8 = g_strdup (dir);
	}
	
	if (!tracker_file_is_valid (dir_utf8)) {
		g_free (dir_utf8);
		return FALSE;
	}

	if (!tracker_is_directory (dir_utf8)) {
		g_free (dir_utf8);
		return FALSE;
	}


/*	if (tracker_is_directory_watched (dir_utf8, db_con)) {
		g_free (dir_utf8);
		return FALSE;
	}
*/

	mylist = g_slist_prepend (mylist, dir_utf8);
	add_dirs_to_watch_list (mylist, TRUE, db_con);
	return TRUE;

} 


static void
signal_handler (int signo) 
{
	
	

	static gboolean in_loop = FALSE;

  	/* avoid re-entrant signals handler calls */
 	 if (in_loop) return;
 
  	in_loop = TRUE;
  
  	switch (signo) {

  		case SIGSEGV:
	  	case SIGBUS:
		case SIGILL:
  		case SIGFPE:
  		case SIGPIPE:
		case SIGABRT:
			if (log_file) {
	   			tracker_log ("Received fatal signal %s so now aborting.",g_strsignal (signo));
			}
    			do_cleanup ();
			exit (1);
    			break;
  
		case SIGTERM:
		case SIGINT:
			if (log_file) {
   				tracker_log ("Received termination signal %s so now exiting", g_strsignal (signo));
			}
			do_cleanup();  
			exit (0);
			break;



		default:
			if (log_file) {
	   			tracker_log ("Received signal %s ", g_strsignal (signo));
			}
			in_loop = FALSE;
    			break;
  	}
}


static void 
delete_file (DBConnection *db_con, FileInfo *info)
{
	char *str_file_id;

	/* info struct may have been deleted in transit here so check if still valid and intact */
	g_return_if_fail ( tracker_file_info_is_valid (info));

	/* if we dont have an entry in the db for the deleted file, we ignore it */
	if (info->file_id == -1) {
		return;
	}

	str_file_id = g_strdup_printf ("%ld", info->file_id);

	tracker_db_exec_stmt (db_con->delete_file_stmt, 1, str_file_id);

	tracker_db_exec_stmt (db_con->delete_metadata_all_stmt, 1, str_file_id);

	g_free (str_file_id);

	tracker_log ("deleting file %s", info->uri);

	return;
}


static void 
delete_directory (DBConnection *db_con, FileInfo *info)
{
	char *str_file_id, *str_path_like, *str_path;

	/* info struct may have been deleted in transit here so check if still valid and intact */
	g_return_if_fail ( tracker_file_info_is_valid (info));

	/* if we dont have an entry in the db for the deleted directory, we ignore it */
	if (info->file_id == -1) {
		return;
	}

	str_file_id = g_strdup_printf ("%ld", info->file_id);

	char *name = g_path_get_basename (info->uri);
	char *path = g_path_get_dirname (info->uri);

	/* This paramter is for SQL like function (which uses % as a wildcard like * for files) */
	str_path_like = g_build_filename (path, name, "%", NULL);

	str_path = g_build_filename (path, name,  NULL);

	g_free (name);
	g_free (path);

	tracker_db_exec_stmt (db_con->delete_file_stmt, 1, str_file_id);

	tracker_db_exec_stmt (db_con->delete_file_child_stmt, 2, str_path, str_path_like);

	tracker_db_exec_stmt (db_con->delete_metadata_all_stmt, 1, str_file_id);

	tracker_db_exec_stmt (db_con->delete_metadata_child_all_stmt, 2, str_path, str_path_like);

	tracker_remove_watch_dir (info->uri, TRUE, db_con);

	tracker_remove_poll_dir (info->uri);

	tracker_log ("deleting directory %s and subdirs like %s", str_path, str_path_like);

	g_free (str_path_like);

	g_free (str_path);

	g_free (str_file_id);

	return;
}


static void
index_file (DBConnection *db_con, FileInfo *info)
{
	char 		*str_dir, *str_link, *str_link_uri, *str_mtime, *str_file_id;
	int 		result;
	GHashTable 	*meta_table;
	time_t		time_stamp;
	struct tm 	*loctime;
	char 		mtime_buffer[20], atime_buffer[20];
	

	/* the file being indexed or info struct may have been deleted in transit so check if still valid and intact */
	g_return_if_fail ( tracker_file_info_is_valid (info));

	if (!tracker_file_is_valid (info->uri)) {
		tracker_log ("Warning - file %s no longer exists - abandoning index on this file", info->uri);
		return;
	}
 	
	/* refresh DB data as previous stuff might be out of date by the time we get here */
	info = tracker_db_get_file_info (db_con, info);
	
	tracker_log ("indexing file %s", info->uri);

	meta_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	if (info->mime) {
		g_free (info->mime);
	}

	if (!info->is_directory) {
		info->mime = tracker_get_mime_type (info->uri);
	} else {
		info->mime = g_strdup ("Folder");
		info->file_size = 0;
	}
		
	if (info->is_link) {
		str_link_uri = g_strconcat (info->link_path, "/", info->link_name, NULL);
	} else {
		str_link_uri = g_strdup (" ");
	}

    	time_stamp = (time_t)info->mtime;
	loctime = localtime (&time_stamp);
	strftime (mtime_buffer, 20, "%Y-%m-%d %H:%M:%S", loctime);

    	time_stamp = (time_t)info->atime;
	loctime = localtime (&time_stamp);
	strftime (atime_buffer, 20, "%Y-%m-%d %H:%M:%S", loctime);

	char *name = g_path_get_basename (info->uri);
	char *path = g_path_get_dirname (info->uri);

	g_hash_table_insert (meta_table, g_strdup ("File.Path"), g_strdup (path));
	g_hash_table_insert (meta_table, g_strdup ("File.Name"), g_strdup (name));
	g_hash_table_insert (meta_table, g_strdup ("File.Link"), g_strdup (str_link_uri));
	g_hash_table_insert (meta_table, g_strdup ("File.Format"), g_strdup (info->mime));
	g_hash_table_insert (meta_table, g_strdup ("File.Size"), g_strdup_printf ("%ld", info->file_size));
	g_hash_table_insert (meta_table, g_strdup ("File.Permissions"), g_strdup (info->permissions));
	g_hash_table_insert (meta_table, g_strdup ("File.Modified"), g_strdup (mtime_buffer));
	g_hash_table_insert (meta_table, g_strdup ("File.Accessed"), g_strdup (atime_buffer));

	str_file_id = g_strdup_printf ("%ld", info->file_id);
	str_dir = g_strdup_printf ("%d", info->is_directory);
	str_link = g_strdup_printf ("%d", info->is_link);
	str_mtime = g_strdup_printf ("%d", info->mtime);

	/* work out whether to insert or update file (file_id of -1 means no existing record so insert) */
	if (info->file_id == -1) {

		result = tracker_db_exec_stmt (db_con->insert_file_stmt, 6, path, name, "0",
					       str_dir, str_link, str_mtime);
		if (result == 1) {
			/* get file_ID of saved file */
			info->file_id = (long) mysql_stmt_insert_id (db_con->insert_file_stmt);
		}	
				
	} else {

		tracker_log ("updating file %s ", info->uri);
		result = tracker_db_exec_stmt (db_con->update_file_stmt, 2, str_mtime, str_file_id);

		/* delete all derived metadata in DB for an updated file */
		tracker_db_exec_stmt (db_con->delete_metadata_derived_stmt, 1, str_file_id);

	}	
			
	if (result == 1) {
		tracker_db_save_metadata (db_con, meta_table, info->file_id);	
	}

	g_hash_table_destroy (meta_table);

	g_free (name);
	g_free (path);

	g_free (str_file_id);
	g_free (str_dir);
	g_free (str_link);
	g_free (str_link_uri);
	g_free (str_mtime);

	if (!info->is_directory && result == 1 && info->file_id != -1) {
		tracker_db_insert_pending_file 	(db_con, info->file_id, info->uri, info->mime, 0, TRACKER_ACTION_EXTRACT_METADATA, info->is_directory);
	}
	
}


static void
schedule_file_check (const char *uri)
{
	FileInfo *info;
	
	info = tracker_create_file_info (uri, TRACKER_ACTION_CHECK, FILE_PAUSE_PERIOD, WATCH_OTHER);

	g_return_if_fail (tracker_file_info_is_valid (info));

	g_async_queue_push (file_pending_queue, info);
}


static void
scan_directory (FileInfo *info)
{
	GSList *file_list = NULL;

	/* info struct may have been deleted in transit here so check if still valid and intact */
	g_return_if_fail (tracker_file_info_is_valid (info));

	g_return_if_fail (tracker_is_directory (info->uri));

	file_list = tracker_get_files (info->uri, FALSE);
	g_slist_foreach (file_list, (GFunc) schedule_file_check, NULL);
	g_slist_foreach (file_list, (GFunc) g_free, NULL); 
	g_slist_free (file_list);
	
	/* recheck directory to update its mtime if its changed whilst scanning */
	schedule_file_check (info->uri);
}


static gboolean
process_scheduled_events (void)
{	
	FileInfo   *info;
	MYSQL_RES *res = NULL;
	MYSQL_ROW  row, end_row;

	info = g_async_queue_try_pop (file_pending_queue);   

	/* mark queued files to be processed as "pending" */ 
	while (info) {
		tracker_db_insert_pending_file 	(main_thread_db_con, info->file_id, info->uri, info->mime, info->counter, info->action, info->is_directory);
		info = tracker_free_file_info (info);
		info = g_async_queue_try_pop (file_pending_queue); 	
	}

	/* decrement counters for pending files */
	tracker_db_exec_stmt (main_thread_db_con->update_pending_files_counter_stmt, 0);

	return TRUE;
}

static gboolean
start_watching (gpointer data)
{	

	if (!tracker_start_watching ()) {
		tracker_log ("File monitoring failed to start");
		do_cleanup ();
		exit (1);
	} else { 
//		mylist = tracker_get_watch_root_dirs ();
		char *watch_folder;

		if (data) {
			watch_folder =  (char *)data;
			watch_dir (watch_folder, main_thread_db_con);
			schedule_file_check (watch_folder);
			g_free (watch_folder);
		} else {
			g_slist_foreach (watch_directory_roots_list, (GFunc) watch_dir, main_thread_db_con);
 			g_slist_foreach (watch_directory_roots_list, (GFunc) schedule_file_check, NULL);
		}

	
		tracker_log ("waiting for file events...");
	}
	return FALSE;
}

static void
extract_metadata_thread () 
{
	FileInfo   *info;
	GHashTable *meta_table;
	DBConnection db_con;
	gboolean has_pending;
	MYSQL_RES *res = NULL;
	MYSQL_ROW  row, end_row;

	/* set thread safe DB connection */
	mysql_thread_init ();

	/* set mutex so we know if the thread is still processing */
	g_mutex_lock (metadata_thread_mutex);

	db_con.db = tracker_db_connect ();

	tracker_db_prepare_queries (&db_con);

	while (is_running) {

		g_mutex_unlock (metadata_thread_mutex);			
		
		info = g_async_queue_try_pop  (file_metadata_queue);

		/* check pending table if we haven't got anything */
		if (!info) {

			has_pending = FALSE;
			
			tracker_exec_sql (db_con.db, CREATE_PENDING_FILES_METADATA);
			tracker_exec_sql (db_con.db, DELETE_PENDING_FILES);
			res = tracker_exec_sql (db_con.db, SELECT_PENDING_FILES);

			if (res) {
				while ((row = mysql_fetch_row (res))) {
					FileInfo *info_tmp;

					info_tmp = tracker_create_file_info (row[1], atoi(row[2]), 0, WATCH_OTHER);
					info_tmp->file_id = atol (row[0]);
					info_tmp->mime = g_strdup (row[3]);
					g_async_queue_push (file_metadata_queue, info_tmp);
					has_pending = TRUE;
				}	
				mysql_free_result (res);
			}

			tracker_exec_sql (db_con.db, DROP_PENDING_FILES);

			if (!has_pending) {
				g_usleep (200000);	
			}
			continue;
		}

	
		g_mutex_lock (metadata_thread_mutex);

		/* info struct may have been deleted in transit here so check if still valid and intact */
		g_return_if_fail ( tracker_file_info_is_valid (info));

		if (info) {
			if (info->uri) {
				char 	*small_thumb_file = NULL, 
					*large_thumb_file = NULL, 
					*file_as_text = NULL;
			
				tracker_log ("Extracting Metadata for file %s with mime %s", info->uri, info->mime);

				/* refresh stat data in case its changed */
				info = tracker_get_file_info (info);
			
				if (info->file_id == -1) {
					info->file_id = (long)tracker_db_get_file_id (&db_con, info->uri);
				}

				meta_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

				tracker_metadata_get_embedded (info->uri, info->mime, meta_table);

				if (g_hash_table_size (meta_table) > 0) {
					tracker_db_save_metadata (&db_con, meta_table, info->file_id);			
	
					/* to do - emit dbus signal here for EmbeddedMetadataChanged */

				}

				g_hash_table_destroy (meta_table);

				/* see if there is a thumbnailer script for the file's mime type */
	
				small_thumb_file = tracker_metadata_get_thumbnail (info->uri, info->mime, THUMB_SMALL);
				if (small_thumb_file) {
				
					large_thumb_file = tracker_metadata_get_thumbnail (info->uri, info->mime, THUMB_LARGE);
				
					if (large_thumb_file) {

						tracker_db_save_thumbs	(&db_con, small_thumb_file, large_thumb_file, info->file_id);

						/* to do - emit dbus signal ThumbNailChanged */

						g_free (large_thumb_file);

					}
					g_free (small_thumb_file);
				}

				file_as_text = tracker_metadata_get_text_file (info->uri, info->mime);
				
				if (file_as_text) {
					tracker_log ("text file is %s", file_as_text);
					/* to do - we need a setting for an upper limit to how much text we read in */
					tracker_db_save_file_contents (&db_con, file_as_text, info->file_id);
					g_free (file_as_text);
							
				}

			}

			info = tracker_dec_info_ref (info);
		}
	}

	mysql_close (db_con.db);
	mysql_thread_end ();

	/* unlock mutex so we know thread has exited */
	g_mutex_unlock (metadata_thread_mutex);
}


/* determines whether an action applies to a file or a directory */
static void
verify_action (FileInfo   *info)
{


	if (info->action == TRACKER_ACTION_CHECK) {
		if (info->is_directory) {
			info->action = TRACKER_ACTION_DIRECTORY_CHECK;	
			info->counter = 0;				
		} else {
			info->action = TRACKER_ACTION_FILE_CHECK;
		} 

	} else {
		if (info->action == TRACKER_ACTION_DELETE || info->action == TRACKER_ACTION_DELETE_SELF) {
			
			/* we are in trouble if we cant find the deleted uri in the DB - assume its a directory (worst case) */
			if (info->file_id == -1) {
				info->is_directory = TRUE;
			}

			info->counter = 0;
			if (info->is_directory) {
				info->action = TRACKER_ACTION_DIRECTORY_DELETED;					
			} else {
				info->action = TRACKER_ACTION_FILE_DELETED;
			}
		} else { 
			if (info->action == TRACKER_ACTION_MOVED_FROM) {
				info->counter = 1;
				if (info->is_directory) {
					info->action = TRACKER_ACTION_DIRECTORY_MOVED_FROM;	
				} else {
					info->action = TRACKER_ACTION_FILE_MOVED_FROM;
				}

			} else {

				if (info->action == TRACKER_ACTION_CREATE) {
					if (info->is_directory) {
						info->action = TRACKER_ACTION_DIRECTORY_CREATED;
						info->counter = 0; /* do not reschedule a created directory */
					} else {
						info->action = TRACKER_ACTION_FILE_CREATED;
					}

				} else {
					if (info->action == TRACKER_ACTION_FILE_MOVED_TO) {
						info->counter = 0;
						if (info->is_directory) {
							info->action = TRACKER_ACTION_DIRECTORY_MOVED_TO;
						} else {
							info->action = TRACKER_ACTION_FILE_MOVED_TO;
						}
					}
				}
			}
		}
	}
}


static void 
process_files_thread ()
{
	FileInfo   *info;
	DBConnection db_con;
	GSList *mylist = NULL, *moved_from_list = NULL; /* list to hold moved_from events whilst waiting for a matching moved_to event */
	gboolean need_index = FALSE, has_pending = FALSE;
	MYSQL_RES *res = NULL;
	MYSQL_ROW  row, end_row;

	/* set thread safe DB connection */
	mysql_thread_init ();

	/* used to indicate if no db action is occuring in thread */
	g_mutex_lock (process_thread_mutex);

	db_con.db = tracker_db_connect ();
	
	tracker_db_prepare_queries (&db_con);

	while (is_running) {

		g_mutex_unlock (process_thread_mutex);		

		info = g_async_queue_try_pop  (file_process_queue);

		/* check pending table if we haven't got anything */
		if (!info) {

			has_pending = FALSE;
			tracker_exec_sql (db_con.db, CREATE_PENDING_FILES);
			tracker_exec_sql (db_con.db, DELETE_PENDING_FILES);
			res = tracker_exec_sql (db_con.db, SELECT_PENDING_FILES);

			if (res) {
				while ((row = mysql_fetch_row (res))) {
					FileInfo *info_tmp;
					tracker_log ("processing %s with event %s", row[1], tracker_actions[atoi(row[2])]);
					info_tmp = tracker_create_file_info (row[1], atoi(row[2]), 0, WATCH_OTHER);
					g_async_queue_push (file_process_queue, info_tmp);
					has_pending = TRUE;
				}	
				mysql_free_result (res);
			}

			tracker_exec_sql (db_con.db, DROP_PENDING_FILES);

			if (!has_pending) {
				g_usleep (200000);	
			}
			continue;
		}


		g_mutex_lock (process_thread_mutex);

		/* info struct may have been deleted in transit here so check if still valid and intact */
		if (!tracker_file_info_is_valid (info)) {
			continue;
		}

		//tracker_log ("processing %s with action %s and counter %d ", info->uri, tracker_actions[info->action], info->counter);

		/* get file ID and other interesting fields from Database if not previously fetched or is newly created */

		if (info->file_id == -1 && info->action != TRACKER_ACTION_CREATE && 
		    info->action != TRACKER_ACTION_DIRECTORY_CREATED && info->action != TRACKER_ACTION_FILE_CREATED) {
			
			info = tracker_db_get_file_info (&db_con, info);

		}

		/* Get more file info if db retrieval returned nothing */
		if (info->file_id == -1 && info->action != TRACKER_ACTION_DELETE && 
		    info->action != TRACKER_ACTION_DIRECTORY_DELETED && info->action != TRACKER_ACTION_FILE_DELETED) {
			info = tracker_get_file_info (info);	
		}

		/* preprocess ambiguous actions when we need to work out if its a file or a directory that the action relates to */
		verify_action (info);

		//tracker_log ("processing %s with action %s and counter %d ", info->uri, tracker_actions[info->action], info->counter);


		/* process deletions */

		if (info->action == TRACKER_ACTION_FILE_DELETED || info->action == TRACKER_ACTION_FILE_MOVED_FROM) {

			delete_file (&db_con, info);

			if (info->action == TRACKER_ACTION_FILE_MOVED_FROM) {
				moved_from_list = g_slist_prepend (moved_from_list, info);				
			} else {
				info = tracker_dec_info_ref (info);
			}

			continue;	

		} else {
			if (info->action == TRACKER_ACTION_DIRECTORY_DELETED || info->action ==  TRACKER_ACTION_DIRECTORY_MOVED_FROM) {

				delete_directory (&db_con, info);

				if (info->action == TRACKER_ACTION_DIRECTORY_MOVED_FROM) {
					moved_from_list = g_slist_prepend (moved_from_list, info);				
				} else {
					info = tracker_dec_info_ref (info);
				}

				continue;
			}
		}

		/* get latest file info from disk */
		if (info->mtime == 0) {
			info = tracker_get_file_info (info);	
		}


		/* check if file needs indexing */
		need_index = (info->mtime > info->indextime);
		
		switch (info->action) {

			case TRACKER_ACTION_FILE_CHECK :
				
	
				break;

			case TRACKER_ACTION_FILE_CHANGED :
			case TRACKER_ACTION_FILE_CREATED :
			case TRACKER_ACTION_WRITABLE_FILE_CLOSED:

				need_index = TRUE;
				
				break;

			case TRACKER_ACTION_FILE_MOVED_TO :

				need_index = FALSE;

				/* to do - look up corresponding info in moved_from_list and update path and name in DB */

				break;
				

			case TRACKER_ACTION_DIRECTORY_CHECK :

				if (need_index) {

					scan_directory (info);
				}
			
				break;

			case TRACKER_ACTION_DIRECTORY_REFRESH :

				//tracker_log ("Refreshing directory %s", info->uri);

				scan_directory (info);
		
					
				break;

			case TRACKER_ACTION_DIRECTORY_CREATED :
			case TRACKER_ACTION_DIRECTORY_MOVED_TO :

				need_index = TRUE;
				tracker_log ("processing created directory %s", info->uri);
	

				/* add to watch folders (including subfolders) */
				//if (info->watch_type == WATCH_SUBFOLDER || info->watch_type == WATCH_ROOT) {
					watch_dir (info->uri, &db_con);
				//}

				/* schedule a rescan for all files in folder to avoid race conditions */
				if (info->action == TRACKER_ACTION_DIRECTORY_CREATED) {
					scan_directory (info);				
					
				}
				
	

				break;
	

			default :
				break;
		}

		if (need_index) {
			index_file (&db_con, info);

		}

		info = tracker_dec_info_ref (info);	

	}

	mysql_close (db_con.db);
	mysql_thread_end ();

	/* unlock mutex so we know thread has exited */
	g_mutex_unlock (process_thread_mutex);
}

static void
process_user_request_queue_thread (GMutex *mutex)
{
	DBusRec *rec;
	DBConnection db_con;

	/* set thread safe DB connection */
	mysql_thread_init ();

	/* set mutex so we know if the thread is still processing */
	g_mutex_lock (mutex);

	db_con.db = tracker_db_connect ();

	tracker_db_prepare_queries (&db_con);

	while (is_running) {

		DBusMessage *reply;
		char **array = NULL;				
		int row_count = 0, i = 0;
		MYSQL_ROW  row = NULL;
		char *str, *str2;

		g_mutex_unlock (mutex);
	
		rec = g_async_queue_pop (user_request_queue);

		rec->user_data = &db_con;

		g_mutex_lock (mutex);

		switch (rec->action) {

			case DBUS_ACTION_PING:
				
				
	
				reply = dbus_message_new_method_return (rec->message);
	
				gboolean result = TRUE;

				dbus_message_append_args (reply,
				  			  DBUS_TYPE_BOOLEAN, &result,
				  			  DBUS_TYPE_INVALID);

				dbus_connection_send (rec->connection, reply, NULL);
				dbus_message_unref (reply);
				g_free (str);	
				break;

			case DBUS_ACTION_GET_METADATA:
			
				tracker_dbus_method_get_metadata (rec);

				break;
		
			case DBUS_ACTION_SET_METADATA:
			
				tracker_dbus_method_set_metadata (rec);

				break;
		

			case DBUS_ACTION_REGISTER_METADATA:
			
				tracker_dbus_method_register_metadata_type (rec);

				break;
		

			case DBUS_ACTION_GET_METADATA_FOR_FILES:
			
				tracker_dbus_method_get_metadata_for_files_in_folder (rec);

				break;
		
			case DBUS_ACTION_SEARCH_METADATA_TEXT:
				tracker_dbus_method_search_metadata_text (rec);
			
				break;

			case DBUS_ACTION_SEARCH_FILES_BY_TEXT_MIME:

				tracker_dbus_method_search_files_by_text_mime (rec);
				
				break;

			case DBUS_ACTION_SEARCH_FILES_BY_TEXT_MIME_LOCATION:

				tracker_dbus_method_search_files_by_text_mime_location (rec);
				
				
				break;

			case DBUS_ACTION_SEARCH_FILES_BY_TEXT_LOCATION:
				tracker_dbus_method_search_files_by_text_location (rec);
				
				break;

			case DBUS_ACTION_SEARCH_FILES_QUERY:

				tracker_dbus_method_search_files_query (rec);
				
				break;



			default :
				break;
		}




		dbus_message_unref (rec->message);

		g_free (rec);


	}

	mysql_close (db_con.db);
	mysql_thread_end ();

	/* unlock mutex so we know thread has exited */
	g_mutex_unlock (mutex);
}


int
main (int argc, char **argv) 
{
	int 		lfp;
  	struct 		sigaction act;
	sigset_t 	empty_mask;
	char 		*prefix, *lock_file, *str, *lock_str, *tracker_data_dir;
	GThread 	*file_metadata_thread, *file_process_thread, *user_request_thread1, *user_request_thread2; 
	GSList 		*mylist = NULL;
	gboolean 	need_setup = FALSE;
	DBConnection 	db_con;
	GMainLoop 	*loop;

	/* mysql vars */
	static char **server_options;
	static char *server_groups[] = {"libmysqd_server", "libmysqd_client", NULL};

	if (!g_thread_supported ()) {
		g_thread_init (NULL);
	}

	dbus_g_thread_init ();	

	/* Set up mutexes for initialisation */
	g_assert (log_access_mutex == NULL);
	log_access_mutex = g_mutex_new ();
	g_assert (scheduler_mutex == NULL);
	scheduler_mutex = g_mutex_new ();


	/* check user data files */
	str = g_build_filename (g_get_home_dir (), ".Tracker", NULL);
	tracker_data_dir = g_build_filename (str, "data", NULL);

	if (!g_file_test (str, G_FILE_TEST_IS_DIR)) {
		need_setup = TRUE;
		g_mkdir (str, 0700);
	}

	if (!g_file_test (tracker_data_dir, G_FILE_TEST_IS_DIR)) {
		need_setup = TRUE;
		g_mkdir (tracker_data_dir, 0700);
	}
	g_free (str);

	umask(077); 
	



	prefix = g_build_filename (g_get_home_dir (), ".Tracker", NULL);
	str = g_strconcat ( g_get_user_name (), "_tracker_lock", NULL);
	log_file = g_build_filename (prefix, "tracker.log", NULL);

	/* check if setup for NFS usage (and enable atomic NFS safe locking) */
	//lock_str = tracker_get_config_option ("NFSLocking");
	lock_str = NULL;
	if (lock_str != NULL) {

		use_nfs_safe_locking = ( strcmp (str , "1" ) == 0); 

		/* place lock file in tmp dir to allow multiple sessions on NFS */
		lock_file = g_build_filename ("/tmp",  str , NULL);

		g_free (lock_str);
		
	} else {

		use_nfs_safe_locking = FALSE;

		/* place lock file in home dir to prevent multiple sessions on NFS (as standard locking might be broken on NFS) */
		lock_file = g_build_filename (prefix ,  str , NULL);
	}	
	

	g_free (prefix);
	g_free (str);
	 
	/* prevent muliple instances  */
	lfp = open (lock_file, O_RDWR|O_CREAT, 0640);
	g_free (lock_file); 

	if (lfp < 0) {
		g_warning ("Cannot open or create lockfile - exiting");
		exit(1);
	}


	if (lockf (lfp, F_TLOCK, 0) <0) {
		g_warning ("Tracker daemon is already running - exiting");
		exit(0); 
	}

	nice (10);


	/* reset log file */
	if (g_file_test (log_file, G_FILE_TEST_EXISTS)) {
		unlink (log_file); 
	}


	/* trap signals */
	sigemptyset (&empty_mask);
	act.sa_handler = signal_handler;
	act.sa_mask    = empty_mask;
	act.sa_flags   = 0;
	sigaction (SIGTERM,  &act, NULL);
	sigaction (SIGILL,  &act, NULL);
	sigaction (SIGBUS,  &act, NULL);
	sigaction (SIGFPE,  &act, NULL);
	sigaction (SIGHUP,  &act, NULL);
	sigaction (SIGSEGV, &act, NULL);
	sigaction (SIGABRT, &act, NULL);
	sigaction (SIGUSR1,  &act, NULL);
	sigaction (SIGINT, &act, NULL);


	tracker_load_config_file ();

	str = g_strdup (DATADIR "/tracker/english");
	if (!tracker_file_is_valid (str)) {
		g_warning ("could not open myswl language file %s", str);
	}

	/* initialise embedded mysql with options*/
	server_options = g_new (char *, 12);
  	server_options[0] = g_strdup ("anything");
  	server_options[1] = g_strconcat  ("--datadir=", tracker_data_dir, NULL);
  	server_options[2] = g_strconcat ("--language=", str,  NULL);
	server_options[3] = g_strdup ("--skip-grant-tables");
	server_options[4] = g_strdup ("--skip-innodb");
	server_options[5] = g_strdup ("--key_buffer_size=1M");
	server_options[6] = g_strdup ("--character-set-server=utf8");
	server_options[7] = g_strdup ("--ft_max_word_len=45");
	server_options[8] = g_strdup ("--ft_min_word_len=3");
	server_options[9] = g_strdup ("--ft_stopword_file=" DATADIR "/tracker/tracker-stop-words.txt");
	server_options[10] = g_strdup ("--myisam-recover=FORCE");
	server_options[11] = NULL;

	mysql_server_init ( 11, server_options, server_groups);

	if (mysql_get_client_version () < 40100) {
		g_warning ("The currently installed version of mysql is too outdated (you need 4.1.* or higher). Exiting...");
		return 1;
	}

	if (mysql_get_client_version () >= 50000) {
		g_warning ("This version of Tracker is not currently compatible with version 5 or higher of mysql (you need 4.1.* or higher)");
		return 1;
	}
	
	
	tracker_log ("DB initialised");

	g_free (str);
	g_free (tracker_data_dir);
	g_strfreev (server_options);



	/* create database if needed */
	if (need_setup) {
		tracker_create_db ();
	}
	
	/* set thread safe DB connection */
	mysql_thread_init ();

	db_con.db = tracker_db_connect ();

	/* clear pending files and watch tables*/
	tracker_exec_sql (db_con.db, "TRUNCATE TABLE FilePending");
	tracker_exec_sql (db_con.db, CREATE_WATCH_TABLE);
	tracker_exec_sql (db_con.db, "TRUNCATE TABLE TMP_WATCH");

	tracker_db_prepare_queries (&db_con);

	main_thread_db_con = &db_con;

	file_scheduler = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	file_metadata_queue = g_async_queue_new ();
	file_process_queue = g_async_queue_new ();
	file_pending_queue = g_async_queue_new ();
	user_request_queue = g_async_queue_new ();


	/* periodically process pending files  table at lowest priority*/
	g_timeout_add_full (G_PRIORITY_LOW, 
			    FILE_SCHEDULE_PERIOD,
		 	    (GSourceFunc) process_scheduled_events,	
			    NULL, NULL	
			   );

	/* periodically poll directories for changes */
	g_timeout_add_full (G_PRIORITY_LOW, 
			    FILE_POLL_PERIOD,
		 	    (GSourceFunc) start_poll,	
			    NULL, NULL	
			   );

	/* schedule the watching of directories so as not to delay start up time*/
	if (argv[1]) {
		g_timeout_add_full (G_PRIORITY_LOW, 
				    500,
			 	    (GSourceFunc) start_watching,	
				    g_strdup (argv[1]), NULL	
				   );
	} else {
		g_timeout_add_full (G_PRIORITY_LOW, 
				    500,
			 	    (GSourceFunc) start_watching,	
				    NULL, NULL	
				   );
	}




  	loop = g_main_loop_new (NULL, TRUE);

	main_connection = tracker_dbus_init ();

	/* this var is used to tell the threads when to quit */
	is_running = TRUE;

	/* thread mutexes - so we know when a thread is not busy */
	g_assert (process_thread_mutex == NULL);
	process_thread_mutex  = g_mutex_new ();

	g_assert (metadata_thread_mutex == NULL);
	metadata_thread_mutex  = g_mutex_new ();

	g_assert (user_thread1_mutex == NULL);
	user_thread1_mutex  = g_mutex_new ();

	g_assert (user_thread2_mutex == NULL);
	user_thread2_mutex  = g_mutex_new ();

	g_assert (poll_thread_mutex == NULL);
	poll_thread_mutex = g_mutex_new ();


	/* execute events and user requests to be processed and indexed in their own threads */
	file_process_thread =  g_thread_create ((GThreadFunc) process_files_thread, NULL, FALSE, NULL); 
	file_metadata_thread = g_thread_create ((GThreadFunc) extract_metadata_thread, NULL, FALSE, NULL); 
	user_request_thread1 =  g_thread_create ((GThreadFunc) process_user_request_queue_thread, user_thread1_mutex, FALSE, NULL); 
	user_request_thread2 =  g_thread_create ((GThreadFunc) process_user_request_queue_thread, user_thread2_mutex, FALSE, NULL); 

	g_main_loop_run (loop);

	mysql_close (db_con.db);

	mysql_thread_end ();

	tracker_dbus_shutdown (main_connection);

	do_cleanup ();

	return 0;
}

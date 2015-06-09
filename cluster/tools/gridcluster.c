/*
OpenIO SDS cluster
Copyright (C) 2014 Worldine, original work as part of Redcurrant
Copyright (C) 2015 OpenIO, modified as part of OpenIO Software Defined Storage

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef G_LOG_DOMAIN
# define G_LOG_DOMAIN "gridcluster.tools"
#endif

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>

#include <metautils/lib/metautils.h>
#include <cluster/lib/gridcluster.h>
#include <cluster/remote/gridcluster_remote.h>

#ifdef HAVE_DEBUG
static void
prompt(void)
{
	gchar buf[1024];

	g_printerr("Type a new line to continue\n");
	fgets(buf, sizeof(buf), stdin);
}
#endif

static void
gba_write(int fd, GByteArray *gba)
{
	gsize written;
	gssize w;

	for (written=0; written < gba->len ;written+=w) {
		w = write(fd, gba->data+written, gba->len-written);
		if (w==0)
			break;
		if (w<0) {
			g_printerr("write error : errno=%d (%s)", errno, strerror(errno));
			return;
		}
		written += w;
	}
}

static void
usage(void)
{
	g_printerr("Usage: gridcluster [OPTION]... <NAMESPACE>...\n\n");
	g_printerr("  %-20s\t%s\n", "--errors,                 -e", "Shows erroneous container identifiers");
	g_printerr("  %-20s\t%s\n", "--clear-services SERVICE    ", "Clear all local RAWX reference in cluster.");
	g_printerr("  %-20s\t%s\n", "--clear-errors              ", "Clear all local erroneous containers references in the cluster");
	g_printerr("  %-20s\t%s\n", "--full,                     ", "Show full services.");
	g_printerr("  %-20s\t%s\n", "--lb-config,                ", "Prints to stdout the namespace LB configuration ");
	g_printerr("  %-20s\t%s\n", "--local-cfg,              -A", "Prints to stdout the namespaces configuration values locally configured");
	g_printerr("  %-20s\t%s\n", "--local-ns,               -L", "Prints to stdout the namespaces locally configured");
	g_printerr("  %-20s\t%s\n", "--local-srv               -l", "List local services monitored on this server.");
	g_printerr("  %-20s\t%s\n", "--local-tasks,            -t", "List internal tasks scheduled on this server.");
	g_printerr("  %-20s\t%s\n", "--raw,                    -r", "Output in parsable mode.");
	g_printerr("  %-20s\t%s\n", "--rules,                  -R", "Dump the logic rules path for the given namespace");
	g_printerr("  %-20s\t%s\n", "--rules-path,             -P", "Dump the logic rules for the given namespace");
	g_printerr("  %-20s\t%s\n", "--service <service desc>, -S", "Select service described by desc.");
	g_printerr("  %-20s\t%s\n", "--set-score <[0..100]>      ", "Set and lock score for the service specified by -S.");
	g_printerr("  %-20s\t%s\n", "--show,                   -s", "Show elements of the cluster.");
	g_printerr("  %-20s\t%s\n", "--unlock-score              ", "Unlock score for the service specified by -S.");
	g_printerr("  %-20s\t%s\n", "--verbose,                -v", "Increases the verbosity");
}

static void
print_formatted_hashtable(GHashTable *ht, const gchar *name)
{
	if (!ht)
		return;

	GList *lk = g_hash_table_get_keys (ht);
	lk = g_list_sort (lk, (GCompareFunc) g_ascii_strcasecmp);
	for (GList *l=lk; l ;l=l->next) {
		const gchar *key = l->data;
		GByteArray *value = g_hash_table_lookup (ht, key);
		g_print("%20s : %s = %.*s\n", name, key, value->len, (gchar*)(value->data));
	}
	g_list_free(lk);
}

static void
print_formated_namespace(namespace_info_t * ns)
{
	g_print("\n");
	g_print("NAMESPACE INFORMATION\n");
	g_print("\n");
	g_print("%20s : %s\n", "Name", ns->name);
	g_print("%20s : %"G_GINT64_FORMAT" bytes\n", "Chunk size", ns->chunk_size);

	print_formatted_hashtable(ns->options, "Option");
	print_formatted_hashtable(ns->storage_policy, "Storage Policy");
	print_formatted_hashtable(ns->storage_class, "Storage Class");
	print_formatted_hashtable(ns->data_security, "Data Security");
	print_formatted_hashtable(ns->data_treatments, "Data Treatments");

	g_print("\n");
}

static void
raw_print_namespace(namespace_info_t * ns)
{
	g_print("NAMESPACE|%s|%"G_GINT64_FORMAT"\n", ns->name, ns->chunk_size);
}

static void
print_formated_services(const gchar * type, GSList * services,
	gboolean show_internals)
{
	if(services && 0 < g_slist_length(services)) {

		gboolean init = FALSE;
		for (GSList *l = services; l; l = l->next) {
			struct service_info_s *si = l->data;
			if(!si)
				continue;
			if(show_internals || !service_info_is_internal(si)) {
				if(!init) {
					g_print("\n-- %s --\n", type);
					init = TRUE;
				}
				char str_score[32];
				char str_addr[STRLEN_ADDRINFO];

				addr_info_to_string(&(si->addr), str_addr, sizeof(str_addr));
				g_snprintf(str_score, sizeof(str_score), "%d", si->score.value);
				g_print("%20s\t%20s\n", str_addr, str_score);
			}
		}
	}
}

static void
print_raw_services(const gchar * ns, const gchar * type, GSList * services,
	gboolean show_internals)
{
	GSList *l;

	if (!services)
		return;
	for (l = services; l; l = l->next) {
		struct service_info_s *si;
		char str_score[32];
		char str_addr[STRLEN_ADDRINFO];

		si = l->data;
		if (!si)
			continue;
		if(show_internals || !service_info_is_internal(si)) {
			addr_info_to_string(&(si->addr), str_addr, sizeof(str_addr));
			g_snprintf(str_score, sizeof(str_score), "%d", si->score.value);
			g_print("%s|%s|%s|score=%d", ns ? ns : si->ns_name,
					type ? type : si->type, str_addr, si->score.value);
			if (si->tags) {
				int i, max;
				struct service_tag_s *tag;
				gchar str_tag_value[256];

				for (i = 0, max = si->tags->len; i < max; i++) {
					tag = g_ptr_array_index(si->tags, i);
					service_tag_to_string(tag, str_tag_value,
							sizeof(str_tag_value));
					g_print("|%s=%s", tag->name, str_tag_value);
				}
			}
			g_print("\n");
		}
	}
}

static void
raw_print_list_task(GSList * tasks)
{
	GSList *le = NULL;
	struct task_s *task;

	for (le = tasks; le && le->data; le = le->next) {
		task = (struct task_s *) le->data;

		g_print("%s|%li|%s\n", task->id, task->next_schedule, task->busy ? "running" : "waiting");
	}
}

static int
set_service_score(const char *service_desc, int score, GError ** error)
{
	int rc=0, nb_match;
	struct service_info_s *si = NULL;
	char *ns_name = NULL;
	char *service_type = NULL;
	char *remaining = NULL;
	char *service_ip = NULL;
	int service_port = 0;
	GSList *list = NULL;
	addr_info_t *cluster_addr = NULL;

	nb_match = sscanf(service_desc, "%a[^|]|%a[^|]|%a[^:]:%i|%as", &ns_name, &service_type, &service_ip, &service_port, &remaining);
	if (nb_match < 4) {
		GSETERROR(error, "Failed to scan service desc in string [%s] : only %d patterns", service_desc, nb_match);
		goto exit_label;
	}

	if (!(cluster_addr = gridcluster_get_conscience_addr(ns_name))) {
		GSETERROR(error, "Unknown namespace");
		goto exit_label;
	}

	si = g_malloc0(sizeof(struct service_info_s));

	if (!service_info_set_address(si, service_ip, service_port, error)) {
		GSETERROR(error, "Invalid service address ip=%s port=%i", service_ip, service_port);
		goto exit_label;
	}

	g_strlcpy(si->ns_name, ns_name, sizeof(si->ns_name));
	g_strlcpy(si->type, service_type, sizeof(si->type));
	si->score.value = score;
	si->score.timestamp = time(0);

	list = g_slist_prepend(NULL, si);
	/* whatever we want to lock or unlock, we say 'TRUE' to act on the lock.
	 * we will unlock with a negative score */
	rc = gcluster_push_services(cluster_addr, 4000, list, TRUE, error);
	g_slist_free(list);

	if (!rc)
		GSETERROR(error, "Failed to lock the service");
exit_label:
	if (remaining)
		free(remaining);
	if (ns_name)
		free(ns_name);
	if (service_type)
		free(service_type);
	if (service_ip)
		free(service_ip);
	if (si)
		service_info_clean(si);
	if (cluster_addr)
		g_free(cluster_addr);
	return rc;
}

static void
enable_debug(void)
{
	gchar *str_enable;

	str_enable = getenv("GS_DEBUG_ENABLE");
	if (!str_enable)
		return;
}

int
main(int argc, char **argv)
{
	int rc = -1;
	gchar *namespace = NULL;
	gboolean has_allcfg = FALSE;
	gboolean has_nslist = FALSE;
	gboolean has_show = TRUE;
	gboolean has_show_internals = FALSE;
	gboolean has_raw = FALSE;
	gboolean has_clear_services = FALSE;
	gboolean has_list = FALSE;
	gboolean has_set_score = FALSE;
	gboolean has_unlock_score = FALSE;
	gboolean has_service = FALSE;
	gboolean has_list_task = FALSE;
	gboolean has_rules = FALSE;
	gboolean has_rules_path = FALSE;
	gboolean has_lbconfig = FALSE;
	gboolean has_flag_full = FALSE;
	int c = 0;
	int option_index = 0;
	int score = -1;
	char service_desc[512], cid_str[128];
	namespace_info_t *ns = NULL;
	GError *error = NULL;
	static struct option long_options[] = {
		/* long options only */
		{"set-score",      1, 0, 4},
		{"unlock-score",   0, 0, 5},
		{"full",           0, 0, 7},

		/* both long and short */
		{"config",         0, 0, 'c'},
		{"rules-path",     0, 0, 'P'},
		{"rules",          0, 0, 'R'},
		{"clear-services", 1, 0, 'C'},
		{"service",        1, 0, 'S'},
		{"local-cfg",      0, 0, 'A'},
		{"local-ns",       0, 0, 'L'},
		{"local-srv",      0, 0, 'l'},
		{"local-tasks",    0, 0, 't'},
		{"show",           0, 0, 's'},
		{"show-internals", 0, 0, 'a'},
		{"raw",            0, 0, 'r'},
		{"help",           0, 0, 'h'},
		{"verbose",        0, 0, 'v'},
		{"lb-config",      0, 0, 'B'},
		{0, 0, 0, 0}
	};

	HC_PROC_INIT(argv, GRID_LOGLVL_INFO);

	memset(service_desc, '\0', sizeof(service_desc));
	memset(cid_str, 0x00, sizeof(cid_str));
	enable_debug();

	while ((c = getopt_long(argc, argv, "ALsvealtrPBRC:S:h", long_options, &option_index)) > -1) {

		switch (c) {
			case 'A':
				has_allcfg = TRUE;
				break;
			case 'B':
				has_lbconfig = TRUE;
				break;
			case 'L':
				has_nslist = TRUE;
				break;
			case 'P':
				if (!optarg) {
					g_printerr("The option '-P' requires an argument. Try %s -h\n", argv[0]);
					abort();
				}
				g_strlcpy(service_desc, optarg, sizeof(service_desc)-1);
				has_rules_path = TRUE;
				break;
			case 'R':
				if (!optarg) {
					g_printerr("The option '-R' requires an argument. Try %s -h\n", argv[0]);
					abort();
				}
				g_strlcpy(service_desc, optarg, sizeof(service_desc)-1);
				has_rules = TRUE;
				break;
			case 'C':
				if (!optarg) {
					g_printerr("The option '-C' requires an argument. Try %s -h\n", argv[0]);
					abort();
				}
				g_strlcpy(service_desc, optarg, sizeof(service_desc)-1);
				has_clear_services = TRUE;
				break;
			case 4:
				has_set_score = TRUE;
				score = atoi(optarg);
				break;
			case 5:
				has_set_score = TRUE;
				has_unlock_score = TRUE;
				break;
			case 7:
				has_flag_full = TRUE;
				break;
			case 'S':
				has_service = TRUE;
				if (!optarg) {
					g_printerr("The option '-S' requires an argument. Try %s -h\n", argv[0]);
					abort();
				}
				g_strlcpy(service_desc, optarg, sizeof(service_desc)-1);
				break;
			case 'l':
				has_list = TRUE;
				break;
			case 't':
				has_list_task = TRUE;
				break;
			case 's':
				has_show = TRUE;
				break;
			case 'r':
				has_raw = TRUE;
				break;
			case 'v':
				logger_verbose();
				break;
			case 'a':
				has_show_internals = TRUE;
				break;
			case 'h':
				rc = 0;
			case '?':
			case 0:
			default:
				usage();
				goto exit_label;
		}
	}

	if (has_allcfg) {
		GHashTable *ht_cfg = gridcluster_parse_config();
		GHashTableIter iter;
		gpointer k, v;
		g_hash_table_iter_init(&iter, ht_cfg);
		while (g_hash_table_iter_next(&iter, &k, &v))
			g_print("%s=%s\n", (gchar*)k, (gchar*)v);
		g_hash_table_destroy(ht_cfg);
		goto success_label;
	}

	if (has_nslist) {
		gchar **pns, **allns;
		allns = gridcluster_list_ns();
		for (pns=allns; *pns ;pns++)
			g_print("%s\n",*pns);
		g_strfreev(allns);
		goto success_label;
	}

	if (!has_list && !has_set_score && !has_list_task) {
		namespace = argv[argc - 1];
		if (argc < 2 || namespace == NULL) {
			g_printerr("\nNo namespace specified in args, aborting.\n\n");
			usage();
			goto exit_label;
		}

		ns = get_namespace_info(namespace, &error);
		if (ns == NULL) {
			g_printerr("Failed to get namespace info :\n");
			g_printerr("%s\n", error->message);
			goto exit_label;
		}
	}

	if (has_lbconfig) {
		GError *err = NULL;
		struct service_update_policies_s *pol;
		gchar *cfg = gridcluster_get_service_update_policy(ns);
		if (!cfg) {
			g_printerr("Invalid NSINFO\n");
			goto exit_label;
		}

		pol = service_update_policies_create();
		err = service_update_reconfigure(pol, cfg);
		g_free(cfg); cfg=NULL;

		if (err) {
			g_printerr("Invalid namespace configuration : (%d) %s\n",
					err->code, err->message);
			service_update_policies_destroy(pol);
			g_clear_error(&err);
			goto exit_label;
		}
		char *tmp = NULL;
		tmp = service_update_policies_dump(pol);
		g_print("%s\n", tmp);
		g_free(tmp);
		service_update_policies_destroy(pol);
	}
	else if (has_rules_path) {
		gchar *path = NULL;
		if (!namespace_get_rules_path(namespace, service_desc, &path, &error)) {
			g_printerr("Failed to get namespace rules :\n");
			g_printerr("%s\n", error->message);
			goto exit_label;
		}
		if (path) {
			g_print("%s\n", path);
			g_free(path);
		}
	}
	else if (has_rules) {
		GByteArray *gba_rules = namespace_get_rules(namespace, service_desc, &error);
		if (!gba_rules) {
			g_printerr("Failed to get namespace rules :\n");
			g_printerr("%s\n", error->message);
			goto exit_label;
		}
		gba_write(1, gba_rules);
		g_byte_array_free(gba_rules, TRUE);
	}
	else if (has_clear_services) {

		if (!clear_namespace_services(namespace, service_desc, &error)) {
			g_printerr("Failed to send clear order to cluster for ns='%s' and service='%s' :\n", namespace,
					service_desc);
			g_printerr("%s\n", error->message);
			goto exit_label;
		}
		else {
			g_print("CLEAR order successfully sent to cluster for ns='%s' and service='%s'.\n", namespace,
					service_desc);
		}

	}
	else if (has_list) {

		GSList *services = list_local_services(&error);

		if (services == NULL && error) {
			g_printerr("Failed to get service list\n");
			g_printerr("%s\n", error->message);
			goto exit_label;
		}

		print_raw_services(NULL, NULL, services, has_show_internals);

	}
	else if (has_list_task) {

		GSList *tasks = list_tasks(&error);

		if (tasks == NULL && error) {
			g_printerr("Failed to get task list \n");
			g_printerr("%s\n", error->message);
			goto exit_label;
		}

		raw_print_list_task(tasks);

	}
	else if (has_set_score) {

		if (!has_service) {
			g_printerr("No service specified.\n");
			g_printerr("Please, use the -S option to specify a service.\n");
			goto exit_label;
		}

		if (!set_service_score(service_desc, has_unlock_score ? -1 : score, &error)) {
			g_printerr("Failed to set score of service [%s] :\n", service_desc);
			g_printerr("%s\n", error->message);
			goto exit_label;
		}

		if (has_unlock_score)
			g_print("Service [%s] score has been successfully unlocked\n", service_desc);
		else
			g_print("Score of service [%s] has been successfully locked to %d\n", service_desc, score);

	}
	else if (has_show) {

		if (has_raw)
			raw_print_namespace(ns);
		else
			print_formated_namespace(ns);

		gchar *csurl = gridcluster_get_conscience(namespace);
		if (!csurl) {
			g_printerr("No conscience address known for [%s]\n", namespace);
			goto exit_label;
		}
		GSList *services_types = list_namespace_service_types(namespace, &error);

		if (!services_types) {
			if (error) {
				g_printerr("Failed to get the services list: %s\n", gerror_get_message(error));
				goto exit_label;
			}
			else {
				g_print("No service type known in namespace=%s\n", namespace);
			}
		}
		else {
			GSList *st;

			for (st = services_types; st; st = st->next) {
				GSList *list_services = NULL;
				gchar *str_type = st->data;

				/* Generate the list */
				if (!has_flag_full || !has_raw) {
					list_services = list_namespace_services(namespace, str_type, &error);
				} else {
					list_services = gcluster_get_services(csurl, 0.0, str_type, TRUE, &error);
				}

				/* Dump the list */
				if (error && !list_services) {
					g_printerr("No service known for namespace %s and service"
							" type %s : %s\n",
							namespace, str_type, gerror_get_message(error));
				}
				else {
					if (has_raw)
						print_raw_services(namespace, str_type, list_services,
								has_show_internals);
					else
						print_formated_services(str_type, list_services,
								has_show_internals);
				}

				if (error)
					g_clear_error(&error);

				/* Clean the list */
				if (list_services) {
					g_slist_foreach(list_services, service_info_gclean, NULL);
					g_slist_free(list_services);
					list_services = NULL;
				}
			}
		}
	}

success_label:
	rc = 0;
exit_label:
	if (ns)
		namespace_info_free(ns);
	if (error)
		g_clear_error(&error);
	return rc;
}

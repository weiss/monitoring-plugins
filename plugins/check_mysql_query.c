/*****************************************************************************
 *
 * Monitoring check_mysql_query plugin
 *
 * License: GPL
 * Copyright (c) 2006-2009 Monitoring Plugins Development Team
 * Original code from check_mysql, copyright 1999 Didi Rieder
 *
 * Description:
 *
 * This file contains the check_mysql_query plugin
 *
 * This plugin is for running arbitrary SQL and checking the results
 *
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 *****************************************************************************/

const char *progname = "check_mysql_query";
const char *copyright = "1999-2007";
const char *email = "devel@monitoring-plugins.org";

#include "common.h"
#include "utils.h"
#include "utils_base.h"
#include "netutils.h"

#include <mysql.h>
#include <errmsg.h>

static char *db_user = NULL;
static char *db_host = NULL;
static char *db_socket = NULL;
static char *db_pass = NULL;
static char *db = NULL;
static char *opt_file = NULL;
static char *opt_group = NULL;
static unsigned int db_port = MYSQL_PORT;

static int process_arguments(int /*argc*/, char ** /*argv*/);
static int validate_arguments(void);
static void print_help(void);
void print_usage(void);

static char *sql_query = NULL;
static int verbose = 0;
static thresholds *my_thresholds = NULL;

int main(int argc, char **argv) {
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	/* Parse extra opts if any */
	argv = np_extra_opts(&argc, argv, progname);

	if (process_arguments(argc, argv) == ERROR)
		usage4(_("Could not parse arguments"));

	MYSQL mysql;
	/* initialize mysql  */
	mysql_init(&mysql);

	if (opt_file != NULL)
		mysql_options(&mysql, MYSQL_READ_DEFAULT_FILE, opt_file);

	if (opt_group != NULL)
		mysql_options(&mysql, MYSQL_READ_DEFAULT_GROUP, opt_group);
	else
		mysql_options(&mysql, MYSQL_READ_DEFAULT_GROUP, "client");

	/* establish a connection to the server and error checking */
	if (!mysql_real_connect(&mysql, db_host, db_user, db_pass, db, db_port, db_socket, 0)) {
		if (mysql_errno(&mysql) == CR_UNKNOWN_HOST)
			die(STATE_WARNING, "QUERY %s: %s\n", _("WARNING"), mysql_error(&mysql));
		else if (mysql_errno(&mysql) == CR_VERSION_ERROR)
			die(STATE_WARNING, "QUERY %s: %s\n", _("WARNING"), mysql_error(&mysql));
		else if (mysql_errno(&mysql) == CR_OUT_OF_MEMORY)
			die(STATE_WARNING, "QUERY %s: %s\n", _("WARNING"), mysql_error(&mysql));
		else if (mysql_errno(&mysql) == CR_IPSOCK_ERROR)
			die(STATE_WARNING, "QUERY %s: %s\n", _("WARNING"), mysql_error(&mysql));
		else if (mysql_errno(&mysql) == CR_SOCKET_CREATE_ERROR)
			die(STATE_WARNING, "QUERY %s: %s\n", _("WARNING"), mysql_error(&mysql));
		else
			die(STATE_CRITICAL, "QUERY %s: %s\n", _("CRITICAL"), mysql_error(&mysql));
	}

	char *error = NULL;
	if (mysql_query(&mysql, sql_query) != 0) {
		error = strdup(mysql_error(&mysql));
		mysql_close(&mysql);
		die(STATE_CRITICAL, "QUERY %s: %s - %s\n", _("CRITICAL"), _("Error with query"), error);
	}

	MYSQL_RES *res;
	/* store the result */
	if ((res = mysql_store_result(&mysql)) == NULL) {
		error = strdup(mysql_error(&mysql));
		mysql_close(&mysql);
		die(STATE_CRITICAL, "QUERY %s: Error with store_result - %s\n", _("CRITICAL"), error);
	}

	/* Check there is some data */
	if (mysql_num_rows(res) == 0) {
		mysql_close(&mysql);
		die(STATE_WARNING, "QUERY %s: %s\n", _("WARNING"), _("No rows returned"));
	}

	MYSQL_ROW row;
	/* fetch the first row */
	if ((row = mysql_fetch_row(res)) == NULL) {
		error = strdup(mysql_error(&mysql));
		mysql_free_result(res);
		mysql_close(&mysql);
		die(STATE_CRITICAL, "QUERY %s: Fetch row error - %s\n", _("CRITICAL"), error);
	}

	if (!is_numeric(row[0])) {
		die(STATE_CRITICAL, "QUERY %s: %s - '%s'\n", _("CRITICAL"), _("Is not a numeric"), row[0]);
	}

	double value = strtod(row[0], NULL);

	/* free the result */
	mysql_free_result(res);

	/* close the connection */
	mysql_close(&mysql);

	if (verbose >= 3)
		printf("mysql result: %f\n", value);

	int status = get_status(value, my_thresholds);

	if (status == STATE_OK) {
		printf("QUERY %s: ", _("OK"));
	} else if (status == STATE_WARNING) {
		printf("QUERY %s: ", _("WARNING"));
	} else if (status == STATE_CRITICAL) {
		printf("QUERY %s: ", _("CRITICAL"));
	}
	printf(_("'%s' returned %f | %s"), sql_query, value,
		   fperfdata("result", value, "", my_thresholds->warning ? true : false, my_thresholds->warning ? my_thresholds->warning->end : 0,
					 my_thresholds->critical ? true : false, my_thresholds->critical ? my_thresholds->critical->end : 0, false, 0, false,
					 0));
	printf("\n");

	return status;
}

/* process command-line arguments */
int process_arguments(int argc, char **argv) {
	static struct option longopts[] = {
		{"hostname", required_argument, 0, 'H'}, {"socket", required_argument, 0, 's'},   {"database", required_argument, 0, 'd'},
		{"username", required_argument, 0, 'u'}, {"password", required_argument, 0, 'p'}, {"file", required_argument, 0, 'f'},
		{"group", required_argument, 0, 'g'},    {"port", required_argument, 0, 'P'},     {"verbose", no_argument, 0, 'v'},
		{"version", no_argument, 0, 'V'},        {"help", no_argument, 0, 'h'},           {"query", required_argument, 0, 'q'},
		{"warning", required_argument, 0, 'w'},  {"critical", required_argument, 0, 'c'}, {0, 0, 0, 0}};

	if (argc < 1)
		return ERROR;

	char *warning = NULL;
	char *critical = NULL;

	while (true) {
		int option = 0;
		int option_char = getopt_long(argc, argv, "hvVP:p:u:d:H:s:q:w:c:f:g:", longopts, &option);

		if (option_char == -1 || option_char == EOF)
			break;

		switch (option_char) {
		case 'H': /* hostname */
			if (is_host(optarg)) {
				db_host = optarg;
			} else {
				usage2(_("Invalid hostname/address"), optarg);
			}
			break;
		case 's': /* socket */
			db_socket = optarg;
			break;
		case 'd': /* database */
			db = optarg;
			break;
		case 'u': /* username */
			db_user = optarg;
			break;
		case 'p': /* authentication information: password */
			db_pass = strdup(optarg);

			/* Delete the password from process list */
			while (*optarg != '\0') {
				*optarg = 'X';
				optarg++;
			}
			break;
		case 'f': /* client options file */
			opt_file = optarg;
			break;
		case 'g': /* client options group */
			opt_group = optarg;
			break;
		case 'P': /* critical time threshold */
			db_port = atoi(optarg);
			break;
		case 'v':
			verbose++;
			break;
		case 'V': /* version */
			print_revision(progname, NP_VERSION);
			exit(STATE_UNKNOWN);
		case 'h': /* help */
			print_help();
			exit(STATE_UNKNOWN);
		case 'q':
			xasprintf(&sql_query, "%s", optarg);
			break;
		case 'w':
			warning = optarg;
			break;
		case 'c':
			critical = optarg;
			break;
		case '?': /* help */
			usage5();
		}
	}

	set_thresholds(&my_thresholds, warning, critical);

	return validate_arguments();
}

int validate_arguments(void) {
	if (sql_query == NULL)
		usage("Must specify a SQL query to run");

	if (db_user == NULL)
		db_user = strdup("");

	if (db_host == NULL)
		db_host = strdup("");

	if (db == NULL)
		db = strdup("");

	return OK;
}

void print_help(void) {
	char *myport;
	xasprintf(&myport, "%d", MYSQL_PORT);

	print_revision(progname, NP_VERSION);

	printf(_(COPYRIGHT), copyright, email);

	printf("%s\n", _("This program checks a query result against threshold levels"));

	printf("\n\n");

	print_usage();

	printf(UT_HELP_VRSN);
	printf(UT_EXTRA_OPTS);
	printf(" -q, --query=STRING\n");
	printf("    %s\n", _("SQL query to run. Only first column in first row will be read"));
	printf(UT_WARN_CRIT_RANGE);
	printf(UT_HOST_PORT, 'P', myport);
	printf(" %s\n", "-s, --socket=STRING");
	printf("    %s\n", _("Use the specified socket (has no effect if -H is used)"));
	printf(" -d, --database=STRING\n");
	printf("    %s\n", _("Database to check"));
	printf(" %s\n", "-f, --file=STRING");
	printf("    %s\n", _("Read from the specified client options file"));
	printf(" %s\n", "-g, --group=STRING");
	printf("    %s\n", _("Use a client options group"));
	printf(" -u, --username=STRING\n");
	printf("    %s\n", _("Username to login with"));
	printf(" -p, --password=STRING\n");
	printf("    %s\n", _("Password to login with"));
	printf("    ==> %s <==\n", _("IMPORTANT: THIS FORM OF AUTHENTICATION IS NOT SECURE!!!"));
	printf("    %s\n", _("Your clear-text password could be visible as a process table entry"));

	printf("\n");
	printf(" %s\n", _("A query is required. The result from the query should be numeric."));
	printf(" %s\n", _("For extra security, create a user with minimal access."));

	printf("\n");
	printf("%s\n", _("Notes:"));
	printf(" %s\n", _("You must specify -p with an empty string to force an empty password,"));
	printf(" %s\n", _("overriding any my.cnf settings."));

	printf(UT_SUPPORT);
}

void print_usage(void) {
	printf("%s\n", _("Usage:"));
	printf(" %s -q SQL_query [-w warn] [-c crit] [-H host] [-P port] [-s socket]\n", progname);
	printf("       [-d database] [-u user] [-p password] [-f optfile] [-g group]\n");
}

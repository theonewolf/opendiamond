#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <string.h>
#include <lib_log.h>
#include <assert.h>
#include "log.h"

#define	MAX_BUFFER	4096


#define	MAX_LEVEL	8
#define	MAX_TYPE	8
#define	MAX_TOKEN	32

typedef struct {
	char	*	key;
	char 	*	descr;
	uint32_t	val;
} flag_ent_t;


flag_ent_t 	level_map[] = {
	{"crit", "critial errors", LOGL_CRIT},
	{"err", "general errors", LOGL_ERR},
	{"info", "General information", LOGL_INFO},
	{"trace", "Trace Information", LOGL_TRACE},
	{"all", "All of the above ", LOGL_ALL},
	{NULL, NULL, 0}
};

flag_ent_t 	src_map[] = {
	{"app", "Application Information ", LOGT_APP},
	{"disk", "object disk system", LOGT_DISK},
	{"filt", "Filter information", LOGT_FILT},
	{"bg", "Host background processing", LOGT_BG},
	{"all", "All of the above", LOGT_ALL},
	{NULL, NULL, 0}
};





/*
 * Convert the level to a human readable string.
 */

void
get_level_string(uint32_t level, char *string, int max)
{


	switch(level) {

		case LOGL_CRIT:
			strncpy(string, "Crit", max);
			string[max-1] = '\0';
			break;

		case LOGL_ERR:
			strncpy(string, "Err", max);
			string[max-1] = '\0';
			break;

		case LOGL_INFO:
			strncpy(string, "Info", max);
			string[max-1] = '\0';
			break;

		case LOGL_TRACE:
			strncpy(string, "Trace", max);
			string[max-1] = '\0';
			break;

		default:
			strncpy(string, "Unknown", max);
			string[max-1] = '\0';
			break;
	}
}

void
get_type_string(uint32_t level, char *string, int max)
{


	switch(level) {

		case LOGT_APP:
			strncpy(string, "App", max);
			string[max-1] = '\0';
			break;

		case LOGT_DISK:
			strncpy(string, "Disk", max);
			string[max-1] = '\0';
			break;

		case LOGT_FILT:
			strncpy(string, "Filt", max);
			string[max-1] = '\0';
			break;

		case LOGT_BG:
			strncpy(string, "Background", max);
			string[max-1] = '\0';
			break;

		default:
			strncpy(string, "Unknown", max);
			string[max-1] = '\0';
			break;
	}
}


void
display_results(log_msg_t *lheader, char *data)
{
	char		source;
	char *		host_id;
	struct in_addr 	iaddr;
	log_ent_t *	log_ent;
	uint32_t	level;
	uint32_t	type;
	uint32_t	dlen;
	char		level_string[MAX_LEVEL];
	char		type_string[MAX_TYPE];
	int		cur_offset, total_len;

	/*
	 * Setup the source of the data.
	 */
	if (lheader->log_type == LOG_SOURCE_BACKGROUND) {
		source = 'H';
	} else {
		source = 'D';
	}


	iaddr.s_addr = lheader->dev_id;

	host_id = inet_ntoa(iaddr);

	total_len = lheader->log_len;
	cur_offset = 0;

	while (cur_offset < total_len) {
		log_ent = (log_ent_t *)&data[cur_offset];


		level = ntohl(log_ent->le_level);	
		type = ntohl(log_ent->le_type);	
		dlen = ntohl(log_ent->le_dlen);
		log_ent->le_data[dlen - 1] = '\0';

		get_level_string(level, level_string, MAX_LEVEL);
		get_type_string(type, type_string, MAX_LEVEL);

		fprintf(stdout, "<%c %s %s %s> %s \n",
			       	source, host_id, level_string, type_string, 
				log_ent->le_data);

		cur_offset += ntohl(log_ent->le_nextoff);

	}




}

void
read_log(int fd)
{
	int	len;
	log_msg_t	lheader;
	char 	*data;

	while (1) {
		len = recv(fd, &lheader, sizeof(lheader), MSG_WAITALL);
		if (len != sizeof(lheader)) {
			return;
		}

		data = (char *)malloc(lheader.log_len);	
		if (data == NULL) {
			perror("Failed malloc \n");
			exit(1);
		}

		len = recv(fd, data, lheader.log_len, MSG_WAITALL);
		if (len != lheader.log_len) {
			return;
		}

		display_results(&lheader, data);

		free(data);
	}
}


static int
table_lookup(char *key, uint32_t * val, flag_ent_t *map)
{
	flag_ent_t *	cur_flag;

	cur_flag = map;
	while (cur_flag->key != NULL) {
		if (strcmp(key, cur_flag->key) == 0) {
			*val |= cur_flag->val;
			return(0);
		}
		cur_flag++;

	}

	return(ENOENT);
}



int
process_arg_string(char *arg_str, uint32_t *data, flag_ent_t *map)
{
	uint32_t	val = 0;
	int		match;
	int		err;
	char *		head;
	char *		delim;
	int		len;
	char *		next_head;
	char		token_data[MAX_TOKEN];

	/*
	 * first try hex conversion.
	 */

	match = sscanf(arg_str, "0x%x", &val);
	if (match == 1) {
		*data = val;
		return(0);
	}

	/*
	 * try to see if this is a lits of items.
	 */

	head = arg_str;
	while ((head != NULL) && (*head != '\0')) {
		delim = index(head, ',');
		if (delim == NULL) {
			len = strlen(head);
			next_head = NULL;
		} else {
			len = delim - head;
			next_head = delim + 1;
		}
		assert(len < MAX_TOKEN);

		strncpy(token_data, head, len);
		token_data[len] = 0;

		err = table_lookup(token_data, &val, map);
		if (err) {
			fprintf(stderr, "uknown level %s \n", token_data);
			exit(1);
		}

		head = next_head;
	}
	*data = val;
	return(0);
}


void
usage()
{
	flag_ent_t *	cur_flag;


	fprintf(stdout, "dlog [-l level_flags] [-s source_flags] [-h] \n");

	fprintf(stdout, "\n");
	fprintf(stdout, "\t -l level_flags \n");
	fprintf(stdout, "\t\t sets the flags on which levels to log.  This \n");
	fprintf(stdout, "\t\t can be a hex value (with leading 0x) or it \n");
	fprintf(stdout, "\t\t be a comma (',') seperated list of symbolic \n");
	fprintf(stdout, "\t\t names,  The supported values are: \n\n");

	cur_flag = level_map;
	while (cur_flag->key != NULL) {
		fprintf(stdout, "\t\t\t %s - %s \n", cur_flag->key, 
				cur_flag->descr);
		cur_flag++;
	}

	fprintf(stdout, "\n");
	fprintf(stdout, "\t -s source_flags \n");
	fprintf(stdout, "\t\t sets flags to indicate which data sources \n");
	fprintf(stdout, "\t\t should be included in the logging.  This can\n");
      	fprintf(stdout, "\t\t be a hex value (with leading 0x) or it \n");
	fprintf(stdout, "\t\t be a comma (',') seperated list of symbolic \n");
	fprintf(stdout, "\t\t names,  The supported values are: \n\n");

	cur_flag = src_map;
	while (cur_flag->key != NULL) {
		fprintf(stdout, "\t\t\t %s - %s \n", cur_flag->key, 
				cur_flag->descr);
		cur_flag++;
	}

	fprintf(stdout, "\n");
	fprintf(stdout, "\t -h show this message \n");
}


void
set_log_flags(int fd, uint32_t level_flags, uint32_t src_flags)
{
	log_set_level_t		log_msg;
	int			len;

	log_msg.log_op = LOG_SETLEVEL_ALL;
	log_msg.log_level = htonl(level_flags);
	log_msg.log_src = htonl(src_flags);
	log_msg.dev_id = 0;


	printf("sending log command \n");
	len = send(fd, &log_msg, sizeof(log_msg), MSG_WAITALL);
	if (len != sizeof(log_msg)) {
		printf("log error  \n");
		if (len == -1) {
			perror("failed to set log flags ");
			exit(1);
		}
		return;
	}
	printf("log done  \n");
}

int
main(int argc, char **argv)
{
	int	fd;
	struct sockaddr_un sa;
	int	err;
	int	c;
	uint32_t	level_flags = (LOGL_ERR|LOGL_CRIT);
	uint32_t	src_flags = LOGT_ALL;
	extern char *	optarg;

	/*
	 * The command line options.
	 */
	while (1) {
		c = getopt(argc, argv, "hl:s:");
		if (c == -1) {
			break;
		}

		switch (c) {
			case 'h':
				usage();
				exit(0);
				break;


			case 'l':
				err = process_arg_string(optarg, &level_flags,
						level_map);
				if (err) {
					usage();
					exit(1);
				}
				break;
	
			case 's':
				err = process_arg_string(optarg, &src_flags,
						src_map);
				if (err) {
					usage();
					exit(1);
				}
				break;

			default:
				printf("unknown option %c\n", c);
				break;
		}
	}
	
	
	/*
	 * This is the main loop.  We just keep trying to open the socket
	 * and the log contents while the socket is valid.
	 *
	 * TODO:  This doesn't work correctly if you have more than
	 * one search going on a machine at the same time.
	 */

	while (1) {
		fd = socket(PF_UNIX, SOCK_STREAM, 0);


		strcpy(sa.sun_path, SOCKET_LOG_NAME);
		sa.sun_family = AF_UNIX;

	
		err = connect(fd, (struct sockaddr *)&sa, sizeof (sa));
		if (err < 0) {
			/*
			 * If the open fails, then nobody is
			 * running, so we sleep for a while
			 * and retry later.
			 */
			sleep(1);
		} else {
			/*
			 * We sucessfully connection, now we set
			 * the parameters we want to log and
			 * read the log.
			 */
			set_log_flags(fd, level_flags, src_flags); 
			read_log(fd);
		}
		close(fd);
	}

	/* we should never get here, but it keeps gcc happy -:) */
	exit(0);
}



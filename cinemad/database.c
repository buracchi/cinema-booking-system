/*	NoSQL Database	*/

#include "database.h"

/*
 *	query language:
 *
 *				SET [KEY] AS [VALUE]
 *				GET [KEY]
 */

#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>

#include "resources.h"
#include "index_table.h"

struct database {
	int fd;
	index_table_t index_table;
};

struct query {
	char key[WORDLEN + 1];
	char value[WORDLEN + 1];
};

int database_read(FILE* strm, char** result);
int database_write(FILE* strm, char* data);
int database_get_stream(FILE** pstrm, int fd);
int get_parsed_query(struct query* parsed_query, const char* query);
int database_get(const database_t handle, struct query parsed_query, char** result);
int database_set(const database_t handle, struct query parsed_query, char** result);

database_t database_init(const char* filename) {
	struct database* database;
	FILE* stream;
	if ((database = malloc(sizeof(struct database))) == NULL) {
		return NULL;
	}
	if ((database->fd = open(filename, O_RDWR, 0666)) == -1) {
		free(database);
		return NULL;
	}
	if (flock(database->fd, LOCK_EX | LOCK_NB) == -1) {	//instead of semget & ftok to avoid mix SysV and POSIX, replace with fcntl
		close(database->fd);
		free(database);
		return NULL;
	}
	if ((database->index_table = index_table_init()) == NULL) {
		close(database->fd);
		free(database);
		return NULL;
	}
	if (database_get_stream(&stream, database->fd)) {
		index_table_destroy(database->index_table);
		close(database->fd);
		free(database);
		return NULL;
	}
	if (index_table_wrlock(database->index_table)) {
		fclose(stream);
		index_table_destroy(database->index_table);
		close(database->fd);
		free(database);
		return NULL;
	}
	if (index_table_update(database->index_table, stream)) {
		fclose(stream);
		index_table_destroy(database->index_table);
		close(database->fd);
		free(database);
		return NULL;
	}
	if (index_table_unlock(database->index_table)) {
		fclose(stream);
		index_table_destroy(database->index_table);
		close(database->fd);
		free(database);
		return NULL;
	}
	if (fclose(stream) == EOF) {
		index_table_destroy(database->index_table);
		close(database->fd);
		free(database);
		return NULL;
	}
	return database;
}

/*	Close database return EOF and set properly errno on error */

int database_close(const database_t handle) {
	struct database* database = (struct database*)handle;
	index_table_destroy(&database->index_table);
	close(database->fd);
	free(database);
	return 0;
}

/*	Execute a query return 1 and set properly errno on error */

int database_execute(const database_t handle, const char* query, char** result) {
	struct database* database = (struct database*)handle;
	int ret = 0;
	struct query parsed_query;
	if (strlen(query) < 5) {
		ret = 1;
	}
	else {
		ret = get_parsed_query(&parsed_query, query + 4);
		if (!ret) {
			if (!strncmp(query, "SET ", 4)) {
				ret = database_set(database, parsed_query, result);
			}
			else if (!strncmp(query, "GET ", 4)) {
				ret = database_get(database, parsed_query, result);
			}
			else {
				*result = strdup(DBMSG_FAIL);
			}
		}
	}
	if (ret) {
		*result = strdup(DBMSG_ERR);
		return 1;
	}
	return 0;
}

/* return 0 on success, 1 on failure */

int database_read(FILE* strm, char** result) {
	for (int i = 0; i < WORDLEN; i++) {
		if ((int)((*result)[i] = (char)fgetc(strm)) == EOF) {
			return 1;
		}
	}
	(*result)[WORDLEN] = 0;
	return 0;
}

/* return 0 on success, 1 on failure */

int database_write(FILE* strm, char* data) {
	for (int i = 0; i < WORDLEN; i++) {
		if (fputc(data[i], strm) == EOF) {
			return 1;
		}
	}
	fflush(strm);
	return 0;
}

/*	return 0 on success, 1 on sys failure	*/

int database_get_stream(FILE** pstrm, int fd) {
	int newfd;
	while ((newfd = dup(fd)) == -1) {
		if (errno != EMFILE) {
			return 1;
		}
		sleep(1);
	}
	if ((*pstrm = fdopen(newfd, "r+")) == NULL) {
		return 1;
	}
	return 0;
}

/* return 0 on success, 1 on failure */

int get_parsed_query(struct query* parsed_query, const char* query) {
	int ret = 0;
	int ntoken = 0;
	char* buff;
	char* saveptr;
	char** token;
	memset(parsed_query->key, 0, WORDLEN + 1);
	memset(parsed_query->value, 0, WORDLEN + 1);
	if ((buff = strdup(query)) == NULL) {
		return 1;
	}
	if ((token = malloc(sizeof(char*))) == NULL) {
		return 1;
	}
	token[0] = strtok_r(buff, " ", &saveptr);
	ntoken++;
	do {
		ntoken++;
		if ((token = realloc(token, sizeof(char*) * (size_t)ntoken)) == NULL) {
			return 1;
		}
		token[ntoken - 1] = strtok_r(NULL, " ", &saveptr);
	} while (token[ntoken - 1] != NULL);
	ntoken--;
	switch (ntoken) {
	case 1:
		if (strlen(token[0]) > WORDLEN) {
			ret = 1;
			break;
		}
		strncpy(parsed_query->key, token[0], WORDLEN + 1);
		break;
	case 3:
		if (strlen(token[0]) > WORDLEN || strlen(token[2]) > WORDLEN || strcmp(token[1], "AS")) {
			ret = 1;
			break;
		}
		strncpy(parsed_query->key, token[0], WORDLEN + 1);
		strncpy(parsed_query->value, token[2], WORDLEN + 1);
		break;
	default:
		ret = 1;
	}
	free(buff);
	free(token);
	if (ret) {
		return 1;
	}
	return 0;
}

int database_get(const database_t handle, struct query parsed_query, char** result) {
	struct database* database = (struct database*)handle;
	int ret;
	FILE* strm;
	long offset;
	pthread_rwlock_t* lock;
	if (database_get_stream(&strm, database->fd)) {
		return 1;
	}
	if (index_table_rdlock(database->index_table)) {
		return 1;
	}
	if ((offset = index_table_key_offset(database->index_table, parsed_query.key)) == -1) {
		*result = strdup(DBMSG_FAIL);
		return 0;
	}
	if (fseek(strm, offset, SEEK_SET) == -1) {
		return 1;
	}
	lock = index_table_key_lock(database->index_table, parsed_query.key);
	if ((*result = malloc(sizeof(char) * (WORDLEN + 1))) == NULL) {
		return 1;
	}
	while ((ret = pthread_rwlock_rdlock(lock)) && errno == EINTR);
	if (ret) {
		free(*result);
		return 1;
	}
	if (database_read(strm, result)) {
		free(*result);
		return 1;
	}
	while ((ret = pthread_rwlock_unlock(lock)) && errno == EINTR);
	if (ret) {
		free(*result);
		return 1;
	}
	if (index_table_unlock(database->index_table)) {
		free(*result);
		return 1;
	}
	if (fclose(strm) == EOF) {
		free(*result);
		return 1;
	}
	return 0;
}

int database_set(const database_t handle, struct query parsed_query, char** result) {
	struct database* database = (struct database*)handle;
	FILE* strm;
	long offset;
	pthread_rwlock_t* lock;
	if (database_get_stream(&strm, database->fd)) {
		return 1;
	}
	if (index_table_rdlock(database->index_table)) {
		return 1;
	}
	if ((offset = index_table_key_offset(database->index_table, parsed_query.key)) == -1) {
		if (index_table_unlock(database->index_table)) {
			return 1;
		}
		if (index_table_wrlock(database->index_table)) {
			return 1;
		}
		if ((offset = index_table_key_offset(database->index_table, parsed_query.key)) == -1) {
			if (fseek(strm, 0, SEEK_END) == -1) {
				return 1;
			}
			if (database_write(strm, parsed_query.key)) {
				return 1;
			}
			if (database_write(strm, parsed_query.value)) {
				return 1;
			}
			if (index_table_update(database->index_table, strm)) {
				return 1;
			}
		}
	}
	if (offset != -1) {
		int ret;
		if (fseek(strm, offset, SEEK_SET) == -1) {
			return 1;
		}
		lock = index_table_key_lock(database->index_table, parsed_query.key);
		while ((ret = pthread_rwlock_wrlock(lock)) && errno == EINTR);
		if (ret) {
			return 1;
		}
		if (database_write(strm, parsed_query.value)) {
			return 1;
		}
		while ((ret = pthread_rwlock_unlock(lock)) && errno == EINTR);
		if (ret) {
			return 1;
		}
	}
	if (index_table_unlock(database->index_table)) {
		return 1;
	}
	if (fclose(strm) == EOF) {
		return 1;
	}
	if ((*result = malloc(sizeof(char) * (WORDLEN + 1))) == NULL) {
		return 1;
	}
	*result = strdup(DBMSG_SUCC);
	return 0;
}
#include "index_table.h"

#include <stdlib.h>
#include <pthread.h>
#include <errno.h>

#include "avl_tree.h"

struct index_table {
	avl_tree_t avl_tree;
	pthread_rwlock_t lock;
};

index_table_t index_table_init(avl_tree_comparison_function* comparison_function) {
	struct index_table* index_table;
	if ((index_table = malloc(sizeof(struct index_table))) == NULL) {
		return NULL;
	}
	if ((index_table->avl_tree = avl_tree_init(comparison_function)) == NULL) {
		free(index_table);
		return NULL;
	}
	int ret;
	while ((ret = pthread_rwlock_init(&index_table->lock, NULL)) && errno == EINTR);
	if (ret) {
		free(index_table);
		return NULL;
	}
	return index_table;
}

int index_table_destroy(index_table_t handle) {
	struct index_table* index_table = (struct index_table*)handle;
	int ret;
	while ((ret = pthread_rwlock_destroy(&index_table->lock)) && errno == EINTR);
	if (ret) {
		return 1;
	}
	if (avl_tree_destroy(index_table->avl_tree)) {
		return 1;
	}
	free(index_table);
	return 0;
}

int index_table_insert(index_table_t handle, const void* key, const void* record) {
	struct index_table* index_table = (struct index_table*)handle;
	int result;
	int ret;
	while ((ret = pthread_rwlock_wrlock(&index_table->lock)) && errno == EINTR);
	if (ret) {
		return 1;
	}
	result = avl_tree_insert(index_table->avl_tree, key, record);
	while ((ret = pthread_rwlock_unlock(&index_table->lock)) && errno == EINTR);
	if (ret) {
		return 1;
	}
	return result;
}

void* index_table_search(index_table_t handle, const void* key) {
	struct index_table* index_table = (struct index_table*)handle;
	void* result;
	int ret;
	while ((ret = pthread_rwlock_rdlock(&index_table->lock)) && errno == EINTR);
	if (ret) {
		return 1;
	}
	result = avl_tree_search(index_table->avl_tree, key);
	while ((ret = pthread_rwlock_unlock(&index_table->lock)) && errno == EINTR);
	if (ret) {
		return 1;
	}
	return result;
}
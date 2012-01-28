#include "drm_idr.h"

void idr_list_init(struct idr *head) {}


int idr_list_get_new_above(struct idr *head,
		void *obj,
		int *handlep)
{
	return 0;
}

void* idr_list_find(struct idr  *head,
		uint32_t	name) {
	return 0;
}

int idr_list_remove(struct idr  *head,
		uint32_t	name) {
	return 0;
}

void idr_list_free(struct idr *head) {}

int idr_list_empty(struct idr *head) {
	return 0;
}

int idr_get_new_above(struct idr *head, void *obj, int start, int *id) {
	return 0;
}

int idr_pre_get(struct idr *head, int flags) {
	return 0;
}

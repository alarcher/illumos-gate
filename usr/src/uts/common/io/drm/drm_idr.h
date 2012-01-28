#ifndef _DRMIDR_H
#define _DRMMP_H

#include <sys/types.h>

struct idr {
	struct idr *next, *prev;
	void *obj;
	uint32_t	handle;
	caddr_t	contain_ptr;
};

extern void idr_list_init(struct idr *head);

extern int idr_list_get_new_above(struct idr *head,
		void *obj,
		int *handlep);

extern void* idr_list_find(struct idr  *head,
		uint32_t	name);

extern int idr_list_remove(struct idr  *head,
		uint32_t	name);

extern void idr_list_free(struct idr *head);

extern int idr_list_empty(struct idr *head);

extern int idr_list_get_new_above(struct idr *head,
			void *obj,
			int *handlep);

extern int idr_get_new_above(struct idr *head, void *obj, int start, int *id);

extern int idr_pre_get(struct idr *head, int flags);
#endif

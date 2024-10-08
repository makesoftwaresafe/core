#ifndef LLIST_H
#define LLIST_H

/* Doubly linked list */
#define DLLIST_PREPEND_FULL(list, item, prev, next) STMT_START { \
	(item)->prev = NULL; \
	(item)->next = *(list); \
	if (*(list) != NULL) (*(list))->prev = (item); \
	*(list) = (item); \
	} STMT_END

#define DLLIST_PREPEND(list, item) \
	DLLIST_PREPEND_FULL(list, item, prev, next)

#define DLLIST_REMOVE_FULL(list, item, prev, next) STMT_START { \
	if ((item)->prev != NULL) \
		(item)->prev->next = (item)->next; \
	else if ((*list) == item) \
		*(list) = (item)->next; \
	if ((item)->next != NULL) { \
		(item)->next->prev = (item)->prev; \
		(item)->next = NULL; \
	} \
	(item)->prev = NULL; \
	} STMT_END

#define DLLIST_REMOVE(list, item) \
	DLLIST_REMOVE_FULL(list, item, prev, next)

/* Doubly linked list with head and tail */
#define DLLIST2_PREPEND_FULL(head, tail, item, prev, next) STMT_START { \
	(item)->prev = NULL; \
	(item)->next = *(head); \
	if (*(head) != NULL) (*(head))->prev = (item); else (*tail) = (item); \
	*(head) = (item); \
	} STMT_END

#define DLLIST2_PREPEND(head, tail, item) \
	DLLIST2_PREPEND_FULL(head, tail, item, prev, next)

#define DLLIST2_APPEND_FULL(head, tail, item, prev, next) STMT_START { \
	(item)->prev = *(tail); \
	(item)->next = NULL; \
	if (*(tail) != NULL) (*(tail))->next = (item); else (*head) = (item); \
	*(tail) = (item); \
	} STMT_END

#define DLLIST2_APPEND(head, tail, item) \
	DLLIST2_APPEND_FULL(head, tail, item, prev, next)

#define DLLIST2_INSERT_AFTER_FULL(head, tail, after, item, prev, next) \
	STMT_START { \
	(item)->prev = (after); \
	(item)->next = (after)->next; \
	if ((after)->next != NULL) \
		(after)->next->prev = (item); \
	(after)->next = (item); \
	if (*(tail) == (after)) \
		*(tail) = (item); \
	} STMT_END

#define DLLIST2_INSERT_AFTER(head, tail, after, item) \
	DLLIST2_INSERT_AFTER_FULL(head, tail, after, item, prev, next)

#define DLLIST2_REMOVE_FULL(head, tail, item, prev, next) STMT_START { \
	if ((item)->prev != NULL) \
		(item)->prev->next = (item)->next; \
	else if (*(head) == item) \
		*(head) = (item)->next; \
	if ((item)->next != NULL) { \
		(item)->next->prev = (item)->prev; \
		(item)->next = NULL; \
	} else if ((*tail) == item) \
		*(tail) = (item)->prev; \
	(item)->prev = NULL; \
	} STMT_END

#define DLLIST2_REMOVE(head, tail, item) \
	DLLIST2_REMOVE_FULL(head, tail, item, prev, next)

#define DLLIST2_JOIN_FULL(head1, tail1, head2, tail2, prev, next) STMT_START { \
	if (*(head1) == NULL) { \
		*(head1) = *(head2); \
		*(tail1) = *(tail2); \
	} else if (*(head2) != NULL) { \
		(*(tail1))->next = *(head2); \
		(*(head2))->prev = *(tail1); \
		(*tail1) = (*tail2); \
	} \
	} STMT_END

#define DLLIST2_JOIN(head1, tail1, head2, tail2) \
	DLLIST2_JOIN_FULL(head1, tail1, head2, tail2, prev, next)

#endif

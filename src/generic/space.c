/*
 * Addess space related routines.
 *
 * Copyright (C) 2008 Bahadir Balban
 */
#include INC_GLUE(memory.h)
#include INC_GLUE(memlayout.h)
#include INC_ARCH(exception.h)
#include INC_SUBARCH(mm.h)
#include <l4/generic/space.h>
#include <l4/generic/container.h>
#include <l4/generic/tcb.h>
#include <l4/api/space.h>
#include <l4/api/errno.h>
#include <l4/api/kip.h>
#include <l4/lib/idpool.h>


void init_address_space_list(struct address_space_list *space_list)
{
	memset(space_list, 0, sizeof(*space_list));

	mutex_init(&space_list->ref_lock);
	spin_lock_init(&space_list->list_lock);
	link_init(&space_list->list);
}

void address_space_reference_lock()
{
	mutex_lock(&curcont->space_list.ref_lock);
}

void address_space_reference_unlock()
{
	mutex_unlock(&curcont->space_list.ref_lock);
}

void address_space_attach(struct ktcb *tcb, struct address_space *space)
{
	tcb->space = space;
	space->ktcb_refs++;
}

struct address_space *address_space_find(l4id_t spid)
{
	struct address_space *space;

	spin_lock(&curcont->space_list.list_lock);
	list_foreach_struct(space, &curcont->space_list.list, list) {
		if (space->spid == spid) {
			spin_unlock(&curcont->space_list.list_lock);
			return space;
		}
	}
	spin_unlock(&curcont->space_list.list_lock);
	return 0;
}

void address_space_add(struct address_space *space)
{
	spin_lock(&curcont->space_list.list_lock);
	BUG_ON(!list_empty(&space->list));
	list_insert(&space->list, &curcont->space_list.list);
	BUG_ON(!++curcont->space_list.count);
	spin_unlock(&curcont->space_list.list_lock);
}

void address_space_remove(struct address_space *space)
{
	spin_lock(&curcont->space_list.list_lock);
	BUG_ON(list_empty(&space->list));
	BUG_ON(--curcont->space_list.count < 0);
	list_remove_init(&space->list);
	spin_unlock(&curcont->space_list.list_lock);
}

/* Assumes address space reflock is already held */
void address_space_delete(struct address_space *space)
{
	BUG_ON(space->ktcb_refs);

	/* Traverse the page tables and delete private pmds */
	delete_page_tables(space);

	/* Return the space id */
	id_del(&kernel_container.space_ids, space->spid);

	/* Deallocate the space structure */
	free_space(space);
}

struct address_space *address_space_create(struct address_space *orig)
{
	struct address_space *space;
	pgd_table_t *pgd;
	int err;

	/* Allocate space structure */
	if (!(space = alloc_space()))
		return PTR_ERR(-ENOMEM);

	/* Allocate pgd */
	if (!(pgd = alloc_pgd())) {
		free_space(space);
		return PTR_ERR(-ENOMEM);
	}

	/* Initialize space structure */
	link_init(&space->list);
	mutex_init(&space->lock);
	space->pgd = pgd;

	/* Copy all kernel entries */
	copy_pgd_kern_all(pgd);

	/*
	 * Set up space id: Always allocate a new one. Specifying a space id
	 * is not allowed since spid field is used to indicate the space to
	 * copy from.
	 */
	space->spid = id_new(&kernel_container.space_ids);

	/* If an original space is supplied */
	if (orig) {
		/* Copy its user entries/tables */
		if ((err = copy_user_tables(space, orig)) < 0) {
			free_pgd(pgd);
			free_space(space);
			return PTR_ERR(err);
		}
	}

	return space;
}

/*
 * FIXME: This does not guarantee that a kernel can access a user pointer.
 * A pager could map an address as requested by the kernel, and unmap it
 * before the kernel has accessed that user address. In order to fix this,
 * per-pte locks (via a bitmap) should be introduced, and map syscalls can
 * check if a pte is locked before going forward with a request.
 */

/*
 * Checks whether the given user address is a valid userspace address.
 * If so, whether it is currently mapped into its own address space.
 * If its not mapped-in, it generates a page-in request to the thread's
 * pager. If fault hasn't cleared, aborts.
 */
int check_access(unsigned long vaddr, unsigned long size, unsigned int flags, int page_in)
{
	int err;
	unsigned long start, end, mapsize;

	/* Do not allow ridiculously big sizes */
	if (size >= USER_AREA_SIZE)
		return -EINVAL;

	/* Get lower and upper page boundaries */
	start = page_align(vaddr);
	end = page_align_up(vaddr + size);
	mapsize = end - start;

	/* Check if the address is mapped with given flags */
	if (!check_mapping(start, mapsize, flags)) {
		/* Is a page in requested? */
		if (page_in) {
			/* Ask pager if paging in is possible */
			if((err = pager_pagein_request(start, mapsize,
						       flags)) < 0)
				return err;
		} else
			return -EFAULT;
	}

	return 0;
}


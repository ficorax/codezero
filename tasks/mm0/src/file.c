/*
 * Copyright (C) 2008 Bahadir Balban
 */
#include <init.h>
#include <vm_area.h>
#include <kmalloc/kmalloc.h>
#include <l4/macros.h>
#include <l4/api/errno.h>
#include <l4lib/types.h>
#include <l4lib/arch/syscalls.h>
#include <l4lib/arch/syslib.h>
#include <l4lib/ipcdefs.h>
#include <l4/api/kip.h>
#include <posix/sys/types.h>
#include <string.h>
#include <file.h>

/* List of all generic files */
LIST_HEAD(vm_file_list);

int vfs_read(unsigned long vnum, unsigned long file_offset,
	     unsigned long npages, void *pagebuf)
{
	int err;

	write_mr(L4SYS_ARG0, vnum);
	write_mr(L4SYS_ARG1, file_offset);
	write_mr(L4SYS_ARG2, npages);
	write_mr(L4SYS_ARG3, (u32)pagebuf);

	if ((err = l4_sendrecv(VFS_TID, VFS_TID, L4_IPC_TAG_PAGER_READ)) < 0) {
		printf("%s: L4 IPC Error: %d.\n", __FUNCTION__, err);
		return err;
	}

	/* Check if syscall was successful */
	if ((err = l4_get_retval()) < 0) {
		printf("%s: Pager from VFS read error: %d.\n", __FUNCTION__, err);
		return err;
	}

	return err;
}

int vfs_write(unsigned long vnum, unsigned long file_offset,
	      unsigned long npages, void *pagebuf)
{
	int err;

	write_mr(L4SYS_ARG0, vnum);
	write_mr(L4SYS_ARG1, file_offset);
	write_mr(L4SYS_ARG2, npages);
	write_mr(L4SYS_ARG3, (u32)pagebuf);

	if ((err = l4_sendrecv(VFS_TID, VFS_TID, L4_IPC_TAG_PAGER_WRITE)) < 0) {
		printf("%s: L4 IPC Error: %d.\n", __FUNCTION__, err);
		return err;
	}

	/* Check if syscall was successful */
	if ((err = l4_get_retval()) < 0) {
		printf("%s: Pager to VFS write error: %d.\n", __FUNCTION__, err);
		return err;
	}

	return err;
}

/*
 * When a new file is opened by the vfs this receives the information
 * about that file so that it can serve that file's content (via
 * read/write/mmap) later to that task.
 */
int vfs_receive_sys_open(l4id_t sender, l4id_t opener, int fd,
			 unsigned long vnum, unsigned long length)
{
	struct vm_file *vmfile;
	struct tcb *t;

	/* Check argument validity */
	if (sender != VFS_TID)
		return -EPERM;

	if (!(t = find_task(opener)))
		return -EINVAL;

	if (fd < 0 || fd > TASK_FILES_MAX)
		return -EINVAL;

	/* Assign vnum to given fd on the task */
	t->fd[fd].vnum = vnum;
	t->fd[fd].cursor = 0;

	/* Check if that vm_file is already in the list */
	list_for_each_entry(vmfile, &vm_file_list, list) {
		/* Check it is a vfs file and if so vnums match. */
		if ((vmfile->type & VM_FILE_VFS) &&
		    vm_file_to_vnum(vmfile) == vnum) {
			/* Add a reference to it from the task */
			t->fd[fd].vmfile = vmfile;
			vmfile->vm_obj.refcnt++;
			return 0;
		}
	}

	/* Otherwise allocate a new one for this vnode */
	if (IS_ERR(vmfile = vfs_file_create()))
		return (int)vmfile;

	/* Initialise and add it to global list */
	vm_file_to_vnum(vmfile) = vnum;
	vmfile->length = length;
	vmfile->vm_obj.pager = &file_pager;
	list_add(&vmfile->vm_obj.list, &vm_file_list);

	return 0;
}


/*
 * Inserts the page to vmfile's list in order of page frame offset.
 * We use an ordered list instead of a radix or btree for now.
 */
int insert_page_olist(struct page *this, struct vm_object *vmo)
{
	struct page *before, *after;

	/* Add if list is empty */
	if (list_empty(&vmo->page_cache)) {
		list_add_tail(&this->list, &vmo->page_cache);
		return 0;
	}
	/* Else find the right interval */
	list_for_each_entry(before, &vmo->page_cache, list) {
		after = list_entry(before->list.next, struct page, list);

		/* If there's only one in list */
		if (before->list.next == &vmo->page_cache) {
			/* Add to end if greater */
			if (this->offset > before->offset)
				list_add_tail(&this->list, &before->list);
			/* Add to beginning if smaller */
			else if (this->offset < before->offset)
				list_add(&this->list, &before->list);
			else
				BUG();
			return 0;
		}

		/* If this page is in-between two other, insert it there */
		if (before->offset < this->offset &&
		    after->offset > this->offset) {
			list_add_tail(&this->list, &before->list);
			return 0;
		}
		BUG_ON(this->offset == before->offset);
		BUG_ON(this->offset == after->offset);
	}
	BUG();
}


/*
 * This reads-in a range of pages from a file and populates the page cache
 * just like a page fault, but its not in the page fault path.
 */
int read_file_pages(struct vm_file *vmfile, unsigned long pfn_start,
		    unsigned long pfn_end)
{
	struct page *page;

	for (int f_offset = pfn_start; f_offset < pfn_end; f_offset++) {
		page = vmfile->vm_obj.pager->ops.page_in(&vmfile->vm_obj,
							 f_offset);
		if (IS_ERR(page)) {
			printf("%s: %s:Could not read page %d "
			       "from file with vnum: 0x%x\n", __TASKNAME__,
			       __FUNCTION__, f_offset, vm_file_to_vnum(vmfile));
			return (int)page;
		}
	}

	return 0;
}

/*
 * Reads a page range from an ordered list of pages into buffer.
 * NOTE: This assumes the page range is consecutively available
 * in the cache. To ensure this, read_file_pages must be called first.
 */
int read_cache_pages(struct vm_file *vmfile, void *buf, unsigned long pfn_start,
		     unsigned long pfn_end, unsigned long offset, int count)
{
	struct page *head, *this;
	int copysize, left;
	void *page_virtual;
	unsigned long last_offset;	/* Last copied page's offset */

	/* Find the head of consecutive pages */
	list_for_each_entry(head, &vmfile->vm_obj.page_cache, list)
		if (head->offset == pfn_start)
			goto copy;

	/* Page not found, nothing read */
	return 0;

copy:
	left = count;

	/* Map the page */
	page_virtual = l4_map_helper((void *)page_to_phys(head), 1);

	/* Copy the first page and unmap. */
	copysize = (left < PAGE_SIZE) ? left : PAGE_SIZE;
	memcpy(buf, page_virtual + offset, copysize);
	left -= copysize;
	l4_unmap_helper(page_virtual, 1);
	last_offset = head->offset;

	/* Map the rest, copy and unmap. */
	list_for_each_entry(this, &head->list, list) {
		if (left == 0 || this->offset == pfn_end)
			break;

		/* Make sure we're advancing on pages consecutively */
		BUG_ON(this->offset != last_offset + 1);

		copysize = (left < PAGE_SIZE) ? left : PAGE_SIZE;
		page_virtual = l4_map_helper((void *)page_to_phys(this), 1);
		memcpy(buf + count - left, page_virtual, copysize);
		l4_unmap_helper(page_virtual, 1);
		left -= copysize;
		last_offset = this->offset;
	}
	BUG_ON(left != 0);

	return count - left;
}

int sys_read(l4id_t sender, int fd, void *buf, int count)
{
	unsigned long pfn_start, pfn_end;
	unsigned long cursor, byte_offset;
	struct vm_file *vmfile;
	struct tcb *t;
	int cnt;
	int err;

	BUG_ON(!(t = find_task(sender)));

	/* TODO: Check user buffer, count and fd validity */
	if (fd < 0 || fd > TASK_FILES_MAX) {
		l4_ipc_return(-EBADF);
		return 0;
	}

	vmfile = t->fd[fd].vmfile;
	cursor = t->fd[fd].cursor;

	/* Start and end pages expected to be read by user */
	pfn_start = __pfn(cursor);
	pfn_end = __pfn(page_align_up(cursor + count));

	/* But we can read up to minimum of file size and expected pfn_end */
	pfn_end = __pfn(vmfile->length) < pfn_end ?
		  __pfn(vmfile->length) : pfn_end;

	/* Read the page range into the cache from file */
	if ((err = read_file_pages(vmfile, pfn_start, pfn_end)) < 0) {
		l4_ipc_return(err);
		return 0;
	}

	/* The offset of cursor on first page */
	byte_offset = PAGE_MASK & cursor;

	/* Read it into the user buffer from the cache */
	if ((cnt = read_cache_pages(vmfile, buf, pfn_start, pfn_end,
				    byte_offset, count)) < 0) {
		l4_ipc_return(cnt);
		return 0;
	}

	/* Update cursor on success */
	t->fd[fd].cursor += cnt;

	return cnt;
}

/* FIXME: Add error handling to this */
/* Extends a file's size by adding it new pages */
int new_file_pages(struct vm_file *f, unsigned long start, unsigned long end)
{
	unsigned long npages = end - start;
	struct page *page;
	void *paddr;

	/* Allocate the memory for new pages */
	if (!(paddr = alloc_page(npages)))
		return -ENOMEM;

	/* Process each page */
	for (unsigned long i = 0; i < npages; i++) {
		page = phys_to_page(paddr + PAGE_SIZE * i);
		page_init(page);
		page->refcnt++;
		page->owner = &f->vm_obj;
		page->offset = start + i;
		page->virtual = 0;

		/* Add the page to file's vm object */
		BUG_ON(!list_empty(&page->list));
		insert_page_olist(page, &f->vm_obj);

	}

	/* Update vm object */
	f->vm_obj.npages += npages;

	return 0;
}

/* Writes user data in buffer into pages in cache */
int write_cache_pages(struct vm_file *vmfile, void *buf, unsigned long pfn_start,
		     unsigned long pfn_end, unsigned long cur_pgoff, int count)
{
	BUG();
	return 0;
}

/*
 * TODO:
 * Page in those writeable pages.
 * Update them,
 * Then page them out.
 *
 * If they're new, fs0 should allocate those pages accordingly.
 */
int sys_write(l4id_t sender, int fd, void *buf, int count)
{
	unsigned long pfn_wstart, pfn_wend;	/* Write start/end */
	unsigned long pfn_fstart, pfn_fend;	/* File start/end */
	unsigned long pfn_nstart, pfn_nend;	/* New pages start/end */
	unsigned long cursor, byte_offset;
	struct vm_file *vmfile;
	struct tcb *t;
	int err;

	BUG_ON(!(t = find_task(sender)));

	/* TODO: Check user buffer, count and fd validity */
	if (fd < 0 || fd > TASK_FILES_MAX) {
		l4_ipc_return(-EBADF);
		return 0;
	}

	vmfile = t->fd[fd].vmfile;
	cursor = t->fd[fd].cursor;

	/* See what pages user wants to write */
	pfn_wstart = __pfn(cursor);
	pfn_wend = __pfn(page_align_up(cursor + count));

	/* Get file start and end pages */
	pfn_fstart = 0;
	pfn_fend = __pfn(page_align_up(vmfile->length));

	/*
	 * Find the intersection to determine which pages are
	 * already part of the file, and which ones are new.
	 */
	if (pfn_wstart < pfn_fend) {
		pfn_fstart = pfn_wstart;

		/*
		 * Shorten the end if end page is
		 * less than file size
		 */
		if (pfn_wend < pfn_fend) {
			pfn_fend = pfn_wend;

			/* This also means no new pages in file */
			pfn_nstart = 0;
			pfn_nend = 0;
		} else {

			/* The new pages start from file end,
			 * and end by write end. */
			pfn_nstart = pfn_fend;
			pfn_nend = pfn_wend;
		}

	} else {
		/* No intersection, its all new pages */
		pfn_fstart = 0;
		pfn_fend = 0;
		pfn_nstart = pfn_wstart;
		pfn_nend = pfn_wend;
	}

	/*
	 * Read in the portion that's already part of the file.
	 */
	if ((err = read_file_pages(vmfile, pfn_fstart, pfn_fend)) < 0) {
		l4_ipc_return(err);
		return 0;
	}

	/* Create new pages for the part that's new in the file */
	if ((err = new_file_pages(vmfile, pfn_nstart, pfn_nend)) < 0) {
		l4_ipc_return(err);
		return 0;
	}

	/*
	 * At this point be it new or existing file pages, all pages
	 * to be written are expected to be in the page cache. Write.
	 */
	byte_offset = PAGE_MASK & cursor;
	if ((err = write_cache_pages(vmfile, buf, pfn_wstart,
				     pfn_wend, byte_offset, count)) < 0) {
		l4_ipc_return(err);
		return 0;
	}

	/*
	 * Update the file size, vfs will be notified of this change
	 * when the file is flushed (e.g. via fflush() or close())
	 */
	vmfile->length += count;

	return 0;
}

/* FIXME: Check for invalid cursor values */
int sys_lseek(l4id_t sender, int fd, off_t offset, int whence)
{
	struct tcb *t;

	BUG_ON(!(t = find_task(sender)));

	if (offset < 0)
		return -EINVAL;

	switch (whence) {
		case SEEK_SET:
			t->fd[fd].cursor = offset;
			break;
		case SEEK_CUR:
			t->fd[fd].cursor += offset;
			break;
		case SEEK_END:
			t->fd[fd].cursor = t->fd[fd].vmfile->length + offset;
			break;
		default:
			return -EINVAL;
	}
	return 0;
}


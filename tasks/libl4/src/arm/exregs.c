/*
 * Generic to arch-specific interface for
 * exchange_registers()
 *
 * Copyright (C) 2008 Bahadir Balban
 */
#include <l4/macros.h>
#include <l4lib/exregs.h>
#include INC_GLUE(message.h)



void exregs_set_mr(struct exregs_data *s, int offset, unsigned long val)
{
	/* Get MR0 */
	u32 *mr = &s->context.MR0_REGISTER;

	/* Sanity check */
	BUG_ON(offset > MR_TOTAL || offset < 0);

	/* Set MR */
	mr[offset] = val;

	/* Set valid bit for mr register */
	s->valid_vect |= FIELD_TO_BIT(exregs_context_t, MR0_REGISTER) << offset;
}

void exregs_set_pager(struct exregs_data *s, l4id_t pagerid)
{
	s->pagerid = pagerid;
	s->flags |= EXREGS_SET_PAGER;
}

void exregs_set_utcb(struct exregs_data *s, unsigned long phys,
		     unsigned long virt)
{
	s->utcb_phys = phys;
	s->utcb_virt = virt;
	s->flags |= EXREGS_SET_UTCB;
}

void exregs_set_stack(struct exregs_data *s, unsigned long sp)
{
	s->context.sp = sp;
	s->valid_vect |= FIELD_TO_BIT(exregs_context_t, sp);
}

void exregs_set_pc(struct exregs_data *s, unsigned long pc)
{
	s->context.pc = pc;
	s->valid_vect |= FIELD_TO_BIT(exregs_context_t, pc);
}


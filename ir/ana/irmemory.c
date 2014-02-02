/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief    Memory disambiguator
 * @author   Michael Beck
 * @date     27.12.2006
 */
#include <stdlib.h>
#include <stdbool.h>

#include "adt/pmap.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "irprog_t.h"
#include "irmemory_t.h"
#include "irmemory.h"
#include "irflag.h"
#include "hashptr.h"
#include "irflag.h"
#include "irouts_t.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "debug.h"
#include "panic.h"
#include "typerep.h"
#include "type_t.h"

/** The debug handle. */
DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)
DEBUG_ONLY(static firm_dbg_module_t *dbgcall = NULL;)

/** The source language specific language disambiguator function. */
static DISAMBIGUATOR_FUNC language_disambuigator = NULL;

/** The global memory disambiguator options. */
static unsigned global_mem_disamgig_opt = aa_opt_no_opt;

const char *get_ir_alias_relation_name(ir_alias_relation rel)
{
#define X(a) case a: return #a
	switch (rel) {
	X(ir_no_alias);
	X(ir_may_alias);
	X(ir_sure_alias);
	}
#undef X
	panic("UNKNOWN alias relation");
}

unsigned get_irg_memory_disambiguator_options(const ir_graph *irg)
{
	unsigned opt = irg->mem_disambig_opt;
	if (opt & aa_opt_inherited)
		return global_mem_disamgig_opt;
	return opt;
}

void set_irg_memory_disambiguator_options(ir_graph *irg, unsigned options)
{
	irg->mem_disambig_opt = options & ~aa_opt_inherited;
}

void set_irp_memory_disambiguator_options(unsigned options)
{
	global_mem_disamgig_opt = options;
}

ir_storage_class_class_t get_base_sc(ir_storage_class_class_t x)
{
	return x & ~ir_sc_modifiers;
}

/**
 * Find the base address and entity of an Sel/Member node.
 *
 * @param node the node
 * @param pEnt after return points to the base entity.
 *
 * @return the base address.
 */
static const ir_node *find_base_addr(const ir_node *node, ir_entity **pEnt)
{
	const ir_node *member = NULL;
	for (;;) {
		if (is_Sel(node)) {
			node = get_Sel_ptr(node);
			continue;
		} else if (is_Member(node)) {
			member = node;
			node   = get_Member_ptr(node);
		} else {
			break;
		}
	}
	if (member != NULL)
		*pEnt = get_Member_entity(member);
	return node;
}

/**
 * Determine the alias relation by checking if addr1 and addr2 are pointer
 * to different type.
 *
 * @param addr1    The first address.
 * @param addr2    The second address.
 */
static ir_alias_relation different_types(const ir_node *addr1,
                                         const ir_node *addr2)
{
	ir_entity *ent1 = NULL;
	if (is_Address(addr1))
		ent1 = get_Address_entity(addr1);
	else if (is_Member(addr1))
		ent1 = get_Member_entity(addr1);

	ir_entity *ent2 = NULL;
	if (is_Address(addr2))
		ent2 = get_Address_entity(addr2);
	else if (is_Member(addr2))
		ent2 = get_Member_entity(addr2);

	if (ent1 != NULL && ent2 != NULL) {
		ir_type *tp1 = get_entity_type(ent1);
		ir_type *tp2 = get_entity_type(ent2);

		if (tp1 != tp2) {
			/* do deref until no pointer types are found */
			while (is_Pointer_type(tp1) && is_Pointer_type(tp2)) {
				tp1 = get_pointer_points_to_type(tp1);
				tp2 = get_pointer_points_to_type(tp2);
			}

			if (get_type_tpop(tp1) != get_type_tpop(tp2)) {
				/* different type structure */
				return ir_no_alias;
			}
			if (is_Class_type(tp1)) {
				/* check class hierarchy */
				if (!is_SubClass_of(tp1, tp2) && !is_SubClass_of(tp2, tp1))
					return ir_no_alias;
			} else {
				/* different types */
				return ir_no_alias;
			}
		}
	}
	return ir_may_alias;
}

/**
 * Returns non-zero if a node is a result on a malloc-like routine.
 *
 * @param node  the Proj node to test
 */
static bool is_malloc_Result(const ir_node *node)
{
	node = get_Proj_pred(node);
	if (!is_Proj(node))
		return false;
	node = get_Proj_pred(node);
	if (!is_Call(node))
		return false;
	ir_entity *callee = get_Call_callee(node);
	return callee != NULL
	    && (get_entity_additional_properties(callee) & mtp_property_malloc);
}

ir_storage_class_class_t classify_pointer(const ir_node *irn,
                                          const ir_entity *ent)
{
	ir_graph                *irg = get_irn_irg(irn);
	ir_storage_class_class_t res = ir_sc_pointer;
	if (is_Address(irn)) {
		ir_entity *entity = get_Address_entity(irn);
		ir_type   *owner  = get_entity_owner(entity);
		res = owner == get_tls_type() ? ir_sc_tls : ir_sc_globalvar;
		if (!(get_entity_usage(entity) & ir_usage_address_taken))
			res |= ir_sc_modifier_nottaken;
	} else if (irn == get_irg_frame(irg)) {
		res = ir_sc_localvar;
		if (ent != NULL && !(get_entity_usage(ent) & ir_usage_address_taken))
			res |= ir_sc_modifier_nottaken;
	} else if (is_Proj(irn) && is_malloc_Result(irn)) {
		return ir_sc_malloced;
	} else if (is_Const(irn)) {
		return ir_sc_globaladdr;
	} else if (is_arg_Proj(irn)) {
		res |= ir_sc_modifier_argument;
	}

	return res;
}

/**
 * Determine the alias relation between two addresses.
 *
 * @param addr1  pointer address of the first memory operation
 * @param type1  the type of the accessed data through addr1
 * @param addr2  pointer address of the second memory operation
 * @param type2  the type of the accessed data through addr2
 *
 * @return found memory relation
 */
static ir_alias_relation _get_alias_relation(
	const ir_node *addr1, const ir_type *const type1,
	const ir_node *addr2, const ir_type *const type2)
{
	if (!get_opt_alias_analysis())
		return ir_may_alias;

	if (addr1 == addr2)
		return ir_sure_alias;

	ir_graph *const irg     = get_irn_irg(addr1);
	unsigned  const options = get_irg_memory_disambiguator_options(irg);

	/* The Armageddon switch */
	if (options & aa_opt_no_alias)
		return ir_no_alias;

	/* do the addresses have constants offsets from the same base?
	 *  Note: sub X, C is normalized to add X, -C
	 */
	long           offset1            = 0;
	long           offset2            = 0;
	const ir_node *sym_offset1        = NULL;
	const ir_node *sym_offset2        = NULL;
	const ir_node *orig_addr1         = addr1;
	const ir_node *orig_addr2         = addr2;
	bool           have_const_offsets = true;

	/*
	 * Currently, only expressions with at most one symbolic
	 * offset can be handled.  To extend this, change
	 * sym_offset{1,2} to be sets, and compare the sets.
	 */

	while (is_Add(addr1)) {
		ir_mode *mode_left = get_irn_mode(get_Add_left(addr1));

		ir_node *ptr_node;
		ir_node *int_node;
		if (mode_is_reference(mode_left)) {
			ptr_node = get_Add_left(addr1);
			int_node = get_Add_right(addr1);
		} else {
			ptr_node = get_Add_right(addr1);
			int_node = get_Add_left(addr1);
		}

		if (is_Const(int_node)) {
			ir_tarval *tv = get_Const_tarval(int_node);
			if (tarval_is_long(tv)) {
				/* TODO: check for overflow */
				offset1 += get_tarval_long(tv);
				goto follow_ptr1;
			}
		}
		if (sym_offset1 == NULL) {
			sym_offset1 = int_node;
		} else {
			// addr1 has more than one symbolic offset.
			// Give up
			have_const_offsets = false;
		}

follow_ptr1:
		addr1 = ptr_node;
	}

	while (is_Add(addr2)) {
		ir_mode *mode_left = get_irn_mode(get_Add_left(addr2));

		ir_node *ptr_node;
		ir_node *int_node;
		if (mode_is_reference(mode_left)) {
			ptr_node = get_Add_left(addr2);
			int_node = get_Add_right(addr2);
		} else {
			ptr_node = get_Add_right(addr2);
			int_node = get_Add_left(addr2);
		}

		if (is_Const(int_node)) {
			ir_tarval *tv = get_Const_tarval(int_node);
			if (tarval_is_long(tv)) {
				/* TODO: check for overflow */
				offset2 += get_tarval_long(tv);
				goto follow_ptr2;
			}
		}
		if (sym_offset2 == NULL) {
			sym_offset2 = int_node;
		} else {
			// addr2 has more than one symbolic offset.
			// Give up
			have_const_offsets = false;
		}

follow_ptr2:
		addr2 = ptr_node;
	}

	unsigned type_size = get_type_size_bytes(type1);
	if (get_type_size_bytes(type2) > type_size) {
		type_size = get_type_size_bytes(type2);
	}

	/* same base address -> compare offsets if possible.
	 * FIXME: type long is not sufficient for this task ...
	 */
	if (addr1 == addr2 && sym_offset1 == sym_offset2 && have_const_offsets) {
		unsigned long first_offset;
		unsigned long last_offset;
		unsigned first_type_size;

		if (offset1 <= offset2) {
			first_offset = offset1;
			last_offset = offset2;
			first_type_size = get_type_size_bytes(type1);
		} else {
			first_offset = offset2;
			last_offset = offset1;
			first_type_size = get_type_size_bytes(type2);
		}

		return first_offset + first_type_size <= last_offset
		     ? ir_no_alias : ir_sure_alias;
	}

	/* skip Sels/Members */
	ir_entity     *ent1  = NULL;
	ir_entity     *ent2  = NULL;
	const ir_node *base1 = find_base_addr(addr1, &ent1);
	const ir_node *base2 = find_base_addr(addr2, &ent2);

	/* same base address -> compare entities */
	if (ent1 != NULL && ent2 != NULL) {
		if (ent1 == ent2)
			return base1 == base2 ? ir_sure_alias : ir_may_alias;
		ir_type *owner1 = get_entity_owner(ent1);
		ir_type *owner2 = get_entity_owner(ent2);
		if (owner1 != owner2) {
			/* TODO: usually selecting different entities from different owners
			 * leads to no alias, but in C we may have a union type where
			 * the first element towards owner1+owner2 and the fields inside
			 * owner1+owner2 are compatible, then they may alias too.
			 * We should detect this case reliably and say no_alias in all
			 * other cases. */
			return ir_may_alias;
		}
		/* same owner, different entities? They may only alias if we have a
		 * union or if we have two bitfield members where the base units
		 * overlap. */
		/* TODO: can we test if the base units actually overlap in the bitfield
		 * case? */
		return is_Union_type(owner1) ||
		    (get_entity_bitfield_size(ent1) > 0
		     || get_entity_bitfield_size(ent2) > 0)
		    ? ir_may_alias : ir_no_alias;
	}

	ir_storage_class_class_t mod1   = classify_pointer(base1, ent1);
	ir_storage_class_class_t mod2   = classify_pointer(base2, ent2);
	ir_storage_class_class_t class1 = get_base_sc(mod1);
	ir_storage_class_class_t class2 = get_base_sc(mod2);

	/* struct-access cannot alias with variables */
	if (ent1 == NULL && ent2 != NULL
	    && (class1 == ir_sc_globalvar || class1 == ir_sc_localvar
	        || class1 == ir_sc_tls || class1 == ir_sc_globaladdr)) {
		return ir_no_alias;
	}
	if (ent2 == NULL && ent1 != NULL
	    && (class2 == ir_sc_globalvar || class2 == ir_sc_localvar
	        || class2 == ir_sc_tls || class2 == ir_sc_globaladdr)) {
		return ir_no_alias;
	}

	if (class1 == ir_sc_pointer || class2 == ir_sc_pointer) {
		/* swap pointer class to class1 */
		if (class2 == ir_sc_pointer) {
			ir_storage_class_class_t temp = mod1;
			mod1 = mod2;
			mod2 = temp;
			class1 = get_base_sc(mod1);
			class2 = get_base_sc(mod2);
		}
		/* a pointer and an object whose address was never taken */
		if (mod2 & ir_sc_modifier_nottaken) {
			return ir_no_alias;
		}
		if (mod1 & ir_sc_modifier_argument) {
			if ( (options & aa_opt_no_alias_args)
					&& (mod2 & ir_sc_modifier_argument))
				return ir_no_alias;
			if ( (options & aa_opt_no_alias_args_global)
					&& (class2 == ir_sc_globalvar
						|| class2 == ir_sc_tls
						|| class2 == ir_sc_globaladdr))
				return ir_no_alias;
		}
	} else if (class1 != class2) {
		/* two objects from different memory spaces */
		return ir_no_alias;
	} else {
		/* both classes are equal */
		if (class1 == ir_sc_globalvar) {
			ir_entity *entity1 = get_Address_entity(base1);
			ir_entity *entity2 = get_Address_entity(base2);
			if (entity1 != entity2)
				return ir_no_alias;

			/* for some reason CSE didn't happen yet for the 2 Addresses... */
			return ir_may_alias;
		} else if (class1 == ir_sc_globaladdr) {
			ir_tarval *tv = get_Const_tarval(base1);
			offset1      += get_tarval_long(tv);
			tv            = get_Const_tarval(base2);
			offset2      += get_tarval_long(tv);

			if ((unsigned long)labs(offset2 - offset1) >= type_size)
				return ir_no_alias;
			else
				return ir_sure_alias;
		} else if (class1 == ir_sc_malloced) {
			return base1 == base2 ? ir_sure_alias : ir_no_alias;
		}
	}

	/* Type based alias analysis */
	if (options & aa_opt_type_based) {
		ir_alias_relation rel;

		if (options & aa_opt_byte_type_may_alias) {
			if (get_type_size_bytes(type1) == 1 || get_type_size_bytes(type2) == 1) {
				/* One of the types address a byte. Assume a ir_may_alias and leave
				   the type based check. */
				goto leave_type_based_alias;
			}
		}

		/* cheap check: If the type sizes did not match, the types MUST be different */
		if (get_type_size_bytes(type1) != get_type_size_bytes(type2))
			return ir_no_alias;

		/* cheap test: if only one is a reference type, no alias */
		if (is_Pointer_type(type1) != is_Pointer_type(type2))
			return ir_no_alias;

		if (is_Primitive_type(type1) && is_Primitive_type(type2)) {
			const ir_mode *const mode1 = get_type_mode(type1);
			const ir_mode *const mode2 = get_type_mode(type2);

			/* cheap test: if arithmetic is different, no alias */
			if (get_mode_arithmetic(mode1) != get_mode_arithmetic(mode2))
				return ir_no_alias;

		}

		rel = different_types(orig_addr1, orig_addr2);
		if (rel != ir_may_alias)
			return rel;
leave_type_based_alias:;
	}

	/* do we have a language specific memory disambiguator? */
	if (language_disambuigator != NULL) {
		ir_alias_relation rel
			= language_disambuigator(orig_addr1, type1, orig_addr2, type2);
		if (rel != ir_may_alias)
			return rel;
	}

	return ir_may_alias;
}

ir_alias_relation get_alias_relation(
	const ir_node *const addr1, const ir_type *const type1,
	const ir_node *const addr2, const ir_type *const type2)
{
	ir_alias_relation rel = _get_alias_relation(addr1, type1, addr2, type2);
	DB((dbg, LEVEL_1, "alias(%+F, %+F) = %s\n", addr1, addr2,
	    get_ir_alias_relation_name(rel)));
	return rel;
}

void set_language_memory_disambiguator(DISAMBIGUATOR_FUNC func)
{
	language_disambuigator = func;
}

/**
 * Check the mode of a Load/Store with the mode of the entity
 * that is accessed.
 * If the mode of the entity and the Load/Store mode do not match, we
 * have the bad reinterpret case:
 *
 * int i;
 * char b = *(char *)&i;
 *
 * We do NOT count this as one value and return address_taken
 * in that case.
 * However, we support an often used case. If the mode is two-complement
 * we allow casts between signed/unsigned.
 *
 * @param mode     the mode of the Load/Store
 * @param ent_mode the mode of the accessed entity
 *
 * @return non-zero if the Load/Store is a hidden cast, zero else
 */
static bool is_hidden_cast(const ir_mode *mode, const ir_mode *ent_mode)
{
	if (ent_mode == NULL)
		return false;

	if (ent_mode != mode &&
		(get_mode_size_bits(ent_mode) != get_mode_size_bits(mode) ||
		 get_mode_arithmetic(ent_mode) != irma_twos_complement ||
		 get_mode_arithmetic(mode) != irma_twos_complement)) {
		return true;
	}
	return false;
}

/**
 * Determine the usage state of a node (or its successor Sels).
 *
 * @param irn  the node
 */
static ir_entity_usage determine_entity_usage(const ir_node *irn,
                                              const ir_entity *entity)
{
	unsigned res = 0;
	foreach_irn_out_r(irn, i, succ) {
		switch (get_irn_opcode(succ)) {
		case iro_Load:
			/* beware: irn might be a Id node here, so irn might be not
			   equal to get_Load_ptr(succ) */
			res |= ir_usage_read;

			/* check if this load is not a hidden conversion */
			ir_mode *mode  = get_Load_mode(succ);
			ir_mode *emode = get_type_mode(get_entity_type(entity));
			if (is_hidden_cast(mode, emode))
				res |= ir_usage_reinterpret_cast;
			break;

		case iro_Store:
			/* check that the node is not the Store's value */
			if (irn == get_Store_value(succ)) {
				res |= ir_usage_unknown;
			}
			if (irn == get_Store_ptr(succ)) {
				res |= ir_usage_write;

				/* check if this Store is not a hidden conversion */
				ir_node *value = get_Store_value(succ);
				ir_mode *mode  = get_irn_mode(value);
				ir_mode *emode = get_type_mode(get_entity_type(entity));
				if (is_hidden_cast(mode, emode))
					res |= ir_usage_reinterpret_cast;
			}
			assert(irn != get_Store_mem(succ));
			break;

		case iro_CopyB: {
			/* CopyB are like Loads/Stores */
			ir_type *tp = get_entity_type(entity);
			if (tp != get_CopyB_type(succ)) {
				/* bad, different types, might be a hidden conversion */
				res |= ir_usage_reinterpret_cast;
			}
			if (irn == get_CopyB_dst(succ)) {
				res |= ir_usage_write;
			} else {
				assert(irn == get_CopyB_src(succ));
				res |= ir_usage_read;
			}
			break;
		}

		case iro_Sel:
		case iro_Add:
		case iro_Sub:
		case iro_Id:
			/* Check the successor of irn. */
			res |= determine_entity_usage(succ, entity);
			break;

		case iro_Member: {
			ir_entity *member_entity = get_Member_entity(succ);
			/* this analysis can't handle unions correctly */
			if (is_Union_type(get_entity_owner(member_entity))) {
				res |= ir_usage_unknown;
				break;
			}
			/* Check the successor of irn. */
			res |= determine_entity_usage(succ, member_entity);
			break;
		}

		case iro_Call:
			if (irn == get_Call_ptr(succ)) {
				/* TODO: we could check for reinterpret casts here...
				 * But I doubt anyone is interested in that bit for
				 * function entities and I'm too lazy to write the code now.
				 */
				res |= ir_usage_read;
			} else {
				assert(irn != get_Call_mem(succ));
				res |= ir_usage_unknown;
			}
			break;

		/* skip tuples */
		case iro_Tuple: {
			for (int input_nr = get_Tuple_n_preds(succ); input_nr-- > 0; ) {
				const ir_node *pred = get_Tuple_pred(succ, input_nr);
				if (pred == irn) {
					/* we found one input */
					foreach_irn_out_r(succ, k, proj) {
						if (is_Proj(proj) && get_Proj_proj(proj) == input_nr) {
							res |= determine_entity_usage(proj, entity);
							break;
						}
					}
				}
			}
			break;
		}

		case iro_Builtin: {
			ir_builtin_kind kind = get_Builtin_kind(succ);
			/* the parameters of the may_alias builtin do not lead to
			 * read/write or address taken. */
			if (kind == ir_bk_may_alias)
				break;
			res |= ir_usage_unknown;
			break;
		}

		default:
			/* another op, we don't know anything (we could do more advanced
			 * things like a dataflow analysis here) */
			res |= ir_usage_unknown;
			break;
		}
	}

	return (ir_entity_usage) res;
}

/**
 * Update the usage flags of all frame entities.
 */
static void analyse_irg_entity_usage(ir_graph *irg)
{
	assure_irg_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_OUTS);

	/* set initial state to not_taken, as this is the "smallest" state */
	ir_type *frame_type = get_irg_frame_type(irg);
	for (size_t i = 0, n = get_class_n_members(frame_type); i < n; ++i) {
		ir_entity *ent = get_class_member(frame_type, i);
		/* methods can only be analyzed globally */
		if (is_method_entity(ent))
			continue;
		ir_entity_usage flags = ir_usage_none;
		if (get_entity_linkage(ent) & IR_LINKAGE_HIDDEN_USER)
			flags = ir_usage_unknown;
		set_entity_usage(ent, flags);
	}

	ir_node *irg_frame = get_irg_frame(irg);

	foreach_irn_out_r(irg_frame, j, succ) {
		if (!is_Member(succ))
			continue;

		ir_entity *entity = get_Member_entity(succ);
		unsigned   flags  = get_entity_usage(entity);
		flags |= determine_entity_usage(succ, entity);
		set_entity_usage(entity, (ir_entity_usage) flags);
	}

	/* check inner functions accessing outer frame */
	int static_link_arg = 0;
	for (size_t i = 0, n = get_class_n_members(frame_type); i < n; ++i) {
		ir_entity *ent = get_class_member(frame_type, i);
		if (!is_method_entity(ent))
			continue;

		ir_graph *inner_irg = get_entity_irg(ent);
		if (inner_irg == NULL)
			continue;

		assure_irg_outs(inner_irg);
		ir_node *args = get_irg_args(inner_irg);
		foreach_irn_out_r(args, j, arg) {
			if (get_Proj_proj(arg) == static_link_arg) {
				foreach_irn_out_r(arg, k, succ) {
					if (is_Member(succ)) {
						ir_entity *entity = get_Member_entity(succ);

						if (get_entity_owner(entity) == frame_type) {
							/* found an access to the outer frame */
							unsigned flags  = get_entity_usage(entity);
							flags |= determine_entity_usage(succ, entity);
							set_entity_usage(entity, (ir_entity_usage) flags);
						}
					}
				}
			}
		}
	}

	/* now computed */
	add_irg_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_ENTITY_USAGE);
}

void assure_irg_entity_usage_computed(ir_graph *irg)
{
	if (irg_has_properties(irg, IR_GRAPH_PROPERTY_CONSISTENT_ENTITY_USAGE))
		return;

	analyse_irg_entity_usage(irg);
}

/**
 * Initialize the entity_usage flag for a global type like type.
 */
static void init_entity_usage(ir_type *tp)
{
	/* We have to be conservative: All external visible entities are unknown */
	for (size_t i = 0, n = get_compound_n_members(tp); i < n; ++i) {
		ir_entity *ent   = get_compound_member(tp, i);
		unsigned   flags = ir_usage_none;

		if (entity_is_externally_visible(ent)) {
			flags |= ir_usage_unknown;
		}
		set_entity_usage(ent, (ir_entity_usage) flags);
	}
}

/**
 * Mark all entities used in the initializer's value as unknown usage.
 *
 * @param value  the value to check
 */
static void check_initializer_value(ir_node *value)
{
	/* Handle each node at most once. */
	if (irn_visited_else_mark(value))
		return;

	/* let's check if it's an address */
	if (is_Address(value)) {
		ir_entity *ent = get_Address_entity(value);
		set_entity_usage(ent, ir_usage_unknown);
	}

	foreach_irn_in(value, i, op) {
		check_initializer_value(op);
	}
}

/**
 * Mark all entities used in the initializer as unknown usage.
 *
 * @param initializer  the initializer to check
 */
static void check_initializer_nodes(ir_initializer_t *initializer)
{
	switch (initializer->kind) {
	case IR_INITIALIZER_CONST: {
		ir_node  *n   = initializer->consti.value;
		ir_graph *irg = get_irn_irg(n);
		ir_reserve_resources(irg, IR_RESOURCE_IRN_VISITED);
		inc_irg_visited(irg);
		check_initializer_value(n);
		ir_free_resources(irg, IR_RESOURCE_IRN_VISITED);
		return;
	}
	case IR_INITIALIZER_TARVAL:
	case IR_INITIALIZER_NULL:
		return;
	case IR_INITIALIZER_COMPOUND:
		for (size_t i = 0; i < initializer->compound.n_initializers; ++i) {
			ir_initializer_t *sub_initializer
				= initializer->compound.initializers[i];
			check_initializer_nodes(sub_initializer);
		}
		return;
	}
	panic("invalid initializer found");
}

/**
 * Mark all entities used in the initializer for the given entity as unknown
 * usage.
 *
 * @param ent  the entity
 */
static void check_initializer(ir_entity *ent)
{
	/* Beware: Methods are always initialized with "themself". This does not
	 * count as a taken address.
	 * TODO: this initialisation with "themself" is wrong and should be removed
	 */
	if (is_Method_type(get_entity_type(ent)))
		return;

	if (ent->initializer != NULL) {
		check_initializer_nodes(ent->initializer);
	}
}


/**
 * Mark all entities used in initializers as unknown usage.
 *
 * @param tp  a compound type
 */
static void check_initializers(ir_type *tp)
{
	for (size_t i = 0, n = get_compound_n_members(tp); i < n; ++i) {
		ir_entity *ent = get_compound_member(tp, i);

		check_initializer(ent);
	}
}

#ifdef DEBUG_libfirm
/**
 * Print the entity usage flags of all entities of a given type for debugging.
 *
 * @param tp  a compound type
 */
static void print_entity_usage_flags(const ir_type *tp)
{
	for (size_t i = 0, n = get_compound_n_members(tp); i < n; ++i) {
		ir_entity      *ent   = get_compound_member(tp, i);
		ir_entity_usage flags = get_entity_usage(ent);

		if (flags == 0)
			continue;
		ir_printf("%+F:", ent);
		if (flags & ir_usage_address_taken)
			printf(" address_taken");
		if (flags & ir_usage_read)
			printf(" read");
		if (flags & ir_usage_write)
			printf(" write");
		if (flags & ir_usage_reinterpret_cast)
			printf(" reinterp_cast");
		printf("\n");
	}
}
#endif /* DEBUG_libfirm */

/**
 * Post-walker: check for global entity address
 */
static void check_global_address(ir_node *irn, void *data)
{
	(void) data;
	if (!is_Address(irn))
		return;

	ir_entity *entity = get_Address_entity(irn);
	unsigned   flags  = get_entity_usage(entity);
	flags |= determine_entity_usage(irn, entity);
	set_entity_usage(entity, (ir_entity_usage) flags);
}

/**
 * Update the entity usage flags of all global entities.
 */
static void analyse_irp_globals_entity_usage(void)
{
	for (ir_segment_t s = IR_SEGMENT_FIRST; s <= IR_SEGMENT_LAST; ++s) {
		ir_type *type = get_segment_type(s);
		init_entity_usage(type);
	}

	for (ir_segment_t s = IR_SEGMENT_FIRST; s <= IR_SEGMENT_LAST; ++s) {
		ir_type *type = get_segment_type(s);
		check_initializers(type);
	}

	foreach_irp_irg(i, irg) {
		assure_irg_outs(irg);
		irg_walk_graph(irg, NULL, check_global_address, NULL);
	}

#ifdef DEBUG_libfirm
	if (firm_dbg_get_mask(dbg) & LEVEL_1) {
		for (ir_segment_t s = IR_SEGMENT_FIRST; s <= IR_SEGMENT_LAST; ++s) {
			print_entity_usage_flags(get_segment_type(s));
		}
	}
#endif /* DEBUG_libfirm */

	/* now computed */
	irp->globals_entity_usage_state = ir_entity_usage_computed;
}

ir_entity_usage_computed_state get_irp_globals_entity_usage_state(void)
{
	return irp->globals_entity_usage_state;
}

void set_irp_globals_entity_usage_state(ir_entity_usage_computed_state state)
{
	irp->globals_entity_usage_state = state;
}

void assure_irp_globals_entity_usage_computed(void)
{
	if (irp->globals_entity_usage_state != ir_entity_usage_not_computed)
		return;

	analyse_irp_globals_entity_usage();
}

void firm_init_memory_disambiguator(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.ana.irmemory");
	FIRM_DBG_REGISTER(dbgcall, "firm.opt.cc");
}

/** Maps method types to cloned method types. */
static pmap *mtp_map;

/**
 * Clone a method type if not already cloned.
 *
 * @param tp  the type to clone
 */
static ir_type *clone_type_and_cache(ir_type *tp)
{
	ir_type *res = pmap_get(ir_type, mtp_map, tp);
	if (res == NULL) {
		res = clone_type_method(tp);
		pmap_insert(mtp_map, tp, res);
	}

	return res;
}

/**
 * Walker: clone all call types of Calls to methods having the
 * mtp_property_private property set.
 */
static void update_calls_to_private(ir_node *call, void *env)
{
	(void)env;
	if (!is_Call(call))
		return;
	ir_entity *callee = get_Call_callee(call);
	if (callee == NULL)
		return;

	ir_type *ctp = get_Call_type(call);
	if ((get_entity_additional_properties(callee) & mtp_property_private)
		&& ((get_method_additional_properties(ctp) & mtp_property_private) == 0)) {
		ctp = clone_type_and_cache(ctp);
		add_method_additional_properties(ctp, mtp_property_private);
		/* clear mismatches in variadicity that can happen in obscure C
		 * programs and break when changing to private calling convention. */
		ir_type *entity_ctp = get_entity_type(callee);
		set_method_variadicity(ctp, get_method_variadicity(entity_ctp));
		set_Call_type(call, ctp);
		DB((dbgcall, LEVEL_1,
		    "changed call to private method %+F using cloned type %+F\n",
		    callee, ctp));
	}
}

void mark_private_methods(void)
{
	assure_irp_globals_entity_usage_computed();
	mtp_map = pmap_create();

	/* first step: change the calling conventions of the local non-escaped entities */
	bool changed = false;
	foreach_irp_irg(i, irg) {
		ir_entity       *ent   = get_irg_entity(irg);
		ir_entity_usage  flags = get_entity_usage(ent);

		if (!(flags & ir_usage_address_taken) && !entity_is_externally_visible(ent)) {
			ir_type *mtp = get_entity_type(ent);

			add_entity_additional_properties(ent, mtp_property_private);
			DB((dbgcall, LEVEL_1, "found private method %+F\n", ent));
			if ((get_method_additional_properties(mtp) & mtp_property_private) == 0) {
				/* need a new type */
				mtp = clone_type_and_cache(mtp);
				add_method_additional_properties(mtp, mtp_property_private);
				set_entity_type(ent, mtp);
				DB((dbgcall, LEVEL_2, "changed entity type of %+F to %+F\n", ent, mtp));
				changed = true;
			}
		}
	}

	if (changed)
		all_irg_walk(NULL, update_calls_to_private, NULL);

	pmap_destroy(mtp_map);
}

/**
 * Find the entity that the given pointer points to.
 *
 * This function returns the entity into which @c ptr points, ignoring
 * any offsets (it assumes that offsets always stay within the
 * entity).
 *
 * This function does *not* always return a top-level entity
 * (i.e. local/global variable), but may also return a member of
 * another entity.
 *
 * If no entity can be found (e.g. pointer is itself result of a
 * Load), NULL is returned.
 */
static ir_entity *find_entity(ir_node *ptr)
{
	switch (get_irn_opcode(ptr)) {
	case iro_Address:
		return get_Address_entity(ptr);
	case iro_Member:
		return get_Member_entity(ptr);
	case iro_Sub:
	case iro_Add: {
		ir_node *left = get_binop_left(ptr);
		if (mode_is_reference(get_irn_mode(left)))
			return find_entity(left);
		ir_node *right = get_binop_right(ptr);
		if (mode_is_reference(get_irn_mode(right)))
			return find_entity(right);
		return NULL;
	}
	default:
		return NULL;
	}
}

/**
 * Returns true, if the entity that the given pointer points to is
 * volatile itself, or if it is part of a larger volatile entity.
 *
 * If no entity can be found (@see find_entity), the functions assumes
 * volatility.
 */
static bool is_inside_volatile_entity(ir_node *ptr)
{
	ir_entity *ent = find_entity(ptr);

	// TODO Probably a pointer, follow the Load(s) to the actual entity
	if (!ent)
		return true;

	if (get_entity_volatility(ent) == volatility_is_volatile) {
		return true;
	}

	if (is_Sel(ptr)) {
		ir_node *sel_ptr = get_Sel_ptr(ptr);
		return is_inside_volatile_entity(sel_ptr);
	} else {
		return false;
	}

}

/**
 * Returns true, if the given type is compound and contains at least
 * one entity which is volatile.
 */
static bool contains_volatile_entity(ir_type *type)
{
	if (!is_compound_type(type))
		return false;

	for (size_t i = 0, n = get_compound_n_members(type); i < n; ++i) {
		ir_entity *ent = get_compound_member(type, i);
		if (get_entity_volatility(ent) == volatility_is_volatile)
			return true;

		ir_type *ent_type = get_entity_type(ent);
		if (contains_volatile_entity(ent_type))
			return true;
	}

	return false;
}

/**
 * Returns true, if the entity that the given pointer points to is...
 * - volatile itself
 * - part of a larger volatile entity
 * - of a type which contains volatile entities.
 *
 * If no entity can be found (@see find_entity), the function assumes
 * volatility.
 */
bool is_partly_volatile(ir_node *ptr)
{
	ir_entity *ent = find_entity(ptr);
	if (!ent)
		return true;

	ir_type *type = get_entity_type(ent);
	return contains_volatile_entity(type) || is_inside_volatile_entity(ptr);
}

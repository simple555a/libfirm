#include <limits.h>

#include "tv.h"
#include "iredges.h"
#include "debug.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "irop_t.h"
#include "irargs_t.h"

#include "../besched.h"

#include "ia32_emitter.h"
#include "gen_ia32_emitter.h"
#include "ia32_nodes_attr.h"
#include "ia32_new_nodes.h"
#include "ia32_map_regs.h"

#define SNPRINTF_BUF_LEN 128

static const arch_env_t *arch_env = NULL;


/*************************************************************
 *             _       _    __   _          _
 *            (_)     | |  / _| | |        | |
 *  _ __  _ __ _ _ __ | |_| |_  | |__   ___| |_ __   ___ _ __
 * | '_ \| '__| | '_ \| __|  _| | '_ \ / _ \ | '_ \ / _ \ '__|
 * | |_) | |  | | | | | |_| |   | | | |  __/ | |_) |  __/ |
 * | .__/|_|  |_|_| |_|\__|_|   |_| |_|\___|_| .__/ \___|_|
 * | |                                       | |
 * |_|                                       |_|
 *************************************************************/

/**
 * Return node's tarval as string.
 */
const char *node_const_to_str(ir_node *n) {
	char   *buf;
	tarval *tv = get_ia32_Immop_tarval(n);

	if (tv) {
		buf = malloc(SNPRINTF_BUF_LEN);
		tarval_snprintf(buf, SNPRINTF_BUF_LEN, tv);
		return buf;
	}
	else if (get_ia32_old_ir(n)) {
		return get_sc_name(get_ia32_old_ir(n));
	}
	else
		return "0";
}

/**
 * Returns node's offset as string.
 */
char *node_offset_to_str(ir_node *n) {
	char   *buf;
	tarval *tv = get_ia32_am_offs(n);

	if (tv) {
		buf = malloc(SNPRINTF_BUF_LEN);
		tarval_snprintf(buf, SNPRINTF_BUF_LEN, tv);
		return buf;
	}
	else
		return "";
}

/* We always pass the ir_node which is a pointer. */
static int ia32_get_arg_type(const lc_arg_occ_t *occ) {
	return lc_arg_type_ptr;
}


/**
 * Returns the register at in position pos.
 */
static const arch_register_t *get_in_reg(ir_node *irn, int pos) {
	ir_node                *op;
	const arch_register_t  *reg = NULL;

	assert(get_irn_arity(irn) > pos && "Invalid IN position");

	/* The out register of the operator at position pos is the
	   in register we need. */
	op = get_irn_n(irn, pos);

	reg = arch_get_irn_register(arch_env, op);

	assert(reg && "no in register found");
	return reg;
}

/**
 * Returns the register at out position pos.
 */
static const arch_register_t *get_out_reg(ir_node *irn, int pos) {
	ir_node                *proj;
	const arch_register_t  *reg = NULL;

	assert(get_irn_n_edges(irn) > pos && "Invalid OUT position");

	/* 1st case: irn is not of mode_T, so it has only                 */
	/*           one OUT register -> good                             */
	/* 2nd case: irn is of mode_T -> collect all Projs and ask the    */
	/*           Proj with the corresponding projnum for the register */

	if (get_irn_mode(irn) != mode_T) {
		reg = arch_get_irn_register(arch_env, irn);
	}
	else if (is_ia32_irn(irn)) {
		reg = get_ia32_out_reg(irn, pos);
	}
	else {
		const ir_edge_t *edge;

		foreach_out_edge(irn, edge) {
			proj = get_edge_src_irn(edge);
			assert(is_Proj(proj) && "non-Proj from mode_T node");
			if (get_Proj_proj(proj) == pos) {
				reg = arch_get_irn_register(arch_env, proj);
				break;
			}
		}
	}

	assert(reg && "no out register found");
	return reg;
}

/**
 * Returns the number of the in register at position pos.
 */
int get_ia32_reg_nr(ir_node *irn, int pos, int in_out) {
	const arch_register_t *reg;
	ir_node               *op;

	if (in_out == 1) {
		/* special case Proj P_fame_base */
		op = get_irn_n(irn, pos);
		if (is_Proj(op) && get_Proj_proj(op) == pn_Start_P_frame_base) {
			return 10;
		}

		reg = get_in_reg(irn, pos);
	}
	else {
		reg = get_out_reg(irn, pos);
	}

	return arch_register_get_index(reg);
}

/**
 * Returns the name of the in register at position pos.
 */
const char *get_ia32_reg_name(ir_node *irn, int pos, int in_out) {
	const arch_register_t *reg;
	ir_node               *op;

	if (in_out == 1) {
		/* special case Proj P_fame_base */
		op = get_irn_n(irn, pos);
		if (is_Proj(op) && get_Proj_proj(op) == pn_Start_P_frame_base) {
			return "x(esp)";
		}
		reg = get_in_reg(irn, pos);
	}
	else {
		reg = get_out_reg(irn, pos);
	}

	return arch_register_get_name(reg);
}

/**
 * Get the register name for a node.
 */
static int ia32_get_reg_name(lc_appendable_t *app,
    const lc_arg_occ_t *occ, const lc_arg_value_t *arg)
{
	const char *buf;
	ir_node    *X  = arg->v_ptr;
	int         nr = occ->width - 1;

	if (!X)
		return lc_arg_append(app, occ, "(null)", 6);

	if (occ->conversion == 'S') {
		buf = get_ia32_reg_name(X, nr, 1);
	}
	else { /* 'D' */
		buf = get_ia32_reg_name(X, nr, 0);
	}

	lc_appendable_chadd(app, '%');
	return lc_arg_append(app, occ, buf, strlen(buf));
}

/**
 * Returns the tarval or offset of an ia32 as a string.
 */
static int ia32_const_to_str(lc_appendable_t *app,
    const lc_arg_occ_t *occ, const lc_arg_value_t *arg)
{
	const char *buf;
	ir_node    *X = arg->v_ptr;

	if (!X)
		return lc_arg_append(app, occ, "(null)", 6);

	if (occ->conversion == 'C') {
		buf = node_const_to_str(X);
	}
	else { /* 'O' */
		buf = node_offset_to_str(X);
	}

	return lc_arg_append(app, occ, buf, strlen(buf));
}

/**
 * Determines the SSE suffix depending on the mode.
 */
static int ia32_get_mode_suffix(lc_appendable_t *app,
    const lc_arg_occ_t *occ, const lc_arg_value_t *arg)
{
	ir_node *X = arg->v_ptr;

	if (!X)
		return lc_arg_append(app, occ, "(null)", 6);

	if (get_mode_size_bits(get_irn_mode(X)) == 32)
		return lc_appendable_chadd(app, 's');
	else
		return lc_appendable_chadd(app, 'd');
}

/**
 * Return the ia32 printf arg environment.
 * We use the firm environment with some additional handlers.
 */
const lc_arg_env_t *ia32_get_arg_env(void) {
	static lc_arg_env_t *env = NULL;

	static const lc_arg_handler_t ia32_reg_handler   = { ia32_get_arg_type, ia32_get_reg_name };
	static const lc_arg_handler_t ia32_const_handler = { ia32_get_arg_type, ia32_const_to_str };
	static const lc_arg_handler_t ia32_mode_handler  = { ia32_get_arg_type, ia32_get_mode_suffix };

	if(env == NULL) {
		/* extend the firm printer */
		env = firm_get_arg_env();
			//lc_arg_new_env();

		lc_arg_register(env, "ia32:sreg", 'S', &ia32_reg_handler);
		lc_arg_register(env, "ia32:dreg", 'D', &ia32_reg_handler);
		lc_arg_register(env, "ia32:cnst", 'C', &ia32_const_handler);
		lc_arg_register(env, "ia32:offs", 'O', &ia32_const_handler);
		lc_arg_register(env, "ia32:mode", 'M', &ia32_mode_handler);
	}

	return env;
}

/**
 * For 2-address code we need to make sure the first src reg is equal to dest reg.
 */
void equalize_dest_src(FILE *F, ir_node *n) {
	if (get_ia32_reg_nr(n, 0, 1) != get_ia32_reg_nr(n, 0, 0)) {
		if (get_irn_arity(n) > 1 && get_ia32_reg_nr(n, 1, 1) == get_ia32_reg_nr(n, 0, 0)) {
			if (! is_op_commutative(get_irn_op(n))) {
				/* we only need to exchange for non-commutative ops */
				lc_efprintf(ia32_get_arg_env(), F, "\txchg %1S, %2S\t\t\t/* xchg src1 <-> src2 for 2 address code */\n", n, n);
			}
		}
		else {
			lc_efprintf(ia32_get_arg_env(), F, "\tmovl %1S, %1D\t\t\t/* src -> dest for 2 address code */\n", n, n);
		}
	}
}

/*
 * Add a number to a prefix. This number will not be used a second time.
 */
char *get_unique_label(char *buf, size_t buflen, const char *prefix) {
	static unsigned long id = 0;
	snprintf(buf, buflen, "%s%lu", prefix, ++id);
	return buf;
}


/*************************************************
 *                 _ _                         _
 *                (_) |                       | |
 *   ___ _ __ ___  _| |_    ___ ___  _ __   __| |
 *  / _ \ '_ ` _ \| | __|  / __/ _ \| '_ \ / _` |
 * |  __/ | | | | | | |_  | (_| (_) | | | | (_| |
 *  \___|_| |_| |_|_|\__|  \___\___/|_| |_|\__,_|
 *
 *************************************************/

/*
 * coding of conditions
 */
struct cmp2conditon_t {
	const char *name;
	pn_Cmp      num;
};

/*
 * positive conditions for signed compares
 */
static const struct cmp2conditon_t cmp2condition_s[] = {
  { NULL,              pn_Cmp_False },  /* always false */
  { "e",               pn_Cmp_Eq },     /* == */
  { "l",               pn_Cmp_Lt },     /* < */
  { "le",              pn_Cmp_Le },     /* <= */
  { "g",               pn_Cmp_Gt },     /* > */
  { "ge",              pn_Cmp_Ge },     /* >= */
  { "ne",              pn_Cmp_Lg },     /* != */
  { "ordered",         pn_Cmp_Leg },    /* Floating point: ordered */
  { "unordered",       pn_Cmp_Uo },     /* FLoting point: unordered */
  { "unordered or ==", pn_Cmp_Ue },     /* Floating point: unordered or == */
  { "unordered or <",  pn_Cmp_Ul },     /* Floating point: unordered or < */
  { "unordered or <=", pn_Cmp_Ule },    /* Floating point: unordered or <= */
  { "unordered or >",  pn_Cmp_Ug },     /* Floating point: unordered or > */
  { "unordered or >=", pn_Cmp_Uge },    /* Floating point: unordered or >= */
  { "unordered or !=", pn_Cmp_Ne },     /* Floating point: unordered or != */
  { NULL,              pn_Cmp_True },   /* always true */
};

/*
 * positive conditions for unsigned compares
 */
static const struct cmp2conditon_t cmp2condition_u[] = {
  { NULL,              pn_Cmp_False },  /* always false */
  { "e",               pn_Cmp_Eq },     /* == */
  { "b",               pn_Cmp_Lt },     /* < */
  { "be",              pn_Cmp_Le },     /* <= */
  { "a",               pn_Cmp_Gt },     /* > */
  { "ae",              pn_Cmp_Ge },     /* >= */
  { "ne",              pn_Cmp_Lg },     /* != */
  { "ordered",         pn_Cmp_Leg },    /* Floating point: ordered */
  { "unordered",       pn_Cmp_Uo },     /* FLoting point: unordered */
  { "unordered or ==", pn_Cmp_Ue },     /* Floating point: unordered or == */
  { "unordered or <",  pn_Cmp_Ul },     /* Floating point: unordered or < */
  { "unordered or <=", pn_Cmp_Ule },    /* Floating point: unordered or <= */
  { "unordered or >",  pn_Cmp_Ug },     /* Floating point: unordered or > */
  { "unordered or >=", pn_Cmp_Uge },    /* Floating point: unordered or >= */
  { "unordered or !=", pn_Cmp_Ne },     /* Floating point: unordered or != */
  { NULL,              pn_Cmp_True },   /* always true */
};

/*
 * returns the condition code
 */
static const char *get_cmp_suffix(int cmp_code, int unsigned_cmp)
{
	assert(cmp2condition_s[cmp_code].num == cmp_code);
	assert(cmp2condition_u[cmp_code].num == cmp_code);

	return unsigned_cmp ? cmp2condition_u[cmp_code & 7].name : cmp2condition_s[cmp_code & 7].name;
}

/**
 * Returns the target label for a control flow node.
 */
static char *get_cfop_target(const ir_node *irn, char *buf) {
	ir_node *bl = get_irn_link(irn);

	snprintf(buf, SNPRINTF_BUF_LEN, "BLOCK_%ld", get_irn_node_nr(bl));
	return buf;
}

/**
 * Emits the jump sequence for a conditional jump (cmp + jmp_true + jmp_false)
 */
static void finish_CondJmp(FILE *F, ir_node *irn) {
	const ir_node   *proj;
	const ir_edge_t *edge;
	char buf[SNPRINTF_BUF_LEN];

	edge = get_irn_out_edge_first(irn);
	proj = get_edge_src_irn(edge);
	assert(is_Proj(proj) && "CondJmp with a non-Proj");

	if (get_Proj_proj(proj) == 1) {
		fprintf(F, "\tj%s %s\t\t\t/* cmp(a, b) == TRUE */\n",
					get_cmp_suffix(get_ia32_pncode(irn), !mode_is_signed(get_irn_mode(get_irn_n(irn, 0)))),
					get_cfop_target(proj, buf));
	}
	else  {
		fprintf(F, "\tjn%s %s\t\t\t/* cmp(a, b) == FALSE */\n",
					get_cmp_suffix(get_ia32_pncode(irn), !mode_is_signed(get_irn_mode(get_irn_n(irn, 0)))),
					get_cfop_target(proj, buf));
	}

	edge = get_irn_out_edge_next(irn, edge);
	if (edge) {
		proj = get_edge_src_irn(edge);
		assert(is_Proj(proj) && "CondJmp with a non-Proj");
		fprintf(F, "\tjmp %s\t\t\t/* otherwise */\n", get_cfop_target(proj, buf));
	}
}

/**
 * Emits code for conditional jump with two variables.
 */
static void emit_ia32_CondJmp(ir_node *irn, emit_env_t *env) {
	FILE *F = env->out;

	lc_efprintf(ia32_get_arg_env(), F, "\tcmp %2S, %1S\t\t\t/* CondJmp(%+F, %+F) */\n", irn, irn,
																	get_irn_n(irn, 0), get_irn_n(irn, 1));
	finish_CondJmp(F, irn);
}

/**
 * Emits code for conditional jump with immediate.
 */
void emit_ia32_CondJmp_i(ir_node *irn, emit_env_t *env) {
	FILE *F = env->out;

	lc_efprintf(ia32_get_arg_env(), F, "\tcmp %C, %1S\t\t\t/* CondJmp_i(%+F) */\n", irn, irn, get_irn_n(irn, 0));
	finish_CondJmp(F, irn);
}



/*********************************************************
 *                 _ _       _
 *                (_) |     (_)
 *   ___ _ __ ___  _| |_     _ _   _ _ __ ___  _ __  ___
 *  / _ \ '_ ` _ \| | __|   | | | | | '_ ` _ \| '_ \/ __|
 * |  __/ | | | | | | |_    | | |_| | | | | | | |_) \__ \
 *  \___|_| |_| |_|_|\__|   | |\__,_|_| |_| |_| .__/|___/
 *                         _/ |               | |
 *                        |__/                |_|
 *********************************************************/

/* jump table entry (target and corresponding number) */
typedef struct _branch_t {
	ir_node *target;
	int      value;
} branch_t;

/* jump table for switch generation */
typedef struct _jmp_tbl_t {
	ir_node  *defProj;         /**< default target */
	int       min_value;       /**< smallest switch case */
	int       max_value;       /**< largest switch case */
	int       num_branches;    /**< number of jumps */
	char     *label;           /**< label of the jump table */
	branch_t *branches;        /**< jump array */
} jmp_tbl_t;

/**
 * Compare two variables of type branch_t. Used to sort all switch cases
 */
static int ia32_cmp_branch_t(const void *a, const void *b) {
	branch_t *b1 = (branch_t *)a;
	branch_t *b2 = (branch_t *)b;

	if (b1->value <= b2->value)
		return -1;
	else
		return 1;
}

/**
 * Emits code for a SwitchJmp (creates a jump table if
 * possible otherwise a cmp-jmp cascade). Port from
 * cggg ia32 backend
 */
void emit_ia32_SwitchJmp(const ir_node *irn, emit_env_t *emit_env) {
	unsigned long       interval;
	char                buf[SNPRINTF_BUF_LEN];
	int                 last_value, i, pn, do_jmp_tbl = 1;
	jmp_tbl_t           tbl;
	ir_node            *proj;
	const ir_edge_t    *edge;
	const lc_arg_env_t *env = ia32_get_arg_env();
	FILE               *F   = emit_env->out;

	/* fill the table structure */
	tbl.label        = malloc(SNPRINTF_BUF_LEN);
	tbl.label        = get_unique_label(tbl.label, SNPRINTF_BUF_LEN, "JMPTBL_");
	tbl.defProj      = NULL;
	tbl.num_branches = get_irn_n_edges(irn);
	tbl.branches     = calloc(tbl.num_branches, sizeof(tbl.branches[0]));
	tbl.min_value    = INT_MAX;
	tbl.max_value    = INT_MIN;

	i = 0;
	/* go over all proj's and collect them */
	foreach_out_edge(irn, edge) {
		proj = get_edge_src_irn(edge);
		assert(is_Proj(proj) && "Only proj allowed at SwitchJmp");

		pn = get_Proj_proj(proj);

		/* create branch entry */
		tbl.branches[i].target = proj;
		tbl.branches[i].value  = pn;

		tbl.min_value = pn < tbl.min_value ? pn : tbl.min_value;
		tbl.max_value = pn > tbl.max_value ? pn : tbl.max_value;

		/* check for default proj */
		if (pn == get_ia32_pncode(irn)) {
			assert(tbl.defProj == NULL && "found two defProjs at SwitchJmp");
			tbl.defProj = proj;
		}

		i++;
	}

	/* sort the branches by their number */
	qsort(tbl.branches, tbl.num_branches, sizeof(tbl.branches[0]), ia32_cmp_branch_t);

	/* two-complement's magic make this work without overflow */
	interval = tbl.max_value - tbl.min_value;

	/* check value interval */
	if (interval > 16 * 1024) {
		do_jmp_tbl = 0;
	}

	/* check ratio of value interval to number of branches */
	if ((float)(interval + 1) / (float)tbl.num_branches > 8.0) {
		do_jmp_tbl = 0;
	}

	if (do_jmp_tbl) {
		/* emit the table */
		if (tbl.min_value != 0) {
			fprintf(F, "\tcmpl %lu, -%d", interval, tbl.min_value);
			lc_efprintf(env, F, "(%1S)\t\t/* first switch value is not 0 */\n", irn);
		}
		else {
			fprintf(F, "\tcmpl %lu, ", interval);
			lc_efprintf(env, F, "%1S\t\t\t/* compare for switch */\n", irn);
		}

		fprintf(F, "\tja %s\t\t\t/* default jump if out of range  */\n", get_cfop_target(tbl.defProj, buf));

		if (tbl.num_branches > 1) {
			/* create table */

			//fprintf(F, "\tjmp *%s", tbl.label);
			lc_efprintf(env, F, "\tjmp *%s(,%1S,4)\t\t/* get jump table entry as target */\n", tbl.label, irn);

			fprintf(F, "\t.section\t.rodata\t\t/* start jump table */\n");
			fprintf(F, "\t.align 4\n");

			fprintf(F, "%s:\n", tbl.label);
			fprintf(F, "\t.long %s\t\t\t/* case %d */\n", get_cfop_target(tbl.branches[0].target, buf), tbl.branches[0].value);

			last_value = tbl.branches[0].value;
			for (i = 1; i < tbl.num_branches; ++i) {
				while (++last_value < tbl.branches[i].value) {
					fprintf(F, "\t.long %s\t\t/* default case */\n", get_cfop_target(tbl.defProj, buf));
				}
				fprintf(F, "\t.long %s\t\t\t/* case %d */\n", get_cfop_target(tbl.branches[i].target, buf), last_value);
			}

			fprintf(F, "\t.text\t\t\t\t/* end of jump table */\n");
		}
		else {
			/* one jump is enough */
			fprintf(F, "\tjmp %s\t\t/* only one case given */\n", get_cfop_target(tbl.branches[0].target, buf));
		}
	}
	else { // no jump table
		for (i = 0; i < tbl.num_branches; ++i) {
			fprintf(F, "\tcmpl %d, ", tbl.branches[i].value);
			lc_efprintf(env, F, "%1S", irn);
			fprintf(F, "\t\t\t/* case %d */\n", tbl.branches[i].value);
			fprintf(F, "\tje %s\n", get_cfop_target(tbl.branches[i].target, buf));
		}

		fprintf(F, "\tjmp %s\t\t\t/* default case */\n", get_cfop_target(tbl.defProj, buf));
	}

	if (tbl.label)
		free(tbl.label);
	if (tbl.branches)
		free(tbl.branches);
}

/**
 * Emits code for a unconditional jump.
 */
void emit_Jmp(ir_node *irn, emit_env_t *env) {
	FILE *F = env->out;

	char buf[SNPRINTF_BUF_LEN];
	ir_fprintf(F, "\tjmp %s\t\t\t/* Jmp(%+F) */\n", get_cfop_target(irn, buf), get_irn_link(irn));
}



/****************************
 *                  _
 *                 (_)
 *  _ __  _ __ ___  _  ___
 * | '_ \| '__/ _ \| |/ __|
 * | |_) | | | (_) | |\__ \
 * | .__/|_|  \___/| ||___/
 * | |            _/ |
 * |_|           |__/
 ****************************/

/**
 * Emits code for a proj -> node
 */
void emit_Proj(ir_node *irn, emit_env_t *env) {
	ir_node *pred = get_Proj_pred(irn);

	if (get_irn_opcode(pred) == iro_Start) {
		switch(get_Proj_proj(irn)) {
			case pn_Start_X_initial_exec:
				emit_Jmp(irn, env);
				break;
			default:
				break;
		}
	}
}



/***********************************************************************************
 *                  _          __                                             _
 *                 (_)        / _|                                           | |
 *  _ __ ___   __ _ _ _ __   | |_ _ __ __ _ _ __ ___   _____      _____  _ __| | __
 * | '_ ` _ \ / _` | | '_ \  |  _| '__/ _` | '_ ` _ \ / _ \ \ /\ / / _ \| '__| |/ /
 * | | | | | | (_| | | | | | | | | | | (_| | | | | | |  __/\ V  V / (_) | |  |   <
 * |_| |_| |_|\__,_|_|_| |_| |_| |_|  \__,_|_| |_| |_|\___| \_/\_/ \___/|_|  |_|\_\
 *
 ***********************************************************************************/

/**
 * Emits code for a node.
 */
void ia32_emit_node(ir_node *irn, void *env) {
	emit_env_t *emit_env   = env;
	firm_dbg_module_t *mod = emit_env->mod;
	FILE              *F   = emit_env->out;

	DBG((mod, LEVEL_1, "emitting code for %+F\n", irn));

#define IA32_EMIT(a) if (is_ia32_##a(irn))               { emit_ia32_##a(irn, emit_env); return; }
#define EMIT(a)      if (get_irn_opcode(irn) == iro_##a) { emit_##a(irn, emit_env); return; }

	/* generated int emitter functions */
	IA32_EMIT(Copy);
	IA32_EMIT(Perm);

	IA32_EMIT(Const);

	IA32_EMIT(Add);
	IA32_EMIT(Add_i);
	IA32_EMIT(Sub);
	IA32_EMIT(Sub_i);
	IA32_EMIT(Minus);
	IA32_EMIT(Inc);
	IA32_EMIT(Dec);

	IA32_EMIT(Max);
	IA32_EMIT(Min);

	IA32_EMIT(And);
	IA32_EMIT(And_i);
	IA32_EMIT(Or);
	IA32_EMIT(Or_i);
	IA32_EMIT(Eor);
	IA32_EMIT(Eor_i);
	IA32_EMIT(Not);

	IA32_EMIT(Shl);
	IA32_EMIT(Shl_i);
	IA32_EMIT(Shr);
	IA32_EMIT(Shr_i);
	IA32_EMIT(Shrs);
	IA32_EMIT(Shrs_i);
	IA32_EMIT(RotL);
	IA32_EMIT(RotL_i);
	IA32_EMIT(RotR);

	IA32_EMIT(Lea);
	IA32_EMIT(Lea_i);

	IA32_EMIT(Mul);
	IA32_EMIT(Mul_i);

	IA32_EMIT(Cltd);
	IA32_EMIT(DivMod);

	IA32_EMIT(Store);
	IA32_EMIT(Load);

	/* generated floating point emitter */
	IA32_EMIT(fConst);

	IA32_EMIT(fAdd);
	IA32_EMIT(fSub);
	IA32_EMIT(fMinus);

	IA32_EMIT(fMul);
	IA32_EMIT(fDiv);

	IA32_EMIT(fMin);
	IA32_EMIT(fMax);

	IA32_EMIT(fLoad);
	IA32_EMIT(fStore);

	/* other emitter functions */
	IA32_EMIT(CondJmp);
	IA32_EMIT(CondJmp_i);
	IA32_EMIT(SwitchJmp);

	EMIT(Jmp);
	EMIT(Proj);

	ir_fprintf(F, "\t\t\t\t\t/* %+F */\n", irn);
}

/**
 * Walks over the nodes in a block connected by scheduling edges
 * and emits code for each node.
 */
void ia32_gen_block(ir_node *block, void *env) {
	ir_node *irn;

	if (! is_Block(block))
		return;

	fprintf(((emit_env_t *)env)->out, "BLOCK_%ld:\n", get_irn_node_nr(block));
	sched_foreach(block, irn) {
		ia32_emit_node(irn, env);
	}
}


/**
 * Emits code for function start.
 */
void ia32_emit_start(FILE *F, ir_graph *irg) {
	const char *irg_name = get_entity_name(get_irg_entity(irg));

	fprintf(F, "\t.text\n");
	fprintf(F, ".globl %s\n", irg_name);
	fprintf(F, "\t.type\t%s, @function\n", irg_name);
	fprintf(F, "%s:\n", irg_name);
}

/**
 * Emits code for function end
 */
void ia32_emit_end(FILE *F, ir_graph *irg) {
	const char *irg_name = get_entity_name(get_irg_entity(irg));

	fprintf(F, "\tret\n");
	fprintf(F, "\t.size\t%s, .-%s\n\n", irg_name, irg_name);
}

/**
 * Sets labels for control flow nodes (jump target)
 * TODO: Jump optimization
 */
void ia32_gen_labels(ir_node *block, void *env) {
	ir_node *pred;
	int n = get_Block_n_cfgpreds(block);

	for (n--; n >= 0; n--) {
		pred = get_Block_cfgpred(block, n);
		set_irn_link(pred, block);
	}
}

/**
 * Main driver
 */
void ia32_gen_routine(FILE *F, ir_graph *irg, const arch_env_t *env) {
	emit_env_t emit_env;

	emit_env.mod      = firm_dbg_register("ir.be.codegen.ia32");
	emit_env.out      = F;
	emit_env.arch_env = env;

	arch_env = env;

	ia32_emit_start(F, irg);
	irg_block_walk_graph(irg, ia32_gen_labels, NULL, &emit_env);
	irg_walk_blkwise_graph(irg, NULL, ia32_gen_block, &emit_env);
	ia32_emit_end(F, irg);
}

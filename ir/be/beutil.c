/**
 * Contains some useful function for the backend.
 * @author Sebastian Hack
 * @cvsid  $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>

#include "pset.h"

#include "irgraph.h"
#include "irgwalk.h"
#include "irdump_t.h"
#include "irdom_t.h"
#include "ircons.h"
#include "iropt.h"
#include "irgopt.h"
#include "irtools.h"
#include "irprintf.h"
#include "iredges.h"

#include "beutil.h"
#include "besched_t.h"
#include "bearch.h"

/* Get an always empty set. */
pset *be_empty_set(void)
{
	static pset *empty_set = NULL;

	if(!empty_set)
		empty_set = pset_new_ptr(1);

	assert(pset_count(empty_set) == 0);
	return empty_set;
}

struct dump_env {
  	FILE *f;
	arch_env_t *env;
};

static void dump_allocated_block(ir_node *block, void *data)
{
	int i, n;
	const ir_node *irn;
	struct dump_env *dump_env = data;
	FILE *f = dump_env->f;
	arch_env_t *env = dump_env->env;

	ir_fprintf(f, "node:{title:\"b%N\"\nlabel:\"", block);
	sched_foreach(block, irn) {
		const char *prefix = "";

		const arch_register_t *reg = arch_get_irn_register(env, irn);

		ir_fprintf(f, "\n");
		if(reg)
			ir_fprintf(f, "%s = ", arch_register_get_name(reg));

		ir_fprintf(f, "%n(", irn);

		if(block != get_irg_start_block(get_irn_irg(block))) {
			for(i = 0, n = get_irn_arity(irn); i < n; ++i) {
				ir_node *op = get_irn_n(irn, i);
				if(arch_is_register_operand(dump_env->env, op, -1)) {
					ir_fprintf(f, "%s%s", prefix,
						arch_register_get_name(arch_get_irn_register(env, op)));
					prefix = ", ";
				}
			}
		}

		ir_fprintf(f, ")");
	}
	ir_fprintf(f, "\"}\n");

	if(get_irg_start_block(get_irn_irg(block)) != block) {
		for(i = 0, n = get_irn_arity(block); i < n; ++i) {
			ir_node *pred_bl = get_nodes_block(get_irn_n(block, i));
			ir_fprintf(f, "edge:{sourcename:\"b%N\" targetname:\"b%N\"}\n", block, pred_bl);
		}
	}
}

void dump_allocated_irg(arch_env_t *arch_env, ir_graph *irg, char *suffix)
{
	char buf[1024];
	struct dump_env env;

	env.env = arch_env;

	ir_snprintf(buf, sizeof(buf), "%F-alloc%s.vcg", irg, suffix);

	if((env.f = fopen(buf, "wt")) != NULL) {
		fprintf(env.f, "graph:{title:\"prg\"\n");
		irg_block_walk_graph(irg, dump_allocated_block, NULL, &env);
		fprintf(env.f, "}\n");
		fclose(env.f);
	}
}

/**
 * Edge hook to dump the schedule edges.
 */
static int sched_edge_hook(FILE *F, ir_node *irn)
{
	if(sched_is_scheduled(irn) && sched_has_prev(irn)) {
		ir_node *prev = sched_prev(irn);
		fprintf(F, "edge:{sourcename:\"");
		PRINT_NODEID(irn);
		fprintf(F, "\" targetname:\"");
		PRINT_NODEID(prev);
		fprintf(F, "\" color:magenta}\n");
	}
	return 1;
}

void dump_ir_block_graph_sched(ir_graph *irg, const char *suffix) {
	DUMP_NODE_EDGE_FUNC old = get_dump_node_edge_hook();

	dump_consts_local(0);
	set_dump_node_edge_hook(sched_edge_hook);
	dump_ir_block_graph(irg, suffix);
	set_dump_node_edge_hook(old);
}

void dump_ir_extblock_graph_sched(ir_graph *irg, const char *suffix) {
	DUMP_NODE_EDGE_FUNC old = get_dump_node_edge_hook();

	dump_consts_local(0);
	set_dump_node_edge_hook(sched_edge_hook);
	dump_ir_extblock_graph(irg, suffix);
	set_dump_node_edge_hook(old);
}

/**
 * Dumps a graph and numbers all dumps.
 * @param irg    The graph
 * @param suffix A suffix to its file name.
 * @param dumper The dump function
 */
void be_dump(ir_graph *irg, const char *suffix, void (*dumper)(ir_graph *, const char *)) {
	static ir_graph *last_irg = NULL;
	static int       nr       = 0;
	char             buf[128];

	if (irg != last_irg) {
		last_irg = irg;
		nr       = strcmp(suffix, "-abi") ? 0 : 1;
	}

	snprintf(buf, sizeof(buf), "-%02d%s", nr++, suffix);
	buf[sizeof(buf) - 1] = '\0';
	dumper(irg, buf);
}



static void collect_phis(ir_node *irn, void *data)
{
  if(is_Phi(irn)) {
    ir_node *bl = get_nodes_block(irn);
    set_irn_link(irn, get_irn_link(bl));
    set_irn_link(bl, irn);
  }
}

void be_clear_links(ir_graph *irg)
{
	irg_walk_graph(irg, firm_clear_link, NULL, NULL);
}

void be_collect_phis(ir_graph *irg)
{
	irg_walk_graph(irg, collect_phis, NULL, NULL);
}

static void count_num_reachable_nodes(ir_node *irn, void *env) {
	int *num = env;
	(*num)++;
}

unsigned get_num_reachable_nodes(ir_graph *irg) {
	int num = 0;
	irg_walk_graph(irg, count_num_reachable_nodes, NULL, &num);
	return num;
}

/**
 * Sets all node inputs to BAD node.
 */
void be_kill_node(ir_node *irn) {
	int      i, first;
	ir_graph *irg;

	if (is_Bad(irn))
		return;

	irg   = get_irn_irg(irn);
	first = 0 - ! is_Block(irn);

	for (i = get_irn_arity(irn) - 1; i >= first; --i) {
		set_irn_n(irn, i, get_irg_bad(irg));
	}
}

/* FIXME: not used. can be deleted? */
ir_node *dom_up_search(pset *accept, ir_node *start_point_exclusive) {
	ir_node *irn, *idom;

	/* search the current block */
	for (irn=sched_prev(start_point_exclusive); irn; irn=sched_prev(irn))
		if (pset_find_ptr(accept, irn))
			return irn;

	/* FIXME: This is obviously buggy: after the first recursive call idom is a block
	   and get_nodes_block will fail.
		 Moreover, why not a simple iteration instead of recursion */
	idom = get_Block_idom(get_nodes_block(start_point_exclusive));

	if (idom)
		return dom_up_search(accept, idom); /* continue search in idom-block */
	else
		return NULL; /* this was the start block and we did not find an acceptable irn */
}

/**
 * Gets the Proj with number pn from irn.
 */
ir_node *be_get_Proj_for_pn(const ir_node *irn, long pn) {
	const ir_edge_t *edge;
	ir_node         *proj;
	assert(get_irn_mode(irn) == mode_T && "need mode_T");

	foreach_out_edge(irn, edge) {
		proj = get_edge_src_irn(edge);

		if (get_Proj_proj(proj) == pn)
			return proj;
	}

	return NULL;
}

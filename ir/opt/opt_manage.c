#include "config.h"

#include <assert.h>
#include <stdbool.h>

#include "irgraph_t.h"
#include "irprog_t.h"
#include "irnode.h"
#include "iroptimize.h"
#include "irgopt.h"
#include "irdom.h"
#include "iredges.h"
#include "irouts.h"
#include "irverify.h"
#include "irdump.h"
#include "opt_manage.h"

static void nop(ir_graph *irg) {
	(void)irg;
}

void perform_irg_optimization(ir_graph *irg, optdesc_t *opt)
{
	ir_graph_properties_t new_irg_state;
	ir_graph_properties_t required = opt->requirements;
	const bool dump = get_irp_optimization_dumps();

	/* It does not make sense to require both: */
	assert (!((required & IR_GRAPH_PROPERTY_ONE_RETURN) && (required & IR_GRAPH_PROPERTY_MANY_RETURNS)));

	/* assure that all requirements for the optimization are fulfilled */
#define PREPARE(property,func) if (property & (required ^ irg->properties)) {func(irg); add_irg_properties(irg,property);}
	PREPARE(IR_GRAPH_PROPERTY_ONE_RETURN,               normalize_one_return)
	PREPARE(IR_GRAPH_PROPERTY_MANY_RETURNS,             normalize_n_returns)
	PREPARE(IR_GRAPH_PROPERTY_NO_CRITICAL_EDGES,        remove_critical_cf_edges)
	PREPARE(IR_GRAPH_PROPERTY_NO_UNREACHABLE_CODE,      remove_unreachable_code)
	PREPARE(IR_GRAPH_PROPERTY_NO_BADS,                  remove_bads)
	PREPARE(IR_GRAPH_PROPERTY_CONSISTENT_DOMINANCE,     assure_doms)
	PREPARE(IR_GRAPH_PROPERTY_CONSISTENT_POSTDOMINANCE, assure_postdoms)
	PREPARE(IR_GRAPH_PROPERTY_CONSISTENT_OUT_EDGES,     assure_edges)
	PREPARE(IR_GRAPH_PROPERTY_CONSISTENT_OUTS,          assure_irg_outs)
	PREPARE(IR_GRAPH_PROPERTY_CONSISTENT_LOOPINFO,      assure_loopinfo)
	PREPARE(IR_GRAPH_PROPERTY_CONSISTENT_ENTITY_USAGE,  assure_irg_entity_usage_computed)

	/* now all the requirements for the optimization are fulfilled */
	if (dump)
		dump_ir_graph(irg, opt->name);

	new_irg_state = opt->optimization(irg);

	if (dump)
		dump_ir_graph(irg, opt->name);

	/* unless the optimization returned that some state is retained,
	 * we disable the corresponding irg state.
	 * Since we currently duplicate information, sometimes another func must be called too.
	 */
#define INVALIDATE(property,func) if (!(property & new_irg_state)) {clear_irg_properties(irg,property); func(irg);}
	INVALIDATE(IR_GRAPH_PROPERTY_NO_CRITICAL_EDGES,        nop)
	INVALIDATE(IR_GRAPH_PROPERTY_NO_UNREACHABLE_CODE,      nop)
	INVALIDATE(IR_GRAPH_PROPERTY_NO_BADS,                  nop)
	INVALIDATE(IR_GRAPH_PROPERTY_ONE_RETURN,               nop)
	INVALIDATE(IR_GRAPH_PROPERTY_MANY_RETURNS,             nop)
	INVALIDATE(IR_GRAPH_PROPERTY_CONSISTENT_DOMINANCE,     nop)
	INVALIDATE(IR_GRAPH_PROPERTY_CONSISTENT_POSTDOMINANCE, nop)
	INVALIDATE(IR_GRAPH_PROPERTY_CONSISTENT_OUTS,          nop)
	INVALIDATE(IR_GRAPH_PROPERTY_CONSISTENT_OUT_EDGES,     edges_deactivate)
	INVALIDATE(IR_GRAPH_PROPERTY_CONSISTENT_LOOPINFO,      nop)
	INVALIDATE(IR_GRAPH_PROPERTY_CONSISTENT_ENTITY_USAGE,  nop)

	remove_End_Bads_and_doublets(get_irg_end(irg));

	irg_verify(irg, VERIFY_ENFORCE_SSA);
}

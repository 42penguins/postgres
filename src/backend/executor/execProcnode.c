/*-------------------------------------------------------------------------
 *
 * execProcnode.c
 *	 contains dispatch functions which call the appropriate "initialize",
 *	 "get a tuple", and "cleanup" routines for the given node type.
 *	 If the node has children, then it will presumably call ExecInitNode,
 *	 ExecProcNode, or ExecEndNode on its subnodes and do the appropriate
 *	 processing.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execProcnode.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecInitNode	-		initialize a plan node and its subplans
 *		ExecProcNode	-		get a tuple by executing the plan node
 *		ExecEndNode		-		shut down a plan node and its subplans
 *
 *	 NOTES
 *		This used to be three files.  It is now all combined into
 *		one file so that it is easier to keep ExecInitNode, ExecProcNode,
 *		and ExecEndNode in sync when new nodes are added.
 *
 *	 EXAMPLE
 *		Suppose we want the age of the manager of the shoe department and
 *		the number of employees in that department.  So we have the query:
 *
 *				select DEPT.no_emps, EMP.age
 *				from DEPT, EMP
 *				where EMP.name = DEPT.mgr and
 *					  DEPT.name = "shoe"
 *
 *		Suppose the planner gives us the following plan:
 *
 *						Nest Loop (DEPT.mgr = EMP.name)
 *						/		\
 *					   /		 \
 *				   Seq Scan		Seq Scan
 *					DEPT		  EMP
 *				(name = "shoe")
 *
 *		ExecutorStart() is called first.
 *		It calls InitPlan() which calls ExecInitNode() on
 *		the root of the plan -- the nest loop node.
 *
 *	  * ExecInitNode() notices that it is looking at a nest loop and
 *		as the code below demonstrates, it calls ExecInitNestLoop().
 *		Eventually this calls ExecInitNode() on the right and left subplans
 *		and so forth until the entire plan is initialized.	The result
 *		of ExecInitNode() is a plan state tree built with the same structure
 *		as the underlying plan tree.
 *
 *	  * Then when ExecutorRun() is called, it calls ExecutePlan() which calls
 *		ExecProcNode() repeatedly on the top node of the plan state tree.
 *		Each time this happens, ExecProcNode() will end up calling
 *		ExecNestLoop(), which calls ExecProcNode() on its subplans.
 *		Each of these subplans is a sequential scan so ExecSeqScan() is
 *		called.  The slots returned by ExecSeqScan() may contain
 *		tuples which contain the attributes ExecNestLoop() uses to
 *		form the tuples it returns.
 *
 *	  * Eventually ExecSeqScan() stops returning tuples and the nest
 *		loop join ends.  Lastly, ExecutorEnd() calls ExecEndNode() which
 *		calls ExecEndNestLoop() which in turn calls ExecEndNode() on
 *		its subplans which result in ExecEndSeqScan().
 *
 *		This should show how the executor works by having
 *		ExecInitNode(), ExecProcNode() and ExecEndNode() dispatch
 *		their work to the appopriate node support routines which may
 *		in turn call these routines themselves on their subplans.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeAppend.h"
#include "executor/nodeBitmapAnd.h"
#include "executor/nodeBitmapHeapscan.h"
#include "executor/nodeBitmapIndexscan.h"
#include "executor/nodeBitmapOr.h"
#include "executor/nodeCtescan.h"
#include "executor/nodeForeignscan.h"
#include "executor/nodeFunctionscan.h"
#include "executor/nodeGroup.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/nodeIndexonlyscan.h"
#include "executor/nodeIndexscan.h"
#include "executor/nodeLimit.h"
#include "executor/nodeLockRows.h"
#include "executor/nodeMaterial.h"
#include "executor/nodeMergeAppend.h"
#include "executor/nodeMergejoin.h"
#include "executor/nodeModifyTable.h"
#include "executor/nodeNestloop.h"
#include "executor/nodeRecursiveunion.h"
#include "executor/nodeResult.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeSetOp.h"
#include "executor/nodeSort.h"
#include "executor/nodeSubplan.h"
#include "executor/nodeSubqueryscan.h"
#include "executor/nodeTidscan.h"
#include "executor/nodeUnique.h"
#include "executor/nodeValuesscan.h"
#include "executor/nodeWindowAgg.h"
#include "executor/nodeWorktablescan.h"
#include "miscadmin.h"

#include "utils/hsearch.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "nodes/pg_list.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "access/htup_details.h"
#include "piggyback/piggyback.h"
#include <limits.h>

extern Piggyback *piggyback;
void buildTwoColumnCombinations(char* valueToConcat, int from, TupleTableSlot *result);
void addToTwoColumnCombinationHashSet(int from, char* valueToConcat, int to, char* value);
void LookForFilterWithEquality(PlanState* result, Oid tableOid, List* qual);

/* ------------------------------------------------------------------------
 *		ExecInitNode
 *
 *		Recursively initializes all the nodes in the plan tree rooted
 *		at 'node'.
 *
 *		Inputs:
 *		  'node' is the current node of the plan produced by the query planner
 *		  'estate' is the shared execution state for the plan tree
 *		  'eflags' is a bitwise OR of flag bits described in executor.h
 *
 *		Returns a PlanState node corresponding to the given Plan node.
 * ------------------------------------------------------------------------
 */
PlanState *
ExecInitNode(Plan *node, EState *estate, int eflags) {
	PlanState *result;
	List *subps;
	ListCell *l;

	// Pointers that are necessary for specific node types like SeqScan.
	SeqScanState* resultAsScanState;
	IndexScanState* resultAsIndexScan;
	IndexOnlyScanState* resultAsIndexOnlyScan;
	int tableOid = -1;
	/*
	 * do nothing when we get to the end of a leaf on tree.
	 */
	if (node == NULL)
		return NULL;

	switch (nodeTag(node)) {
	/*
	 * control nodes
	 */
	case T_Result:
		result = (PlanState *) ExecInitResult((Result *) node, estate, eflags);
		break;

	case T_ModifyTable:
		result = (PlanState *) ExecInitModifyTable((ModifyTable *) node, estate,
				eflags);
		break;

	case T_Append:
		result = (PlanState *) ExecInitAppend((Append *) node, estate, eflags);
		break;

	case T_MergeAppend:
		result = (PlanState *) ExecInitMergeAppend((MergeAppend *) node, estate,
				eflags);
		break;

	case T_RecursiveUnion:
		result = (PlanState *) ExecInitRecursiveUnion((RecursiveUnion *) node,
				estate, eflags);
		break;

	case T_BitmapAnd:
		result = (PlanState *) ExecInitBitmapAnd((BitmapAnd *) node, estate,
				eflags);
		break;

	case T_BitmapOr:
		result = (PlanState *) ExecInitBitmapOr((BitmapOr *) node, estate,
				eflags);
		break;

		/*
		 * scan nodes
		 */
	case T_SeqScan:
		resultAsScanState = ExecInitSeqScan((SeqScan *) node, estate,
				eflags);
		result = (PlanState *) resultAsScanState;

		if (resultAsScanState)
		{
			tableOid = resultAsScanState->ss_currentRelation->rd_id;
		}

		if (tableOid != -1)
		{
			LookForFilterWithEquality(result, tableOid, result->qual);
		}
		break;

	case T_IndexScan:
		resultAsIndexScan = ExecInitIndexScan((IndexScan *) node, estate,
				eflags);
		result = (PlanState *) resultAsIndexScan;

		if (resultAsIndexScan)
		{
			tableOid = resultAsIndexScan->ss.ss_currentRelation->rd_id;
		}

		if (tableOid != -1)
		{
			LookForFilterWithEquality(result, tableOid, resultAsIndexScan->indexqualorig);
		}
		break;

	// TODO: search for examples for IndexOnlyScan and test this case (examples on https://wiki.postgresql.org/wiki/Index-only_scans)
	case T_IndexOnlyScan:
		resultAsIndexOnlyScan = ExecInitIndexOnlyScan((IndexOnlyScan *) node, estate,
				eflags);
		result = (PlanState *) resultAsIndexOnlyScan;

		if (resultAsIndexOnlyScan)
		{
			tableOid = resultAsIndexOnlyScan->ss.ss_currentRelation->rd_id;
		}

		if (tableOid != -1)
		{
			LookForFilterWithEquality(result, tableOid, resultAsIndexOnlyScan->indexqual);
		}
		break;

	case T_BitmapIndexScan:
		result = (PlanState *) ExecInitBitmapIndexScan((BitmapIndexScan *) node,
				estate, eflags);
		break;

	case T_BitmapHeapScan:
		result = (PlanState *) ExecInitBitmapHeapScan((BitmapHeapScan *) node,
				estate, eflags);
		break;

	case T_TidScan:
		result = (PlanState *) ExecInitTidScan((TidScan *) node, estate,
				eflags);
		break;

	case T_SubqueryScan:
		result = (PlanState *) ExecInitSubqueryScan((SubqueryScan *) node,
				estate, eflags);
		break;

	case T_FunctionScan:
		result = (PlanState *) ExecInitFunctionScan((FunctionScan *) node,
				estate, eflags);
		break;

	case T_ValuesScan:
		result = (PlanState *) ExecInitValuesScan((ValuesScan *) node, estate,
				eflags);
		break;

	case T_CteScan:
		result = (PlanState *) ExecInitCteScan((CteScan *) node, estate,
				eflags);
		break;

	case T_WorkTableScan:
		result = (PlanState *) ExecInitWorkTableScan((WorkTableScan *) node,
				estate, eflags);
		break;

	case T_ForeignScan:
		result = (PlanState *) ExecInitForeignScan((ForeignScan *) node, estate,
				eflags);
		break;

		/*
		 * join nodes
		 */
	case T_NestLoop:
		result = (PlanState *) ExecInitNestLoop((NestLoop *) node, estate,
				eflags);
		break;

	case T_MergeJoin:
		result = (PlanState *) ExecInitMergeJoin((MergeJoin *) node, estate,
				eflags);
		break;

	case T_HashJoin:
		result = (PlanState *) ExecInitHashJoin((HashJoin *) node, estate,
				eflags);
		break;

		/*
		 * materialization nodes
		 */
	case T_Material:
		result = (PlanState *) ExecInitMaterial((Material *) node, estate,
				eflags);
		break;

	case T_Sort:
		result = (PlanState *) ExecInitSort((Sort *) node, estate, eflags);
		break;

	case T_Group:
		result = (PlanState *) ExecInitGroup((Group *) node, estate, eflags);
		break;

	case T_Agg:
		result = ExecInitAgg((Agg *) node, estate, eflags);
		break;

	case T_WindowAgg:
		result = (PlanState *) ExecInitWindowAgg((WindowAgg *) node, estate,
				eflags);
		break;

	case T_Unique:
		result = (PlanState *) ExecInitUnique((Unique *) node, estate, eflags);
		break;

	case T_Hash:
		result = (PlanState *) ExecInitHash((Hash *) node, estate, eflags);
		break;

	case T_SetOp:
		result = (PlanState *) ExecInitSetOp((SetOp *) node, estate, eflags);
		break;

	case T_LockRows:
		result = (PlanState *) ExecInitLockRows((LockRows *) node, estate,
				eflags);
		break;

	case T_Limit:
		result = (PlanState *) ExecInitLimit((Limit *) node, estate, eflags);
		break;

	default:
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
		result = NULL; /* keep compiler quiet */
		break;
	}

	/*
	 * Initialize any initPlans present in this node.  The planner put them in
	 * a separate list for us.
	 */
	subps = NIL;
	foreach(l, node->initPlan)
	{
		SubPlan *subplan = (SubPlan *) lfirst(l);
		SubPlanState *sstate;

		Assert(IsA(subplan, SubPlan));
		sstate = ExecInitSubPlan(subplan, result);
		subps = lappend(subps, sstate);
	}
	result->initPlan = subps;

	/* Set up instrumentation for this node if requested */
	if (estate->es_instrument)
		result->instrument = InstrAlloc(1, estate->es_instrument);

	return result;
}

void
LookForFilterWithEquality(PlanState* result, Oid tableOid, List* qual)
{
	if (qual) {
		int opno = ((OpExpr*) ((ExprState*) linitial(qual))->expr)->opno;
		int columnId = ((Var*) ((OpExpr*) ((ExprState*) linitial(qual))->expr)->args->head->data.ptr_value)->varattno;
		be_PGAttDesc *columnData = (be_PGAttDesc*) malloc(sizeof(be_PGAttDesc));
		columnData->srccolumnid = columnId;

		// TODO: write this in a method that returns i for better readability
		int i = 0;
		for (; i < piggyback->numberOfAttributes; i++)
		{
			if (tableOid == piggyback->resultStatistics->columnStatistics[i].columnDescriptor->srctableid
					&& columnData->srccolumnid == piggyback->resultStatistics->columnStatistics[i].columnDescriptor->srccolumnid)
				break;
		}

		// TODO: if (i < piggyback->numberOfAttributes) set useBaseStatistics to false

		if(opno == 94 || opno == 96 || opno == 410 || opno == 416 || opno == 1862 || opno == 1868 || opno == 15 || opno == 532 || opno == 533) { // it is a equality like number_of_tracks = 3
			int numberOfAttributes = result->plan->targetlist->length;

			int *minAndMaxAndAvg = (int*) malloc(sizeof(int));
			minAndMaxAndAvg = &(((Const*) ((OpExpr*) ((ExprState*) linitial(qual))->expr)->args->tail->data.ptr_value)->constvalue);

			// we always set the type to 8byte-integer because we don't need a detailed differentiation
			columnData->typid = 20;

			// only write values, if the selected field is part of the result table
			if (i < piggyback->numberOfAttributes)
			{
				piggyback->resultStatistics->columnStatistics[i].columnDescriptor = columnData;
				piggyback->resultStatistics->columnStatistics[i].isNumeric = 1;
				piggyback->resultStatistics->columnStatistics[i].maxValue = minAndMaxAndAvg;
				piggyback->resultStatistics->columnStatistics[i].minValue = minAndMaxAndAvg;
				piggyback->resultStatistics->columnStatistics[i].mostFrequentValue = minAndMaxAndAvg;
				piggyback->resultStatistics->columnStatistics[i].distinct_status = 1;

				// the meta data for this column is complete and should not be calculated again
				piggyback->resultStatistics->columnStatistics[i].n_distinctIsFinal = 1;
				piggyback->resultStatistics->columnStatistics[i].minValueIsFinal = 1;
				piggyback->resultStatistics->columnStatistics[i].maxValueIsFinal = 1;
				piggyback->resultStatistics->columnStatistics[i].mostFrequentValueIsFinal = 1;
			}
			else
			{
				printf("there are statistics results from the selection that are not part of the result table\n");
			}
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecProcNode
 *
 *		Execute the given node to return a(nother) tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecProcNode(PlanState *node) {
	TupleTableSlot *result;

	CHECK_FOR_INTERRUPTS();

	if (node->chgParam != NULL) /* something changed */
		ExecReScan(node); /* let ReScan handle this */

	if (node->instrument)
		InstrStartNode(node->instrument);

	switch (nodeTag(node)) {
	/*
	 * control nodes
	 */
	case T_ResultState:
		result = ExecResult((ResultState *) node);
		break;

	case T_ModifyTableState:
		result = ExecModifyTable((ModifyTableState *) node);
		break;

	case T_AppendState:
		result = ExecAppend((AppendState *) node);
		break;

	case T_MergeAppendState:
		result = ExecMergeAppend((MergeAppendState *) node);
		break;

	case T_RecursiveUnionState:
		result = ExecRecursiveUnion((RecursiveUnionState *) node);
		break;

		/* BitmapAndState does not yield tuples */

		/* BitmapOrState does not yield tuples */

		/*
		 * scan nodes
		 */
	case T_SeqScanState:
		result = ExecSeqScan((SeqScanState *) node);
		break;

	case T_IndexScanState:
		result = ExecIndexScan((IndexScanState *) node);
		break;

	case T_IndexOnlyScanState:
		result = ExecIndexOnlyScan((IndexOnlyScanState *) node);
		break;

		/* BitmapIndexScanState does not yield tuples */

	case T_BitmapHeapScanState:
		result = ExecBitmapHeapScan((BitmapHeapScanState *) node);
		break;

	case T_TidScanState:
		result = ExecTidScan((TidScanState *) node);
		break;

	case T_SubqueryScanState:
		result = ExecSubqueryScan((SubqueryScanState *) node);
		break;

	case T_FunctionScanState:
		result = ExecFunctionScan((FunctionScanState *) node);
		break;

	case T_ValuesScanState:
		result = ExecValuesScan((ValuesScanState *) node);
		break;

	case T_CteScanState:
		result = ExecCteScan((CteScanState *) node);
		break;

	case T_WorkTableScanState:
		result = ExecWorkTableScan((WorkTableScanState *) node);
		break;

	case T_ForeignScanState:
		result = ExecForeignScan((ForeignScanState *) node);
		break;

		/*
		 * join nodes
		 */
	case T_NestLoopState:
		result = ExecNestLoop((NestLoopState *) node);
		break;

	case T_MergeJoinState:
		result = ExecMergeJoin((MergeJoinState *) node);
		break;

	case T_HashJoinState:
		result = ExecHashJoin((HashJoinState *) node);
		break;

		/*
		 * materialization nodes
		 */
	case T_MaterialState:
		result = ExecMaterial((MaterialState *) node);
		break;

	case T_SortState:
		result = ExecSort((SortState *) node);
		break;

	case T_GroupState:
		result = ExecGroup((GroupState *) node);
		break;

	case T_AggState:
		result = ExecAgg((AggState *) node);
		break;

	case T_WindowAggState:
		result = ExecWindowAgg((WindowAggState *) node);
		break;

	case T_UniqueState:
		result = ExecUnique((UniqueState *) node);
		break;

	case T_HashState:
		result = ExecHash((HashState *) node);
		break;

	case T_SetOpState:
		result = ExecSetOp((SetOpState *) node);
		break;

	case T_LockRowsState:
		result = ExecLockRows((LockRowsState *) node);
		break;

	case T_LimitState:
		result = ExecLimit((LimitState *) node);
		break;

	default:
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
		result = NULL;
		break;
	}

	/*
	 * Process with piggyback if current node is root node.
	 */
	if (piggyback != NULL) {
		if (node->plan == piggyback->root && result && !result->tts_isempty && result->tts_tupleDescriptor) {
			piggyback->numberOfAttributes = result->tts_tupleDescriptor->natts;
			Form_pg_attribute *attrList = result->tts_tupleDescriptor->attrs;

			Datum datum;
			bool isNull;

			// fetch all data
			slot_getallattrs(result);

			int i;
			for (i = 0; i < piggyback->numberOfAttributes; i++) {
				datum = slot_getattr(result, i + 1, &isNull);
				if (isNull) {
					piggyback->slotValues[i] = "";
					continue;
				}

				// Use data type aware conversion.
				Form_pg_attribute attr = attrList[i];

				switch (attr->atttypid) {
				case INT8OID:
				case INT2OID:
				case INT2VECTOROID:
				case INT4OID: { // Int
					piggyback->resultStatistics->columnStatistics[i].isNumeric = 1;
					int *val_pntr = (int*) malloc(sizeof(int));
					int value = (int) (datum);
					*val_pntr = value;

					// Write temporary slot value for FD calculation
					char* cvalue = calloc(20, sizeof(char));
					sprintf(cvalue, "%d", value);
					piggyback->slotValues[i] = cvalue;

					if (!piggyback->resultStatistics->columnStatistics[i].minValueIsFinal)
						if (value < *((int*)(piggyback->resultStatistics->columnStatistics[i].minValue))
								|| *((int*)(piggyback->resultStatistics->columnStatistics[i].minValue)) == INT_MAX)
							piggyback->resultStatistics->columnStatistics[i].minValue = val_pntr;
					if (!piggyback->resultStatistics->columnStatistics[i].maxValueIsFinal)
						if (value > *((int*)(piggyback->resultStatistics->columnStatistics[i].maxValue))
								|| *((int*)(piggyback->resultStatistics->columnStatistics[i].maxValue)) == INT_MIN)
							piggyback->resultStatistics->columnStatistics[i].maxValue = val_pntr;
					if (piggyback->resultStatistics->columnStatistics[i].n_distinctIsFinal == 0) {
						hashset_add_integer(piggyback->distinctValues[i], value);
					}
					break;
				}
				case NUMERICOID: { // Decimal
					//piggyback->resultStatistics->columnStatistics[i].isNumeric = 1;
					int value = (float) (datum);
					//printf("Numeric: %f, casted: %d \n",(float)(datum),value);
					char* cvalue = calloc(20, sizeof(char));
					sprintf(cvalue, "%d", value);
					piggyback->slotValues[i] = cvalue;

					break;
				}
				case BPCHAROID:
				case VARCHAROID: { // Varchar
					piggyback->slotValues[i] = TextDatumGetCString(datum);

					piggyback->resultStatistics->columnStatistics[i].isNumeric = 0;
					if (piggyback->resultStatistics->columnStatistics[i].n_distinctIsFinal == 0) {
						hashset_add_string(piggyback->distinctValues[i], piggyback->slotValues[i]);
					}
					break;
				}
				default:
					piggyback->slotValues[i] = "";
					break;
				}
			}
			for (i = 0; i < piggyback->numberOfAttributes; i++) {
				buildTwoColumnCombinations(piggyback->slotValues[i], i + 1, result);
			}
		}
	}

	if (node->instrument)
		InstrStopNode(node->instrument, TupIsNull(result) ? 0.0 : 1.0);

	return result;
}

void buildTwoColumnCombinations(char* valueToConcat, int from, TupleTableSlot *result) {
	int i;
	if (from == piggyback->numberOfAttributes) {
		return;
	}

	for (i = from; i < piggyback->numberOfAttributes; i++) {
		addToTwoColumnCombinationHashSet(from, valueToConcat, i + 1, piggyback->slotValues[i]);
	}
}

void addToTwoColumnCombinationHashSet(int from, char* valueToConcat, int to, char* value) {
	int index = 0;
	int i;
	for (i = 1; i < from; i++) {
		index += piggyback->numberOfAttributes - i;
	}
	index += to - from - 1;

	//printf("FD: addtoColCombArray %d: from: %d, valueConcat: %s, to: %d, value: %s \n", index, from, valueToConcat, to, value);

	const size_t v1Length = strlen(valueToConcat);
	const size_t v2Length = strlen(value);
	const size_t totalLength = v1Length + v2Length;

	char * const strBuf = malloc(totalLength + 1);
	if (strBuf == NULL) {
		fprintf(stderr, "malloc failed\n");
		exit(EXIT_FAILURE);
	}
	//TODO add delimeter
	strcpy(strBuf, valueToConcat);
	strcpy(strBuf + v1Length, value);

	//printf("FD: fill ColCombinationArray on index %d with content %s (Merged from %s and %s) \n",index,strBuf,valueToConcat,value);
	hashset_add_string(piggyback->twoColumnsCombinations[index], strBuf);

	//free(strBuf);
}

/* ----------------------------------------------------------------
 *		MultiExecProcNode
 *
 *		Execute a node that doesn't return individual tuples
 *		(it might return a hashtable, bitmap, etc).  Caller should
 *		check it got back the expected kind of Node.
 *
 * This has essentially the same responsibilities as ExecProcNode,
 * but it does not do InstrStartNode/InstrStopNode (mainly because
 * it can't tell how many returned tuples to count).  Each per-node
 * function must provide its own instrumentation support.
 * ----------------------------------------------------------------
 */
Node *
MultiExecProcNode(PlanState *node) {
	Node *result;

	CHECK_FOR_INTERRUPTS();

	if (node->chgParam != NULL) /* something changed */
		ExecReScan(node); /* let ReScan handle this */

	switch (nodeTag(node)) {
	/*
	 * Only node types that actually support multiexec will be listed
	 */

	case T_HashState:
		result = MultiExecHash((HashState *) node);
		break;

	case T_BitmapIndexScanState:
		result = MultiExecBitmapIndexScan((BitmapIndexScanState *) node);
		break;

	case T_BitmapAndState:
		result = MultiExecBitmapAnd((BitmapAndState *) node);
		break;

	case T_BitmapOrState:
		result = MultiExecBitmapOr((BitmapOrState *) node);
		break;

	default:
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
		result = NULL;
		break;
	}

	return result;
}

/* ----------------------------------------------------------------
 *		ExecEndNode
 *
 *		Recursively cleans up all the nodes in the plan rooted
 *		at 'node'.
 *
 *		After this operation, the query plan will not be able to be
 *		processed any further.	This should be called only after
 *		the query plan has been fully executed.
 * ----------------------------------------------------------------
 */
void ExecEndNode(PlanState *node) {
	// TODO: remove memory leak
	if (piggyback) {
		printMetaData();

		piggyback = NULL;
	}

	/*
	 * do nothing when we get to the end of a leaf on tree.
	 */
	if (node == NULL)
		return;

	if (node->chgParam != NULL) {
		bms_free(node->chgParam);
		node->chgParam = NULL;
	}

	switch (nodeTag(node)) {
	/*
	 * control nodes
	 */
	case T_ResultState:
		ExecEndResult((ResultState *) node);
		break;

	case T_ModifyTableState:
		ExecEndModifyTable((ModifyTableState *) node);
		break;

	case T_AppendState:
		ExecEndAppend((AppendState *) node);
		break;

	case T_MergeAppendState:
		ExecEndMergeAppend((MergeAppendState *) node);
		break;

	case T_RecursiveUnionState:
		ExecEndRecursiveUnion((RecursiveUnionState *) node);
		break;

	case T_BitmapAndState:
		ExecEndBitmapAnd((BitmapAndState *) node);
		break;

	case T_BitmapOrState:
		ExecEndBitmapOr((BitmapOrState *) node);
		break;

		/*
		 * scan nodes
		 */
	case T_SeqScanState:
		ExecEndSeqScan((SeqScanState *) node);
		break;

	case T_IndexScanState:
		ExecEndIndexScan((IndexScanState *) node);
		break;

	case T_IndexOnlyScanState:
		ExecEndIndexOnlyScan((IndexOnlyScanState *) node);
		break;

	case T_BitmapIndexScanState:
		ExecEndBitmapIndexScan((BitmapIndexScanState *) node);
		break;

	case T_BitmapHeapScanState:
		ExecEndBitmapHeapScan((BitmapHeapScanState *) node);
		break;

	case T_TidScanState:
		ExecEndTidScan((TidScanState *) node);
		break;

	case T_SubqueryScanState:
		ExecEndSubqueryScan((SubqueryScanState *) node);
		break;

	case T_FunctionScanState:
		ExecEndFunctionScan((FunctionScanState *) node);
		break;

	case T_ValuesScanState:
		ExecEndValuesScan((ValuesScanState *) node);
		break;

	case T_CteScanState:
		ExecEndCteScan((CteScanState *) node);
		break;

	case T_WorkTableScanState:
		ExecEndWorkTableScan((WorkTableScanState *) node);
		break;

	case T_ForeignScanState:
		ExecEndForeignScan((ForeignScanState *) node);
		break;

		/*
		 * join nodes
		 */
	case T_NestLoopState:
		ExecEndNestLoop((NestLoopState *) node);
		break;

	case T_MergeJoinState:
		ExecEndMergeJoin((MergeJoinState *) node);
		break;

	case T_HashJoinState:
		ExecEndHashJoin((HashJoinState *) node);
		break;

		/*
		 * materialization nodes
		 */
	case T_MaterialState:
		ExecEndMaterial((MaterialState *) node);
		break;

	case T_SortState:
		ExecEndSort((SortState *) node);
		break;

	case T_GroupState:
		ExecEndGroup((GroupState *) node);
		break;

	case T_AggState:
		ExecEndAgg((AggState *) node);
		break;

	case T_WindowAggState:
		ExecEndWindowAgg((WindowAggState *) node);
		break;

	case T_UniqueState:
		ExecEndUnique((UniqueState *) node);
		break;

	case T_HashState:
		ExecEndHash((HashState *) node);
		break;

	case T_SetOpState:
		ExecEndSetOp((SetOpState *) node);
		break;

	case T_LockRowsState:
		ExecEndLockRows((LockRowsState *) node);
		break;

	case T_LimitState:
		ExecEndLimit((LimitState *) node);
		break;

	default:
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
		break;
	}
}

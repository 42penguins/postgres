/*-------------------------------------------------------------------------
 *
 * piggyback.c
 *	 piggyback metadata while execution of a query
 *
 *
 * IDENTIFICATION
 *	src/backend/piggyback/piggyback.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "piggyback/piggyback.h"
#include "miscadmin.h"

//#include "utils/hsearch.h"
//#include "utils/builtins.h"
//#include "nodes/pg_list.h"

void printIt()
{
	printf("THIS IS PRINTED");
	return;
}
// Singleton Piggyback instance.
Piggyback *piggyback = NULL;

/*
 * Initialize piggyback if not already done.
 */
void initPiggyback()
{
	piggyback = (Piggyback*)(malloc(sizeof(Piggyback)));
}

/*
 * Set root node to enable data collection.
 */
void setPiggybackRootNode(Plan *rootNode)
{
	// Save root node for later processing.
	piggyback->root = rootNode;

	// Flag to recognize first processing of root node.
	piggyback->newProcessing = true;

	//init attribute list
	piggyback->columnNames = NIL;
}

void printMetaData() {
	printDistinctValues();
}

void printDistinctValues() {
	if(!piggyback) return;
	int i;

	for (i = 0; i < piggyback->numberOfAttributes; i++) {
		char * columnName = (char *)list_nth(piggyback->columnNames, i);
		long distinctValues = hash_get_num_entries(piggyback->distinctValues[i]);
		printf("column %s (%d) has %ld distinct values.\n", columnName, i, distinctValues);
	}
}

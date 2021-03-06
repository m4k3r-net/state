#include <stdlib.h>
#include "nfa.h"
#include "bitset.h"

#define TRANSINC 16
#define NODEINC 16
#define FINALINC 16

/* graph */

struct nfa *
nfa_init(struct nfa *nfa)
{
	nfa->nnodes = 0;
	nfa->nodes = 0;
	return nfa;
}

void
nfa_fini(struct nfa *nfa)
{
	unsigned i, j;
	for (i = 0; i < nfa->nnodes; i++) {
		struct node *n = &nfa->nodes[i];
		for (j = 0; j < n->nedges; ++j) {
			cclass_free(n->edges[j].cclass);
		}
		free(n->edges);
		free(n->finals);
	}
	free(nfa->nodes);
	nfa->nodes = 0;
	nfa->nnodes = 0;
}

struct nfa *
nfa_new()
{
	struct nfa *nfa = malloc(sizeof *nfa);

	if (nfa) {
		nfa_init(nfa);
	}
	return nfa;
}


void
nfa_free(struct nfa *nfa)
{
	if (nfa) {
		nfa_fini(nfa);
		free(nfa);
	}
}

unsigned
nfa_new_node(struct nfa *nfa)
{
	unsigned i;
	if ((nfa->nnodes % NODEINC) == 0) {
		nfa->nodes = realloc(nfa->nodes,
			(nfa->nnodes + NODEINC) * sizeof *nfa->nodes);
	}
	i = nfa->nnodes++;
	memset(&nfa->nodes[i], 0, sizeof nfa->nodes[i]);
	return i;
}

struct edge *
nfa_new_edge(struct nfa *nfa, unsigned from, unsigned to)
{
	struct node *n = &nfa->nodes[from];
	struct edge *edge;
	if (n->nedges % TRANSINC == 0) {
		n->edges = realloc(n->edges,
			(n->nedges + TRANSINC) * sizeof *n->edges);
	}
	edge = &n->edges[n->nedges++];
	edge->cclass = 0;
	edge->dest = to;
	return edge;
}

void
nfa_add_final(struct nfa *nfa, unsigned i, const void *final)
{
	struct node *n = &nfa->nodes[i];
	unsigned j;

	/* Check if the value is already in the final set */
	for (j = 0; j < n->nfinals; ++j)
		if (n->finals[j] == final)
			return;

	if (n->nfinals % FINALINC == 0) {
		n->finals = realloc(n->finals,
			(n->nfinals + FINALINC) * sizeof *n->finals);
	}
	n->finals[n->nfinals++] = final;
}

static int
edge_is_epsilon(const struct edge *edge)
{
	return !edge->cclass;
}

/*
 * Compute the epsilon-closure of the set s.
 * That's all the states reachable through zero or more epsilon edges
 * in nfa from any of the states in s.
 * @param nfa the graph with the epsilon edges
 * @param s the set to expand to epsilon closure
 */
void
epsilon_closure(const struct nfa *nfa, bitset *s)
{
	unsigned ni, j;
	struct node *n;
	struct edge *e;
	bitset *tocheck = bitset_dup(s);

	while (!bitset_is_empty(tocheck)) {
		bitset_for(ni, tocheck) {
			n = &nfa->nodes[ni];
			bitset_remove(tocheck, ni);
			for (j = 0; j < n->nedges; ++j) {
				e = &n->edges[j];
				if (edge_is_epsilon(e)) {
					if (bitset_insert(s, e->dest)) {
						bitset_insert(tocheck, e->dest);
					}
				}
			}
		}
	}
	bitset_free(tocheck);
}

/*
 * Equivalance set: a mapping from DFA node IDs to a set of NFA nodes.
 * We need the nfa graph so that bitsets can be allocated with the same
 * capacity as the number of nodes in the nfa.
 */
struct equiv {
	const struct nfa *nfa;
	unsigned max, avail;
	bitset **set;
};

static void
equiv_init(struct equiv *equiv, const struct nfa *nfa)
{
	equiv->nfa = nfa;
	equiv->max = 0;
	equiv->avail = 0;
	equiv->set = 0;
}

/*
 * Returns the bitset corresponding to DFA node i.
 * Allocates storage in the equiv map as needed;
 * initializes never-before requested bitsets to empty.
 */
static bitset *
equiv_get(struct equiv *equiv, unsigned i)
{
	if (equiv->avail <= i) {
		unsigned oldavail = equiv->avail;
		while (equiv->avail <= i) equiv->avail += 32;
		equiv->set = realloc(equiv->set,
			equiv->avail * sizeof *equiv->set);
		memset(equiv->set + oldavail, 0,
			(equiv->avail - oldavail) * sizeof *equiv->set);
	}
	if (!equiv->set[i]) {
		equiv->set[i] = bitset_new(equiv->nfa->nnodes);
		if (i >= equiv->max)
			equiv->max = i + 1;
	}
	return equiv->set[i];
}

static void
equiv_cleanup(struct equiv *equiv) {
	unsigned i;

	for (i = 0; i < equiv->max; ++i)
		bitset_free(equiv->set[i]);
	free(equiv->set);
}

/*
 * Find (or create new) an equivalent DFA state for a given
 * set of NFA nodes.
 * Searches the equiv map for the bitset bs.
 * Note that this may add nodes to the DFA.
 */
static unsigned
equiv_lookup(struct nfa *dfa, struct equiv *equiv, const bitset *bs)
{
	unsigned i, j, n;

	/* Check to see if we've already constructed the equivalent-node */
	for (i = 0; i < equiv->max; ++i) {
		if (equiv->set[i] && bitset_cmp(equiv->set[i], bs) == 0) {
			return i;
		}
	}

	/* Haven't seen that NFA set before, so let's allocate a DFA node */
	n = nfa_new_node(dfa);

	/* Merge the set of final pointers */
	bitset_for(j, bs) {
		const struct node *jnode = &equiv->nfa->nodes[j];
		for (i = 0; i < jnode->nfinals; ++i) {
			nfa_add_final(dfa, n, jnode->finals[i]);
		}
	}

	bitset_copy(equiv_get(equiv, n), bs);
	return n;
}

/* Unsigned integer comparator for qsort */
static int
unsigned_cmp(const void *a, const void *b)
{
	unsigned aval = *(const unsigned *)a;
	unsigned bval = *(const unsigned *)b;

	return aval < bval ? -1 : aval > bval;
}

/*
 * Construct the break set of all edges in all the
 * given nodes.
 * For example, the cclasses [p-y] and [pt] expand to the
 * interval ranges:
 *    [a-z] =  [p,z)
 *    [pt]  =  [p,q),[t,u)
 * The break set of these two cclasses is the
 * simple set union of the his and los:
 *
 *     {p,q,t,u,z}
 *
 * @param nfa            the graph from which to draw the nodes
 * @param nodes          the set of nodes from which to draw the cclasses
 * @param nbreaks_return where to store the length of the returned set
 * @return the array of breakpoints.
 */
static unsigned *
cclass_breaks(const struct nfa *nfa, const bitset *nodes,
	      unsigned *nbreaks_return)
{
	unsigned ni;
	unsigned i, j;

	/* Count the number of intervals */
	unsigned nintervals = 0;
	bitset_for(ni, nodes) {
		const struct node *n = &nfa->nodes[ni];
		for (j = 0; j < n->nedges; ++j) {
			const cclass *cc = n->edges[j].cclass;
			if (cc) {
				nintervals += cc->nintervals;
			}
		}
	}

	if (!nintervals) {
		/* shortcut empty set */
		*nbreaks_return = 0;
		return NULL;
	}

	/* Build the non-normalized set of cclass breaks
	 * by just collecting all lo and hi values */
	unsigned *breaks = malloc(nintervals * 2 * sizeof (unsigned));
	unsigned nbreaks = 0;
	bitset_for(ni, nodes) {
		const struct node *n = &nfa->nodes[ni];
		for (j = 0; j < n->nedges; ++j) {
			const cclass *cc = n->edges[j].cclass;
			if (cc) {
				for (i = 0; i < cc->nintervals; ++i) {
					breaks[nbreaks++] = cc->interval[i].lo;
					breaks[nbreaks++] = cc->interval[i].hi;
				}
			}
		}
	}

	/* Sort the array of breaks */
	qsort(breaks, nbreaks, sizeof *breaks, unsigned_cmp);

	/* Remove duplicates */
	unsigned lastbreak = breaks[0];
	unsigned newlen = 1;
	for (i = 1; i < nbreaks; ++i) {
		if (breaks[i] != lastbreak) {
			lastbreak = breaks[newlen++] = breaks[i];
		}
	}
	nbreaks = newlen;

	/* Reduce storage overhead */
	breaks = realloc(breaks, nbreaks * sizeof *breaks);

	*nbreaks_return = nbreaks;
	return breaks;
}

/*
 * Constructs a deterministic automaton that simulates the
 * input nfa, but only has deterministic edges (that is
 * each node has only 0 or 1 edges for any character).
 * @param dfa an empty graph into which to store the DFA
 * @param nfa the input (non-deterministic) graph
 */
static void
make_dfa(struct nfa *dfa, const struct nfa *nfa)
{
	struct bitset *bs;
	struct equiv equiv;
	unsigned ei;

	equiv_init(&equiv, nfa);

	/* the initial dfa node is the epislon closure of the nfa's initial */
	bs = bitset_new(nfa->nnodes);
	bitset_insert(bs, 0);
	epsilon_closure(nfa, bs);
	equiv_lookup(dfa, &equiv, bs) /* == 0 */;
	bitset_free(bs);

	/*
	 * Iterate ei over the unprocessed DFA nodes.
	 * Each iteration may add more DFA nodes, but it
	 * won't add duplicates.
	 */
	for (ei = 0; ei < dfa->nnodes; ei++) {
		const struct node *en = &dfa->nodes[ei];
		unsigned nbreaks, bi;
		unsigned *breaks;
		struct bitset *src;

		/* src is the set of NFA nodes corresponding to
		 * the current DFA node ei */
		src = equiv_get(&equiv, ei);

		/* We want to combine all the edge cclasses
		 * of src together, and then efficiently walk over
		 * their members.
		 * The breaks list speeds this up because if
		 * characters c1,c2 appear adjacent in the breaks list,
		 * then we can reason that for any cclass in any src
		 * edges, either:
		 *   [c1,c2) is wholly within that cclass
		 *   [c1,c2) is wholly outside that cclass
		 */
		breaks = cclass_breaks(nfa, src, &nbreaks);
		for (bi = 1; bi < nbreaks; ++bi) {
			const unsigned lo = breaks[bi - 1];
			const unsigned hi = breaks[bi];
			unsigned ni, di, j;

			/* Find the set of NFA states, dest, to which
			 * the cclass [lo,hi) edges to from the
			 * src set. We can do this by just checking for
			 * membership of lo. */
			bitset *dest = bitset_alloca(nfa->nnodes);
			bitset_for(ni, src) {
			    const struct node *n = &nfa->nodes[ni];
			    for (j = 0; j < n->nedges; ++j) {
				if (n->edges[j].cclass &&
				    cclass_contains_ch(n->edges[j].cclass, lo))
				{
				    bitset_insert(dest, n->edges[j].dest);
				}
			    }
			}
			/* Expand the resulting dest set to its
			 * epsilon closure */
			epsilon_closure(nfa, dest);

			/* Find or make di, the DFA equivalent
			 * node for {dest} */
			di = equiv_lookup(dfa, &equiv, dest);

			/* (Recompute pointers here because nodes may have
			 *  been realloced) */
			en = &dfa->nodes[ei];

			/* Create or find an existing edge from ei->di */
			struct edge *e = NULL;
			for (j = 0; j < en->nedges; ++j) {
				if (en->edges[j].dest == di) {
					e = &en->edges[j];
					break;
				}
			}
			if (!e) {
				e = nfa_new_edge(dfa, ei, di);
				e->cclass = cclass_new();
			}

			/* Add the edge along [lo,hi) into the DFA */
			cclass_add(e->cclass, lo, hi);
		}
		free(breaks);
	}

	/* TODO: remove duplicate states */

	equiv_cleanup(&equiv);
}

void
nfa_to_dfa(struct nfa *nfa)
{
	/* move the content of nfa into a temporary copy */
	struct nfa copy = *nfa;
	/* reset the output automaton */
	nfa_init(nfa);
	/* construct a DFA from the copy */
	make_dfa(nfa, &copy);
	/* now nfa actually contains a dfa! */
	nfa_fini(&copy);
}

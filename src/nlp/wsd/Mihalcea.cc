/*
 * Mihalcea.cc
 *
 * Implements the Rada Mihalcea word-sense disambiguation algorithm.
 *
 * Copyright (c) 2008 Linas Vepstas <linas@linas.org>
 */
#include <stdio.h>

#include "Mihalcea.h"
#include "MihalceaEdge.h"
#include "MihalceaLabel.h"
#include "ParseRank.h"
#include "SenseRank.h"
#include "ReportRank.h"

using namespace opencog;

Mihalcea::Mihalcea(void)
{
	atom_space = NULL;
	labeller = new MihalceaLabel();
	edger = new MihalceaEdge();
	nn_adjuster = new NNAdjust();
	parse_ranker = new ParseRank();
	sense_ranker = new SenseRank();
	reporter = new ReportRank();

	previous_parse = UNDEFINED_HANDLE;
}

Mihalcea::~Mihalcea()
{
	atom_space = NULL;
	delete labeller;
	delete edger;
	delete nn_adjuster;
	delete parse_ranker;
	delete sense_ranker;
	delete reporter;
}

void Mihalcea::set_atom_space(AtomSpace *as)
{
	atom_space = as;
	labeller->set_atom_space(as);
	edger->set_atom_space(as);
}

void Mihalcea::process_sentence(Handle h)
{
	// Add handle to sentence to our running list.
	sentence_list.push_back(h);

	Handle top_parse = parse_ranker->get_top_ranked_parse(h);

	labeller->annotate_parse(top_parse);
	edger->annotate_parse(top_parse);
	// nn_adjuster->adjust_parse(top_parse);

	// Link sentences together ... 
	if (UNDEFINED_HANDLE != previous_parse)
	{
		edger->annotate_parse_pair(previous_parse, top_parse);
	}
	previous_parse = top_parse;


	sense_ranker->rank_parse(top_parse);
	reporter->report_parse(top_parse);
}


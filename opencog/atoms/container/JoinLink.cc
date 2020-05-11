/*
 * JoinLink.cc
 *
 * Copyright (C) 2020 Linas Vepstas
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the
 * exceptions at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public
 * License along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <algorithm>
#include <iterator>

#include <opencog/util/oc_assert.h>
#include <opencog/atoms/atom_types/NameServer.h>
#include <opencog/atoms/atom_types/atom_types.h>
#include <opencog/atoms/core/FindUtils.h>
#include <opencog/atoms/core/TypeUtils.h>
#include <opencog/atoms/value/LinkValue.h>
#include <opencog/atomspace/AtomSpace.h>

#include "JoinLink.h"

using namespace opencog;

void JoinLink::init(void)
{
	Type t = get_type();
	if (not nameserver().isA(t, JOIN_LINK))
	{
		const std::string& tname = nameserver().getTypeName(t);
		throw InvalidParamException(TRACE_INFO,
			"Expecting a JoinLink, got %s", tname.c_str());
	}
	if (JOIN_LINK == t)
		throw InvalidParamException(TRACE_INFO,
			"JoinLinks are private and cannot be instantiated.");

	validate();
	setup_meet();
	setup_top_types();
}

JoinLink::JoinLink(const HandleSeq&& hseq, Type t)
	: PrenexLink(std::move(hseq), t)
{
	init();
}

/* ================================================================= */

/// Temporary scaffolding to validate what we can do, so far.
void JoinLink::validate(void)
{
	for (size_t i=1; i<_outgoing.size(); i++)
	{
		const Handle& clause(_outgoing[i]);
		Type t = clause->get_type();

		// Replacement links get special treatment, here.
		if (REPLACEMENT_LINK == t) continue;

		// Anything evaluatable goes into the MeetLink
		if (PRESENT_LINK == t) continue;
		if (clause->is_evaluatable()) continue;
		if (nameserver().isA(t, EVALUATABLE_LINK)) continue;

		// Tye type nodes get applied to the container.
		if (nameserver().isA(t, TYPE_NODE)) continue;
		if (nameserver().isA(t, TYPE_LINK)) continue;
		if (nameserver().isA(t, TYPE_OUTPUT_LINK)) continue;

		throw SyntaxException(TRACE_INFO, "Not supported (yet?)");
	}
}

/* ================================================================= */

/// setup_meet() -- create a search that can find the all of
/// the locations that will be joined together.
void JoinLink::setup_meet(void)
{
	HandleSeq jclauses;
	HandleSet done;
	for (size_t i=1; i<_outgoing.size(); i++)
	{
		const Handle& clause(_outgoing[i]);

		// Clauses handled at the container level
		Type t = clause->get_type();
		if (REPLACEMENT_LINK == t) continue;
		if (nameserver().isA(t, TYPE_NODE)) continue;
		if (nameserver().isA(t, TYPE_LINK)) continue;
		if (nameserver().isA(t, TYPE_OUTPUT_LINK)) continue;

		jclauses.push_back(clause);

		// Find the variables in the clause
		FreeVariables fv;
		fv.find_variables(clause);
		if (0 == fv.varset.size())
			_const_terms.insert(clause);
		else
			done.merge(fv.varset);
	}

	_vsize = _variables.varseq.size();
	if (0 == _vsize) return;

	// Are there any variables that are NOT in some clause? If so,
	// then create a PresentLink for each.
	for (const Handle& var: _variables.varseq)
	{
		if (done.find(var) != done.end()) continue;
		Handle pres(createLink(PRESENT_LINK, var));
		jclauses.emplace_back(pres);
	}

	// Build a Meet
	HandleSeq vardecls;
	for (const Handle& var : _variables.varseq)
	{
		Handle typedecl(_variables.get_type_decl(var, var));
		vardecls.emplace_back(typedecl);
	}

	Handle hdecls(createLink(std::move(vardecls), VARIABLE_LIST));
	Handle hbody(createLink(std::move(jclauses), AND_LINK));
	_meet = createLink(MEET_LINK, hdecls, hbody);
}

/* ================================================================= */

/// Setup the type constraints that will be applied to the top.
void JoinLink::setup_top_types(void)
{
	for (size_t i=1; i<_outgoing.size(); i++)
	{
		const Handle& clause(_outgoing[i]);
		Type t = clause->get_type();

		// Tye type nodes get applied to the container.
		if (nameserver().isA(t, TYPE_NODE) or
		    nameserver().isA(t, TYPE_LINK) or
		    nameserver().isA(t, TYPE_OUTPUT_LINK))
		{
			_top_types.push_back(clause);
		}
	}
}

/* ================================================================= */

/// Scan for ReplacementLinks in the body of the JoinLink.
/// Each of these should have a corresponding variable declaration.
/// Update the replacement map so that the "from" part of the variable
/// (obtained from the signature) gets replaced by the ... replacement.
void JoinLink::fixup_replacements(Traverse& trav) const
{
	for (size_t i=1; i<_outgoing.size(); i++)
	{
		const Handle& h(_outgoing[i]);
		if (h->get_type() != REPLACEMENT_LINK) continue;
		if (h->get_arity() != 2)
			throw SyntaxException(TRACE_INFO,
				"ReplacementLink expecting two arguments, got %s",
				h->to_short_string().c_str());

		const Handle& from(h->getOutgoingAtom(0));
		bool found = false;
		for (const auto& pr : trav.replace_map)
		{
			if (pr.second != from) continue;
			trav.replace_map[pr.first] = h->getOutgoingAtom(1);
			found = true;
		}

		if (not found)
			throw SyntaxException(TRACE_INFO,
				"No matching variable declaration for: %s",
				h->to_short_string().c_str());
	}
}

/* ================================================================= */

/// Given the JoinLink, obtain a set of atoms that lie "below" the join.
/// Below, in the sense that the join is guaranteed to be contained
/// in the incoming trees of these atoms. The minimal join (the
/// supremum) is guaranteed to be a slice through the above trees,
/// and its the minimal one that joins the atoms. Basically, we let
/// the pattern engine do all the hard work of checking for the
/// satisfiability of all the various clauses.
///
/// Explained a different way: this performs a wild-card search, to find
/// all of the principal elements specified by the wild-cards.
///
/// During construction, several maps are built. One is a "replacement
/// map", which is needed to perform substitution on the discovered
/// results.  It pairs up atoms in the atomspace with variables in
/// the JoinLink. Another map is the "join map"; it reverses the
/// pairing. It is needed to rule out unjoined responses from the
/// pattern engine.
///
/// An example: one is looking for MemberLinks:
///
///    (Join
///       (VariableList
///          (TypedVariable (Variable "X") (Type 'ConceptNode))
///          (TypedVariable (Variable "Y") (Type 'ConceptNode)))
///       (Present (Member (Variable "X") (Variable "Y"))))
///
/// and that the atomspace contains:
///
///    (Member (Concept "sea") (Concept "beach"))
///    (Member (Concept "sand") (Concept "beach"))
///
/// then the replacement map will have three pairs, of the form
///
///    { (Concept "sea"),   (Variable "X") }
///    { (Concept "sand"),  (Variable "X") }
///    { (Concept "beach"), (Variable "Y") }
///
/// The join-map will contain
///
///    { (Variable "X"), { (Concept "sea"), (Concept "sand") }}
///    { (Variable "Y"), { (Concept "beach") }}
///
HandleSet JoinLink::principals(AtomSpace* as,
                               Traverse& trav) const
{
	// No variables, no search needed.
	if (0 == _vsize)
		return _const_terms;

	// If we are here, the expression had variables in it.
	// Perform a search to ground those.
	const bool TRANSIENT_SPACE = true;

	AtomSpace temp(as, TRANSIENT_SPACE);
	Handle meet = temp.add_atom(_meet);
	ValuePtr vp = meet->execute();

	// The MeetLink returned everything that the variables in the
	// clause could ever be...
	const HandleSeq& varseq(_variables.varseq);
	if (1 == _vsize)
	{
		const Handle& var(varseq[0]);
		HandleSet princes(_const_terms);
		for (const Handle& hst : LinkValueCast(vp)->to_handle_seq())
		{
			princes.insert(hst);
			trav.replace_map.insert({hst, var});
		}
		trav.join_map.push_back(princes);
		return princes;
	}

	// If we are here, then the MeetLink has returned a collection
	// of ListLinks, holding the variable values in the lists.
	HandleSet princes(_const_terms);
	trav.join_map.resize(_vsize);
	for (const Handle& hst : LinkValueCast(vp)->to_handle_seq())
	{
		const HandleSeq& glist(hst->getOutgoingSet());
		for (size_t i=0; i<_vsize; i++)
		{
			princes.insert(glist[i]);
			trav.replace_map.insert({glist[i], varseq[i]});
			trav.join_map[i].insert(glist[i]);
		}
	}
	return princes;
}

/* ================================================================= */

/// principal_filter() - Get everything that contains `h`.
/// This is the "principal filter" on the "principal element" `h`.
/// Algorithmically: walk upwards from h and insert everything in
/// it's incoming tree into the handle-set. This recursively walks to
/// the top, till there is no more. Of course, this can get large.
void JoinLink::principal_filter(HandleSet& containers,
                                const Handle& h) const
{
	// Ignore type specifications, other containers!
	if (nameserver().isA(h->get_type(), TYPE_OUTPUT_LINK) or
	    nameserver().isA(h->get_type(), JOIN_LINK))
		return;

	IncomingSet is(h->getIncomingSet());
	containers.insert(h);

	for (const Handle& ih: is)
		principal_filter(containers, ih);
}

/* ================================================================= */

/// Compute the upper set -- the intersection of all of the principal
/// filters for each mandatory clause.
///
HandleSet JoinLink::upper_set(AtomSpace* as, bool silent,
                              Traverse& trav) const
{
	HandleSet princes(principals(as, trav));

	// Get a principal filter for each principal element,
	// and union all of them together.
	HandleSet containers;
	for (const Handle& pr: princes)
		principal_filter(containers, pr);

	if (1 == _vsize)
		return containers;

	// The meet link provided us with elements that are "too low",
	// fail to be joins. Remove them. Ther shouldn't be all that
	// many of them; it depends on how the join got written.
	// Well, this could be rather CPU intensive... there's a lot
	// of fishing going on here.
	//
	// So - two steps. First, create a set of unjoined elements.
	HandleSet unjoined;
	for (const Handle& h : containers)
	{
		for (size_t i=0; i<_vsize; i++)
		{
			if (not any_atom_in_tree(h, trav.join_map[i]))
			{
				unjoined.insert(h);
				break;
			}
		}
	}

	// and now banish them
	HandleSet joined;
	std::set_difference(containers.begin(), containers.end(),
	                    unjoined.begin(), unjoined.end(),
	                    std::inserter(joined, joined.begin()));
	return joined;
}

/* ================================================================= */

/// Return the supremum of all the clauses. If there is only one
/// clause, it's easy, just get the set of principal elements for
/// that one clause, and we are done. If there is more than one
/// clause, then it's harder: we have to:
///
/// (1) Get the principal elements for each clause.
/// (2) Get the principal filters for each principal element.
/// (3) Intersect the filters to get the upper set of the clauses.
/// (4) Remove all elements that are not minimal.
///
/// The general concern here is that this algo is inefficient, but
/// I cannot think of any better way of doing it. In particular,
/// walking to the top for step (2) seems unavoidable, and I cannot
/// think of any way of combining steps (2) and (3) that would avoid
/// step (4) ... or even would reduce the work for stpe (4). Oh well.
///
/// TODO: it might be faster to use hash tables instead of rb-trees
/// i.e. to use UnorderedHandleSet instead of HandleSet. XXX FIXME.
HandleSet JoinLink::supremum(AtomSpace* as, bool silent,
                             Traverse& trav) const
{
	HandleSet upset = upper_set(as, silent, trav);

	// Create a set of non-minimal elements.
	HandleSet non_minimal;
	for (const Handle& h : upset)
	{
		if (h->is_node()) continue;
		for (const Handle& ho : h->getOutgoingSet())
		{
			if (upset.find(ho) != upset.end())
			{
				non_minimal.insert(h);
				break;
			}
		}
	}

	// Remove the non-minimal elements.
	HandleSet minimal;
	std::set_difference(upset.begin(), upset.end(),
	                    non_minimal.begin(), non_minimal.end(),
	                    std::inserter(minimal, minimal.begin()));
	return minimal;
}

/* ================================================================= */

/// find_top() - walk upwards from `h` and insert topmost atoms into
/// the container set.  This recursively walks to the top, until there
/// is nothing more above.
void JoinLink::find_top(HandleSet& containers, const Handle& h) const
{
	// Ignore other containers!
	if (nameserver().isA(h->get_type(), JOIN_LINK))
		return;

	IncomingSet is(h->getIncomingSet());
	if (0 == is.size())
	{
		containers.insert(h);
		return;
	}

	for (const Handle& ih: is)
		find_top(containers, ih);
}

/* ================================================================= */

HandleSet JoinLink::constrain(AtomSpace* as, bool silent,
                              const HandleSet& containers) const
{
	HandleSet rejects;
	for (const Handle& h : containers)
	{
		for (const Handle& toty : _top_types)
		{
			if (value_is_type(toty, h)) continue;
			rejects.insert(h);
			break;
		}
	}

	// Remove the rejects
	HandleSet accept;
	std::set_difference(containers.begin(), containers.end(),
	                    rejects.begin(), rejects.end(),
	                    std::inserter(accept, accept.begin()));
	return accept;
}

/* ================================================================= */

HandleSet JoinLink::container(AtomSpace* as, bool silent) const
{
	Traverse trav;
	HandleSet containers(supremum(as, silent, trav));
	if (MAXIMAL_JOIN_LINK == get_type())
	{
		HandleSet tops;
		for (const Handle& h: containers)
			find_top(tops, h);
		containers.swap(tops);
	}

	// Apply constraints on the top type, if any
	if (0 < _top_types.size())
		containers = constrain(as, silent, containers);

	// Perform the actual rewriting.
	fixup_replacements(trav);
	return replace(containers, trav);
}

/* ================================================================= */

/// Given a top-level set of containing links, perform
/// replacements, substituting the bottom-most atoms as requested,
/// while honoring all scoping and quoting.
HandleSet JoinLink::replace(const HandleSet& containers,
                            const Traverse& trav) const
{
	// Use the FreeVariables utility, so that all scoping and
	// quoting is handled correctly.
	HandleSet replaced;
	for (const Handle& top: containers)
	{
		Handle rep = FreeVariables::replace_nocheck(top, trav.replace_map);
		replaced.insert(rep);
	}

	return replaced;
}

/* ================================================================= */

QueueValuePtr JoinLink::do_execute(AtomSpace* as, bool silent)
{
	if (nullptr == as) as = _atom_space;

	HandleSet hs = container(as, silent);

	// XXX FIXME this is really dumb, using a queue and then
	// copying things into it. Whatever. Fix this.
	QueueValuePtr qvp(createQueueValue());
	for (const Handle& h : hs)
		qvp->push(as->add_atom(h));

	qvp->close();
	return qvp;
}

ValuePtr JoinLink::execute(AtomSpace* as, bool silent)
{
	return do_execute(as, silent);
}

DEFINE_LINK_FACTORY(JoinLink, JOIN_LINK)

/* ===================== END OF FILE ===================== */

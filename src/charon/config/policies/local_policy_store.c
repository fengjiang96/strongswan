/**
 * @file local_policy_store.c
 *
 * @brief Implementation of local_policy_store_t.
 *
 */

/*
 * Copyright (C) 2006 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <string.h>

#include "local_policy_store.h"

#include <daemon.h>
#include <utils/linked_list.h>


typedef struct private_local_policy_store_t private_local_policy_store_t;

/**
 * Private data of an local_policy_store_t object
 */
struct private_local_policy_store_t {

	/**
	 * Public part
	 */
	local_policy_store_t public;
	
	/**
	 * list of policy_t's
	 */
	linked_list_t *policies;
	
	/**
	 * Mutex to exclusivly access list
	 */
	pthread_mutex_t mutex;
};

/**
 * Implementation of policy_store_t.add_policy.
 */
static void add_policy(private_local_policy_store_t *this, policy_t *policy)
{
	pthread_mutex_lock(&(this->mutex));
	this->policies->insert_last(this->policies, (void*)policy);
	pthread_mutex_unlock(&(this->mutex));
}

/**
 * Check if a policy contains traffic selectors
 */
static bool contains_traffic_selectors(policy_t *policy, bool mine, 
									   linked_list_t *ts, host_t *host)
{
	linked_list_t *selected;
	bool contains = FALSE;
	
	if (mine)
	{
		selected = policy->select_my_traffic_selectors(policy, ts, host);
	}
	else
	{
		selected = policy->select_other_traffic_selectors(policy, ts, host);
	}
	if (selected->get_count(selected))
	{
		contains = TRUE;
	}
	selected->destroy_offset(selected, offsetof(traffic_selector_t, destroy));
	return contains;
}

/**
 * Implementation of policy_store_t.get_policy.
 */
static policy_t *get_policy(private_local_policy_store_t *this, 
							identification_t *my_id, identification_t *other_id,
						    linked_list_t *my_ts, linked_list_t *other_ts,
						    host_t *my_host, host_t *other_host,
							linked_list_t *requested_ca_keyids)
{
	typedef enum {
		PRIO_UNDEFINED = 	0x00,
		PRIO_ID_ANY = 		0x01,
		PRIO_ID_MATCH =		PRIO_ID_ANY + MAX_WILDCARDS,
	} prio_t;

	prio_t best_prio = PRIO_UNDEFINED;

	iterator_t *iterator;
	policy_t *candidate;
	policy_t *found = NULL;
	
	DBG2(DBG_CFG, "searching policy for ID pair '%D'...'%D'", my_id, other_id);

	pthread_mutex_lock(&(this->mutex));
	iterator = this->policies->create_iterator(this->policies, TRUE);

	/* determine closest matching policy */
	while (iterator->iterate(iterator, (void**)&candidate))
	{
		identification_t *candidate_my_id;
		identification_t *candidate_other_id;
		int wildcards;

		candidate_my_id = candidate->get_my_id(candidate);
		candidate_other_id = candidate->get_other_id(candidate);

		/* my_id is either %any or if set must match exactly */
		if (candidate_my_id->matches(candidate_my_id, my_id, &wildcards))
		{
			prio_t prio = PRIO_UNDEFINED;

			/* wildcard match for other_id */
			if (other_id->matches(other_id, candidate_other_id, &wildcards))
			{
				prio = PRIO_ID_MATCH - wildcards;
			}
			
			/* only accept if traffic selectors match */
			if (!contains_traffic_selectors(candidate, TRUE, my_ts, my_host) ||
				!contains_traffic_selectors(candidate, FALSE, other_ts, other_host))
			{
				DBG2(DBG_CFG, "candidate '%s' inacceptable due traffic "
					 "selector mismatch", candidate->get_name(candidate));
				continue;
			}

			DBG2(DBG_CFG, "candidate policy '%s': '%D'...'%D' (prio=%d)",
				 candidate->get_name(candidate),
				 candidate_my_id, candidate_other_id, prio);

			if (prio > best_prio)
			{
				found = candidate;
				best_prio = prio;
			}
		}
	}
	iterator->destroy(iterator);
	
	if (found)
	{
		DBG1(DBG_CFG, "found matching policy '%s': '%D'...'%D' (prio=%d)",
			 found->get_name(found), found->get_my_id(found),
			 found->get_other_id(found), best_prio);
		/* give out a new reference to it */
		found->get_ref(found);
	}
	pthread_mutex_unlock(&(this->mutex));
	return found;
}

/**
 * Implementation of policy_store_t.get_policy_by_name.
 */
static policy_t *get_policy_by_name(private_local_policy_store_t *this, char *name)
{
	iterator_t *iterator;
	policy_t *current, *found = NULL;
	
	DBG2(DBG_CFG, "looking for policy '%s'", name);
	
	pthread_mutex_lock(&(this->mutex));
	iterator = this->policies->create_iterator(this->policies, TRUE);
	while (iterator->iterate(iterator, (void **)&current))
	{
		if (strcmp(current->get_name(current), name) == 0)
		{
			found = current;
		}
	}
	iterator->destroy(iterator);
	pthread_mutex_unlock(&(this->mutex));
	
	/* give out a new reference */
	found->get_ref(found);
	return found;
}

/**
 * Implementation of policy_store_t.delete_policy.
 */
static status_t delete_policy(private_local_policy_store_t *this, char *name)
{
	iterator_t *iterator;
	policy_t *current;
	bool found = FALSE;
	
	pthread_mutex_lock(&(this->mutex));
	iterator = this->policies->create_iterator(this->policies, TRUE);
	while (iterator->iterate(iterator, (void **)&current))
	{
		if (strcmp(current->get_name(current), name) == 0)
		{
			/* remove policy from list, and destroy it */
			iterator->remove(iterator);
			current->destroy(current);
			found = TRUE;
			/* we do not break here, as there may be multipe policies */
		}
	}
	iterator->destroy(iterator);
	pthread_mutex_unlock(&(this->mutex));
	if (found)
	{
		return SUCCESS;
	}
	return NOT_FOUND;
}

/**
 * Implementation of policy_store_t.create_iterator.
 */
static iterator_t* create_iterator(private_local_policy_store_t *this)
{
	return this->policies->create_iterator_locked(this->policies,
												  &this->mutex);
}

/**
 * Implementation of policy_store_t.destroy.
 */
static void destroy(private_local_policy_store_t *this)
{
	pthread_mutex_lock(&(this->mutex));
	this->policies->destroy_offset(this->policies, offsetof(policy_t, destroy));
	pthread_mutex_unlock(&(this->mutex));
	free(this);
}

/**
 * Described in header.
 */
local_policy_store_t *local_policy_store_create(void)
{
	private_local_policy_store_t *this = malloc_thing(private_local_policy_store_t);
	
	this->public.policy_store.add_policy = (void (*) (policy_store_t*,policy_t*))add_policy;
	this->public.policy_store.get_policy = (policy_t* (*) (policy_store_t*,identification_t*,identification_t*,
											linked_list_t*,linked_list_t*,host_t*,host_t*,linked_list_t*))get_policy;
	this->public.policy_store.get_policy_by_name = (policy_t* (*) (policy_store_t*,char*))get_policy_by_name;
	this->public.policy_store.delete_policy = (status_t (*) (policy_store_t*,char*))delete_policy;
	this->public.policy_store.create_iterator = (iterator_t* (*) (policy_store_t*))create_iterator;
	this->public.policy_store.destroy = (void (*) (policy_store_t*))destroy;
	
	/* private variables */
	this->policies = linked_list_create();
	pthread_mutex_init(&(this->mutex), NULL);
	
	return (&this->public);
}

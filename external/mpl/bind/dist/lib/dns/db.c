/*	$NetBSD: db.c,v 1.8 2022/09/23 12:15:29 christos Exp $	*/

/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*! \file */

/***
 *** Imports
 ***/

#include <inttypes.h>
#include <stdbool.h>

#include <isc/buffer.h>
#include <isc/mem.h>
#include <isc/once.h>
#include <isc/rwlock.h>
#include <isc/string.h>
#include <isc/util.h>

#include <dns/callbacks.h>
#include <dns/clientinfo.h>
#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/log.h>
#include <dns/master.h>
#include <dns/rdata.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/result.h>

/***
 *** Private Types
 ***/

struct dns_dbimplementation {
	const char *name;
	dns_dbcreatefunc_t create;
	isc_mem_t *mctx;
	void *driverarg;
	ISC_LINK(dns_dbimplementation_t) link;
};

/***
 *** Supported DB Implementations Registry
 ***/

/*
 * Built in database implementations are registered here.
 */

#include "rbtdb.h"

static ISC_LIST(dns_dbimplementation_t) implementations;
static isc_rwlock_t implock;
static isc_once_t once = ISC_ONCE_INIT;

static dns_dbimplementation_t rbtimp;

static void
initialize(void) {
	isc_rwlock_init(&implock, 0, 0);

	rbtimp.name = "rbt";
	rbtimp.create = dns_rbtdb_create;
	rbtimp.mctx = NULL;
	rbtimp.driverarg = NULL;
	ISC_LINK_INIT(&rbtimp, link);

	ISC_LIST_INIT(implementations);
	ISC_LIST_APPEND(implementations, &rbtimp, link);
}

static dns_dbimplementation_t *
impfind(const char *name) {
	dns_dbimplementation_t *imp;

	for (imp = ISC_LIST_HEAD(implementations); imp != NULL;
	     imp = ISC_LIST_NEXT(imp, link))
	{
		if (strcasecmp(name, imp->name) == 0) {
			return (imp);
		}
	}
	return (NULL);
}

/***
 *** Basic DB Methods
 ***/

isc_result_t
dns_db_create(isc_mem_t *mctx, const char *db_type, const dns_name_t *origin,
	      dns_dbtype_t type, dns_rdataclass_t rdclass, unsigned int argc,
	      char *argv[], dns_db_t **dbp) {
	dns_dbimplementation_t *impinfo;

	RUNTIME_CHECK(isc_once_do(&once, initialize) == ISC_R_SUCCESS);

	/*
	 * Create a new database using implementation 'db_type'.
	 */

	REQUIRE(dbp != NULL && *dbp == NULL);
	REQUIRE(dns_name_isabsolute(origin));

	RWLOCK(&implock, isc_rwlocktype_read);
	impinfo = impfind(db_type);
	if (impinfo != NULL) {
		isc_result_t result;
		result = ((impinfo->create)(mctx, origin, type, rdclass, argc,
					    argv, impinfo->driverarg, dbp));
		RWUNLOCK(&implock, isc_rwlocktype_read);
		return (result);
	}

	RWUNLOCK(&implock, isc_rwlocktype_read);

	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE, DNS_LOGMODULE_DB,
		      ISC_LOG_ERROR, "unsupported database type '%s'", db_type);

	return (ISC_R_NOTFOUND);
}

void
dns_db_attach(dns_db_t *source, dns_db_t **targetp) {
	/*
	 * Attach *targetp to source.
	 */

	REQUIRE(DNS_DB_VALID(source));
	REQUIRE(targetp != NULL && *targetp == NULL);

	(source->methods->attach)(source, targetp);

	ENSURE(*targetp == source);
}

void
dns_db_detach(dns_db_t **dbp) {
	/*
	 * Detach *dbp from its database.
	 */

	REQUIRE(dbp != NULL);
	REQUIRE(DNS_DB_VALID(*dbp));

	((*dbp)->methods->detach)(dbp);

	ENSURE(*dbp == NULL);
}

bool
dns_db_iscache(dns_db_t *db) {
	/*
	 * Does 'db' have cache semantics?
	 */

	REQUIRE(DNS_DB_VALID(db));

	if ((db->attributes & DNS_DBATTR_CACHE) != 0) {
		return (true);
	}

	return (false);
}

bool
dns_db_iszone(dns_db_t *db) {
	/*
	 * Does 'db' have zone semantics?
	 */

	REQUIRE(DNS_DB_VALID(db));

	if ((db->attributes & (DNS_DBATTR_CACHE | DNS_DBATTR_STUB)) == 0) {
		return (true);
	}

	return (false);
}

bool
dns_db_isstub(dns_db_t *db) {
	/*
	 * Does 'db' have stub semantics?
	 */

	REQUIRE(DNS_DB_VALID(db));

	if ((db->attributes & DNS_DBATTR_STUB) != 0) {
		return (true);
	}

	return (false);
}

bool
dns_db_isdnssec(dns_db_t *db) {
	/*
	 * Is 'db' secure or partially secure?
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) == 0);

	if (db->methods->isdnssec != NULL) {
		return ((db->methods->isdnssec)(db));
	}
	return ((db->methods->issecure)(db));
}

bool
dns_db_issecure(dns_db_t *db) {
	/*
	 * Is 'db' secure?
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) == 0);

	return ((db->methods->issecure)(db));
}

bool
dns_db_ispersistent(dns_db_t *db) {
	/*
	 * Is 'db' persistent?
	 */

	REQUIRE(DNS_DB_VALID(db));

	return ((db->methods->ispersistent)(db));
}

dns_name_t *
dns_db_origin(dns_db_t *db) {
	/*
	 * The origin of the database.
	 */

	REQUIRE(DNS_DB_VALID(db));

	return (&db->origin);
}

dns_rdataclass_t
dns_db_class(dns_db_t *db) {
	/*
	 * The class of the database.
	 */

	REQUIRE(DNS_DB_VALID(db));

	return (db->rdclass);
}

isc_result_t
dns_db_beginload(dns_db_t *db, dns_rdatacallbacks_t *callbacks) {
	/*
	 * Begin loading 'db'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(DNS_CALLBACK_VALID(callbacks));

	return ((db->methods->beginload)(db, callbacks));
}

isc_result_t
dns_db_endload(dns_db_t *db, dns_rdatacallbacks_t *callbacks) {
	dns_dbonupdatelistener_t *listener;

	/*
	 * Finish loading 'db'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(DNS_CALLBACK_VALID(callbacks));
	REQUIRE(callbacks->add_private != NULL);

	for (listener = ISC_LIST_HEAD(db->update_listeners); listener != NULL;
	     listener = ISC_LIST_NEXT(listener, link))
	{
		listener->onupdate(db, listener->onupdate_arg);
	}

	return ((db->methods->endload)(db, callbacks));
}

isc_result_t
dns_db_load(dns_db_t *db, const char *filename, dns_masterformat_t format,
	    unsigned int options) {
	isc_result_t result, eresult;
	dns_rdatacallbacks_t callbacks;

	/*
	 * Load master file 'filename' into 'db'.
	 */

	REQUIRE(DNS_DB_VALID(db));

	if ((db->attributes & DNS_DBATTR_CACHE) != 0) {
		options |= DNS_MASTER_AGETTL;
	}

	dns_rdatacallbacks_init(&callbacks);
	result = dns_db_beginload(db, &callbacks);
	if (result != ISC_R_SUCCESS) {
		return (result);
	}
	result = dns_master_loadfile(filename, &db->origin, &db->origin,
				     db->rdclass, options, 0, &callbacks, NULL,
				     NULL, db->mctx, format, 0);
	eresult = dns_db_endload(db, &callbacks);
	/*
	 * We always call dns_db_endload(), but we only want to return its
	 * result if dns_master_loadfile() succeeded.  If dns_master_loadfile()
	 * failed, we want to return the result code it gave us.
	 */
	if (eresult != ISC_R_SUCCESS &&
	    (result == ISC_R_SUCCESS || result == DNS_R_SEENINCLUDE))
	{
		result = eresult;
	}

	return (result);
}

isc_result_t
dns_db_serialize(dns_db_t *db, dns_dbversion_t *version, FILE *file) {
	REQUIRE(DNS_DB_VALID(db));
	if (db->methods->serialize == NULL) {
		return (ISC_R_NOTIMPLEMENTED);
	}
	return ((db->methods->serialize)(db, version, file));
}

isc_result_t
dns_db_dump(dns_db_t *db, dns_dbversion_t *version, const char *filename) {
	return ((db->methods->dump)(db, version, filename,
				    dns_masterformat_text));
}

/***
 *** Version Methods
 ***/

void
dns_db_currentversion(dns_db_t *db, dns_dbversion_t **versionp) {
	/*
	 * Open the current version for reading.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) == 0);
	REQUIRE(versionp != NULL && *versionp == NULL);

	(db->methods->currentversion)(db, versionp);
}

isc_result_t
dns_db_newversion(dns_db_t *db, dns_dbversion_t **versionp) {
	/*
	 * Open a new version for reading and writing.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) == 0);
	REQUIRE(versionp != NULL && *versionp == NULL);

	return ((db->methods->newversion)(db, versionp));
}

void
dns_db_attachversion(dns_db_t *db, dns_dbversion_t *source,
		     dns_dbversion_t **targetp) {
	/*
	 * Attach '*targetp' to 'source'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) == 0);
	REQUIRE(source != NULL);
	REQUIRE(targetp != NULL && *targetp == NULL);

	(db->methods->attachversion)(db, source, targetp);

	ENSURE(*targetp != NULL);
}

void
dns_db_closeversion(dns_db_t *db, dns_dbversion_t **versionp, bool commit) {
	dns_dbonupdatelistener_t *listener;

	/*
	 * Close version '*versionp'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) == 0);
	REQUIRE(versionp != NULL && *versionp != NULL);

	(db->methods->closeversion)(db, versionp, commit);

	if (commit) {
		for (listener = ISC_LIST_HEAD(db->update_listeners);
		     listener != NULL; listener = ISC_LIST_NEXT(listener, link))
		{
			listener->onupdate(db, listener->onupdate_arg);
		}
	}

	ENSURE(*versionp == NULL);
}

/***
 *** Node Methods
 ***/

isc_result_t
dns_db_findnode(dns_db_t *db, const dns_name_t *name, bool create,
		dns_dbnode_t **nodep) {
	/*
	 * Find the node with name 'name'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(nodep != NULL && *nodep == NULL);

	if (db->methods->findnode != NULL) {
		return ((db->methods->findnode)(db, name, create, nodep));
	} else {
		return ((db->methods->findnodeext)(db, name, create, NULL, NULL,
						   nodep));
	}
}

isc_result_t
dns_db_findnodeext(dns_db_t *db, const dns_name_t *name, bool create,
		   dns_clientinfomethods_t *methods,
		   dns_clientinfo_t *clientinfo, dns_dbnode_t **nodep) {
	/*
	 * Find the node with name 'name', passing 'arg' to the database
	 * implementation.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(nodep != NULL && *nodep == NULL);

	if (db->methods->findnodeext != NULL) {
		return ((db->methods->findnodeext)(db, name, create, methods,
						   clientinfo, nodep));
	} else {
		return ((db->methods->findnode)(db, name, create, nodep));
	}
}

isc_result_t
dns_db_findnsec3node(dns_db_t *db, const dns_name_t *name, bool create,
		     dns_dbnode_t **nodep) {
	/*
	 * Find the node with name 'name'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(nodep != NULL && *nodep == NULL);

	return ((db->methods->findnsec3node)(db, name, create, nodep));
}

isc_result_t
dns_db_find(dns_db_t *db, const dns_name_t *name, dns_dbversion_t *version,
	    dns_rdatatype_t type, unsigned int options, isc_stdtime_t now,
	    dns_dbnode_t **nodep, dns_name_t *foundname,
	    dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset) {
	/*
	 * Find the best match for 'name' and 'type' in version 'version'
	 * of 'db'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(type != dns_rdatatype_rrsig);
	REQUIRE(nodep == NULL || *nodep == NULL);
	REQUIRE(dns_name_hasbuffer(foundname));
	REQUIRE(rdataset == NULL || (DNS_RDATASET_VALID(rdataset) &&
				     !dns_rdataset_isassociated(rdataset)));
	REQUIRE(sigrdataset == NULL ||
		(DNS_RDATASET_VALID(sigrdataset) &&
		 !dns_rdataset_isassociated(sigrdataset)));

	if (db->methods->find != NULL) {
		return ((db->methods->find)(db, name, version, type, options,
					    now, nodep, foundname, rdataset,
					    sigrdataset));
	} else {
		return ((db->methods->findext)(db, name, version, type, options,
					       now, nodep, foundname, NULL,
					       NULL, rdataset, sigrdataset));
	}
}

isc_result_t
dns_db_findext(dns_db_t *db, const dns_name_t *name, dns_dbversion_t *version,
	       dns_rdatatype_t type, unsigned int options, isc_stdtime_t now,
	       dns_dbnode_t **nodep, dns_name_t *foundname,
	       dns_clientinfomethods_t *methods, dns_clientinfo_t *clientinfo,
	       dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset) {
	/*
	 * Find the best match for 'name' and 'type' in version 'version'
	 * of 'db', passing in 'arg'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(type != dns_rdatatype_rrsig);
	REQUIRE(nodep == NULL || *nodep == NULL);
	REQUIRE(dns_name_hasbuffer(foundname));
	REQUIRE(rdataset == NULL || (DNS_RDATASET_VALID(rdataset) &&
				     !dns_rdataset_isassociated(rdataset)));
	REQUIRE(sigrdataset == NULL ||
		(DNS_RDATASET_VALID(sigrdataset) &&
		 !dns_rdataset_isassociated(sigrdataset)));

	if (db->methods->findext != NULL) {
		return ((db->methods->findext)(
			db, name, version, type, options, now, nodep, foundname,
			methods, clientinfo, rdataset, sigrdataset));
	} else {
		return ((db->methods->find)(db, name, version, type, options,
					    now, nodep, foundname, rdataset,
					    sigrdataset));
	}
}

isc_result_t
dns_db_findzonecut(dns_db_t *db, const dns_name_t *name, unsigned int options,
		   isc_stdtime_t now, dns_dbnode_t **nodep,
		   dns_name_t *foundname, dns_name_t *dcname,
		   dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset) {
	/*
	 * Find the deepest known zonecut which encloses 'name' in 'db'.
	 * foundname is the zonecut, dcname is the deepest name we have
	 * in database that is part of queried name.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) != 0);
	REQUIRE(nodep == NULL || *nodep == NULL);
	REQUIRE(dns_name_hasbuffer(foundname));
	REQUIRE(sigrdataset == NULL ||
		(DNS_RDATASET_VALID(sigrdataset) &&
		 !dns_rdataset_isassociated(sigrdataset)));

	return ((db->methods->findzonecut)(db, name, options, now, nodep,
					   foundname, dcname, rdataset,
					   sigrdataset));
}

void
dns_db_attachnode(dns_db_t *db, dns_dbnode_t *source, dns_dbnode_t **targetp) {
	/*
	 * Attach *targetp to source.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(source != NULL);
	REQUIRE(targetp != NULL && *targetp == NULL);

	(db->methods->attachnode)(db, source, targetp);
}

void
dns_db_detachnode(dns_db_t *db, dns_dbnode_t **nodep) {
	/*
	 * Detach *nodep from its node.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(nodep != NULL && *nodep != NULL);

	(db->methods->detachnode)(db, nodep);

	ENSURE(*nodep == NULL);
}

void
dns_db_transfernode(dns_db_t *db, dns_dbnode_t **sourcep,
		    dns_dbnode_t **targetp) {
	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(targetp != NULL && *targetp == NULL);
	/*
	 * This doesn't check the implementation magic.  If we find that
	 * we need such checks in future then this will be done in the
	 * method.
	 */
	REQUIRE(sourcep != NULL && *sourcep != NULL);

	UNUSED(db);

	if (db->methods->transfernode == NULL) {
		*targetp = *sourcep;
		*sourcep = NULL;
	} else {
		(db->methods->transfernode)(db, sourcep, targetp);
	}

	ENSURE(*sourcep == NULL);
}

isc_result_t
dns_db_expirenode(dns_db_t *db, dns_dbnode_t *node, isc_stdtime_t now) {
	/*
	 * Mark as stale all records at 'node' which expire at or before 'now'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) != 0);
	REQUIRE(node != NULL);

	return ((db->methods->expirenode)(db, node, now));
}

void
dns_db_printnode(dns_db_t *db, dns_dbnode_t *node, FILE *out) {
	/*
	 * Print a textual representation of the contents of the node to
	 * 'out'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(node != NULL);

	(db->methods->printnode)(db, node, out);
}

/***
 *** DB Iterator Creation
 ***/

isc_result_t
dns_db_createiterator(dns_db_t *db, unsigned int flags,
		      dns_dbiterator_t **iteratorp) {
	/*
	 * Create an iterator for version 'version' of 'db'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(iteratorp != NULL && *iteratorp == NULL);

	return (db->methods->createiterator(db, flags, iteratorp));
}

/***
 *** Rdataset Methods
 ***/

isc_result_t
dns_db_findrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
		    dns_rdatatype_t type, dns_rdatatype_t covers,
		    isc_stdtime_t now, dns_rdataset_t *rdataset,
		    dns_rdataset_t *sigrdataset) {
	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(node != NULL);
	REQUIRE(DNS_RDATASET_VALID(rdataset));
	REQUIRE(!dns_rdataset_isassociated(rdataset));
	REQUIRE(covers == 0 || type == dns_rdatatype_rrsig);
	REQUIRE(type != dns_rdatatype_any);
	REQUIRE(sigrdataset == NULL ||
		(DNS_RDATASET_VALID(sigrdataset) &&
		 !dns_rdataset_isassociated(sigrdataset)));

	return ((db->methods->findrdataset)(db, node, version, type, covers,
					    now, rdataset, sigrdataset));
}

isc_result_t
dns_db_allrdatasets(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
		    isc_stdtime_t now, dns_rdatasetiter_t **iteratorp) {
	/*
	 * Make '*iteratorp' an rdataset iteratator for all rdatasets at
	 * 'node' in version 'version' of 'db'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(iteratorp != NULL && *iteratorp == NULL);

	return ((db->methods->allrdatasets)(db, node, version, now, iteratorp));
}

isc_result_t
dns_db_addrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
		   isc_stdtime_t now, dns_rdataset_t *rdataset,
		   unsigned int options, dns_rdataset_t *addedrdataset) {
	/*
	 * Add 'rdataset' to 'node' in version 'version' of 'db'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(node != NULL);
	REQUIRE(((db->attributes & DNS_DBATTR_CACHE) == 0 && version != NULL) ||
		((db->attributes & DNS_DBATTR_CACHE) != 0 && version == NULL &&
		 (options & DNS_DBADD_MERGE) == 0));
	REQUIRE((options & DNS_DBADD_EXACT) == 0 ||
		(options & DNS_DBADD_MERGE) != 0);
	REQUIRE(DNS_RDATASET_VALID(rdataset));
	REQUIRE(dns_rdataset_isassociated(rdataset));
	REQUIRE(rdataset->rdclass == db->rdclass);
	REQUIRE(addedrdataset == NULL ||
		(DNS_RDATASET_VALID(addedrdataset) &&
		 !dns_rdataset_isassociated(addedrdataset)));

	return ((db->methods->addrdataset)(db, node, version, now, rdataset,
					   options, addedrdataset));
}

isc_result_t
dns_db_subtractrdataset(dns_db_t *db, dns_dbnode_t *node,
			dns_dbversion_t *version, dns_rdataset_t *rdataset,
			unsigned int options, dns_rdataset_t *newrdataset) {
	/*
	 * Remove any rdata in 'rdataset' from 'node' in version 'version' of
	 * 'db'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(node != NULL);
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) == 0 && version != NULL);
	REQUIRE(DNS_RDATASET_VALID(rdataset));
	REQUIRE(dns_rdataset_isassociated(rdataset));
	REQUIRE(rdataset->rdclass == db->rdclass);
	REQUIRE(newrdataset == NULL ||
		(DNS_RDATASET_VALID(newrdataset) &&
		 !dns_rdataset_isassociated(newrdataset)));

	return ((db->methods->subtractrdataset)(db, node, version, rdataset,
						options, newrdataset));
}

isc_result_t
dns_db_deleterdataset(dns_db_t *db, dns_dbnode_t *node,
		      dns_dbversion_t *version, dns_rdatatype_t type,
		      dns_rdatatype_t covers) {
	/*
	 * Make it so that no rdataset of type 'type' exists at 'node' in
	 * version version 'version' of 'db'.
	 */

	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(node != NULL);
	REQUIRE(((db->attributes & DNS_DBATTR_CACHE) == 0 && version != NULL) ||
		((db->attributes & DNS_DBATTR_CACHE) != 0 && version == NULL));

	return ((db->methods->deleterdataset)(db, node, version, type, covers));
}

void
dns_db_overmem(dns_db_t *db, bool overmem) {
	REQUIRE(DNS_DB_VALID(db));

	(db->methods->overmem)(db, overmem);
}

isc_result_t
dns_db_getsoaserial(dns_db_t *db, dns_dbversion_t *ver, uint32_t *serialp) {
	isc_result_t result;
	dns_dbnode_t *node = NULL;
	dns_rdataset_t rdataset;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	isc_buffer_t buffer;

	REQUIRE(dns_db_iszone(db) || dns_db_isstub(db));

	result = dns_db_findnode(db, dns_db_origin(db), false, &node);
	if (result != ISC_R_SUCCESS) {
		return (result);
	}

	dns_rdataset_init(&rdataset);
	result = dns_db_findrdataset(db, node, ver, dns_rdatatype_soa, 0,
				     (isc_stdtime_t)0, &rdataset, NULL);
	if (result != ISC_R_SUCCESS) {
		goto freenode;
	}

	result = dns_rdataset_first(&rdataset);
	if (result != ISC_R_SUCCESS) {
		goto freerdataset;
	}
	dns_rdataset_current(&rdataset, &rdata);
	result = dns_rdataset_next(&rdataset);
	INSIST(result == ISC_R_NOMORE);

	INSIST(rdata.length > 20);
	isc_buffer_init(&buffer, rdata.data, rdata.length);
	isc_buffer_add(&buffer, rdata.length);
	isc_buffer_forward(&buffer, rdata.length - 20);
	*serialp = isc_buffer_getuint32(&buffer);

	result = ISC_R_SUCCESS;

freerdataset:
	dns_rdataset_disassociate(&rdataset);

freenode:
	dns_db_detachnode(db, &node);
	return (result);
}

unsigned int
dns_db_nodecount(dns_db_t *db) {
	REQUIRE(DNS_DB_VALID(db));

	return ((db->methods->nodecount)(db));
}

size_t
dns_db_hashsize(dns_db_t *db) {
	REQUIRE(DNS_DB_VALID(db));

	if (db->methods->hashsize == NULL) {
		return (0);
	}

	return ((db->methods->hashsize)(db));
}

isc_result_t
dns_db_adjusthashsize(dns_db_t *db, size_t size) {
	REQUIRE(DNS_DB_VALID(db));

	if (db->methods->adjusthashsize != NULL) {
		return ((db->methods->adjusthashsize)(db, size));
	}

	return (ISC_R_NOTIMPLEMENTED);
}

void
dns_db_settask(dns_db_t *db, isc_task_t *task) {
	REQUIRE(DNS_DB_VALID(db));

	(db->methods->settask)(db, task);
}

isc_result_t
dns_db_register(const char *name, dns_dbcreatefunc_t create, void *driverarg,
		isc_mem_t *mctx, dns_dbimplementation_t **dbimp) {
	dns_dbimplementation_t *imp;

	REQUIRE(name != NULL);
	REQUIRE(dbimp != NULL && *dbimp == NULL);

	RUNTIME_CHECK(isc_once_do(&once, initialize) == ISC_R_SUCCESS);

	RWLOCK(&implock, isc_rwlocktype_write);
	imp = impfind(name);
	if (imp != NULL) {
		RWUNLOCK(&implock, isc_rwlocktype_write);
		return (ISC_R_EXISTS);
	}

	imp = isc_mem_get(mctx, sizeof(dns_dbimplementation_t));
	imp->name = name;
	imp->create = create;
	imp->mctx = NULL;
	imp->driverarg = driverarg;
	isc_mem_attach(mctx, &imp->mctx);
	ISC_LINK_INIT(imp, link);
	ISC_LIST_APPEND(implementations, imp, link);
	RWUNLOCK(&implock, isc_rwlocktype_write);

	*dbimp = imp;

	return (ISC_R_SUCCESS);
}

void
dns_db_unregister(dns_dbimplementation_t **dbimp) {
	dns_dbimplementation_t *imp;

	REQUIRE(dbimp != NULL && *dbimp != NULL);

	RUNTIME_CHECK(isc_once_do(&once, initialize) == ISC_R_SUCCESS);

	imp = *dbimp;
	*dbimp = NULL;
	RWLOCK(&implock, isc_rwlocktype_write);
	ISC_LIST_UNLINK(implementations, imp, link);
	isc_mem_putanddetach(&imp->mctx, imp, sizeof(dns_dbimplementation_t));
	RWUNLOCK(&implock, isc_rwlocktype_write);
	ENSURE(*dbimp == NULL);
}

isc_result_t
dns_db_getoriginnode(dns_db_t *db, dns_dbnode_t **nodep) {
	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(dns_db_iszone(db));
	REQUIRE(nodep != NULL && *nodep == NULL);

	if (db->methods->getoriginnode != NULL) {
		return ((db->methods->getoriginnode)(db, nodep));
	}

	return (ISC_R_NOTFOUND);
}

dns_stats_t *
dns_db_getrrsetstats(dns_db_t *db) {
	REQUIRE(DNS_DB_VALID(db));

	if (db->methods->getrrsetstats != NULL) {
		return ((db->methods->getrrsetstats)(db));
	}

	return (NULL);
}

isc_result_t
dns_db_setcachestats(dns_db_t *db, isc_stats_t *stats) {
	REQUIRE(DNS_DB_VALID(db));

	if (db->methods->setcachestats != NULL) {
		return ((db->methods->setcachestats)(db, stats));
	}

	return (ISC_R_NOTIMPLEMENTED);
}

isc_result_t
dns_db_getnsec3parameters(dns_db_t *db, dns_dbversion_t *version,
			  dns_hash_t *hash, uint8_t *flags,
			  uint16_t *iterations, unsigned char *salt,
			  size_t *salt_length) {
	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(dns_db_iszone(db));

	if (db->methods->getnsec3parameters != NULL) {
		return ((db->methods->getnsec3parameters)(db, version, hash,
							  flags, iterations,
							  salt, salt_length));
	}

	return (ISC_R_NOTFOUND);
}

isc_result_t
dns_db_getsize(dns_db_t *db, dns_dbversion_t *version, uint64_t *records,
	       uint64_t *bytes) {
	REQUIRE(DNS_DB_VALID(db));
	REQUIRE(dns_db_iszone(db));

	if (db->methods->getsize != NULL) {
		return ((db->methods->getsize)(db, version, records, bytes));
	}

	return (ISC_R_NOTFOUND);
}

isc_result_t
dns_db_setsigningtime(dns_db_t *db, dns_rdataset_t *rdataset,
		      isc_stdtime_t resign) {
	if (db->methods->setsigningtime != NULL) {
		return ((db->methods->setsigningtime)(db, rdataset, resign));
	}
	return (ISC_R_NOTIMPLEMENTED);
}

isc_result_t
dns_db_getsigningtime(dns_db_t *db, dns_rdataset_t *rdataset,
		      dns_name_t *name) {
	if (db->methods->getsigningtime != NULL) {
		return ((db->methods->getsigningtime)(db, rdataset, name));
	}
	return (ISC_R_NOTFOUND);
}

void
dns_db_resigned(dns_db_t *db, dns_rdataset_t *rdataset,
		dns_dbversion_t *version) {
	if (db->methods->resigned != NULL) {
		(db->methods->resigned)(db, rdataset, version);
	}
}

/*
 * Attach a database to policy zone databases.
 * This should only happen when the caller has already ensured that
 * it is dealing with a database that understands response policy zones.
 */
void
dns_db_rpz_attach(dns_db_t *db, void *rpzs, uint8_t rpz_num) {
	REQUIRE(db->methods->rpz_attach != NULL);
	(db->methods->rpz_attach)(db, rpzs, rpz_num);
}

/*
 * Finish loading a response policy zone.
 */
isc_result_t
dns_db_rpz_ready(dns_db_t *db) {
	if (db->methods->rpz_ready == NULL) {
		return (ISC_R_SUCCESS);
	}
	return ((db->methods->rpz_ready)(db));
}

/**
 * Attach a notify-on-update function the database
 */
isc_result_t
dns_db_updatenotify_register(dns_db_t *db, dns_dbupdate_callback_t fn,
			     void *fn_arg) {
	dns_dbonupdatelistener_t *listener;

	REQUIRE(db != NULL);
	REQUIRE(fn != NULL);

	listener = isc_mem_get(db->mctx, sizeof(dns_dbonupdatelistener_t));

	listener->onupdate = fn;
	listener->onupdate_arg = fn_arg;

	ISC_LINK_INIT(listener, link);
	ISC_LIST_APPEND(db->update_listeners, listener, link);

	return (ISC_R_SUCCESS);
}

isc_result_t
dns_db_updatenotify_unregister(dns_db_t *db, dns_dbupdate_callback_t fn,
			       void *fn_arg) {
	dns_dbonupdatelistener_t *listener;

	REQUIRE(db != NULL);

	for (listener = ISC_LIST_HEAD(db->update_listeners); listener != NULL;
	     listener = ISC_LIST_NEXT(listener, link))
	{
		if ((listener->onupdate == fn) &&
		    (listener->onupdate_arg == fn_arg)) {
			ISC_LIST_UNLINK(db->update_listeners, listener, link);
			isc_mem_put(db->mctx, listener,
				    sizeof(dns_dbonupdatelistener_t));
			return (ISC_R_SUCCESS);
		}
	}

	return (ISC_R_NOTFOUND);
}

isc_result_t
dns_db_nodefullname(dns_db_t *db, dns_dbnode_t *node, dns_name_t *name) {
	REQUIRE(db != NULL);
	REQUIRE(node != NULL);
	REQUIRE(name != NULL);

	if (db->methods->nodefullname == NULL) {
		return (ISC_R_NOTIMPLEMENTED);
	}
	return ((db->methods->nodefullname)(db, node, name));
}

isc_result_t
dns_db_setservestalettl(dns_db_t *db, dns_ttl_t ttl) {
	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) != 0);

	if (db->methods->setservestalettl != NULL) {
		return ((db->methods->setservestalettl)(db, ttl));
	}
	return (ISC_R_NOTIMPLEMENTED);
}

isc_result_t
dns_db_getservestalettl(dns_db_t *db, dns_ttl_t *ttl) {
	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) != 0);

	if (db->methods->getservestalettl != NULL) {
		return ((db->methods->getservestalettl)(db, ttl));
	}
	return (ISC_R_NOTIMPLEMENTED);
}

isc_result_t
dns_db_setservestalerefresh(dns_db_t *db, uint32_t interval) {
	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) != 0);

	if (db->methods->setservestalerefresh != NULL) {
		return ((db->methods->setservestalerefresh)(db, interval));
	}
	return (ISC_R_NOTIMPLEMENTED);
}

isc_result_t
dns_db_getservestalerefresh(dns_db_t *db, uint32_t *interval) {
	REQUIRE(DNS_DB_VALID(db));
	REQUIRE((db->attributes & DNS_DBATTR_CACHE) != 0);

	if (db->methods->getservestalerefresh != NULL) {
		return ((db->methods->getservestalerefresh)(db, interval));
	}
	return (ISC_R_NOTIMPLEMENTED);
}

isc_result_t
dns_db_setgluecachestats(dns_db_t *db, isc_stats_t *stats) {
	REQUIRE(dns_db_iszone(db));
	REQUIRE(stats != NULL);

	if (db->methods->setgluecachestats != NULL) {
		return ((db->methods->setgluecachestats)(db, stats));
	}

	return (ISC_R_NOTIMPLEMENTED);
}

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <json.h>
#include <assert.h>

#include "rsyslog.h"
#include "srUtils.h"
#include "errmsg.h"
#include "lookup.h"
#include "msg.h"
#include "rsconf.h"
#include "dirty.h"
#include "unicode-helper.h"

/* definitions for objects we access */
DEFobjStaticHelpers
DEFobjCurrIf(errmsg)
DEFobjCurrIf(statsobj)

#define DYNSTATS_PARAM_NAME "name"
#define DYNSTATS_PARAM_RESETTABLE "resettable"
#define DYNSTATS_PARAM_MAX_CARDINALITY "maxCardinality"
#define DYNSTATS_PARAM_UNUSED_METRIC_LIFE "unusedMetricLife"

#define DYNSTATS_DEFAULT_RESETTABILITY 1
#define DYNSTATS_DEFAULT_MAX_CARDINALITY 2000
#define DYNSTATS_DEFAULT_UNUSED_METRIC_LIFE 60 /* minutes */

#define DYNSTATS_MAX_BUCKET_NS_METRIC_LENGTH 100
#define DYNSTATS_METRIC_NAME_SEPARATOR ':'

static struct cnfparamdescr modpdescr[] = {
	{ DYNSTATS_PARAM_NAME, eCmdHdlrString, CNFPARAM_REQUIRED },
	{ DYNSTATS_PARAM_RESETTABLE, eCmdHdlrBinary, 0 },
	{ DYNSTATS_PARAM_MAX_CARDINALITY, eCmdHdlrPositiveInt, 0},
	{ DYNSTATS_PARAM_UNUSED_METRIC_LIFE, eCmdHdlrPositiveInt, 0} /* in minutes */
};

static struct cnfparamblk modpblk =
{ CNFPARAMBLK_VERSION,
  sizeof(modpdescr)/sizeof(struct cnfparamdescr),
  modpdescr
};

rsRetVal
dynstatsClassInit(void) {
	DEFiRet;
	CHKiRet(objGetObjInterface(&obj));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(statsobj, CORE_COMPONENT));
finalize_it:
	RETiRet;
}

void
dynstats_destroyBucket(dynstats_bucket_t* b) {
    dynstats_ctr_t *ctr;
    dynstats_buckets_t *bkts;
    
	bkts = &loadConf->dynstats_buckets;

    hdestroy_r(&b->table);
    statsobj.Destruct(&b->stats);
    while(1) {
        ctr = SLIST_FIRST(&b->ctrs);
        if (ctr == NULL) {
            break;
        } else {
            SLIST_REMOVE_HEAD(&b->ctrs, link);
            free(ctr);
        }
    }
    free(b->name);
	pthread_rwlock_destroy(&b->lock);
    pthread_mutex_destroy(&b->mutMetricCount);
	statsobj.DestructCounter(bkts->global_stats, b->pOpsOverflowCtr);
	statsobj.DestructCounter(bkts->global_stats, b->pNewMetricAddCtr);
	free(b);
}

static rsRetVal
dynstats_addBucketMetrics(dynstats_buckets_t *bkts, dynstats_bucket_t *b, const uchar* name) {
	uchar *metric_name_buff, *metric_suffix;
	const uchar *suffix_litteral;
	int name_len;
	DEFiRet;

	name_len = ustrlen(name);
	CHKmalloc(metric_name_buff = malloc(name_len * sizeof(uchar) + DYNSTATS_MAX_BUCKET_NS_METRIC_LENGTH));

	ustrncpy(metric_name_buff, name, name_len);
	metric_suffix = metric_name_buff + name_len;
	*metric_suffix = DYNSTATS_METRIC_NAME_SEPARATOR;
	metric_suffix++;

	suffix_litteral = UCHAR_CONSTANT("ops_overflow");
	ustrncpy(metric_suffix, suffix_litteral, strlen(suffix_litteral));
	STATSCOUNTER_INIT(b->ctrOpsOverflow, b->mutCtrOpsOverflow);
	CHKiRet(statsobj.AddManagedCounter(bkts->global_stats, metric_name_buff, ctrType_IntCtr,
									   CTR_FLAG_RESETTABLE, &(b->ctrOpsOverflow), &b->pOpsOverflowCtr));

    suffix_litteral = UCHAR_CONSTANT("new_metric_add");
	ustrncpy(metric_suffix, suffix_litteral, strlen(suffix_litteral));
	STATSCOUNTER_INIT(b->ctrNewMetricAdd, b->mutCtrNewMetricAdd);
	CHKiRet(statsobj.AddManagedCounter(bkts->global_stats, metric_name_buff, ctrType_IntCtr,
									   CTR_FLAG_RESETTABLE, &(b->ctrNewMetricAdd), &b->pNewMetricAddCtr));
finalize_it:
	free(metric_name_buff);
    if (iRet != RS_RET_OK) {
        if (b->pOpsOverflowCtr != NULL) {
            statsobj.DestructCounter(bkts->global_stats, b->pOpsOverflowCtr);
        }
        if (b->pNewMetricAddCtr != NULL) {
            statsobj.DestructCounter(bkts->global_stats, b->pNewMetricAddCtr);
        }
    }
	RETiRet;
}

static rsRetVal
dynstats_resetBucket(dynstats_bucket_t *b, uint8_t do_purge) {
    dynstats_ctr_t *ctr;
    DEFiRet;
    pthread_rwlock_wrlock(&b->lock);
    if (do_purge) {
        hdestroy_r(&b->table);
        statsobj.Destruct(&b->stats);//TODO: optimize this, full destruct is not necessary
        while(1) {
            ctr = SLIST_FIRST(&b->ctrs);
            if (b == NULL) {
                break;
            } else {
                SLIST_REMOVE_HEAD(&b->ctrs, link);
                free(ctr);
            }
        }
    }
    ATOMIC_STORE_0_TO_INT(&b->metricCount, &b->mutMetricCount);
    CHKiRet(statsobj.Construct(&b->stats));
    CHKiRet(statsobj.SetOrigin(b->stats, UCHAR_CONSTANT("dynstats.bucket")));
    CHKiRet(statsobj.SetName(b->stats, b->name));
	CHKiRet(statsobj.ConstructFinalize(b->stats));
    SLIST_INIT(&b->ctrs);
    if (! hcreate_r(b->maxCardinality, &b->table)) {
		errmsg.LogError(errno, RS_RET_INTERNAL_ERROR, "error trying to initialize hash-table for dyn-stats bucket named: %s", b->name);
		ABORT_FINALIZE(RS_RET_INTERNAL_ERROR);
	}
finalize_it:
    pthread_rwlock_unlock(&b->lock);
    if (iRet != RS_RET_OK) {
        statsobj.Destruct(&b->stats);
    }
    RETiRet;
}

static rsRetVal
dynstats_newBucket(const uchar* name, uint8_t resettable, uint32_t maxCardinality, uint32_t unusedMetricLife) {
	dynstats_bucket_t *b;
	dynstats_buckets_t *bkts;
    uint8_t lock_initialized, metric_count_mutex_initialized;
	DEFiRet;

    lock_initialized = metric_count_mutex_initialized = 0;
    b = NULL;
    
	bkts = &loadConf->dynstats_buckets;

    if (bkts->initialized) {
        CHKmalloc(b = calloc(1, sizeof(dynstats_bucket_t)));
        b->resettable = resettable;
        b->maxCardinality = maxCardinality;
        b->unusedMetricLife = unusedMetricLife;
        CHKmalloc(b->name = ustrdup(name));

        pthread_rwlock_init(&b->lock, NULL);
        lock_initialized = 1;
        pthread_mutex_init(&b->mutMetricCount, NULL);
        metric_count_mutex_initialized = 1;
        CHKiRet(dynstats_resetBucket(b, 0));

        CHKiRet(dynstats_addBucketMetrics(bkts, b, name));

        timeoutComp(&b->metricCleanupTimeout, b->unusedMetricLife);
    
        pthread_rwlock_wrlock(&bkts->lock);
        SLIST_INSERT_HEAD(&bkts->list, b, link);
        pthread_rwlock_unlock(&bkts->lock);
    } else {
        errmsg.LogError(0, RS_RET_INTERNAL_ERROR, "dynstats: bucket creation failed, as global-initialization of buckets was unsuccessful");
        ABORT_FINALIZE(RS_RET_INTERNAL_ERROR);
    }
finalize_it:
    if (iRet != RS_RET_OK) {
        if (metric_count_mutex_initialized) {
            pthread_mutex_destroy(&b->mutMetricCount);
        }
        if (lock_initialized) {
            pthread_rwlock_destroy(&b->lock);
        }
        if (b != NULL) {
            free(b->name);
            free(b);
        }
    }
	RETiRet;
}

rsRetVal
dynstats_processCnf(struct cnfobj *o) {
	struct cnfparamvals *pvals;
	short i;
	uchar *name;
	uint8_t resettable = DYNSTATS_DEFAULT_RESETTABILITY;
	uint32_t maxCardinality = DYNSTATS_DEFAULT_MAX_CARDINALITY;
	uint32_t unusedMetricLife = DYNSTATS_DEFAULT_UNUSED_METRIC_LIFE;
	DEFiRet;

	pvals = nvlstGetParams(o->nvlst, &modpblk, NULL);
	if(pvals == NULL) {
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}
	
	for(i = 0 ; i < modpblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(modpblk.descr[i].name, DYNSTATS_PARAM_NAME)) {
			CHKmalloc(name = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL));
		} else if (!strcmp(modpblk.descr[i].name, DYNSTATS_PARAM_RESETTABLE)) {
			resettable = (pvals[i].val.d.n != 0);
		} else if (!strcmp(modpblk.descr[i].name, DYNSTATS_PARAM_MAX_CARDINALITY)) {
			maxCardinality = (uint32_t) pvals[i].val.d.n;
		} else if (!strcmp(modpblk.descr[i].name, DYNSTATS_PARAM_UNUSED_METRIC_LIFE)) {
			unusedMetricLife = (uint32_t) pvals[i].val.d.n;
		} else {
			dbgprintf("dyn_stats: program error, non-handled "
					  "param '%s'\n", modpblk.descr[i].name);
		}
	}
	CHKiRet(dynstats_newBucket(name, resettable, maxCardinality, unusedMetricLife));

finalize_it:
	cnfparamvalsDestruct(pvals, &modpblk);
	RETiRet;
}

static void
dynstats_resetIfExpired(dynstats_bucket_t *b) {
    if (timeoutVal(&b->metricCleanupTimeout) == 0) {
        dynstats_resetBucket(b, 1);
        timeoutComp(&b->metricCleanupTimeout, b->unusedMetricLife);
    }
}

void
dynstats_resetExpired() {
    dynstats_buckets_t *bkts;
	dynstats_bucket_t *b;
	bkts = &loadConf->dynstats_buckets;
    if (bkts->initialized) {
        pthread_rwlock_rdlock(&bkts->lock);
        SLIST_FOREACH(b, &bkts->list, link) {
            dynstats_resetIfExpired(b);
        }
        pthread_rwlock_unlock(&bkts->lock);
    }
}

static void
dynstats_readCallback(statsobj_t *ignore) {
    dynstats_resetExpired();
}

rsRetVal
dynstats_initCnf(dynstats_buckets_t *bkts) {
    DEFiRet;

    bkts->initialized = 0;
    
	SLIST_INIT(&bkts->list);
	CHKiRet(statsobj.Construct(&bkts->global_stats));
    CHKiRet(statsobj.SetOrigin(bkts->global_stats, UCHAR_CONSTANT("dynstats")));
    CHKiRet(statsobj.SetName(bkts->global_stats, UCHAR_CONSTANT("global")));
    statsobj.SetReadNotifier(bkts->global_stats, dynstats_readCallback);
    
	STATSCOUNTER_INIT(bkts->metricsAdded, bkts->mutMetricsAdded);
	CHKiRet(statsobj.AddCounter(bkts->global_stats, UCHAR_CONSTANT("metrics_added"),
								ctrType_IntCtr, CTR_FLAG_RESETTABLE, &(bkts->metricsAdded)));
	STATSCOUNTER_INIT(bkts->metricsPurged, bkts->mutMetricsPurged);
	CHKiRet(statsobj.AddCounter(bkts->global_stats, UCHAR_CONSTANT("metrics_purged"),
								ctrType_IntCtr, CTR_FLAG_RESETTABLE, &(bkts->metricsPurged)));
	CHKiRet(statsobj.ConstructFinalize(bkts->global_stats));
    pthread_rwlock_init(&bkts->lock, NULL);

    bkts->initialized = 1;
    
finalize_it:
    if (iRet != RS_RET_OK) {
        statsobj.Destruct(&bkts->global_stats);
    }
    RETiRet;
}

void
dynstats_destroyAllBuckets() {
	dynstats_buckets_t *bkts;
	dynstats_bucket_t *b;
	bkts = &loadConf->dynstats_buckets;
    if (bkts->initialized) {
        while(1) {
            b = SLIST_FIRST(&bkts->list);
            if (b == NULL) {
                break;
            } else {
                SLIST_REMOVE_HEAD(&bkts->list, link);
                dynstats_destroyBucket(b);
            }
        }
        pthread_rwlock_destroy(&bkts->lock);
    }
}

dynstats_bucket_t *
dynstats_findBucket(const uchar* name) {
	dynstats_buckets_t *bkts;
	dynstats_bucket_t *b;
	bkts = &loadConf->dynstats_buckets;
    if (bkts->initialized) {
        pthread_rwlock_rdlock(&bkts->lock);
        SLIST_FOREACH(b, &bkts->list, link) {
            if (! ustrcmp(name, b->name)) {
                break;
            }
        }
        pthread_rwlock_unlock(&bkts->lock);
    } else {
        b = NULL;
        errmsg.LogError(0, RS_RET_INTERNAL_ERROR, "dynstats: bucket lookup failed, as global-initialization of buckets was unsuccessful");
    }

    return b;
}

static rsRetVal
dynstats_createCtr(dynstats_bucket_t *b, const uchar* metric, dynstats_ctr_t **ctr) {
    DEFiRet;
    
    CHKmalloc(*ctr = calloc(1, sizeof(dynstats_ctr_t)));
	STATSCOUNTER_INIT((*ctr)->ctr, (*ctr)->mutCtr);
	CHKiRet(statsobj.AddManagedCounter(b->stats, metric, ctrType_IntCtr,
                                       b->resettable, &(*ctr)->ctr, &(*ctr)->pCtr));
finalize_it:
    if (iRet != RS_RET_OK) {
        free(ctr);
    }
    RETiRet;
}

static void
dynstats_destroyCtr(dynstats_bucket_t *b, dynstats_ctr_t *ctr) {
    statsobj.DestructCounter(b->stats, ctr->pCtr);
    free(ctr);
}

static rsRetVal
dynstats_addNewCtr(dynstats_bucket_t *b, const uchar* metric, uint8_t doInitialIncrement) {
    dynstats_ctr_t *ctr;
    dynstats_ctr_t *found_ctr;
    ENTRY lookup, *entry;
    int found, created;
    DEFiRet;

    found = created = 0;
    lookup.key = NULL;

    if (ATOMIC_FETCH_32BIT(&b->metricCount, &b->mutMetricCount) >= b->maxCardinality) {
        ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);
    }
    lookup.key = ustrdup(metric);
    
    CHKiRet(dynstats_createCtr(b, metric, &ctr));
    lookup.data = ctr;

    pthread_rwlock_wrlock(&b->lock);
    found = hsearch_r(lookup, FIND, &entry, &b->table);//TODO: see what happens on 2nd ENTER for same key, it may be simplifiable.
    if (found) {
        found_ctr = ((dynstats_ctr_t*) entry->data)->ctr;
        if (doInitialIncrement) {
            STATSCOUNTER_INC(found_ctr->ctr, found_ctr->mutCtr);
        }
    } else {
        created = hsearch_r(lookup, ENTER, &entry, &b->table);
        if (created) {
            SLIST_INSERT_HEAD(&b->ctrs, ctr, link);
            if (doInitialIncrement) {
                STATSCOUNTER_INC(ctr->ctr, ctr->mutCtr);
            }
        }
    }
    pthread_rwlock_unlock(&b->lock);

    if (found) {
        //ignore
    } else if (created) {
        ATOMIC_INC(&b->metricCount, &b->mutMetricCount);
        STATSCOUNTER_INC(b->ctrNewMetricAdd, b->mutCtrNewMetricAdd);
    } else {
        ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);
    }
    
finalize_it:
    if (! created) {
        free(lookup.key);
        dynstats_destroyCtr(b, ctr);
    }
    RETiRet;
}

rsRetVal
dynstats_inc(dynstats_bucket_t *b, uchar* metric) {
    ENTRY lookup;
    ENTRY *found;
    int succeed;
    DEFiRet;

    lookup.key = metric;
    
    pthread_rwlock_rdlock(&b->lock);
    succeed = hsearch_r(lookup, FIND, &found, &b->table);
    if (succeed) {
        STATSCOUNTER_INC(((dynstats_ctr_t *) found->data)->ctr, ctr->mutCtr);
    }
    pthread_rwlock_unlock(&b->lock);

    if (!succeed) {
        CHKiRet(dynstats_addNewCtr(b, metric, 1));
    }
finalize_it:
    if (iRet != RS_RET_OK) {
        STATSCOUNTER_INC(b->ctrOpsOverflow, b->mutCtrOpsOverflow);
    }
    RETiRet;
}


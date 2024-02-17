/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "cluster.h"
#include "atomicvar.h"
#include "latency.h"
#include "script.h"
#include "functions.h"
#include "cluster.h"

#include <signal.h>
#include <ctype.h>

/* Structure for DB iterator that allows iterating across multiple slot specific dictionaries in cluster mode. */
struct dbIterator {
    redisDb *db;
    int slot;
    int next_slot;
    dictIterator di;
    dbKeyType keyType;
};

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

/* Flags for expireIfNeeded */
#define EXPIRE_FORCE_DELETE_EXPIRED 1
#define EXPIRE_AVOID_DELETE_EXPIRED 2

int expireIfNeeded(redisDb *db, robj *key, int flags);
int keyIsExpired(redisDb *db, robj *key);
void cumulativeKeyCountAdd(redisDb *db, int idx, long delta, dbKeyType keyType);
static void dbSetValue(redisDb *db, robj *key, robj *val, int overwrite, dictEntry *de);

dict *dbGetDictFromIterator(dbIterator *dbit) {
    if (dbit->keyType == DB_MAIN)
        return dbit->db->dict[dbit->slot];
    else if (dbit->keyType == DB_EXPIRES)
        return dbit->db->expires[dbit->slot];
    else
        serverPanic("Unknown keyType");
}

/* Returns next dictionary from the iterator, or NULL if iteration is complete. */
dict *dbIteratorNextDict(dbIterator *dbit) {
    if (dbit->next_slot == -1) return NULL;
    dbit->slot = dbit->next_slot;
    dbit->next_slot = dbGetNextNonEmptySlot(dbit->db, dbit->slot, dbit->keyType);
    return dbGetDictFromIterator(dbit);
}

int dbIteratorGetCurrentSlot(dbIterator *dbit) {
    serverAssert(dbit->slot >= 0 && dbit->slot < CLUSTER_SLOTS);
    return dbit->slot;
}

/* Returns next entry from the multi slot db. */
dictEntry *dbIteratorNext(dbIterator *dbit) {
    dictEntry *de = dbit->di.d ? dictNext(&dbit->di) : NULL;
    if (!de) { /* No current dict or reached the end of the dictionary. */
        dict *d = dbIteratorNextDict(dbit);
        if (!d) return NULL;

        if (dbit->di.d) {
            /* Before we move to the next dict, reset the iter of the previous dict. */
            dictIterator *iter = &dbit->di;
            dictResetIterator(iter);
        }
        dictInitSafeIterator(&dbit->di, d);
        de = dictNext(&dbit->di);
    }
    return de;
}

/* Returns DB iterator that can be used to iterate through sub-dictionaries.
 * Primary database contains only one dictionary when node runs without cluster mode,
 * or 16k dictionaries (one per slot) when node runs with cluster mode enabled.
 *
 * The caller should free the resulting dbit with dbReleaseIterator. */
dbIterator *dbIteratorInit(redisDb *db, dbKeyType keyType) {
    dbIterator *dbit = zmalloc(sizeof(*dbit));
    dbit->db = db;
    dbit->slot = -1;
    dbit->keyType = keyType;
    dbit->next_slot = findSlotByKeyIndex(dbit->db, 1, dbit->keyType); /* Finds first non-empty slot. */
    dictInitSafeIterator(&dbit->di, NULL);
    return dbit;
}

/* Free the dbit returned by dbIteratorInit. */
void dbReleaseIterator(dbIterator *dbit) {
    dictIterator *iter = &dbit->di;
    dictResetIterator(iter);

    zfree(dbit);
}

/* Returns next non-empty slot strictly after given one, or -1 if provided slot is the last one. */
int dbGetNextNonEmptySlot(redisDb *db, int slot, dbKeyType keyType) {
    unsigned long long next_key = cumulativeKeyCountRead(db, slot, keyType) + 1;
    return next_key <= dbSize(db, keyType) ? findSlotByKeyIndex(db, next_key, keyType) : -1;
}


/* Update LFU when an object is accessed.
 * Firstly, decrement the counter if the decrement time is reached.
 * Then logarithmically increment the counter, and update the access time. */
void updateLFU(robj *val) {
    unsigned long counter = LFUDecrAndReturn(val);
    counter = LFULogIncr(counter);
    val->lru = (LFUGetTimeInMinutes()<<8) | counter;
}

/* Lookup a key for read or write operations, or return NULL if the key is not
 * found in the specified DB. This function implements the functionality of
 * lookupKeyRead(), lookupKeyWrite() and their ...WithFlags() variants.
 *
 * Side-effects of calling this function:
 *
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key's last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 * 4. If keyspace notifications are enabled, a "keymiss" notification is fired.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): No special flags are passed.
 *  LOOKUP_NOTOUCH: Don't alter the last access time of the key.
 *  LOOKUP_NONOTIFY: Don't trigger keyspace event on key miss.
 *  LOOKUP_NOSTATS: Don't increment key hits/misses counters.
 *  LOOKUP_WRITE: Prepare the key for writing (delete expired keys even on
 *                replicas, use separate keyspace stats and events (TODO)).
 *  LOOKUP_NOEXPIRE: Perform expiration check, but avoid deleting the key,
 *                   so that we don't have to propagate the deletion.
 *
 * Note: this function also returns NULL if the key is logically expired but
 * still existing, in case this is a replica and the LOOKUP_WRITE is not set.
 * Even if the key expiry is master-driven, we can correctly report a key is
 * expired on replicas even if the master is lagging expiring our key via DELs
 * in the replication link. */
robj *lookupKey(redisDb *db, robj *key, int flags) {
    dictEntry *de = dbFind(db, key->ptr, DB_MAIN);
    robj *val = NULL;
    if (de) {
        val = dictGetVal(de);
        /* Forcing deletion of expired keys on a replica makes the replica
         * inconsistent with the master. We forbid it on readonly replicas, but
         * we have to allow it on writable replicas to make write commands
         * behave consistently.
         *
         * It's possible that the WRITE flag is set even during a readonly
         * command, since the command may trigger events that cause modules to
         * perform additional writes. */
        int is_ro_replica = server.masterhost && server.repl_slave_ro;
        int expire_flags = 0;
        if (flags & LOOKUP_WRITE && !is_ro_replica)
            expire_flags |= EXPIRE_FORCE_DELETE_EXPIRED;
        if (flags & LOOKUP_NOEXPIRE)
            expire_flags |= EXPIRE_AVOID_DELETE_EXPIRED;
        if (expireIfNeeded(db, key, expire_flags)) {
            /* The key is no longer valid. */
            val = NULL;
        }
    }

    if (val) {
        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        if (server.current_client && server.current_client->flags & CLIENT_NO_TOUCH &&
            server.current_client->cmd->proc != touchCommand)
            flags |= LOOKUP_NOTOUCH;
        if (!hasActiveChildProcess() && !(flags & LOOKUP_NOTOUCH)){
            if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
                updateLFU(val);
            } else {
                val->lru = LRU_CLOCK();
            }
        }

        if (!(flags & (LOOKUP_NOSTATS | LOOKUP_WRITE)))
            server.stat_keyspace_hits++;
        /* TODO: Use separate hits stats for WRITE */
    } else {
        if (!(flags & (LOOKUP_NONOTIFY | LOOKUP_WRITE)))
            notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
        if (!(flags & (LOOKUP_NOSTATS | LOOKUP_WRITE)))
            server.stat_keyspace_misses++;
        /* TODO: Use separate misses stats and notify event for WRITE */
    }

    return val;
}

/* Lookup a key for read operations, or return NULL if the key is not found
 * in the specified DB.
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * This function is equivalent to lookupKey(). The point of using this function
 * rather than lookupKey() directly is to indicate that the purpose is to read
 * the key. */
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    serverAssert(!(flags & LOOKUP_WRITE));
    return lookupKey(db, key, flags);
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the
 * common case. */
robj *lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* Lookup a key for write operations, and as a side effect, if needed, expires
 * the key if its TTL is reached. It's equivalent to lookupKey() with the
 * LOOKUP_WRITE flag added.
 *
 * Returns the linked value object if the key exists or NULL if the key
 * does not exist in the specified DB. */
robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags) {
    return lookupKey(db, key, flags | LOOKUP_WRITE);
}

robj *lookupKeyWrite(redisDb *db, robj *key) {
    return lookupKeyWriteWithFlags(db, key, LOOKUP_NONE);
}

robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReplyOrErrorObject(c, reply);
    return o;
}

robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReplyOrErrorObject(c, reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * If the update_if_existing argument is false, the program is aborted
 * if the key already exists, otherwise, it can fall back to dbOverwrite. */
static void dbAddInternal(redisDb *db, robj *key, robj *val, int update_if_existing) {
    dictEntry *existing;
    int slot = getKeySlot(key->ptr);
    dict *d = db->dict[slot];
    dictEntry *de = dictAddRaw(d, key->ptr, &existing);
    if (update_if_existing && existing) {
        dbSetValue(db, key, val, 1, existing);
        return;
    }
    serverAssertWithInfo(NULL, key, de != NULL);
    dictSetKey(d, de, sdsdup(key->ptr));
    initObjectLRUOrLFU(val);
    dictSetVal(d, de, val);
    cumulativeKeyCountAdd(db, slot, 1, DB_MAIN);
    signalKeyAsReady(db, key, val->type);
    notifyKeyspaceEvent(NOTIFY_NEW,"new",key,db->id);
}

void dbAdd(redisDb *db, robj *key, robj *val) {
    dbAddInternal(db, key, val, 0);
}

/* Returns key's hash slot when cluster mode is enabled, or 0 when disabled.
 * The only difference between this function and getKeySlot, is that it's not using cached key slot from the current_client
 * and always calculates CRC hash.
 * This is useful when slot needs to be calculated for a key that user didn't request for, such as in case of eviction. */
int calculateKeySlot(sds key) {
    return server.cluster_enabled ? keyHashSlot(key, (int) sdslen(key)) : 0;
}

/* Return slot-specific dictionary for key based on key's hash slot when cluster mode is enabled, else 0.*/
int getKeySlot(sds key) {
    /* This is performance optimization that uses pre-set slot id from the current command,
     * in order to avoid calculation of the key hash.
     * This optimization is only used when current_client flag `CLIENT_EXECUTING_COMMAND` is set.
     * It only gets set during the execution of command under `call` method. Other flows requesting
     * the key slot would fallback to calculateKeySlot.
     */
    if (server.current_client && server.current_client->slot >= 0 && server.current_client->flags & CLIENT_EXECUTING_COMMAND) {
        debugServerAssertWithInfo(server.current_client, NULL, calculateKeySlot(key)==server.current_client->slot);
        return server.current_client->slot;
    }
    return calculateKeySlot(key);
}

/* This is a special version of dbAdd() that is used only when loading
 * keys from the RDB file: the key is passed as an SDS string that is
 * retained by the function (and not freed by the caller).
 *
 * Moreover this function will not abort if the key is already busy, to
 * give more control to the caller, nor will signal the key as ready
 * since it is not useful in this context.
 *
 * The function returns 1 if the key was added to the database, taking
 * ownership of the SDS string, otherwise 0 is returned, and is up to the
 * caller to free the SDS string. */
int dbAddRDBLoad(redisDb *db, sds key, robj *val) {
    int slot = getKeySlot(key);
    dict *d = db->dict[slot];
    dictEntry *de = dictAddRaw(d, key, NULL);
    if (de == NULL) return 0;
    initObjectLRUOrLFU(val);
    dictSetVal(d, de, val);
    cumulativeKeyCountAdd(db, slot, 1, DB_MAIN);
    return 1;
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The 'overwrite' flag is an indication whether this is done as part of a
 * complete replacement of their key, which can be thought as a deletion and
 * replacement (in which case we need to emit deletion signals), or just an
 * update of a value of an existing key (when false).
 *
 * The dictEntry input is optional, can be used if we already have one.
 *
 * The program is aborted if the key was not already present. */
static void dbSetValue(redisDb *db, robj *key, robj *val, int overwrite, dictEntry *de) {
    if (!de) de = dbFind(db, key->ptr, DB_MAIN);
    serverAssertWithInfo(NULL,key,de != NULL);
    robj *old = dictGetVal(de);

    val->lru = old->lru;

    if (overwrite) {
        /* RM_StringDMA may call dbUnshareStringValue which may free val, so we
         * need to incr to retain old */
        incrRefCount(old);
        /* Although the key is not really deleted from the database, we regard
         * overwrite as two steps of unlink+add, so we still need to call the unlink
         * callback of the module. */
        moduleNotifyKeyUnlink(key,old,db->id,DB_FLAG_KEY_OVERWRITE);
        /* We want to try to unblock any module clients or clients using a blocking XREADGROUP */
        signalDeletedKeyAsReady(db,key,old->type);
        decrRefCount(old);
        /* Because of RM_StringDMA, old may be changed, so we need get old again */
        old = dictGetVal(de);
    }
    dict *d = db->dict[getKeySlot(key->ptr)];
    dictSetVal(d, de, val);

    if (server.lazyfree_lazy_server_del) {
        freeObjAsync(key,old,db->id);
    } else {
        /* This is just decrRefCount(old); */
        d->type->valDestructor(d, old);
    }
}

/* Replace an existing key with a new value, we just replace value and don't
 * emit any events */
void dbReplaceValue(redisDb *db, robj *key, robj *val) {
    dbSetValue(db, key, val, 0, NULL);
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent),
 *    unless 'SETKEY_KEEPTTL' is enabled in flags.
 * 4) The key lookup can take place outside this interface outcome will be
 *    delivered with 'SETKEY_ALREADY_EXIST' or 'SETKEY_DOESNT_EXIST'
 *
 * All the new keys in the database should be created via this interface.
 * The client 'c' argument may be set to NULL if the operation is performed
 * in a context where there is no clear client performing the operation. */
void setKey(client *c, redisDb *db, robj *key, robj *val, int flags) {
    int keyfound = 0;

    if (flags & SETKEY_ALREADY_EXIST)
        keyfound = 1;
    else if (flags & SETKEY_ADD_OR_UPDATE)
        keyfound = -1;
    else if (!(flags & SETKEY_DOESNT_EXIST))
        keyfound = (lookupKeyWrite(db,key) != NULL);

    if (!keyfound) {
        dbAdd(db,key,val);
    } else if (keyfound<0) {
        dbAddInternal(db,key,val,1);
    } else {
        dbSetValue(db,key,val,1,NULL);
    }
    incrRefCount(val);
    if (!(flags & SETKEY_KEEPTTL)) removeExpire(db,key);
    if (!(flags & SETKEY_NO_SIGNAL)) signalModifiedKey(c,db,key);
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * The function makes sure to return keys not already expired. */
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;
    int maxtries = 100;
    int allvolatile = dbSize(db, DB_MAIN) == dbSize(db, DB_EXPIRES);

    while(1) {
        sds key;
        robj *keyobj;
        int randomSlot = getFairRandomSlot(db, DB_MAIN);
        de = dictGetFairRandomKey(db->dict[randomSlot]);
        if (de == NULL) return NULL;

        key = dictGetKey(de);
        keyobj = createStringObject(key,sdslen(key));
        if (dbFind(db, key, DB_EXPIRES)) {
            if (allvolatile && server.masterhost && --maxtries == 0) {
                /* If the DB is composed only of keys with an expire set,
                 * it could happen that all the keys are already logically
                 * expired in the slave, so the function cannot stop because
                 * expireIfNeeded() is false, nor it can stop because
                 * dictGetFairRandomKey() returns NULL (there are keys to return).
                 * To prevent the infinite loop we do some tries, but if there
                 * are the conditions for an infinite loop, eventually we
                 * return a key name that may be already expired. */
                return keyobj;
            }
            if (expireIfNeeded(db,keyobj,0)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. */
            }
        }
        return keyobj;
    }
}

/* Updates binary index tree (also known as Fenwick tree), increasing key count for a given slot.
 * You can read more about this data structure here https://en.wikipedia.org/wiki/Fenwick_tree
 * Time complexity is O(log(CLUSTER_SLOTS)). */
void cumulativeKeyCountAdd(redisDb *db, int slot, long delta, dbKeyType keyType) {
    db->sub_dict[keyType].key_count += delta;
    dict *d = (keyType == DB_MAIN ? db->dict[slot] : db->expires[slot]);
    if (dictSize(d) == 1)
        db->sub_dict[keyType].non_empty_slots++;
    if (dictSize(d) == 0)
        db->sub_dict[keyType].non_empty_slots--;

    /* BIT does not need to be calculated when the cluster is turned off. */
    if (!server.cluster_enabled) return;
    int idx = slot + 1; /* Unlike slots, BIT is 1-based, so we need to add 1. */
    while (idx <= CLUSTER_SLOTS) {
        if (delta < 0) {
            serverAssert(db->sub_dict[keyType].slot_size_index[idx] >= (unsigned long long)labs(delta));
        }
        db->sub_dict[keyType].slot_size_index[idx] += delta;
        idx += (idx & -idx);
    }
}

/* Returns total (cumulative) number of keys up until given slot (inclusive).
 * Time complexity is O(log(CLUSTER_SLOTS)). */
unsigned long long cumulativeKeyCountRead(redisDb *db, int slot, dbKeyType keyType) {
    if (!server.cluster_enabled) {
        serverAssert(slot == 0);
        return dbSize(db, keyType);
    }
    int idx = slot + 1;
    unsigned long long sum = 0;
    while (idx > 0) {
        sum += db->sub_dict[keyType].slot_size_index[idx];
        idx -= (idx & -idx);
    }
    return sum;
}

/* Returns fair random slot, probability of each slot being returned is proportional to the number of elements that slot dictionary holds.
 * This function guarantees that it returns a slot whose dict is non-empty, unless the entire db is empty.
 * Time complexity of this function is O(log(CLUSTER_SLOTS)). */
int getFairRandomSlot(redisDb *db, dbKeyType keyType) {
    unsigned long target = dbSize(db, keyType) ? (randomULong() % dbSize(db, keyType)) + 1 : 0;
    int slot = findSlotByKeyIndex(db, target, keyType);
    return slot;
}

/* Finds a slot containing target element in a key space ordered by slot id.
 * Consider this example. Slots are represented by brackets and keys by dots:
 *  #0   #1   #2     #3    #4
 * [..][....][...][.......][.]
 *                    ^
 *                 target
 *
 * In this case slot #3 contains key that we are trying to find.
 * 
 * The return value is 0 based slot, and the range of the target is [1..dbSize], dbSize inclusive.
 * 
 * To find the slot, we start with the root node of the binary index tree and search through its children
 * from the highest index (2^14 in our case) to the lowest index. At each node, we check if the target 
 * value is greater than the node's value. If it is, we remove the node's value from the target and recursively
 * search for the new target using the current node as the parent.
 * Time complexity of this function is O(log(CLUSTER_SLOTS))
 */
int findSlotByKeyIndex(redisDb *db, unsigned long target, dbKeyType keyType) {
    if (!server.cluster_enabled || dbSize(db, keyType) == 0) return 0;
    serverAssert(target <= dbSize(db, keyType));

    int result = 0, bit_mask = 1 << CLUSTER_SLOT_MASK_BITS;
    for (int i = bit_mask; i != 0; i >>= 1) {
        int current = result + i;
        /* When the target index is greater than 'current' node value the we will update
         * the target and search in the 'current' node tree. */
        if (target > db->sub_dict[keyType].slot_size_index[current]) {
            target -= db->sub_dict[keyType].slot_size_index[current];
            result = current;
        }
    }
    /* Adjust the result to get the correct slot:
     * 1. result += 1;
     *    After the calculations, the index of target in slot_size_index should be the next one,
     *    so we should add 1.
     * 2. result -= 1;
     *    Unlike BIT(slot_size_index is 1-based), slots are 0-based, so we need to subtract 1.
     * As the addition and subtraction cancel each other out, we can simply return the result. */
    return result;
}

/* Helper for sync and async delete. */
int dbGenericDelete(redisDb *db, robj *key, int async, int flags) {
    dictEntry **plink;
    int table;
    int slot = getKeySlot(key->ptr);
    dict *d = db->dict[slot];
    dictEntry *de = dictTwoPhaseUnlinkFind(d,key->ptr,&plink,&table);
    if (de) {
        robj *val = dictGetVal(de);
        /* RM_StringDMA may call dbUnshareStringValue which may free val, so we
         * need to incr to retain val */
        incrRefCount(val);
        /* Tells the module that the key has been unlinked from the database. */
        moduleNotifyKeyUnlink(key,val,db->id,flags);
        /* We want to try to unblock any module clients or clients using a blocking XREADGROUP */
        signalDeletedKeyAsReady(db,key,val->type);
        /* We should call decr before freeObjAsync. If not, the refcount may be
         * greater than 1, so freeObjAsync doesn't work */
        decrRefCount(val);
        if (async) {
            /* Because of dbUnshareStringValue, the val in de may change. */
            freeObjAsync(key, dictGetVal(de), db->id);
            dictSetVal(d, de, NULL);
        }
        /* Deleting an entry from the expires dict will not free the sds of
         * the key, because it is shared with the main dictionary. */
        if (dictSize(db->expires[slot]) > 0) {
            if (dictDelete(db->expires[slot],key->ptr) == DICT_OK) {
                cumulativeKeyCountAdd(db, slot, -1, DB_EXPIRES);
            }
        }
        dictTwoPhaseUnlinkFree(d,de,plink,table);
        cumulativeKeyCountAdd(db, slot, -1, DB_MAIN);
        return 1;
    } else {
        return 0;
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
int dbSyncDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, 0, DB_FLAG_KEY_DELETED);
}

/* Delete a key, value, and associated expiration entry if any, from the DB. If
 * the value consists of many allocations, it may be freed asynchronously. */
int dbAsyncDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, 1, DB_FLAG_KEY_DELETED);
}

/* This is a wrapper whose behavior depends on the Redis lazy free
 * configuration. Deletes the key synchronously or asynchronously. */
int dbDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, server.lazyfree_lazy_server_del, DB_FLAG_KEY_DELETED);
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    serverAssert(o->type == OBJ_STRING);
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        dbReplaceValue(db,key,o);
    }
    return o;
}

/* Remove all keys from the database(s) structure. The dbarray argument
 * may not be the server main DBs (could be a temporary DB).
 *
 * The dbnum can be -1 if all the DBs should be emptied, or the specified
 * DB index if we want to empty only a single database.
 * The function returns the number of keys removed from the database(s). */
long long emptyDbStructure(redisDb *dbarray, int dbnum, int async,
                           void(callback)(dict*))
{
    long long removed = 0;
    int startdb, enddb;

    if (dbnum == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbnum;
    }

    for (int j = startdb; j <= enddb; j++) {
        removed += dbSize(&dbarray[j], DB_MAIN);
        if (async) {
            emptyDbAsync(&dbarray[j]);
        } else {
            dbDictMetadata *metadata;
            for (int k = 0; k < dbarray[j].dict_count; k++) {
                dictEmpty(dbarray[j].dict[k],callback);
                metadata = (dbDictMetadata *)dictMetadata(dbarray[j].dict[k]);
                if (metadata->rehashing_node) {
                    listDelNode(server.rehashing, metadata->rehashing_node);
                    metadata->rehashing_node = NULL;
                }

                dictEmpty(dbarray[j].expires[k],callback);
                metadata = (dbDictMetadata *)dictMetadata(dbarray[j].expires[k]);
                if (metadata->rehashing_node) {
                    listDelNode(server.rehashing, metadata->rehashing_node);
                    metadata->rehashing_node = NULL;
                }
            }
        }
        /* Because all keys of database are removed, reset average ttl. */
        dbarray[j].avg_ttl = 0;
        dbarray[j].expires_cursor = 0;
        for (dbKeyType subdict = DB_MAIN; subdict <= DB_EXPIRES; subdict++) {
            dbarray[j].sub_dict[subdict].non_empty_slots = 0;
            dbarray[j].sub_dict[subdict].key_count = 0;
            dbarray[j].sub_dict[subdict].resize_cursor = -1;
            if (server.cluster_enabled) {
                dbarray[j].sub_dict[subdict].bucket_count = 0;
                unsigned long long *slot_size_index = dbarray[j].sub_dict[subdict].slot_size_index;
                memset(slot_size_index, 0, sizeof(unsigned long long) * (CLUSTER_SLOTS + 1));
            }
        }
    }

    return removed;
}

/* Remove all data (keys and functions) from all the databases in a
 * Redis server. If callback is given the function is called from
 * time to time to signal that work is in progress.
 *
 * The dbnum can be -1 if all the DBs should be flushed, or the specified
 * DB number if we want to flush only a single Redis database number.
 *
 * Flags are be EMPTYDB_NO_FLAGS if no special flags are specified or
 * EMPTYDB_ASYNC if we want the memory to be freed in a different thread
 * and the function to return ASAP. EMPTYDB_NOFUNCTIONS can also be set
 * to specify that we do not want to delete the functions.
 *
 * On success the function returns the number of keys removed from the
 * database(s). Otherwise -1 is returned in the specific case the
 * DB number is out of range, and errno is set to EINVAL. */
long long emptyData(int dbnum, int flags, void(callback)(dict*)) {
    int async = (flags & EMPTYDB_ASYNC);
    int with_functions = !(flags & EMPTYDB_NOFUNCTIONS);
    RedisModuleFlushInfoV1 fi = {REDISMODULE_FLUSHINFO_VERSION,!async,dbnum};
    long long removed = 0;

    if (dbnum < -1 || dbnum >= server.dbnum) {
        errno = EINVAL;
        return -1;
    }

    /* Fire the flushdb modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_FLUSHDB,
                          REDISMODULE_SUBEVENT_FLUSHDB_START,
                          &fi);

    /* Make sure the WATCHed keys are affected by the FLUSH* commands.
     * Note that we need to call the function while the keys are still
     * there. */
    signalFlushedDb(dbnum, async);

    /* Empty redis database structure. */
    removed = emptyDbStructure(server.db, dbnum, async, callback);

    if (dbnum == -1) flushSlaveKeysWithExpireList();

    if (with_functions) {
        serverAssert(dbnum == -1);
        functionsLibCtxClearCurrent(async);
    }

    /* Also fire the end event. Note that this event will fire almost
     * immediately after the start event if the flush is asynchronous. */
    moduleFireServerEvent(REDISMODULE_EVENT_FLUSHDB,
                          REDISMODULE_SUBEVENT_FLUSHDB_END,
                          &fi);

    return removed;
}

/* Initialize temporary db on replica for use during diskless replication. */
redisDb *initTempDb(void) {
    redisDb *tempDb = zcalloc(sizeof(redisDb)*server.dbnum);
    for (int i=0; i<server.dbnum; i++) {
        tempDb[i].dict_count = (server.cluster_enabled) ? CLUSTER_SLOTS : 1;
        tempDb[i].dict = dictCreateMultiple(&dbDictType, tempDb[i].dict_count);
        tempDb[i].expires = dictCreateMultiple(&dbExpiresDictType, tempDb[i].dict_count);
        for (dbKeyType subdict = DB_MAIN; subdict <= DB_EXPIRES; subdict++) {
            tempDb[i].sub_dict[subdict].slot_size_index = server.cluster_enabled ? zcalloc(sizeof(unsigned long long) * (CLUSTER_SLOTS + 1)) : NULL;
        }
    }

    return tempDb;
}

/* Discard tempDb, this can be slow (similar to FLUSHALL), but it's always async. */
void discardTempDb(redisDb *tempDb, void(callback)(dict*)) {
    int async = 1;

    /* Release temp DBs. */
    emptyDbStructure(tempDb, -1, async, callback);
    for (int i=0; i<server.dbnum; i++) {
        for (int j=0; j<tempDb[i].dict_count; j++) {
            dictRelease(tempDb[i].dict[j]);
            dictRelease(tempDb[i].expires[j]);
        }
        zfree(tempDb[i].dict);
        zfree(tempDb[i].expires);
        for (dbKeyType subdict = DB_MAIN; subdict <= DB_EXPIRES; subdict++) {
            if (server.cluster_enabled) {
                zfree(tempDb[i].sub_dict[subdict].slot_size_index);
            }
        }
    }

    zfree(tempDb);
}

int selectDb(client *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return C_ERR;
    c->db = &server.db[id];
    return C_OK;
}

long long dbTotalServerKeyCount(void) {
    long long total = 0;
    int j;
    for (j = 0; j < server.dbnum; j++) {
        total += dbSize(&server.db[j], DB_MAIN);
    }
    return total;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/

/* Note that the 'c' argument may be NULL if the key was modified out of
 * a context of a client. */
void signalModifiedKey(client *c, redisDb *db, robj *key) {
    touchWatchedKey(db,key);
    trackingInvalidateKey(c,key,1);
}

void signalFlushedDb(int dbid, int async) {
    int startdb, enddb;
    if (dbid == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbid;
    }

    for (int j = startdb; j <= enddb; j++) {
        scanDatabaseForDeletedKeys(&server.db[j], NULL);
        touchAllWatchedKeysInDb(&server.db[j], NULL);
    }

    trackingInvalidateKeysOnFlush(async);

    /* Changes in this method may take place in swapMainDbWithTempDb as well,
     * where we execute similar calls, but with subtle differences as it's
     * not simply flushing db. */
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

/* Return the set of flags to use for the emptyData() call for FLUSHALL
 * and FLUSHDB commands.
 *
 * sync: flushes the database in an sync manner.
 * async: flushes the database in an async manner.
 * no option: determine sync or async according to the value of lazyfree-lazy-user-flush.
 *
 * On success C_OK is returned and the flags are stored in *flags, otherwise
 * C_ERR is returned and the function sends an error to the client. */
int getFlushCommandFlags(client *c, int *flags) {
    /* Parse the optional ASYNC option. */
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"sync")) {
        *flags = EMPTYDB_NO_FLAGS;
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"async")) {
        *flags = EMPTYDB_ASYNC;
    } else if (c->argc == 1) {
        *flags = server.lazyfree_lazy_user_flush ? EMPTYDB_ASYNC : EMPTYDB_NO_FLAGS;
    } else {
        addReplyErrorObject(c,shared.syntaxerr);
        return C_ERR;
    }
    return C_OK;
}

/* Flushes the whole server data set. */
void flushAllDataAndResetRDB(int flags) {
    server.dirty += emptyData(-1,flags,NULL);
    if (server.child_type == CHILD_TYPE_RDB) killRDBChild();
    if (server.saveparamslen > 0) {
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        rdbSave(SLAVE_REQ_NONE,server.rdb_filename,rsiptr,RDBFLAGS_NONE, getpid());
    }

#if defined(USE_JEMALLOC)
    /* jemalloc 5 doesn't release pages back to the OS when there's no traffic.
     * for large databases, flushdb blocks for long anyway, so a bit more won't
     * harm and this way the flush and purge will be synchronous. */
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
}

/* FLUSHDB [ASYNC]
 *
 * Flushes the currently SELECTed Redis DB. */
void flushdbCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    /* flushdb should not flush the functions */
    server.dirty += emptyData(c->db->id,flags | EMPTYDB_NOFUNCTIONS,NULL);

    /* Without the forceCommandPropagation, when DB was already empty,
     * FLUSHDB will not be replicated nor put into the AOF. */
    forceCommandPropagation(c, PROPAGATE_REPL | PROPAGATE_AOF);

    addReply(c,shared.ok);

#if defined(USE_JEMALLOC)
    /* jemalloc 5 doesn't release pages back to the OS when there's no traffic.
     * for large databases, flushdb blocks for long anyway, so a bit more won't
     * harm and this way the flush and purge will be synchronous. */
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
}

/* FLUSHALL [ASYNC]
 *
 * Flushes the whole server data set. */
void flushallCommand(client *c) {
    int flags;
    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    /* flushall should not flush the functions */
    flushAllDataAndResetRDB(flags | EMPTYDB_NOFUNCTIONS);

    /* Without the forceCommandPropagation, when DBs were already empty,
     * FLUSHALL will not be replicated nor put into the AOF. */
    forceCommandPropagation(c, PROPAGATE_REPL | PROPAGATE_AOF);

    addReply(c,shared.ok);
}

/* This command implements DEL and UNLINK. */
void delGenericCommand(client *c, int lazy) {
    int numdel = 0, j;

    for (j = 1; j < c->argc; j++) {
        expireIfNeeded(c->db,c->argv[j],0);
        int deleted  = lazy ? dbAsyncDelete(c->db,c->argv[j]) :
                              dbSyncDelete(c->db,c->argv[j]);
        if (deleted) {
            signalModifiedKey(c,c->db,c->argv[j]);
            notifyKeyspaceEvent(NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            server.dirty++;
            numdel++;
        }
    }
    addReplyLongLong(c,numdel);
}

void delCommand(client *c) {
    delGenericCommand(c,server.lazyfree_lazy_user_del);
}

void unlinkCommand(client *c) {
    delGenericCommand(c,1);
}

/* EXISTS key1 key2 ... key_N.
 * Return value is the number of keys existing. */
void existsCommand(client *c) {
    long long count = 0;
    int j;

    for (j = 1; j < c->argc; j++) {
        if (lookupKeyReadWithFlags(c->db,c->argv[j],LOOKUP_NOTOUCH)) count++;
    }
    addReplyLongLong(c,count);
}

void selectCommand(client *c) {
    int id;

    if (getIntFromObjectOrReply(c, c->argv[1], &id, NULL) != C_OK)
        return;

    if (server.cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
    if (selectDb(c,id) == C_ERR) {
        addReplyError(c,"DB index is out of range");
    } else {
        addReply(c,shared.ok);
    }
}

void randomkeyCommand(client *c) {
    robj *key;

    if ((key = dbRandomKey(c->db)) == NULL) {
        addReplyNull(c);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}

void keysCommand(client *c) {
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern), allkeys, pslot = -1;
    long numkeys = 0;
    void *replylen = addReplyDeferredLen(c);
    allkeys = (pattern[0] == '*' && plen == 1);
    if (server.cluster_enabled && !allkeys) {
        pslot = patternHashSlot(pattern, plen);
    }
    dictIterator *di = NULL;
    dbIterator *dbit = NULL;
    if (pslot != -1) {
        di = dictGetSafeIterator(c->db->dict[pslot]);
    } else {
        dbit = dbIteratorInit(c->db, DB_MAIN);
    }
    robj keyobj;
    while ((de = di ? dictNext(di) : dbIteratorNext(dbit)) != NULL) {
        sds key = dictGetKey(de);

        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            initStaticStringObject(keyobj, key);
            if (!keyIsExpired(c->db, &keyobj)) {
                addReplyBulkCBuffer(c, key, sdslen(key));
                numkeys++;
            }
        }
        if (c->flags & CLIENT_CLOSE_ASAP)
            break;
    }
    if (di)
        dictReleaseIterator(di);
    if (dbit)
        dbReleaseIterator(dbit);
    setDeferredArrayLen(c,replylen,numkeys);
}

/* Data used by the dict scan callback. */
typedef struct {
    list *keys;   /* elements that collect from dict */
    robj *o;      /* o must be a hash/set/zset object, NULL means current db */
    long long type; /* the particular type when scan the db */
    sds pattern;  /* pattern string, NULL means no pattern */
    long sampled; /* cumulative number of keys sampled */
} scanData;

/* Helper function to compare key type in scan commands */
int objectTypeCompare(robj *o, long long target) {
    if (o->type != OBJ_MODULE) {
        if (o->type != target) 
            return 0;
        else 
            return 1;
    }
    /* module type compare */
    long long mt = (long long)REDISMODULE_TYPE_SIGN(((moduleValue *)o->ptr)->type->id);
    if (target != -mt)
        return 0;
    else 
        return 1;
}
/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
void scanCallback(void *privdata, const dictEntry *de) {
    scanData *data = (scanData *)privdata;
    list *keys = data->keys;
    robj *o = data->o;
    sds val = NULL;
    sds key = NULL;
    data->sampled++;

    /* o and typename can not have values at the same time. */
    serverAssert(!((data->type != LLONG_MAX) && o));

    /* Filter an element if it isn't the type we want. */
    /* TODO: uncomment in redis 8.0
    if (!o && data->type != LLONG_MAX) {
        robj *rval = dictGetVal(de);
        if (!objectTypeCompare(rval, data->type)) return;
    }*/

    /* Filter element if it does not match the pattern. */
    sds keysds = dictGetKey(de);
    if (data->pattern) {
        if (!stringmatchlen(data->pattern, sdslen(data->pattern), keysds, sdslen(keysds), 0)) {
            return;
        }
    }

    if (o == NULL) {
        key = keysds;
    } else if (o->type == OBJ_SET) {
        key = keysds;
    } else if (o->type == OBJ_HASH) {
        key = keysds;
        val = dictGetVal(de);
    } else if (o->type == OBJ_ZSET) {
        char buf[MAX_LONG_DOUBLE_CHARS];
        int len = ld2string(buf, sizeof(buf), *(double *)dictGetVal(de), LD_STR_AUTO);
        key = sdsdup(keysds);
        val = sdsnewlen(buf, len);
    } else {
        serverPanic("Type not handled in SCAN callback.");
    }

    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns C_OK. Otherwise return C_ERR and send an error to the
 * client. */
int parseScanCursorOrReply(client *c, robj *o, unsigned long long *cursor) {
    if (!string2ull(o->ptr, cursor)) {
        addReplyError(c, "invalid cursor");
        return C_ERR;
    }
    return C_OK;
}

char *obj_type_name[OBJ_TYPE_MAX] = {
    "string", 
    "list", 
    "set", 
    "zset", 
    "hash", 
    NULL, /* module type is special */
    "stream"
};

/* Helper function to get type from a string in scan commands */
long long getObjectTypeByName(char *name) {

    for (long long i = 0; i < OBJ_TYPE_MAX; i++) {
        if (obj_type_name[i] && !strcasecmp(name, obj_type_name[i])) {
            return i;
        }
    }

    moduleType *mt = moduleTypeLookupModuleByNameIgnoreCase(name);
    if (mt != NULL) return -(REDISMODULE_TYPE_SIGN(mt->id));

    return LLONG_MAX;
}

char *getObjectTypeName(robj *o) {
    if (o == NULL) {
        return "none";
    }

    serverAssert(o->type >= 0 && o->type < OBJ_TYPE_MAX);

    if (o->type == OBJ_MODULE) {
        moduleValue *mv = o->ptr;
        return mv->type->name;
    } else {
        return obj_type_name[o->type];
    }
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be a Hash, Set or Zset object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash. */
void scanGenericCommand(client *c, robj *o, unsigned long long cursor) {
    int i, j;
    listNode *node;
    long count = 10;
    sds pat = NULL;
    sds typename = NULL;
    long long type = LLONG_MAX;
    int patlen = 0, use_pattern = 0;
    dict *ht;

    /* Object must be NULL (to iterate keys names), or the type of the object
     * must be Set, Sorted Set, or Hash. */
    serverAssert(o == NULL || o->type == OBJ_SET || o->type == OBJ_HASH ||
                o->type == OBJ_ZSET);

    /* Set i to the first option argument. The previous one is the cursor. */
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. */

    /* Step 1: Parse options. */
    while (i < c->argc) {
        j = c->argc - i;
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != C_OK)
            {
                return;
            }

            if (count < 1) {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            pat = c->argv[i+1]->ptr;
            patlen = sdslen(pat);

            /* The pattern always matches if it is exactly "*", so it is
             * equivalent to disabling it. */
            use_pattern = !(patlen == 1 && pat[0] == '*');

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "type") && o == NULL && j >= 2) {
            /* SCAN for a particular type only applies to the db dict */
            typename = c->argv[i+1]->ptr;
            type = getObjectTypeByName(typename);
            if (type == LLONG_MAX) {
                /* TODO: uncomment in redis 8.0
                addReplyErrorFormat(c, "unknown type name '%s'", typename);
                return; */
            }
            i+= 2;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    /* Step 2: Iterate the collection.
     *
     * Note that if the object is encoded with a listpack, intset, or any other
     * representation that is not a hash table, we are sure that it is also
     * composed of a small number of elements. So to avoid taking state we
     * just return everything inside the object in a single call, setting the
     * cursor to zero to signal the end of the iteration. */

    /* Handle the case of a hash table. */
    ht = NULL;
    if (o == NULL) {
        ht = NULL;
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
    } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
    } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        ht = zs->dict;
    }

    list *keys = listCreate();
    /* Set a free callback for the contents of the collected keys list.
     * For the main keyspace dict, and when we scan a key that's dict encoded
     * (we have 'ht'), we don't need to define free method because the strings
     * in the list are just a shallow copy from the pointer in the dictEntry.
     * When scanning a key with other encodings (e.g. listpack), we need to
     * free the temporary strings we add to that list.
     * The exception to the above is ZSET, where we do allocate temporary
     * strings even when scanning a dict. */
    if (o && (!ht || o->type == OBJ_ZSET)) {
        listSetFreeMethod(keys, (void (*)(void*))sdsfree);
    }

    /* For main dictionary scan or data structure using hashtable. */
    if (!o || ht) {
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. */
        long maxiterations = count*10;

        /* We pass scanData which have three pointers to the callback:
         * 1. data.keys: the list to which it will add new elements;
         * 2. data.o: the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way;
         * 3. data.type: the specified type scan in the db, LLONG_MAX means
         * type matching is no needed;
         * 4. data.pattern: the pattern string
         * 5. data.sampled: the maxiteration limit is there in case we're
         * working on an empty dict, one with a lot of empty buckets, and
         * for the buckets are not empty, we need to limit the spampled number
         * to prevent a long hang time caused by filtering too many keys*/
        scanData data = {
            .keys = keys,
            .o = o,
            .type = type,
            .pattern = use_pattern ? pat : NULL,
            .sampled = 0,
        };

        /* A pattern may restrict all matching keys to one cluster slot. */
        int onlyslot = -1;
        if (o == NULL && use_pattern && server.cluster_enabled) {
            onlyslot = patternHashSlot(pat, patlen);
        }
        do {
            /* In cluster mode there is a separate dictionary for each slot.
             * If cursor is empty, we should try exploring next non-empty slot. */
            if (o == NULL) {
                cursor = dbScan(c->db, DB_MAIN, cursor, onlyslot, scanCallback, NULL, &data);
            } else {
                cursor = dictScan(ht, cursor, scanCallback, &data);
            }
        } while (cursor && maxiterations-- && data.sampled < count);
    } else if (o->type == OBJ_SET) {
        char *str;
        char buf[LONG_STR_SIZE];
        size_t len;
        int64_t llele;
        setTypeIterator *si = setTypeInitIterator(o);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            if (str == NULL) {
                len = ll2string(buf, sizeof(buf), llele);
            }
            char *key = str ? str : buf;
            if (use_pattern && !stringmatchlen(pat, sdslen(pat), key, len, 0)) {
                continue;
            }
            listAddNodeTail(keys, sdsnewlen(key, len));
        }
        setTypeReleaseIterator(si);
        cursor = 0;
    } else if ((o->type == OBJ_HASH || o->type == OBJ_ZSET) &&
               o->encoding == OBJ_ENCODING_LISTPACK)
    {
        unsigned char *p = lpFirst(o->ptr);
        unsigned char *str;
        int64_t len;
        unsigned char intbuf[LP_INTBUF_SIZE];

        while(p) {
            str = lpGet(p, &len, intbuf);
            /* point to the value */
            p = lpNext(o->ptr, p);
            if (use_pattern && !stringmatchlen(pat, sdslen(pat), (char *)str, len, 0)) {
                /* jump to the next key/val pair */
                p = lpNext(o->ptr, p);
                continue;
            }
            /* add key object */
            listAddNodeTail(keys, sdsnewlen(str, len));
            /* add value object */
            str = lpGet(p, &len, intbuf);
            listAddNodeTail(keys, sdsnewlen(str, len));
            p = lpNext(o->ptr, p);
        }
        cursor = 0;
    } else {
        serverPanic("Not handled encoding in SCAN.");
    }

    /* Step 3: Filter the expired keys */
    if (o == NULL && listLength(keys)) {
        robj kobj;
        listIter li;
        listNode *ln;
        listRewind(keys, &li);
        while ((ln = listNext(&li))) {
            sds key = listNodeValue(ln);
            initStaticStringObject(kobj, key);
            /* Filter an element if it isn't the type we want. */
            /* TODO: remove this in redis 8.0 */
            if (typename) {
                robj* typecheck = lookupKeyReadWithFlags(c->db, &kobj, LOOKUP_NOTOUCH|LOOKUP_NONOTIFY);
                if (!typecheck || !objectTypeCompare(typecheck, type)) {
                    listDelNode(keys, ln);
                }
                continue;
            }
            if (expireIfNeeded(c->db, &kobj, 0)) {
                listDelNode(keys, ln);
            }
        }
    }

    /* Step 4: Reply to the client. */
    addReplyArrayLen(c, 2);
    addReplyBulkLongLong(c,cursor);

    addReplyArrayLen(c, listLength(keys));
    while ((node = listFirst(keys)) != NULL) {
        sds key = listNodeValue(node);
        addReplyBulkCBuffer(c, key, sdslen(key));
        listDelNode(keys, node);
    }

    listRelease(keys);
}

void addSlotIdToCursor(int slot, unsigned long long *cursor) {
    if (!server.cluster_enabled) return;
    /* Slot id can be -1 when iteration is over and there are no more slots to visit. */
    if (slot < 0) return;
    *cursor = (*cursor << CLUSTER_SLOT_MASK_BITS) | slot;
}

int getAndClearSlotIdFromCursor(unsigned long long *cursor) {
    if (!server.cluster_enabled) return 0;
    int slot = (int) (*cursor & CLUSTER_SLOT_MASK);
    *cursor = *cursor >> CLUSTER_SLOT_MASK_BITS;
    return slot;
}

/* The SCAN command completely relies on scanGenericCommand. */
void scanCommand(client *c) {
    unsigned long long cursor;
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == C_ERR) return;
    scanGenericCommand(c,NULL,cursor);
}

void dbsizeCommand(client *c) {
    redisDb *db = c->db;
    unsigned long long int size = dbSize(db, DB_MAIN);
    addReplyLongLong(c, size);
}

unsigned long long int dbSize(redisDb *db, dbKeyType keyType) {
    return db->sub_dict[keyType].key_count;
}

int dbNonEmptySlots(redisDb *db, dbKeyType keyType) {
    return db->sub_dict[keyType].non_empty_slots;
}

/* This method provides the cumulative sum of all the dictionary buckets
 * across dictionaries in a database. */
unsigned long dbBuckets(redisDb *db, dbKeyType keyType) {
    if (server.cluster_enabled) {
        return db->sub_dict[keyType].bucket_count;
    } else {
        if (keyType == DB_MAIN)
            return dictBuckets(db->dict[0]);
        else if (keyType == DB_EXPIRES)
            return dictBuckets(db->expires[0]);
        else
            serverPanic("Unknown keyType");
    }
}

size_t dbMemUsage(redisDb *db, dbKeyType keyType) {
    size_t mem = 0;
    unsigned long long keys_count = dbSize(db, keyType);
    mem += keys_count * dictEntryMemUsage() +
            dbBuckets(db, keyType) * sizeof(dictEntry*) +
            db->dict_count * (sizeof(dict) + dictMetadataSize(db->dict[0]));
    if (keyType == DB_MAIN) {
        mem+=keys_count * sizeof(robj);
    }
    return mem;
}

dictEntry *dbFind(redisDb *db, void *key, dbKeyType keyType){
    int slot = getKeySlot(key);
    if (keyType == DB_MAIN)
        return dictFind(db->dict[slot], key);
    else if (keyType == DB_EXPIRES)
        return dictFind(db->expires[slot], key);
    else
        serverPanic("Unknown keyType");
}

/* 
 * This method is used to iterate over the elements of the entire database specifically across slots.
 * It's a three pronged approach. 
 *
 * 1. It uses the provided cursor `v` to retrieve the slot from it. 
 * 2. If the dictionary is in a valid state checked through the provided callback `dictScanValidFunction`,
 *    it performs a dictScan over the appropriate `keyType` dictionary of `db`.
 * 3. If the slot is entirely scanned i.e. the cursor has reached 0, the next non empty slot is discovered. 
 *    The slot information is embedded into the cursor and returned.
 *
 * To restrict the scan to a single cluster slot, pass a valid slot as
 * 'onlyslot', otherwise pass -1.
 */
unsigned long long dbScan(redisDb *db, dbKeyType keyType, unsigned long long v,
                          int onlyslot, dictScanFunction *fn,
                          int (dictScanValidFunction)(dict *d), void *privdata) {
    dict *d;
    unsigned long long cursor = 0;
    /* During main dictionary traversal in cluster mode, 48 lower bits in the cursor are used for positioning in the HT.
     * Following 14 bits are used for the slot number, ranging from 0 to 2^14-1.
     * Slot is always 0 at the start of iteration and can be incremented only in cluster mode. */
    int slot = getAndClearSlotIdFromCursor(&v);
    if (onlyslot >= 0) {
        if (slot < onlyslot) {
            /* Fast-forward to onlyslot. */
            serverAssert(onlyslot < CLUSTER_SLOTS);
            slot = onlyslot;
            v = 0;
        } else if (slot > onlyslot) {
            /* The cursor is already past onlyslot. */
            return 0;
        }
    }
    if (keyType == DB_MAIN)
        d = db->dict[slot];
    else if (keyType == DB_EXPIRES)
        d = db->expires[slot];
    else
        serverPanic("Unknown keyType");

    int is_dict_valid = (dictScanValidFunction == NULL || dictScanValidFunction(d) == C_OK);
    if (is_dict_valid) {
        cursor = dictScan(d, v, fn, privdata);
    } else {
        serverLog(LL_DEBUG, "Slot [%d] not valid for scanning, skipping.", slot);
    }
    /* scanning done for the current dictionary or if the scanning wasn't possible, move to the next slot. */
    if (cursor == 0 || !is_dict_valid) {
        if (onlyslot >= 0)
            return 0;
        slot = dbGetNextNonEmptySlot(db, slot, keyType);
    }
    if (slot == -1) {
        return 0;
    }
    addSlotIdToCursor(slot, &cursor);
    return cursor;
}

void lastsaveCommand(client *c) {
    addReplyLongLong(c,server.lastsave);
}

void typeCommand(client *c) {
    robj *o;
    o = lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH);
    addReplyStatus(c, getObjectTypeName(o));
}

void shutdownCommand(client *c) {
    int flags = SHUTDOWN_NOFLAGS;
    int abort = 0;
    for (int i = 1; i < c->argc; i++) {
        if (!strcasecmp(c->argv[i]->ptr,"nosave")) {
            flags |= SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(c->argv[i]->ptr,"save")) {
            flags |= SHUTDOWN_SAVE;
        } else if (!strcasecmp(c->argv[i]->ptr, "now")) {
            flags |= SHUTDOWN_NOW;
        } else if (!strcasecmp(c->argv[i]->ptr, "force")) {
            flags |= SHUTDOWN_FORCE;
        } else if (!strcasecmp(c->argv[i]->ptr, "abort")) {
            abort = 1;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }
    if ((abort && flags != SHUTDOWN_NOFLAGS) ||
        (flags & SHUTDOWN_NOSAVE && flags & SHUTDOWN_SAVE))
    {
        /* Illegal combo. */
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    if (abort) {
        if (abortShutdown() == C_OK)
            addReply(c, shared.ok);
        else
            addReplyError(c, "No shutdown in progress.");
        return;
    }

    if (!(flags & SHUTDOWN_NOW) && c->flags & CLIENT_DENY_BLOCKING) {
        addReplyError(c, "SHUTDOWN without NOW or ABORT isn't allowed for DENY BLOCKING client");
        return;
    }

    if (!(flags & SHUTDOWN_NOSAVE) && isInsideYieldingLongCommand()) {
        /* Script timed out. Shutdown allowed only with the NOSAVE flag. See
         * also processCommand where these errors are returned. */
        if (server.busy_module_yield_flags && server.busy_module_yield_reply) {
            addReplyErrorFormat(c, "-BUSY %s", server.busy_module_yield_reply);
        } else if (server.busy_module_yield_flags) {
            addReplyErrorObject(c, shared.slowmoduleerr);
        } else if (scriptIsEval()) {
            addReplyErrorObject(c, shared.slowevalerr);
        } else {
            addReplyErrorObject(c, shared.slowscripterr);
        }
        return;
    }

    blockClientShutdown(c);
    if (prepareForShutdown(flags) == C_OK) exit(0);
    /* If we're here, then shutdown is ongoing (the client is still blocked) or
     * failed (the client has received an error). */
}

void renameGenericCommand(client *c, int nx) {
    robj *o;
    long long expire;
    int samekey = 0;

    /* When source and dest key is the same, no operation is performed,
     * if the key exists, however we still return an error on unexisting key. */
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) samekey = 1;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    if (samekey) {
        addReply(c,nx ? shared.czero : shared.ok);
        return;
    }

    incrRefCount(o);
    expire = getExpire(c->db,c->argv[1]);
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one
         * with the same name. */
        dbDelete(c->db,c->argv[2]);
    }
    dbAdd(c->db,c->argv[2],o);
    if (expire != -1) setExpire(c,c->db,c->argv[2],expire);
    dbDelete(c->db,c->argv[1]);
    signalModifiedKey(c,c->db,c->argv[1]);
    signalModifiedKey(c,c->db,c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

void renameCommand(client *c) {
    renameGenericCommand(c,0);
}

void renamenxCommand(client *c) {
    renameGenericCommand(c,1);
}

void moveCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid, dbid;
    long long expire;

    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers */
    src = c->db;
    srcid = c->db->id;

    if (getIntFromObjectOrReply(c, c->argv[2], &dbid, NULL) != C_OK)
        return;

    if (selectDb(c,dbid) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    }
    dst = c->db;
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    if (src == dst) {
        addReplyErrorObject(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
    expire = getExpire(c->db,c->argv[1]);

    /* Return zero if the key already exists in the target DB */
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    dbAdd(dst,c->argv[1],o);
    if (expire != -1) setExpire(c,dst,c->argv[1],expire);
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB */
    dbDelete(src,c->argv[1]);
    signalModifiedKey(c,src,c->argv[1]);
    signalModifiedKey(c,dst,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,
                "move_from",c->argv[1],src->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,
                "move_to",c->argv[1],dst->id);

    server.dirty++;
    addReply(c,shared.cone);
}

void copyCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid, dbid;
    long long expire;
    int j, replace = 0, delete = 0;

    /* Obtain source and target DB pointers 
     * Default target DB is the same as the source DB 
     * Parse the REPLACE option and targetDB option. */
    src = c->db;
    dst = c->db;
    srcid = c->db->id;
    dbid = c->db->id;
    for (j = 3; j < c->argc; j++) {
        int additional = c->argc - j - 1;
        if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr, "db") && additional >= 1) {
            if (getIntFromObjectOrReply(c, c->argv[j+1], &dbid, NULL) != C_OK)
                return;

            if (selectDb(c, dbid) == C_ERR) {
                addReplyError(c,"DB index is out of range");
                return;
            }
            dst = c->db;
            selectDb(c,srcid); /* Back to the source DB */
            j++; /* Consume additional arg. */
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    if ((server.cluster_enabled == 1) && (srcid != 0 || dbid != 0)) {
        addReplyError(c,"Copying to another database is not allowed in cluster mode");
        return;
    }

    /* If the user select the same DB as
     * the source DB and using newkey as the same key
     * it is probably an error. */
    robj *key = c->argv[1];
    robj *newkey = c->argv[2];
    if (src == dst && (sdscmp(key->ptr, newkey->ptr) == 0)) {
        addReplyErrorObject(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    o = lookupKeyRead(c->db, key);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
    expire = getExpire(c->db,key);

    /* Return zero if the key already exists in the target DB. 
     * If REPLACE option is selected, delete newkey from targetDB. */
    if (lookupKeyWrite(dst,newkey) != NULL) {
        if (replace) {
            delete = 1;
        } else {
            addReply(c,shared.czero);
            return;
        }
    }

    /* Duplicate object according to object's type. */
    robj *newobj;
    switch(o->type) {
        case OBJ_STRING: newobj = dupStringObject(o); break;
        case OBJ_LIST: newobj = listTypeDup(o); break;
        case OBJ_SET: newobj = setTypeDup(o); break;
        case OBJ_ZSET: newobj = zsetDup(o); break;
        case OBJ_HASH: newobj = hashTypeDup(o); break;
        case OBJ_STREAM: newobj = streamDup(o); break;
        case OBJ_MODULE:
            newobj = moduleTypeDupOrReply(c, key, newkey, dst->id, o);
            if (!newobj) return;
            break;
        default:
            addReplyError(c, "unknown type object");
            return;
    }

    if (delete) {
        dbDelete(dst,newkey);
    }

    dbAdd(dst,newkey,newobj);
    if (expire != -1) setExpire(c, dst, newkey, expire);

    /* OK! key copied */
    signalModifiedKey(c,dst,c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"copy_to",c->argv[2],dst->id);

    server.dirty++;
    addReply(c,shared.cone);
}

/* Helper function for dbSwapDatabases(): scans the list of keys that have
 * one or more blocked clients for B[LR]POP or other blocking commands
 * and signal the keys as ready if they are of the right type. See the comment
 * where the function is used for more info. */
void scanDatabaseForReadyKeys(redisDb *db) {
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(db->blocking_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        dictEntry *kde = dbFind(db, key->ptr, DB_MAIN);
        if (kde) {
            robj *value = dictGetVal(kde);
            signalKeyAsReady(db, key, value->type);
        }
    }
    dictReleaseIterator(di);
}

/* Since we are unblocking XREADGROUP clients in the event the
 * key was deleted/overwritten we must do the same in case the
 * database was flushed/swapped. */
void scanDatabaseForDeletedKeys(redisDb *emptied, redisDb *replaced_with) {
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(emptied->blocking_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        int existed = 0, exists = 0;
        int original_type = -1, curr_type = -1;

        dictEntry *kde = dbFind(emptied, key->ptr, DB_MAIN);
        if (kde) {
            robj *value = dictGetVal(kde);
            original_type = value->type;
            existed = 1;
        }

        if (replaced_with) {
            kde = dbFind(replaced_with, key->ptr, DB_MAIN);
            if (kde) {
                robj *value = dictGetVal(kde);
                curr_type = value->type;
                exists = 1;
            }
        }
        /* We want to try to unblock any client using a blocking XREADGROUP */
        if ((existed && !exists) || original_type != curr_type)
            signalDeletedKeyAsReady(emptied, key, original_type);
    }
    dictReleaseIterator(di);
}

/* Swap two databases at runtime so that all clients will magically see
 * the new database even if already connected. Note that the client
 * structure c->db points to a given DB, so we need to be smarter and
 * swap the underlying referenced structures, otherwise we would need
 * to fix all the references to the Redis DB structure.
 *
 * Returns C_ERR if at least one of the DB ids are out of range, otherwise
 * C_OK is returned. */
int dbSwapDatabases(int id1, int id2) {
    if (id1 < 0 || id1 >= server.dbnum ||
        id2 < 0 || id2 >= server.dbnum) return C_ERR;
    if (id1 == id2) return C_OK;
    redisDb aux = server.db[id1];
    redisDb *db1 = &server.db[id1], *db2 = &server.db[id2];

    /* Swapdb should make transaction fail if there is any
     * client watching keys */
    touchAllWatchedKeysInDb(db1, db2);
    touchAllWatchedKeysInDb(db2, db1);

    /* Try to unblock any XREADGROUP clients if the key no longer exists. */
    scanDatabaseForDeletedKeys(db1, db2);
    scanDatabaseForDeletedKeys(db2, db1);

    /* Swap hash tables. Note that we don't swap blocking_keys,
     * ready_keys and watched_keys, since we want clients to
     * remain in the same DB they were. */
    db1->dict = db2->dict;
    db1->expires = db2->expires;
    db1->avg_ttl = db2->avg_ttl;
    db1->expires_cursor = db2->expires_cursor;
    db1->dict_count = db2->dict_count;
    for (dbKeyType subdict = DB_MAIN; subdict <= DB_EXPIRES; subdict++) {
        db1->sub_dict[subdict].key_count = db2->sub_dict[subdict].key_count;
        db1->sub_dict[subdict].bucket_count = db2->sub_dict[subdict].bucket_count;
        db1->sub_dict[subdict].non_empty_slots = db2->sub_dict[subdict].non_empty_slots;
        db1->sub_dict[subdict].resize_cursor = db2->sub_dict[subdict].resize_cursor;
        db1->sub_dict[subdict].slot_size_index = db2->sub_dict[subdict].slot_size_index;
    }

    db2->dict = aux.dict;
    db2->expires = aux.expires;
    db2->avg_ttl = aux.avg_ttl;
    db2->expires_cursor = aux.expires_cursor;
    db2->dict_count = aux.dict_count;
    for (dbKeyType subdict = DB_MAIN; subdict <= DB_EXPIRES; subdict++) {
        db2->sub_dict[subdict].key_count = aux.sub_dict[subdict].key_count;
        db2->sub_dict[subdict].bucket_count = aux.sub_dict[subdict].bucket_count;
        db2->sub_dict[subdict].non_empty_slots = aux.sub_dict[subdict].non_empty_slots;
        db2->sub_dict[subdict].resize_cursor = aux.sub_dict[subdict].resize_cursor;
        db2->sub_dict[subdict].slot_size_index = aux.sub_dict[subdict].slot_size_index;
    }

    /* Now we need to handle clients blocked on lists: as an effect
     * of swapping the two DBs, a client that was waiting for list
     * X in a given DB, may now actually be unblocked if X happens
     * to exist in the new version of the DB, after the swap.
     *
     * However normally we only do this check for efficiency reasons
     * in dbAdd() when a list is created. So here we need to rescan
     * the list of clients blocked on lists and signal lists as ready
     * if needed. */
    scanDatabaseForReadyKeys(db1);
    scanDatabaseForReadyKeys(db2);
    return C_OK;
}

/* Logically, this discards (flushes) the old main database, and apply the newly loaded
 * database (temp) as the main (active) database, the actual freeing of old database
 * (which will now be placed in the temp one) is done later. */
void swapMainDbWithTempDb(redisDb *tempDb) {
    for (int i=0; i<server.dbnum; i++) {
        redisDb aux = server.db[i];
        redisDb *activedb = &server.db[i], *newdb = &tempDb[i];

        /* Swapping databases should make transaction fail if there is any
         * client watching keys. */
        touchAllWatchedKeysInDb(activedb, newdb);

        /* Try to unblock any XREADGROUP clients if the key no longer exists. */
        scanDatabaseForDeletedKeys(activedb, newdb);

        /* Swap hash tables. Note that we don't swap blocking_keys,
         * ready_keys and watched_keys, since clients 
         * remain in the same DB they were. */
        activedb->dict = newdb->dict;
        activedb->expires = newdb->expires;
        activedb->avg_ttl = newdb->avg_ttl;
        activedb->expires_cursor = newdb->expires_cursor;
        activedb->dict_count = newdb->dict_count;
        for (dbKeyType subdict = DB_MAIN; subdict <= DB_EXPIRES; subdict++) {
            activedb->sub_dict[subdict].key_count = newdb->sub_dict[subdict].key_count;
            activedb->sub_dict[subdict].bucket_count = newdb->sub_dict[subdict].bucket_count;
            activedb->sub_dict[subdict].non_empty_slots = newdb->sub_dict[subdict].non_empty_slots;
            activedb->sub_dict[subdict].resize_cursor = newdb->sub_dict[subdict].resize_cursor;
            activedb->sub_dict[subdict].slot_size_index = newdb->sub_dict[subdict].slot_size_index;
        }

        newdb->dict = aux.dict;
        newdb->expires = aux.expires;
        newdb->avg_ttl = aux.avg_ttl;
        newdb->expires_cursor = aux.expires_cursor;
        newdb->dict_count = aux.dict_count;
        for (dbKeyType subdict = DB_MAIN; subdict <= DB_EXPIRES; subdict++) {
            newdb->sub_dict[subdict].key_count = aux.sub_dict[subdict].key_count;
            newdb->sub_dict[subdict].bucket_count = aux.sub_dict[subdict].bucket_count;
            newdb->sub_dict[subdict].non_empty_slots = aux.sub_dict[subdict].non_empty_slots;
            newdb->sub_dict[subdict].resize_cursor = aux.sub_dict[subdict].resize_cursor;
            newdb->sub_dict[subdict].slot_size_index = aux.sub_dict[subdict].slot_size_index;
        }
        /* Now we need to handle clients blocked on lists: as an effect
         * of swapping the two DBs, a client that was waiting for list
         * X in a given DB, may now actually be unblocked if X happens
         * to exist in the new version of the DB, after the swap.
         *
         * However normally we only do this check for efficiency reasons
         * in dbAdd() when a list is created. So here we need to rescan
         * the list of clients blocked on lists and signal lists as ready
         * if needed. */
        scanDatabaseForReadyKeys(activedb);
    }

    trackingInvalidateKeysOnFlush(1);
    flushSlaveKeysWithExpireList();
}

/* SWAPDB db1 db2 */
void swapdbCommand(client *c) {
    int id1, id2;

    /* Not allowed in cluster mode: we have just DB 0 there. */
    if (server.cluster_enabled) {
        addReplyError(c,"SWAPDB is not allowed in cluster mode");
        return;
    }

    /* Get the two DBs indexes. */
    if (getIntFromObjectOrReply(c, c->argv[1], &id1,
        "invalid first DB index") != C_OK)
        return;

    if (getIntFromObjectOrReply(c, c->argv[2], &id2,
        "invalid second DB index") != C_OK)
        return;

    /* Swap... */
    if (dbSwapDatabases(id1,id2) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    } else {
        RedisModuleSwapDbInfo si = {REDISMODULE_SWAPDBINFO_VERSION,id1,id2};
        moduleFireServerEvent(REDISMODULE_EVENT_SWAPDB,0,&si);
        server.dirty++;
        addReply(c,shared.ok);
    }
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/

int removeExpire(redisDb *db, robj *key) {
    int slot = getKeySlot(key->ptr);
    if (dictDelete(db->expires[slot],key->ptr) == DICT_OK) {
        cumulativeKeyCountAdd(db, slot, -1, DB_EXPIRES);
        return 1;
    } else {
        return 0;
    }
}

/* Set an expire to the specified key. If the expire is set in the context
 * of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds
 * after which the key will no longer be considered valid. */
void setExpire(client *c, redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de, *existing;

    /* Reuse the sds from the main dict in the expire dict */
    kde = dbFind(db, key->ptr, DB_MAIN);
    serverAssertWithInfo(NULL,key,kde != NULL);
    int slot = getKeySlot(key->ptr);
    de = dictAddRaw(db->expires[slot], dictGetKey(kde), &existing);
    if (existing) {
        dictSetSignedIntegerVal(existing, when);
    } else {
        dictSetSignedIntegerVal(de, when);
        cumulativeKeyCountAdd(db, slot, 1, DB_EXPIRES);
    }

    int writable_slave = server.masterhost && server.repl_slave_ro == 0;
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires[getKeySlot(key->ptr)]) == 0 ||
       (de = dbFind(db,key->ptr, DB_EXPIRES)) == NULL) return -1;

    return dictGetSignedIntegerVal(de);
}

/* Delete the specified expired key and propagate expire. */
void deleteExpiredKeyAndPropagate(redisDb *db, robj *keyobj) {
    mstime_t expire_latency;
    latencyStartMonitor(expire_latency);
    dbGenericDelete(db,keyobj,server.lazyfree_lazy_expire,DB_FLAG_KEY_EXPIRED);
    latencyEndMonitor(expire_latency);
    latencyAddSampleIfNeeded("expire-del",expire_latency);
    notifyKeyspaceEvent(NOTIFY_EXPIRED,"expired",keyobj,db->id);
    signalModifiedKey(NULL, db, keyobj);
    propagateDeletion(db,keyobj,server.lazyfree_lazy_expire);
    server.stat_expiredkeys++;
}

/* Propagate an implicit key deletion into replicas and the AOF file.
 * When a key was deleted in the master by eviction, expiration or a similar
 * mechanism a DEL/UNLINK operation for this key is sent
 * to all the replicas and the AOF file if enabled.
 *
 * This way the key deletion is centralized in one place, and since both
 * AOF and the replication link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against deleted
 * keys.
 *
 * This function may be called from:
 * 1. Within call(): Example: Lazy-expire on key access.
 *    In this case the caller doesn't have to do anything
 *    because call() handles server.also_propagate(); or
 * 2. Outside of call(): Example: Active-expire, eviction, slot ownership changed.
 *    In this the caller must remember to call
 *    postExecutionUnitOperations, preferably just after a
 *    single deletion batch, so that DEL/UNLINK will NOT be wrapped
 *    in MULTI/EXEC */
void propagateDeletion(redisDb *db, robj *key, int lazy) {
    robj *argv[2];

    argv[0] = lazy ? shared.unlink : shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    /* If the master decided to delete a key we must propagate it to replicas no matter what.
     * Even if module executed a command without asking for propagation. */
    int prev_replication_allowed = server.replication_allowed;
    server.replication_allowed = 1;
    alsoPropagate(db->id,argv,2,PROPAGATE_AOF|PROPAGATE_REPL);
    server.replication_allowed = prev_replication_allowed;

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/* Check if the key is expired. */
int keyIsExpired(redisDb *db, robj *key) {
    /* Don't expire anything while loading. It will be done later. */
    if (server.loading) return 0;

    mstime_t when = getExpire(db,key);
    mstime_t now;

    if (when < 0) return 0; /* No expire for this key */

    now = commandTimeSnapshot();

    /* The key expired if the current (virtual or real) time is greater
     * than the expire time of the key. */
    return now > when;
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because by default replicas do not delete expired keys. They
 * wait for DELs from the master for consistency matters. However even
 * replicas will try to have a coherent return value for the function,
 * so that read commands executed in the replica side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * On replicas, this function does not delete expired keys by default, but
 * it still returns 1 if the key is logically expired. To force deletion
 * of logically expired keys even on replicas, use the EXPIRE_FORCE_DELETE_EXPIRED
 * flag. Note though that if the current client is executing
 * replicated commands from the master, keys are never considered expired.
 *
 * On the other hand, if you just want expiration check, but need to avoid
 * the actual key deletion and propagation of the deletion, use the
 * EXPIRE_AVOID_DELETE_EXPIRED flag.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */
int expireIfNeeded(redisDb *db, robj *key, int flags) {
    if (server.lazy_expire_disabled) return 0;
    if (!keyIsExpired(db,key)) return 0;

    /* If we are running in the context of a replica, instead of
     * evicting the expired key from the database, we return ASAP:
     * the replica key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys. The
     * exception is when write operations are performed on writable
     * replicas.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time.
     *
     * When replicating commands from the master, keys are never considered
     * expired. */
    if (server.masterhost != NULL) {
        if (server.current_client && (server.current_client->flags & CLIENT_MASTER)) return 0;
        if (!(flags & EXPIRE_FORCE_DELETE_EXPIRED)) return 1;
    }

    /* In some cases we're explicitly instructed to return an indication of a
     * missing key without actually deleting it, even on masters. */
    if (flags & EXPIRE_AVOID_DELETE_EXPIRED)
        return 1;

    /* If 'expire' action is paused, for whatever reason, then don't expire any key.
     * Typically, at the end of the pause we will properly expire the key OR we
     * will have failed over and the new primary will send us the expire. */
    if (isPausedActionsWithUpdate(PAUSE_ACTION_EXPIRE)) return 1;

    /* The key needs to be converted from static to heap before deleted */
    int static_key = key->refcount == OBJ_STATIC_REFCOUNT;
    if (static_key) {
        key = createStringObject(key->ptr, sdslen(key->ptr));
    }
    /* Delete the key */
    deleteExpiredKeyAndPropagate(db,key);
    if (static_key) {
        decrRefCount(key);
    }
    return 1;
}

/*
 * This functions increases size of the main/expires db to match desired number.
 * In cluster mode resizes all individual dictionaries for slots that this node owns.
 *
 * Based on the parameter `try_expand`, appropriate dict expand API is invoked.
 * if try_expand is set to 1, `dictTryExpand` is used else `dictExpand`.
 * The return code is either `DICT_OK`/`DICT_ERR` for both the API(s).
 * `DICT_OK` response is for successful expansion. However ,`DICT_ERR` response signifies failure in allocation in
 * `dictTryExpand` call and in case of `dictExpand` call it signifies no expansion was performed.
 */
int dbExpand(const redisDb *db, uint64_t db_size, dbKeyType keyType, int try_expand) {
    dict *d;
    if (server.cluster_enabled) {
        /* We don't know exact number of keys that would fall into each slot, but we can
         * approximate it, assuming even distribution, divide it by the number of slots. */
        int slots = getMyShardSlotCount();
        if (slots == 0) return C_OK;
        db_size = db_size / slots;

        for (int i = 0; i < CLUSTER_SLOTS; i++) {
            if (clusterNodeCoversSlot(getMyClusterNode(), i)) {
                if (keyType == DB_MAIN) {
                    d = db->dict[i];
                } else {
                    d = db->expires[i];
                }
                int result = try_expand ? dictTryExpand(d, db_size) : dictExpand(d, db_size);
                if (try_expand && result == DICT_ERR) {
                    serverLog(LL_WARNING, "Dict expansion failed for db type: %s, slot: %d",
                                keyType == DB_MAIN ? "main" : "expires", i);
                    return C_ERR;
                } else if (result == DICT_ERR) {
                    serverLog(LL_DEBUG, "Dict expansion skipped for db type: %s, slot: %d",
                                keyType == DB_MAIN ? "main" : "expires", i);
                }
            }
        }
    } else {
        if (keyType == DB_MAIN) {
            d = db->dict[0];
        } else {
            d = db->expires[0];
        }
        int result = try_expand ? dictTryExpand(d, db_size) : dictExpand(d, db_size);
        if (try_expand && result == DICT_ERR) {
            serverLog(LL_WARNING, "Dict expansion failed for db type: %s",
                        keyType == DB_MAIN ? "main" : "expires");
            return C_ERR;
        }
    }
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

/* Prepare the getKeysResult struct to hold numkeys, either by using the
 * pre-allocated keysbuf or by allocating a new array on the heap.
 *
 * This function must be called at least once before starting to populate
 * the result, and can be called repeatedly to enlarge the result array.
 */
keyReference *getKeysPrepareResult(getKeysResult *result, int numkeys) {
    /* GETKEYS_RESULT_INIT initializes keys to NULL, point it to the pre-allocated stack
     * buffer here. */
    if (!result->keys) {
        serverAssert(!result->numkeys);
        result->keys = result->keysbuf;
    }

    /* Resize if necessary */
    if (numkeys > result->size) {
        if (result->keys != result->keysbuf) {
            /* We're not using a static buffer, just (re)alloc */
            result->keys = zrealloc(result->keys, numkeys * sizeof(keyReference));
        } else {
            /* We are using a static buffer, copy its contents */
            result->keys = zmalloc(numkeys * sizeof(keyReference));
            if (result->numkeys)
                memcpy(result->keys, result->keysbuf, result->numkeys * sizeof(keyReference));
        }
        result->size = numkeys;
    }

    return result->keys;
}

/* Returns a bitmask with all the flags found in any of the key specs of the command.
 * The 'inv' argument means we'll return a mask with all flags that are missing in at least one spec. */
int64_t getAllKeySpecsFlags(struct redisCommand *cmd, int inv) {
    int64_t flags = 0;
    for (int j = 0; j < cmd->key_specs_num; j++) {
        keySpec *spec = cmd->key_specs + j;
        flags |= inv? ~spec->flags : spec->flags;
    }
    return flags;
}

/* Fetch the keys based of the provided key specs. Returns the number of keys found, or -1 on error.
 * There are several flags that can be used to modify how this function finds keys in a command.
 * 
 * GET_KEYSPEC_INCLUDE_NOT_KEYS: Return 'fake' keys as if they were keys.
 * GET_KEYSPEC_RETURN_PARTIAL:   Skips invalid and incomplete keyspecs but returns the keys
 *                               found in other valid keyspecs. 
 */
int getKeysUsingKeySpecs(struct redisCommand *cmd, robj **argv, int argc, int search_flags, getKeysResult *result) {
    int j, i, last, first, step;
    keyReference *keys;
    serverAssert(result->numkeys == 0); /* caller should initialize or reset it */

    for (j = 0; j < cmd->key_specs_num; j++) {
        keySpec *spec = cmd->key_specs + j;
        serverAssert(spec->begin_search_type != KSPEC_BS_INVALID);
        /* Skip specs that represent 'fake' keys */
        if ((spec->flags & CMD_KEY_NOT_KEY) && !(search_flags & GET_KEYSPEC_INCLUDE_NOT_KEYS)) {
            continue;
        }

        first = 0;
        if (spec->begin_search_type == KSPEC_BS_INDEX) {
            first = spec->bs.index.pos;
        } else if (spec->begin_search_type == KSPEC_BS_KEYWORD) {
            int start_index = spec->bs.keyword.startfrom > 0 ? spec->bs.keyword.startfrom : argc+spec->bs.keyword.startfrom;
            int end_index = spec->bs.keyword.startfrom > 0 ? argc-1: 1;
            for (i = start_index; i != end_index; i = start_index <= end_index ? i + 1 : i - 1) {
                if (i >= argc || i < 1)
                    break;
                if (!strcasecmp((char*)argv[i]->ptr,spec->bs.keyword.keyword)) {
                    first = i+1;
                    break;
                }
            }
            /* keyword not found */
            if (!first) {
                continue;
            }
        } else {
            /* unknown spec */
            goto invalid_spec;
        }

        if (spec->find_keys_type == KSPEC_FK_RANGE) {
            step = spec->fk.range.keystep;
            if (spec->fk.range.lastkey >= 0) {
                last = first + spec->fk.range.lastkey;
            } else {
                if (!spec->fk.range.limit) {
                    last = argc + spec->fk.range.lastkey;
                } else {
                    serverAssert(spec->fk.range.lastkey == -1);
                    last = first + ((argc-first)/spec->fk.range.limit + spec->fk.range.lastkey);
                }
            }
        } else if (spec->find_keys_type == KSPEC_FK_KEYNUM) {
            step = spec->fk.keynum.keystep;
            long long numkeys;
            if (spec->fk.keynum.keynumidx >= argc)
                goto invalid_spec;

            sds keynum_str = argv[first + spec->fk.keynum.keynumidx]->ptr;
            if (!string2ll(keynum_str,sdslen(keynum_str),&numkeys) || numkeys < 0) {
                /* Unable to parse the numkeys argument or it was invalid */
                goto invalid_spec;
            }

            first += spec->fk.keynum.firstkey;
            last = first + (int)numkeys-1;
        } else {
            /* unknown spec */
            goto invalid_spec;
        }

        int count = ((last - first)+1);
        keys = getKeysPrepareResult(result, result->numkeys + count);

        /* First or last is out of bounds, which indicates a syntax error */
        if (last >= argc || last < first || first >= argc) {
            goto invalid_spec;
        }

        for (i = first; i <= last; i += step) {
            if (i >= argc || i < first) {
                /* Modules commands, and standard commands with a not fixed number
                 * of arguments (negative arity parameter) do not have dispatch
                 * time arity checks, so we need to handle the case where the user
                 * passed an invalid number of arguments here. In this case we
                 * return no keys and expect the command implementation to report
                 * an arity or syntax error. */
                if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                    continue;
                } else {
                    serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
                }
            }
            keys[result->numkeys].pos = i;
            keys[result->numkeys].flags = spec->flags;
            result->numkeys++;
        }

        /* Handle incomplete specs (only after we added the current spec
         * to `keys`, just in case GET_KEYSPEC_RETURN_PARTIAL was given) */
        if (spec->flags & CMD_KEY_INCOMPLETE) {
            goto invalid_spec;
        }

        /* Done with this spec */
        continue;

invalid_spec:
        if (search_flags & GET_KEYSPEC_RETURN_PARTIAL) {
            continue;
        } else {
            result->numkeys = 0;
            return -1;
        }
    }

    return result->numkeys;
}

/* Return all the arguments that are keys in the command passed via argc / argv. 
 * This function will eventually replace getKeysFromCommand.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is a heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 * 
 * Along with the position, this command also returns the flags that are
 * associated with how Redis will access the key.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0]. */
int getKeysFromCommandWithSpecs(struct redisCommand *cmd, robj **argv, int argc, int search_flags, getKeysResult *result) {
    /* The command has at least one key-spec not marked as NOT_KEY */
    int has_keyspec = (getAllKeySpecsFlags(cmd, 1) & CMD_KEY_NOT_KEY);
    /* The command has at least one key-spec marked as VARIABLE_FLAGS */
    int has_varflags = (getAllKeySpecsFlags(cmd, 0) & CMD_KEY_VARIABLE_FLAGS);

    /* We prefer key-specs if there are any, and their flags are reliable. */
    if (has_keyspec && !has_varflags) {
        int ret = getKeysUsingKeySpecs(cmd,argv,argc,search_flags,result);
        if (ret >= 0)
            return ret;
        /* If the specs returned with an error (probably an INVALID or INCOMPLETE spec),
         * fallback to the callback method. */
    }

    /* Resort to getkeys callback methods. */
    if (cmd->flags & CMD_MODULE_GETKEYS)
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,result);

    /* We use native getkeys as a last resort, since not all these native getkeys provide
     * flags properly (only the ones that correspond to INVALID, INCOMPLETE or VARIABLE_FLAGS do.*/
    if (cmd->getkeys_proc)
        return cmd->getkeys_proc(cmd,argv,argc,result);
    return 0;
}

/* This function returns a sanity check if the command may have keys. */
int doesCommandHaveKeys(struct redisCommand *cmd) {
    return cmd->getkeys_proc ||                                 /* has getkeys_proc (non modules) */
        (cmd->flags & CMD_MODULE_GETKEYS) ||                    /* module with GETKEYS */
        (getAllKeySpecsFlags(cmd, 1) & CMD_KEY_NOT_KEY);        /* has at least one key-spec not marked as NOT_KEY */
}

/* A simplified channel spec table that contains all of the redis commands
 * and which channels they have and how they are accessed. */
typedef struct ChannelSpecs {
    redisCommandProc *proc; /* Command procedure to match against */
    uint64_t flags;         /* CMD_CHANNEL_* flags for this command */
    int start;              /* The initial position of the first channel */
    int count;              /* The number of channels, or -1 if all remaining
                             * arguments are channels. */
} ChannelSpecs;

ChannelSpecs commands_with_channels[] = {
    {subscribeCommand, CMD_CHANNEL_SUBSCRIBE, 1, -1},
    {ssubscribeCommand, CMD_CHANNEL_SUBSCRIBE, 1, -1},
    {unsubscribeCommand, CMD_CHANNEL_UNSUBSCRIBE, 1, -1},
    {sunsubscribeCommand, CMD_CHANNEL_UNSUBSCRIBE, 1, -1},
    {psubscribeCommand, CMD_CHANNEL_PATTERN | CMD_CHANNEL_SUBSCRIBE, 1, -1},
    {punsubscribeCommand, CMD_CHANNEL_PATTERN | CMD_CHANNEL_UNSUBSCRIBE, 1, -1},
    {publishCommand, CMD_CHANNEL_PUBLISH, 1, 1},
    {spublishCommand, CMD_CHANNEL_PUBLISH, 1, 1},
    {NULL,0} /* Terminator. */
};

/* Returns 1 if the command may access any channels matched by the flags
 * argument. */
int doesCommandHaveChannelsWithFlags(struct redisCommand *cmd, int flags) {
    /* If a module declares get channels, we are just going to assume
     * has channels. This API is allowed to return false positives. */
    if (cmd->flags & CMD_MODULE_GETCHANNELS) {
        return 1;
    }
    for (ChannelSpecs *spec = commands_with_channels; spec->proc != NULL; spec += 1) {
        if (cmd->proc == spec->proc) {
            return !!(spec->flags & flags);
        }
    }
    return 0;
}

/* Return all the arguments that are channels in the command passed via argc / argv. 
 * This function behaves similar to getKeysFromCommandWithSpecs, but with channels 
 * instead of keys.
 * 
 * The command returns the positions of all the channel arguments inside the array,
 * so the actual return value is a heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 * 
 * Along with the position, this command also returns the flags that are
 * associated with how Redis will access the channel.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0]. */
int getChannelsFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    keyReference *keys;
    /* If a module declares get channels, use that. */
    if (cmd->flags & CMD_MODULE_GETCHANNELS) {
        return moduleGetCommandChannelsViaAPI(cmd, argv, argc, result);
    }
    /* Otherwise check the channel spec table */
    for (ChannelSpecs *spec = commands_with_channels; spec != NULL; spec += 1) {
        if (cmd->proc == spec->proc) {
            int start = spec->start;
            int stop = (spec->count == -1) ? argc : start + spec->count;
            if (stop > argc) stop = argc;
            int count = 0;
            keys = getKeysPrepareResult(result, stop - start);
            for (int i = start; i < stop; i++ ) {
                keys[count].pos = i;
                keys[count++].flags = spec->flags;
            }
            result->numkeys = count;
            return count;
        }
    }
    return 0;
}

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step).
 * This function works only on command with the legacy_range_key_spec,
 * all other commands should be handled by getkeys_proc. 
 * 
 * If the commands keyspec is incomplete, no keys will be returned, and the provided
 * keys function should be called instead.
 * 
 * NOTE: This function does not guarantee populating the flags for 
 * the keys, in order to get flags you should use getKeysUsingKeySpecs. */
int getKeysUsingLegacyRangeSpec(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int j, i = 0, last, first, step;
    keyReference *keys;
    UNUSED(argv);

    if (cmd->legacy_range_key_spec.begin_search_type == KSPEC_BS_INVALID) {
        result->numkeys = 0;
        return 0;
    }

    first = cmd->legacy_range_key_spec.bs.index.pos;
    last = cmd->legacy_range_key_spec.fk.range.lastkey;
    if (last >= 0)
        last += first;
    step = cmd->legacy_range_key_spec.fk.range.keystep;

    if (last < 0) last = argc+last;

    int count = ((last - first)+1);
    keys = getKeysPrepareResult(result, count);

    for (j = first; j <= last; j += step) {
        if (j >= argc || j < first) {
            /* Modules commands, and standard commands with a not fixed number
             * of arguments (negative arity parameter) do not have dispatch
             * time arity checks, so we need to handle the case where the user
             * passed an invalid number of arguments here. In this case we
             * return no keys and expect the command implementation to report
             * an arity or syntax error. */
            if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                result->numkeys = 0;
                return 0;
            } else {
                serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
            }
        }
        keys[i].pos = j;
        /* Flags are omitted from legacy key specs */
        keys[i++].flags = 0;
    }
    result->numkeys = i;
    return i;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is a heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
int getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    if (cmd->flags & CMD_MODULE_GETKEYS) {
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,result);
    } else if (cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,result);
    } else {
        return getKeysUsingLegacyRangeSpec(cmd,argv,argc,result);
    }
}

/* Free the result of getKeysFromCommand. */
void getKeysFreeResult(getKeysResult *result) {
    if (result && result->keys != result->keysbuf)
        zfree(result->keys);
}

/* Helper function to extract keys from following commands:
 * COMMAND [destkey] <num-keys> <key> [...] <key> [...] ... <options>
 *
 * eg:
 * ZUNION <num-keys> <key> <key> ... <key> <options>
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 *
 * 'storeKeyOfs': destkey index, 0 means destkey not exists.
 * 'keyCountOfs': num-keys index.
 * 'firstKeyOfs': firstkey index.
 * 'keyStep': the interval of each key, usually this value is 1.
 * 
 * The commands using this function have a fully defined keyspec, so returning flags isn't needed. */
int genericGetKeys(int storeKeyOfs, int keyCountOfs, int firstKeyOfs, int keyStep,
                    robj **argv, int argc, getKeysResult *result) {
    int i, num;
    keyReference *keys;

    num = atoi(argv[keyCountOfs]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. (no input keys). */
    if (num < 1 || num > (argc - firstKeyOfs)/keyStep) {
        result->numkeys = 0;
        return 0;
    }

    int numkeys = storeKeyOfs ? num + 1 : num;
    keys = getKeysPrepareResult(result, numkeys);
    result->numkeys = numkeys;

    /* Add all key positions for argv[firstKeyOfs...n] to keys[] */
    for (i = 0; i < num; i++) {
        keys[i].pos = firstKeyOfs+(i*keyStep);
        keys[i].flags = 0;
    } 

    if (storeKeyOfs) {
        keys[num].pos = storeKeyOfs;
        keys[num].flags = 0;
    } 
    return result->numkeys;
}

int sintercardGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 1, 2, 1, argv, argc, result);
}

int zunionInterDiffStoreGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(1, 2, 3, 1, argv, argc, result);
}

int zunionInterDiffGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 1, 2, 1, argv, argc, result);
}

int evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 2, 3, 1, argv, argc, result);
}

int functionGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 2, 3, 1, argv, argc, result);
}

int lmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 1, 2, 1, argv, argc, result);
}

int blmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 2, 3, 1, argv, argc, result);
}

int zmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 1, 2, 1, argv, argc, result);
}

int bzmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 2, 3, 1, argv, argc, result);
}

/* Helper function to extract keys from the SORT RO command.
 *
 * SORT <sort-key>
 *
 * The second argument of SORT is always a key, however an arbitrary number of
 * keys may be accessed while doing the sort (the BY and GET args), so the
 * key-spec declares incomplete keys which is why we have to provide a concrete
 * implementation to fetch the keys.
 *
 * This command declares incomplete keys, so the flags are correctly set for this function */
int sortROGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    keyReference *keys;
    UNUSED(cmd);
    UNUSED(argv);
    UNUSED(argc);

    keys = getKeysPrepareResult(result, 1);
    keys[0].pos = 1; /* <sort-key> is always present. */
    keys[0].flags = CMD_KEY_RO | CMD_KEY_ACCESS;
    result->numkeys = 1;
    return result->numkeys;
}

/* Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. 
 * 
 * This command declares incomplete keys, so the flags are correctly set for this function */
int sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, j, num, found_store = 0;
    keyReference *keys;
    UNUSED(cmd);

    num = 0;
    keys = getKeysPrepareResult(result, 2); /* Alloc 2 places for the worst case. */
    keys[num].pos = 1; /* <sort-key> is always present. */
    keys[num++].flags = CMD_KEY_RO | CMD_KEY_ACCESS;

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. */
    struct {
        char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. */
    };

    for (i = 2; i < argc; i++) {
        for (j = 0; skiplist[j].name != NULL; j++) {
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;
                break;
            } else if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. */
                found_store = 1;
                keys[num].pos = i+1; /* <store-key> */
                keys[num].flags = CMD_KEY_OW | CMD_KEY_UPDATE;
                break;
            }
        }
    }
    result->numkeys = num + found_store;
    return result->numkeys;
}

/* This command declares incomplete keys, so the flags are correctly set for this function */
int migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, j, num, first;
    keyReference *keys;
    UNUSED(cmd);

    /* Assume the obvious form. */
    first = 3;
    num = 1;

    /* But check for the extended one with the KEYS option. */
    struct {
        char* name;
        int skip;
    } skip_keywords[] = {       
        {"copy", 0},
        {"replace", 0},
        {"auth", 1},
        {"auth2", 2},
        {NULL, 0}
    };
    if (argc > 6) {
        for (i = 6; i < argc; i++) {
            if (!strcasecmp(argv[i]->ptr, "keys")) {
                if (sdslen(argv[3]->ptr) > 0) {
                    /* This is a syntax error. So ignore the keys and leave
                     * the syntax error to be handled by migrateCommand. */
                    num = 0; 
                } else {
                    first = i + 1;
                    num = argc - first;
                }
                break;
            }
            for (j = 0; skip_keywords[j].name != NULL; j++) {
                if (!strcasecmp(argv[i]->ptr, skip_keywords[j].name)) {
                    i += skip_keywords[j].skip;
                    break;
                }
            }
        }
    }

    keys = getKeysPrepareResult(result, num);
    for (i = 0; i < num; i++) {
        keys[i].pos = first+i;
        keys[i].flags = CMD_KEY_RW | CMD_KEY_ACCESS | CMD_KEY_DELETE;
    } 
    result->numkeys = num;
    return num;
}

/* Helper function to extract keys from following commands:
 * GEORADIUS key x y radius unit [WITHDIST] [WITHHASH] [WITHCOORD] [ASC|DESC]
 *                             [COUNT count] [STORE key|STOREDIST key]
 * GEORADIUSBYMEMBER key member radius unit ... options ...
 * 
 * This command has a fully defined keyspec, so returning flags isn't needed. */
int georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num;
    keyReference *keys;
    UNUSED(cmd);

    /* Check for the presence of the stored key in the command */
    int stored_key = -1;
    for (i = 5; i < argc; i++) {
        char *arg = argv[i]->ptr;
        /* For the case when user specifies both "store" and "storedist" options, the
         * second key specified would override the first key. This behavior is kept
         * the same as in georadiusCommand method.
         */
        if ((!strcasecmp(arg, "store") || !strcasecmp(arg, "storedist")) && ((i+1) < argc)) {
            stored_key = i+1;
            i++;
        }
    }
    num = 1 + (stored_key == -1 ? 0 : 1);

    /* Keys in the command come from two places:
     * argv[1] = key,
     * argv[5...n] = stored key if present
     */
    keys = getKeysPrepareResult(result, num);

    /* Add all key positions to keys[] */
    keys[0].pos = 1;
    keys[0].flags = 0;
    if(num > 1) {
         keys[1].pos = stored_key;
         keys[1].flags = 0;
    }
    result->numkeys = num;
    return num;
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] [GROUP <groupname> <ttl>]
 *       STREAMS key_1 key_2 ... key_N ID_1 ID_2 ... ID_N
 *
 * This command has a fully defined keyspec, so returning flags isn't needed. */
int xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num = 0;
    keyReference *keys;
    UNUSED(cmd);

    /* We need to parse the options of the command in order to seek the first
     * "STREAMS" string which is actually the option. This is needed because
     * "STREAMS" could also be the name of the consumer group and even the
     * name of the stream key. */
    int streams_pos = -1;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i]->ptr;
        if (!strcasecmp(arg, "block")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "count")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "group")) {
            i += 2; /* Skip option argument. */
        } else if (!strcasecmp(arg, "noack")) {
            /* Nothing to do. */
        } else if (!strcasecmp(arg, "streams")) {
            streams_pos = i;
            break;
        } else {
            break; /* Syntax error. */
        }
    }
    if (streams_pos != -1) num = argc - streams_pos - 1;

    /* Syntax error. */
    if (streams_pos == -1 || num == 0 || num % 2 != 0) {
        result->numkeys = 0;
        return 0;
    }
    num /= 2; /* We have half the keys as there are arguments because
                 there are also the IDs, one per key. */

    keys = getKeysPrepareResult(result, num);
    for (i = streams_pos+1; i < argc-num; i++) {
        keys[i-streams_pos-1].pos = i;
        keys[i-streams_pos-1].flags = 0; 
    } 
    result->numkeys = num;
    return num;
}

/* Helper function to extract keys from the SET command, which may have
 * a read flag if the GET argument is passed in. */
int setGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    keyReference *keys;
    UNUSED(cmd);

    keys = getKeysPrepareResult(result, 1);
    keys[0].pos = 1; /* We always know the position */
    result->numkeys = 1;

    for (int i = 3; i < argc; i++) {
        char *arg = argv[i]->ptr;
        if ((arg[0] == 'g' || arg[0] == 'G') &&
            (arg[1] == 'e' || arg[1] == 'E') &&
            (arg[2] == 't' || arg[2] == 'T') && arg[3] == '\0')
        {
            keys[0].flags = CMD_KEY_RW | CMD_KEY_ACCESS | CMD_KEY_UPDATE;
            return 1;
        }
    }

    keys[0].flags = CMD_KEY_OW | CMD_KEY_UPDATE;
    return 1;
}

/* Helper function to extract keys from the BITFIELD command, which may be
 * read-only if the BITFIELD GET subcommand is used. */
int bitfieldGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    keyReference *keys;
    int readonly = 1;
    UNUSED(cmd);

    keys = getKeysPrepareResult(result, 1);
    keys[0].pos = 1; /* We always know the position */
    result->numkeys = 1;

    for (int i = 2; i < argc; i++) {
        int remargs = argc - i - 1; /* Remaining args other than current. */
        char *arg = argv[i]->ptr;
        if (!strcasecmp(arg, "get") && remargs >= 2) {
            i += 2;
        } else if ((!strcasecmp(arg, "set") || !strcasecmp(arg, "incrby")) && remargs >= 3) {
            readonly = 0;
            i += 3;
            break;
        } else if (!strcasecmp(arg, "overflow") && remargs >= 1) {
            i += 1;
        } else {
            readonly = 0; /* Syntax error. safer to assume non-RO. */
            break;
        }
    }

    if (readonly) {
        keys[0].flags = CMD_KEY_RO | CMD_KEY_ACCESS;
    } else {
        keys[0].flags = CMD_KEY_RW | CMD_KEY_ACCESS | CMD_KEY_UPDATE;
    }
    return 1;
}

void dbGetStats(char *buf, size_t bufsize, redisDb *db, int full, dbKeyType keyType) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;
    dictStats *mainHtStats = NULL;
    dictStats *rehashHtStats = NULL;
    dict *d;
    dbIterator *dbit = dbIteratorInit(db, keyType);
    while ((d = dbIteratorNextDict(dbit))) {
        dictStats *stats = dictGetStatsHt(d, 0, full);
        if (!mainHtStats) {
            mainHtStats = stats;
        } else {
            dictCombineStats(stats, mainHtStats);
            dictFreeStats(stats);
        }
        if (dictIsRehashing(d)) {
            stats = dictGetStatsHt(d, 1, full);
            if (!rehashHtStats) {
                rehashHtStats = stats;
            } else {
                dictCombineStats(stats, rehashHtStats);
                dictFreeStats(stats);
            }
        }
    }
    dbReleaseIterator(dbit);
    l = dictGetStatsMsg(buf, bufsize, mainHtStats, full);
    dictFreeStats(mainHtStats);
    buf += l;
    bufsize -= l;
    if (rehashHtStats && bufsize > 0) {
        dictGetStatsMsg(buf, bufsize, rehashHtStats, full);
        dictFreeStats(rehashHtStats);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize - 1] = '\0';
}

/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA.

   This file is part of MooseFS.

   MooseFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   MooseFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MooseFS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#include <sys/stat.h>
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include "MFSCommunication.h"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "matocuserv.h"
#include "matocsserv.h"
#include "cfg.h"

#include "chunks.h"
#include "filesystem.h"
#include "datapack.h"

#include "datacachemgr.h"
#include "acl.h"
#include "main.h"
#include "changelog.h"

#define NOT_USED(x) ( (void)(x) )

#define USE_FREENODE_BUCKETS 1
#define USE_CUIDREC_BUCKETS 1
#define EDGEHASH 1
#define BACKGROUND_METASTORE 1

#define NODEHASHBITS (26)
#define NODEHASHSIZE (1<<NODEHASHBITS)
#define NODEHASHPOS(nodeid) ((nodeid)&(NODEHASHSIZE-1))

#ifdef EDGEHASH
#define EDGEHASHBITS (26)
#define EDGEHASHSIZE (1<<EDGEHASHBITS)
#define EDGEHASHPOS(hash) ((hash)&(EDGEHASHSIZE-1))
#define LOOKUPNOHASHLIMIT 10
#endif

//#define GOAL(x) ((x)&0xF)
//#define DELETE(x) (((x)>>4)&1)
//#define SETGOAL(x,y) ((x)=((x)&0xF0)|((y)&0xF))
//#define SETDELETE(x,y) ((x)=((x)&0xF)|((y)&0x10))
#define DEFAULT_GOAL 1
#define DEFAULT_TRASHTIME 86400

#define MAXFNAMELENG 255

#define MAX_INDEX 0x7FFF
#define MAX_CHUNKS_PER_FILE (MAX_INDEX+1)

#define CHIDS_NO 0
#define CHIDS_YES 1
#define CHIDS_AUTO 2

/* log print control */
//#define LOG_COUNT 1000
static uint32_t log_structure_error=0;
static uint32_t log_test_files=0;
static uint32_t LOG_COUNT;

FILE * msgfd;
char *logfile;
uint64_t logsize;

typedef struct _bstnode {
	uint32_t val,count;
	struct _bstnode *left,*right;
} bstnode;

typedef struct _sessionidrec {
	uint32_t sessionid;
	struct _sessionidrec *next;
} sessionidrec;

struct _fsnode;

typedef struct _fsedge {
	struct _fsnode *child,*parent;
	struct _fsedge *nextchild,*nextparent;
	struct _fsedge **prevchild,**prevparent;
#ifdef EDGEHASH
	struct _fsedge *next,**prev;
#endif
	uint16_t nleng;
	uint8_t *name;
} fsedge;

typedef struct _statsrecord {
	uint32_t inodes;
	uint32_t dirs;
	uint32_t files;
	uint32_t chunks;
	uint64_t length;
	uint64_t size;
	uint64_t realsize;
} statsrecord;

typedef struct _quotanode {
	uint8_t exceeded;	// hard quota exceeded or soft quota reached time limit
	uint8_t flags;
	uint32_t stimestamp;	// time when soft quota exceeded
	uint32_t sinodes,hinodes;
	uint64_t slength,hlength;
	uint64_t ssize,hsize;
	uint64_t srealsize,hrealsize;
	struct _fsnode *node;
	struct _quotanode *next,**prev;
} quotanode;

static quotanode *quotahead;
static uint32_t QuotaTimeLimit;

typedef struct _fsnode {
	uint32_t id;
	uint32_t ctime,mtime,atime;
	uint8_t type;
	uint8_t goal;
	uint16_t mode;	// only 12 lowest bits are used for mode, in unix standard upper 4 are used for object type, but since there is field "type" this bits can be used as extra flags
	uint32_t uid;
	uint32_t gid;
	uint32_t trashtime;
	union _data {
		struct _ddata {				// type==TYPE_DIRECTORY
			fsedge *children;
			uint32_t nlink;
			uint32_t elements;
//			uint8_t quotaexceeded:1;	// quota exceeded
			statsrecord *stats;
			quotanode *quota;
		} ddata;
		struct _sdata {				// type==TYPE_SYMLINK
			uint32_t pleng;
			uint8_t *path;
		} sdata;
		uint32_t rdev;				// type==TYPE_BLOCKDEV ; type==TYPE_CHARDEV
		struct _fdata {				// type==TYPE_FILE
			uint64_t length;
			uint64_t *chunktab;
			uint32_t chunks;
			sessionidrec *sessionids;
		} fdata;
	} data;
	fsedge *parents;
	struct _fsnode *next;
} fsnode;

typedef struct _freenode {
	uint32_t id;
	uint32_t ftime;
	struct _freenode *next;
} freenode;

static uint32_t *freebitmask;
static uint32_t bitmasksize;
static uint32_t searchpos;
static freenode *freelist,**freetail;

static fsedge *trash;
static fsedge *reserved;
static fsnode *root;
static fsnode* nodehash[NODEHASHSIZE];
#ifdef EDGEHASH
static fsedge* edgehash[EDGEHASHSIZE];
#endif

static uint32_t maxnodeid;
uint32_t nextsessionid;
static uint32_t nodes;

static uint64_t version;
static uint64_t trashspace;
static uint64_t reservedspace;
static uint32_t trashnodes;
static uint32_t reservednodes;
static uint32_t filenodes;
static uint32_t dirnodes;

#define MSGBUFFSIZE 1000000
#define ERRORS_LOG_MAX 500

static uint32_t fsinfo_files=0;
static uint32_t fsinfo_ugfiles=0;
static uint32_t fsinfo_mfiles=0;
static uint32_t fsinfo_chunks=0;
static uint32_t fsinfo_ugchunks=0;
static uint32_t fsinfo_mchunks=0;
static char *fsinfo_msgbuff=NULL;
static uint32_t fsinfo_msgbuffleng=0;
static uint32_t fsinfo_loopstart=0;
static uint32_t fsinfo_loopend=0;

static uint32_t starttime;

static uint32_t stats_statfs=0;
static uint32_t stats_getattr=0;
static uint32_t stats_setattr=0;
static uint32_t stats_lookup=0;
static uint32_t stats_mkdir=0;
static uint32_t stats_rmdir=0;
static uint32_t stats_symlink=0;
static uint32_t stats_readlink=0;
static uint32_t stats_mknod=0;
static uint32_t stats_unlink=0;
static uint32_t stats_rename=0;
static uint32_t stats_link=0;
static uint32_t stats_readdir=0;
static uint32_t stats_open=0;
static uint32_t stats_read=0;
static uint32_t stats_write=0;

//to avoid epoll&fork problem

void fs_stats(uint32_t stats[16]) {
	stats[0] = stats_statfs;
	stats[1] = stats_getattr;
	stats[2] = stats_setattr;
	stats[3] = stats_lookup;
	stats[4] = stats_mkdir;
	stats[5] = stats_rmdir;
	stats[6] = stats_symlink;
	stats[7] = stats_readlink;
	stats[8] = stats_mknod;
	stats[9] = stats_unlink;
	stats[10] = stats_rename;
	stats[11] = stats_link;
	stats[12] = stats_readdir;
	stats[13] = stats_open;
	stats[14] = stats_read;
	stats[15] = stats_write;
	stats_statfs=0;
	stats_getattr=0;
	stats_setattr=0;
	stats_lookup=0;
	stats_mkdir=0;
	stats_rmdir=0;
	stats_symlink=0;
	stats_readlink=0;
	stats_mknod=0;
	stats_unlink=0;
	stats_rename=0;
	stats_link=0;
	stats_readdir=0;
	stats_open=0;
	stats_read=0;
	stats_write=0;
}


#ifdef USE_FREENODE_BUCKETS
#define FREENODE_BUCKET_SIZE 5000

typedef struct _freenode_bucket {
	freenode bucket[FREENODE_BUCKET_SIZE];
	uint32_t firstfree;
	struct _freenode_bucket *next;
} freenode_bucket;

static freenode_bucket *fnbhead = NULL;
static freenode *fnfreehead = NULL;

static inline freenode* freenode_malloc() {
	freenode_bucket *fnb;
	freenode *ret;
	if (fnfreehead) {
		ret = fnfreehead;
		fnfreehead = ret->next;
		return ret;
	}
	if (fnbhead==NULL || fnbhead->firstfree==FREENODE_BUCKET_SIZE) {
		fnb = (freenode_bucket*)malloc(sizeof(freenode_bucket));
		fnb->next = fnbhead;
		fnb->firstfree = 0;
		fnbhead = fnb;
	}
	ret = (fnbhead->bucket)+(fnbhead->firstfree);
	fnbhead->firstfree++;
	return ret;
}

static inline void freenode_free(freenode *p) {
	p->next = fnfreehead;
	fnfreehead = p;
}
#else /* USE_FREENODE_BUCKETS */

static inline freenode* freenode_malloc() {
	return (freenode*)malloc(sizeof(freenode));
}

static inline void freenode_free(freenode* p) {
	free(p);
}

#endif /* USE_FREENODE_BUCKETS */

#ifdef USE_CUIDREC_BUCKETS
#define CUIDREC_BUCKET_SIZE 1000

typedef struct _sessionidrec_bucket {
	sessionidrec bucket[CUIDREC_BUCKET_SIZE];
	uint32_t firstfree;
	struct _sessionidrec_bucket *next;
} sessionidrec_bucket;

static sessionidrec_bucket *crbhead = NULL;
static sessionidrec *crfreehead = NULL;

static inline sessionidrec* sessionidrec_malloc() {
	sessionidrec_bucket *crb;
	sessionidrec *ret;
	if (crfreehead) {
		ret = crfreehead;
		crfreehead = ret->next;
		return ret;
	}
	if (crbhead==NULL || crbhead->firstfree==CUIDREC_BUCKET_SIZE) {
		crb = (sessionidrec_bucket*)malloc(sizeof(sessionidrec_bucket));
		crb->next = crbhead;
		crb->firstfree = 0;
		crbhead = crb;
	}
	ret = (crbhead->bucket)+(crbhead->firstfree);
	crbhead->firstfree++;
	return ret;
}

static inline void sessionidrec_free(sessionidrec *p) {
	p->next = crfreehead;
	crfreehead = p;
}
#else /* USE_CUIDREC_BUCKETS */

static inline sessionidrec* sessionidrec_malloc() {
	return (sessionidrec*)malloc(sizeof(sessionidrec));
}

static inline void sessionidrec_free(sessionidrec* p) {
	free(p);
}

#endif /* USE_CUIDREC_BUCKETS */

uint32_t fsnodes_get_next_id() {
	uint32_t i,mask;
	while (searchpos<bitmasksize && freebitmask[searchpos]==0xFFFFFFFF) {
		searchpos++;
	}
	if (searchpos==bitmasksize) {	// no more freeinodes
		bitmasksize+=0xF0;
		freebitmask = (uint32_t*)realloc(freebitmask,bitmasksize*sizeof(uint32_t));
		memset(freebitmask+searchpos,0,0xF0*sizeof(uint32_t));
	}
	mask = freebitmask[searchpos];
	i=0;
	while (mask&1) {
		i++;
		mask>>=1;
	}
	mask = 1<<i;
	freebitmask[searchpos] |= mask;
	i+=(searchpos<<5);
	if (i>maxnodeid) {
		maxnodeid=i;
	}
	return i;
}

void fsnodes_free_id(uint32_t id,uint32_t ts) {
	freenode *n;
	n = freenode_malloc();
	n->id = id;
	n->ftime = ts;
	n->next = NULL;
	*freetail = n;
	freetail = &(n->next);
}

uint8_t shadow_fs_freeinodes(uint32_t ts,uint32_t freeinodes) {
        uint32_t fi,now,pos,mask;
        freenode *n,*an;
	now = ts;
	fi = 0;
        n = freelist;
        while (n && n->ftime+86400<now) {
                fi++;
                pos = (n->id >> 5);
                mask = 1<<(n->id&0x1F);
                freebitmask[pos] &= ~mask;
                if (pos<searchpos) {
                        searchpos = pos;
                }
                an = n->next;
                freenode_free(n);
                n = an;
        }
        if (n) {
                freelist = n;
        } else {
                freelist = NULL;
                freetail = &(freelist);
        }
	version++;
        if (freeinodes!=fi) {
//		syslog(LOG_NOTICE,"shadow_fs_freeinodes failed:fi is %d,freeinodes is %d",fi,freeinodes);
                return 1;
        }
        return 0;
}

void fsnodes_init_freebitmask (void) {
	bitmasksize = 0x200+(((maxnodeid)>>5)&0xFFFFFF80);
	freebitmask = (uint32_t*)malloc(bitmasksize*sizeof(uint32_t));
	memset(freebitmask,0,bitmasksize*sizeof(uint32_t));
	freebitmask[0]=1;	// reserve inode 0
	searchpos = 0;
}

void fsnodes_used_inode (uint32_t id) {
	uint32_t pos,mask;
	pos = id>>5;
	mask = 1<<(id&0x1F);
	freebitmask[pos]|=mask;
}


/*
char* fsnodes_escape_name(uint16_t nleng,const uint8_t *name) {
	static uint8_t escname[3*MAXFNAMELENG+1];
	uint32_t i;
	uint8_t c;
	i = 0;
	while (nleng>0) {
		c=*name++;
		if (c<32 || c>127 || c==',' || c=='%' || c=='(' || c==')' || c==0) {
			escname[i++]='%';
			escname[i++]="0123456789ABCDEF"[(c>>4)&0xF];
			escname[i++]="0123456789ABCDEF"[c&0xF];
		} else {
			escname[i++]=c;
		}
		nleng--;
	}
	escname[i]=0;
	return (char*)escname;
}
*/

static char* fsnodes_escape_name(uint16_t nleng,const uint8_t *name) {
	static char *escname[2]={NULL,NULL};
	static uint32_t escnamesize[2]={0,0};
	static uint8_t buffid=0;
	char *currescname=NULL;
	uint32_t i;
	uint8_t c;
	buffid = 1-buffid;
	i = nleng;
	i = i*3+1;
	if (i>escnamesize[buffid]) {
		escnamesize[buffid] = ((i/1000)+1)*1000;
		if (escname[buffid]!=NULL) {
			free(escname[buffid]);
		}
		escname[buffid] = malloc(escnamesize[buffid]);
	}
	i = 0;
	currescname = escname[buffid];
	while (nleng>0) {
		c = *name;
		if (c<32 || c>=127 || c==',' || c=='%' || c=='(' || c==')') {
			currescname[i++]='%';
			currescname[i++]="0123456789ABCDEF"[(c>>4)&0xF];
			currescname[i++]="0123456789ABCDEF"[c&0xF];
		} else {
			currescname[i++]=c;
		}
		name++;
		nleng--;
	}
	currescname[i]=0;
	return currescname;
}

#ifdef EDGEHASH
static inline uint32_t fsnodes_hash(uint32_t parentid,uint16_t nleng,const uint8_t *name) {
	uint32_t hash,i;
	hash = ((parentid * 0x5F2318BD) + nleng);
	for (i=0 ; i<nleng ; i++) {
		hash = hash*33+name[i];
	}
	return hash;
}
#endif

static int fsnodes_nameisused(fsnode *node,uint16_t nleng,const uint8_t *name) {
	fsedge *ei;
#ifdef EDGEHASH
	if (node->data.ddata.elements>LOOKUPNOHASHLIMIT) {
		ei = edgehash[EDGEHASHPOS(fsnodes_hash(node->id,nleng,name))];
		while (ei) {
			if (ei->parent==node && nleng==ei->nleng && memcmp((char*)(ei->name),(char*)name,nleng)==0) {
				return 1;
			}
			ei = ei->next;
		}
	} else {
		ei = node->data.ddata.children;
		while (ei) {
			if (nleng==ei->nleng && memcmp((char*)(ei->name),(char*)name,nleng)==0) {
				return 1;
			}
			ei = ei->nextchild;
		}
	}
#else
	ei = node->data.ddata.children;
	while (ei) {
		if (nleng==ei->nleng && memcmp((char*)(ei->name),(char*)name,nleng)==0) {
			return 1;
		}
		ei = ei->nextchild;
	}
#endif
	return 0;
}

static fsedge* fsnodes_lookup(fsnode *node,uint16_t nleng,const uint8_t *name) {
	fsedge *ei;

	if (node->type!=TYPE_DIRECTORY) {
		return NULL;
	}
#ifdef EDGEHASH
	if (node->data.ddata.elements>LOOKUPNOHASHLIMIT) {
		ei = edgehash[EDGEHASHPOS(fsnodes_hash(node->id,nleng,name))];
		while (ei) {
			if (ei->parent==node && nleng==ei->nleng && memcmp((char*)(ei->name),(char*)name,nleng)==0) {
				return ei;
			}
			ei = ei->next;
		}
	} else {
		ei = node->data.ddata.children;
		while (ei) {
			if (nleng==ei->nleng && memcmp((char*)(ei->name),(char*)name,nleng)==0) {
				return ei;
			}
			ei = ei->nextchild;
		}
	}
#else
	ei = node->data.ddata.children;
	while (ei) {
		if (nleng==ei->nleng && memcmp((char*)(ei->name),(char*)name,nleng)==0) {
			return ei;
		}
		ei = ei->nextchild;
	}
#endif
	return NULL;
}

static inline fsnode* fsnodes_id_to_node(uint32_t id) {
	fsnode *p;
	uint32_t nodepos = NODEHASHPOS(id);
	for (p=nodehash[nodepos]; p ; p=p->next ) {
		if (p->id == id) {
			return p;
		}
	}
	return NULL;
}


/*
static inline uint8_t fsnodes_geteattr(fsnode *p) {
	fsedge *e;
	uint8_t eattr;
	eattr = (p->mode>>12) & (EATTR_NOOWNER | EATTR_NOACACHE);
	for (e=p->parents ; e ; e=e->nextparent) { // check all parents of 'p' because 'p' can be any object, so it can be hardlinked
		p=e->parent; // warning !!! since this point 'p' is used as temporary variable
		while (p) { // here 'p' is always a directory so it should have only one parent - no recursion is needed
			eattr |= (p->mode>>12);
			p=(p->parents)?p->parents->parent:NULL;
		}
	}
	return eattr;
}

static inline uint8_t fsnodes_changeids(fsnode *p) {
	fsedge *e;
	if ((p->mode>>12) & EATTR_NOOWNER) {
		return CHIDS_YES;
	}
	for (e=p->parents ; e ; e=e->nextparent) { // check all parents of 'p' because 'p' can be any object, so it can be hardlinked
		p=e->parent; // warning !!! since this point 'p' is used as temporary variable
		while (p) { // here 'p' is always a directory so it should have only one parent - no recursion is needed
			if ((p->mode>>12) & EATTR_NOOWNER) {
				return CHIDS_YES;
			}
			p=(p->parents)?p->parents->parent:NULL;
		}
	}
	return CHIDS_NO;
}
*/

// returns 1 only if f is ancestor of p
static inline int fsnodes_isancestor(fsnode *f,fsnode *p) {
	fsedge *e;
//	if (f==root) {	// root is ancestor of every node
//		return 1;
//	}
	for (e=p->parents ; e ; e=e->nextparent) {	// check all parents of 'p' because 'p' can be any object, so it can be hardlinked
		p=e->parent;	// warning !!! since this point 'p' is used as temporary variable
		while (p) {
			if (f==p) {
				return 1;
			}
			if (p->parents) {
				p = p->parents->parent;	// here 'p' is always a directory so it should have only one parent
			} else {
				p = NULL;
			}
		}
	}
	return 0;
}


// quotas

static inline quotanode* fsnodes_new_quotanode() {
	quotanode *qn;
	qn = malloc(sizeof(quotanode));
	memset(qn,0,sizeof(quotanode));
	qn->next = quotahead;
	if (qn->next) {
		qn->next->prev = &(qn->next);
	}
	qn->prev = &(quotahead);
	quotahead = qn;
	return qn;
}

static inline void fsnodes_delete_quotanode(quotanode *qn) {
	*(qn->prev) = qn->next;
	if (qn->next) {
		qn->next->prev = qn->prev;
	}
	free(qn);
}

static inline void fsnodes_check_quotanode(quotanode *qn,uint32_t ts) {
	statsrecord *psr = qn->node->data.ddata.stats;
	uint8_t hq,sq;
	hq=0;
	if (qn->flags&QUOTA_FLAG_HINODES) {
		if (psr->inodes>qn->hinodes) {
			hq=1;
		}
	}
	if (qn->flags&QUOTA_FLAG_HLENGTH) {
		if (psr->length>qn->hlength) {
			hq=1;
		}
	}
	if (qn->flags&QUOTA_FLAG_HSIZE) {
		if (psr->size>qn->hsize) {
			hq=1;
		}
	}
	if (qn->flags&QUOTA_FLAG_HREALSIZE) {
		if (psr->realsize>qn->hrealsize) {
			hq=1;
		}
	}
	sq=0;
	if (qn->flags&QUOTA_FLAG_SINODES) {
		if (psr->inodes>qn->sinodes) {
			sq=1;
		}
	}
	if (qn->flags&QUOTA_FLAG_SLENGTH) {
		if (psr->length>qn->slength) {
			sq=1;
		}
	}
	if (qn->flags&QUOTA_FLAG_SSIZE) {
		if (psr->size>qn->ssize) {
			sq=1;
		}
	}
	if (qn->flags&QUOTA_FLAG_SREALSIZE) {
		if (psr->realsize>qn->srealsize) {
			sq=1;
		}
	}
	if (sq==0 && qn->stimestamp>0) {
		qn->stimestamp=0;
	} else if (sq && qn->stimestamp==0) {
		qn->stimestamp = ts;
	}
	qn->exceeded = (hq || (qn->stimestamp && qn->stimestamp+QuotaTimeLimit<ts))?1:0;
}


static inline uint8_t fsnodes_test_quota(fsnode *node) {
	fsedge *e;
	if (node) {
		if (node->type==TYPE_DIRECTORY && node->data.ddata.quota && node->data.ddata.quota->exceeded) {
			return 1;
		}
		if (node!=root) {
			for (e=node->parents ; e ; e=e->nextparent) {
				if (fsnodes_test_quota(e->parent)) {
					return 1;
				}
			}
		}
	}
	return 0;
}

// stats

static inline void fsnodes_get_stats(fsnode *node,statsrecord *sr) {
	uint32_t i,lastchunk,lastchunksize;
	switch (node->type) {
	case TYPE_DIRECTORY:
		*sr = *(node->data.ddata.stats);
		sr->inodes++;
		sr->dirs++;
		break;
	case TYPE_FILE:
	case TYPE_TRASH:
	case TYPE_RESERVED:
		sr->inodes = 1;
		sr->dirs = 0;
		sr->files = 1;
		sr->chunks = 0;
		sr->length = node->data.fdata.length;
		sr->size = 0;
		if (node->data.fdata.length>0) {
			lastchunk = (node->data.fdata.length-1)>>26;
			lastchunksize = ((((node->data.fdata.length-1)&0x3FFFFFF)+0x10000)&0x7FFF0000)+0x1400;
		} else {
			lastchunk = 0;
			lastchunksize = 0x1400;
		}
		for (i=0 ; i<node->data.fdata.chunks ; i++) {
			if (node->data.fdata.chunktab[i]>0) {
				if (i<lastchunk) {
					sr->size+=0x4001400;
				} else if (i==lastchunk) {
					sr->size+=lastchunksize;
				}
				sr->chunks++;
			}
		}
		sr->realsize = sr->size * node->goal;
		break;
	case TYPE_SYMLINK:
		sr->inodes = 1;
		sr->files = 0;
		sr->dirs = 0;
		sr->chunks = 0;
		sr->length = node->data.sdata.pleng;
		sr->size = 0;
		sr->realsize = 0;
		break;
	default:
		sr->inodes = 1;
		sr->files = 0;
		sr->dirs = 0;
		sr->chunks = 0;
		sr->length = 0;
		sr->size = 0;
		sr->realsize = 0;
	}
}

static inline void fsnodes_sub_stats(fsnode *parent,statsrecord *sr) {
	statsrecord *psr;
	fsedge *e;
	if (parent) {
		psr = parent->data.ddata.stats;
		psr->inodes -= sr->inodes;
		psr->dirs -= sr->dirs;
		psr->files -= sr->files;
		psr->chunks -= sr->chunks;
		psr->length -= sr->length;
		psr->size -= sr->size;
		psr->realsize -= sr->realsize;
		if (parent!=root) {
//			if (parent->data.ddata.hasquota) {
//				fsnodes_check_quota_state(parent);
//			}
			for (e=parent->parents ; e ; e=e->nextparent) {
				fsnodes_sub_stats(e->parent,sr);
			}
		}
	}
}

static inline void fsnodes_add_stats(fsnode *parent,statsrecord *sr) {
	statsrecord *psr;
	fsedge *e;
	if (parent) {
		psr = parent->data.ddata.stats;
		psr->inodes += sr->inodes;
		psr->dirs += sr->dirs;
		psr->files += sr->files;
		psr->chunks += sr->chunks;
		psr->length += sr->length;
		psr->size += sr->size;
		psr->realsize += sr->realsize;
		if (parent!=root) {
//			if (parent->data.ddata.hasquota) {
//				fsnodes_check_quota_state(parent);
//			}
			for (e=parent->parents ; e ; e=e->nextparent) {
				fsnodes_add_stats(e->parent,sr);
			}
		}
	}
}

static inline void fsnodes_add_sub_stats(fsnode *parent,statsrecord *newsr,statsrecord *prevsr) {
	statsrecord sr;
	sr.inodes = newsr->inodes - prevsr->inodes;
	sr.dirs = newsr->dirs - prevsr->dirs;
	sr.files = newsr->files - prevsr->files;
	sr.chunks = newsr->chunks - prevsr->chunks;
	sr.length = newsr->length - prevsr->length;
	sr.size = newsr->size - prevsr->size;
	sr.realsize = newsr->realsize - prevsr->realsize;
	fsnodes_add_stats(parent,&sr);
}


static inline void fsnodes_remove_edge(uint32_t ts,fsedge *e) {
	statsrecord sr;
	if (e->parent) {
		fsnodes_get_stats(e->child,&sr);
		fsnodes_sub_stats(e->parent,&sr);
		e->parent->mtime = e->parent->ctime = ts;
		e->parent->data.ddata.elements--;
		if (e->child->type==TYPE_DIRECTORY) {
			e->parent->data.ddata.nlink--;
		}
	}
	if (e->child) {
		e->child->ctime = ts;
	}
	*(e->prevchild) = e->nextchild;
	if (e->nextchild) {
		e->nextchild->prevchild = e->prevchild;
	}
	*(e->prevparent) = e->nextparent;
	if (e->nextparent) {
		e->nextparent->prevparent = e->prevparent;
	}
#ifdef EDGEHASH
	if (e->prev) {
		*(e->prev) = e->next;
		if (e->next) {
			e->next->prev = e->prev;
		}
	}
#endif
	free(e->name);
	free(e);
}

static inline void shadow_fsnodes_remove_edge(uint32_t ts,fsedge *e) {
        if (e->parent) {
                e->parent->mtime = e->parent->ctime = ts;
                e->parent->data.ddata.elements--;
                if (e->child->type==TYPE_DIRECTORY) {
                        e->parent->data.ddata.nlink--;
                }
        }
        if (e->child) {
                e->child->ctime = ts;
        }
        *(e->prevchild) = e->nextchild;
        if (e->nextchild) {
                e->nextchild->prevchild = e->prevchild;
        }
        *(e->prevparent) = e->nextparent;
        if (e->nextparent) {
                e->nextparent->prevparent = e->prevparent;
        }
#ifdef EDGEHASH
        if (e->prev) {
                *(e->prev) = e->next;
                if (e->next) {
                        e->next->prev = e->prev;
                }
        }
#endif
        free(e->name);
        free(e);
}

static inline void fsnodes_link(uint32_t ts,fsnode *parent,fsnode *child,uint16_t nleng,const uint8_t *name) {
	fsedge *e;
	statsrecord sr;
#ifdef EDGEHASH
	uint32_t hpos;
#endif

	e = malloc(sizeof(fsedge));
	e->nleng = nleng;
	e->name = malloc(nleng);
	memcpy(e->name,name,nleng);
	e->child = child;
	e->parent = parent;
	e->nextchild = parent->data.ddata.children;
	if (e->nextchild) {
		e->nextchild->prevchild = &(e->nextchild);
	}
	parent->data.ddata.children = e;
	e->prevchild = &(parent->data.ddata.children);
	e->nextparent = child->parents;
	if (e->nextparent) {
		e->nextparent->prevparent = &(e->nextparent);
	}
	child->parents = e;
	e->prevparent = &(child->parents);
#ifdef EDGEHASH
	hpos = EDGEHASHPOS(fsnodes_hash(parent->id,nleng,name));
	e->next = edgehash[hpos];
	if (e->next) {
		e->next->prev = &(e->next);
	}
	edgehash[hpos] = e;
	e->prev = &(edgehash[hpos]);
#endif

	parent->data.ddata.elements++;
	if (child->type==TYPE_DIRECTORY) {
		parent->data.ddata.nlink++;
	}
	fsnodes_get_stats(child,&sr);
	fsnodes_add_stats(parent,&sr);
	if (ts>0) {
		parent->mtime = parent->ctime = ts;
		child->ctime = ts;
	}
}

static inline fsnode* fsnodes_create_node(uint32_t ts,fsnode* node,uint16_t nleng,const uint8_t *name,uint8_t type,uint16_t mode,uint32_t uid,uint32_t gid) {
	fsnode *p;
	statsrecord *sr;
	uint32_t nodepos;
	p = malloc(sizeof(fsnode));
	nodes++;
	if (type==TYPE_DIRECTORY) {
		dirnodes++;
	}
	if (type==TYPE_FILE) {
		filenodes++;
	}
	p->id = fsnodes_get_next_id();
	p->type = type;
	p->ctime = p->mtime = p->atime = ts;
	if (type==TYPE_DIRECTORY || type==TYPE_FILE) {
		p->goal = node->goal;
		p->trashtime = node->trashtime;
	} else {
		p->goal = DEFAULT_GOAL;
		p->trashtime = DEFAULT_TRASHTIME;
	}
	if (type==TYPE_DIRECTORY) {
		p->mode = (mode&07777) | (node->mode&0xF000);
	} else {
		p->mode = (mode&07777) | (node->mode&(0xF000&(~(EATTR_NOECACHE<<12))));
	}
	p->uid = uid;
	p->gid = gid;
	switch (type) {
	case TYPE_DIRECTORY:
		sr = malloc(sizeof(statsrecord));
		memset(sr,0,sizeof(statsrecord));
		p->data.ddata.stats = sr;
		p->data.ddata.quota = NULL;
		p->data.ddata.children = NULL;
		p->data.ddata.nlink = 2;
		p->data.ddata.elements = 0;
		break;
	case TYPE_FILE:
		p->data.fdata.length = 0;
		p->data.fdata.chunks = 0;
		p->data.fdata.chunktab = NULL;
		p->data.fdata.sessionids = NULL;
		break;
	case TYPE_SYMLINK:
		p->data.sdata.pleng = 0;
		p->data.sdata.path = NULL;
		break;
	case TYPE_BLOCKDEV:
	case TYPE_CHARDEV:
		p->data.rdev = 0;
	}
	p->parents = NULL;
	nodepos = NODEHASHPOS(p->id);
	p->next = nodehash[nodepos];
	nodehash[nodepos] = p;
	fsnodes_link(ts,node,p,nleng,name);
	return p;
}

static inline uint32_t fsnodes_getpath_size(fsedge *e) {
	uint32_t size;
	fsnode *p;
	if (e==NULL) {
		return 0;
	}
	p = e->parent;
	size = e->nleng;
	while (p!=root && p->parents) {
		size += p->parents->nleng+1;
		p = p->parents->parent;
	}
	return size;
}

static inline void fsnodes_getpath_data(fsedge *e,uint8_t *path,uint32_t size) {
	fsnode *p;
	if (e==NULL) {
		return;
	}
	if (size>=e->nleng) {
		size-=e->nleng;
		memcpy(path+size,e->name,e->nleng);
	} else if (size>0) {
		memcpy(path,e->name+(e->nleng-size),size);
		size=0;
	}
	if (size>0) {
		path[--size]='/';
	}
	p = e->parent;
	while (p!=root && p->parents) {
		if (size>=p->parents->nleng) {
			size-=p->parents->nleng;
			memcpy(path+size,p->parents->name,p->parents->nleng);
		} else if (size>0) {
			memcpy(path,p->parents->name+(p->parents->nleng-size),size);
			size=0;
		}
		if (size>0) {
			path[--size]='/';
		}
		p = p->parents->parent;
	}
}

static inline void fsnodes_getpath(fsedge *e,uint16_t *pleng,uint8_t **path) {
	uint32_t size;
	uint8_t *ret;
	fsnode *p;

	p = e->parent;
	size = e->nleng;
	while (p!=root && p->parents) {
		size += p->parents->nleng+1;	// get first parent !!!
		p = p->parents->parent;		// when folders can be hardlinked it's the only way to obtain path (one of them)
	}
	if (size>65535) {
		MFSLOG(LOG_WARNING,"path too long !!! - truncate");
		size=65535;
	}
	*pleng = size;
	ret = malloc(size);
	size -= e->nleng;
	memcpy(ret+size,e->name,e->nleng);
	if (size>0) {
		ret[--size]='/';
	}
	p = e->parent;
	while (p!=root && p->parents) {
		if (size>=p->parents->nleng) {
			size-=p->parents->nleng;
			memcpy(ret+size,p->parents->name,p->parents->nleng);
		} else {
			if (size>0) {
				memcpy(ret,p->parents->name+(p->parents->nleng-size),size);
				size=0;
			}
		}
		if (size>0) {
			ret[--size]='/';
		}
		p = p->parents->parent;
	}
	*path = ret;
}



static inline void fsnodes_fill_attr(fsnode *node,fsnode *parent,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint8_t sesflags,uint8_t attr[35]) {
	uint8_t *ptr;
	uint16_t mode;
	uint32_t nlink;
	fsedge *e;
	(void)sesflags;
	ptr = attr;
	if (node->type==TYPE_TRASH || node->type==TYPE_RESERVED) {
		put8bit(&ptr,TYPE_FILE);
	} else {
		put8bit(&ptr,node->type);
	}
	mode = node->mode&07777;
	if (parent) {
		if (parent->mode&(EATTR_NOECACHE<<12)) {
			mode |= (MATTR_NOECACHE<<12);
		}
	}
	if ((node->mode&((EATTR_NOOWNER|EATTR_NOACACHE)<<12)) || (sesflags&SESFLAG_MAPALL)) {
		mode |= (MATTR_NOACACHE<<12);
	}
	if ((node->mode&(EATTR_NODATACACHE<<12))==0) {
		mode |= (MATTR_ALLOWDATACACHE<<12);
	}
	put16bit(&ptr,mode);
	if ((node->mode&(EATTR_NOOWNER<<12)) && uid!=0) {
		if (sesflags&SESFLAG_MAPALL) {
			put32bit(&ptr,auid);
			put32bit(&ptr,agid);
		} else {
			put32bit(&ptr,uid);
			put32bit(&ptr,gid);
		}
	} else {
		if (sesflags&SESFLAG_MAPALL && auid!=0) {
			if (node->uid==uid) {
				put32bit(&ptr,auid);
			} else {
				put32bit(&ptr,0);
			}
			if (node->gid==gid) {
				put32bit(&ptr,agid);
			} else {
				put32bit(&ptr,0);
			}
		} else {
			put32bit(&ptr,node->uid);
			put32bit(&ptr,node->gid);
		}
	}
	put32bit(&ptr,node->atime);
	put32bit(&ptr,node->mtime);
	put32bit(&ptr,node->ctime);
	nlink = 0;
	for (e=node->parents ; e ; e=e->nextparent) {
		nlink++;
	}
	switch (node->type) {
	case TYPE_FILE:
	case TYPE_TRASH:
	case TYPE_RESERVED:
		put32bit(&ptr,nlink);
		put64bit(&ptr,node->data.fdata.length);
		break;
	case TYPE_DIRECTORY:
		put32bit(&ptr,node->data.ddata.nlink);
		put64bit(&ptr,node->data.ddata.stats->length>>30); // Rescale length to GB because Linux can't bear too long directories :) - It's so funny why Linux is so popular.
		break;
	case TYPE_SYMLINK:
		put32bit(&ptr,nlink);
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		put32bit(&ptr,node->data.sdata.pleng);
		break;
	case TYPE_BLOCKDEV:
	case TYPE_CHARDEV:
		put32bit(&ptr,nlink);
		put32bit(&ptr,node->data.rdev);
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		break;
	default:
		put32bit(&ptr,nlink);
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
	}
}

/*
static inline void fsnodes_fill_attr(fsnode *node,fsnode *parent,uint32_t uid,uint32_t gid,uint8_t sesflags,uint8_t attr[35]) {
	uint8_t *ptr;
	uint16_t mode;
	uint32_t nlink;
	fsedge *e;

	ptr = attr;
	if (node->type==TYPE_TRASH || node->type==TYPE_RESERVED) {
		put8bit(&ptr,TYPE_FILE);
	} else {
		put8bit(&ptr,node->type);
	}
	mode = node->mode&07777;
	if (parent) {
		if (parent->mode&(EATTR_NOECACHE<<12)) {
			mode |= (MATTR_NOECACHE<<12);
		}
	}
	if ((node->mode&((EATTR_NOOWNER|EATTR_NOACACHE)<<12)) || (sesflags&SESFLAG_MAPALL)) {
		mode |= (MATTR_NOACACHE<<12);
	}
	put16bit(&ptr,mode);
	if ((node->mode&(EATTR_NOOWNER<<12)) && uid!=0) {
		put32bit(&ptr,uid);
		put32bit(&ptr,gid);
	} else {
		put32bit(&ptr,node->uid);
		put32bit(&ptr,node->gid);
	}
	put32bit(&ptr,node->atime);
	put32bit(&ptr,node->mtime);
	put32bit(&ptr,node->ctime);
	nlink = 0;
	for (e=node->parents ; e ; e=e->nextparent) {
		nlink++;
	}
	switch (node->type) {
	case TYPE_FILE:
	case TYPE_TRASH:
	case TYPE_RESERVED:
		put32bit(&ptr,nlink);
		put64bit(&ptr,node->data.fdata.length);
		break;
	case TYPE_DIRECTORY:
		put32bit(&ptr,node->data.ddata.nlink);
		put64bit(&ptr,node->data.ddata.stats->length>>30); // Rescale length to GB because Linux can't bear too long directories :) - It's so funny why Linux is so popular.
		break;
	case TYPE_SYMLINK:
		put32bit(&ptr,nlink);
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		put32bit(&ptr,node->data.sdata.pleng);
		break;
	case TYPE_BLOCKDEV:
	case TYPE_CHARDEV:
		put32bit(&ptr,nlink);
		put32bit(&ptr,node->data.rdev);
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		break;
	default:
		put32bit(&ptr,nlink);
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
		*ptr++=0;
	}
}
*/

static inline uint32_t fsnodes_getdetachedsize(fsedge *start) {
	fsedge *e;
	uint32_t result=0;
	for (e = start ; e ; e=e->nextchild) {
		if (e->nleng>240) {
			result+=245;
		} else {
			result+=5+e->nleng;
		}
	}
	return result;
}

static inline void fsnodes_getdetacheddata(fsedge *start,uint8_t *dbuff) {
	fsedge *e;
	uint8_t *sptr;
	uint8_t c;
	for (e = start ; e ; e=e->nextchild) {
		if (e->nleng>240) {
			*dbuff=240;
			dbuff++;
			memcpy(dbuff,"(...)",5);
			dbuff+=5;
			sptr = e->name+(e->nleng-235);
			for (c=0 ; c<235 ; c++) {
				if (*sptr=='/') {
					*dbuff='|';
				} else {
					*dbuff = *sptr;
				}
				sptr++;
				dbuff++;
			}
		} else {
			*dbuff=e->nleng;
			dbuff++;
			sptr = e->name;
			for (c=0 ; c<e->nleng ; c++) {
				if (*sptr=='/') {
					*dbuff='|';
				} else {
					*dbuff = *sptr;
				}
				sptr++;
				dbuff++;
			}
		}
		put32bit(&dbuff,e->child->id);
	}
}


static inline uint32_t fsnodes_getdirsize(fsnode *p,uint8_t withattr) {
	uint32_t result = ((withattr)?40:6)*2+3;	// for '.' and '..'
	fsedge *e;
	for (e = p->data.ddata.children ; e ; e=e->nextchild) {
		result+=((withattr)?40:6)+e->nleng;
	}
	return result;
}

static inline void fsnodes_getdirdata(uint32_t ts,uint32_t rootinode,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint8_t sesflags,fsnode *p,uint8_t *dbuff,uint8_t withattr) {
	fsedge *e;
	p->atime = ts;
// '.' - self
	dbuff[0]=1;
	dbuff[1]='.';
	dbuff+=2;
	if (p->id!=rootinode) {
		put32bit(&dbuff,p->id);
	} else {
		put32bit(&dbuff,MFS_ROOT_ID);
	}
	if (withattr) {
		fsnodes_fill_attr(p,p,uid,gid,auid,agid,sesflags,dbuff);
		dbuff+=35;
	} else {
		put8bit(&dbuff,TYPE_DIRECTORY);
	}
// '..' - parent
	dbuff[0]=2;
	dbuff[1]='.';
	dbuff[2]='.';
	dbuff+=3;
	if (p->id==rootinode) { // root node should returns self as its parent
		put32bit(&dbuff,MFS_ROOT_ID);
		if (withattr) {
			fsnodes_fill_attr(p,p,uid,gid,auid,agid,sesflags,dbuff);
			dbuff+=35;
		} else {
			put8bit(&dbuff,TYPE_DIRECTORY);
		}
	} else {
		if (p->parents && p->parents->parent->id!=rootinode) {
			put32bit(&dbuff,p->parents->parent->id);
		} else {
			put32bit(&dbuff,MFS_ROOT_ID);
		}
		if (withattr) {
			if (p->parents) {
				fsnodes_fill_attr(p->parents->parent,p,uid,gid,auid,agid,sesflags,dbuff);
			} else {
				if (rootinode==MFS_ROOT_ID) {
					fsnodes_fill_attr(root,p,uid,gid,auid,agid,sesflags,dbuff);
				} else {
					fsnode *rn = fsnodes_id_to_node(rootinode);
					if (rn) {	// it should be always true because it's checked before, but better check than sorry
						fsnodes_fill_attr(rn,p,uid,gid,auid,agid,sesflags,dbuff);
					} else {
						memset(dbuff,0,35);
					}
				}
			}
			dbuff+=35;
		} else {
			put8bit(&dbuff,TYPE_DIRECTORY);
		}
	}
// entries
	for (e = p->data.ddata.children ; e ; e=e->nextchild) {
		dbuff[0]=e->nleng;
		dbuff++;
		memcpy(dbuff,e->name,e->nleng);
		dbuff+=e->nleng;
		put32bit(&dbuff,e->child->id);
		if (withattr) {
			fsnodes_fill_attr(e->child,p,uid,gid,auid,agid,sesflags,dbuff);
			dbuff+=35;
		} else {
			put8bit(&dbuff,e->child->type);
		}
	}
}

static inline void fsnodes_checkfile(fsnode *p,uint16_t chunkcount[256]) {
	uint32_t i;
	uint64_t chunkid;
	uint8_t count;
	for (i=0 ; i<256 ; i++) {
		chunkcount[i]=0;
	}
	for (i=0 ; i<p->data.fdata.chunks ; i++) {
		chunkid = p->data.fdata.chunktab[i];
		if (chunkid>0) {
			chunk_get_validcopies(chunkid,&count);
			chunkcount[count]++;
		}
	}
}

//shadow master interface
static inline uint8_t shadow_fsnodes_appendchunks(uint32_t ts,fsnode *dstobj,fsnode *srcobj) {
	uint64_t chunkid,length;
	uint32_t i;
	uint32_t srcchunks,dstchunks;
	statsrecord psr,nsr;
	fsedge *e;

	srcchunks=0;
	for (i=0 ; i<srcobj->data.fdata.chunks ; i++) {
		if (srcobj->data.fdata.chunktab[i]!=0) {
			srcchunks = i+1;
		}
	}
	if (srcchunks==0) {
		return STATUS_OK;
	}
	dstchunks=0;
	for (i=0 ; i<dstobj->data.fdata.chunks ; i++) {
		if (dstobj->data.fdata.chunktab[i]!=0) {
			dstchunks = i+1;
		}
	}
	i = srcchunks+dstchunks-1;	// last new chunk pos
	if (i>MAX_INDEX) {	// chain too long
		return ERROR_INDEXTOOBIG;
	}
	fsnodes_get_stats(dstobj,&psr);
	if (i>=dstobj->data.fdata.chunks) {
		uint32_t newsize;
		if (i<8) {
			newsize=i+1;
		} else if (i<64) {
			newsize=(i&0xFFFFFFF8)+8;
		} else {
			newsize = (i&0xFFFFFFC0)+64;
		}
		if (dstobj->data.fdata.chunktab==NULL) {
			dstobj->data.fdata.chunktab = (uint64_t*)malloc(sizeof(uint64_t)*newsize);
		} else {
			dstobj->data.fdata.chunktab = (uint64_t*)realloc(dstobj->data.fdata.chunktab,sizeof(uint64_t)*newsize);
		}
		for (i=dstobj->data.fdata.chunks ; i<newsize ; i++) {
			dstobj->data.fdata.chunktab[i]=0;
		}
		dstobj->data.fdata.chunks = newsize;
	}

	for (i=0 ; i<srcchunks ; i++) {
		chunkid = srcobj->data.fdata.chunktab[i];
		dstobj->data.fdata.chunktab[i+dstchunks] = chunkid;
		if (chunkid>0) {
			if (shadow_chunk_add_file(chunkid,dstobj->id,i+dstchunks,dstobj->goal)!=STATUS_OK) {
				log_structure_error %= LOG_COUNT;
				if (log_structure_error++ == 0){
					 MFSLOG(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,srcobj->id,i);
				}
			}
		}
	}

	length = (((uint64_t)dstchunks)<<26)+srcobj->data.fdata.length;
	if (dstobj->type==TYPE_TRASH) {
		trashspace -= dstobj->data.fdata.length;
		trashspace += length;
	} else if (dstobj->type==TYPE_RESERVED) {
		reservedspace -= dstobj->data.fdata.length;
		reservedspace += length;
	}
	dstobj->data.fdata.length = length;
	fsnodes_get_stats(dstobj,&nsr);
	for (e=dstobj->parents ; e ; e=e->nextparent) {
		fsnodes_add_sub_stats(e->parent,&nsr,&psr);
	}
	dstobj->mtime = ts;
	dstobj->atime = ts;
	srcobj->atime = ts;
	return STATUS_OK;
}

static inline uint8_t fsnodes_appendchunks(uint32_t ts,fsnode *dstobj,fsnode *srcobj) {
	uint64_t chunkid,length;
	uint32_t i;
	uint32_t srcchunks,dstchunks;
	statsrecord psr,nsr;
	fsedge *e;

	srcchunks=0;
	for (i=0 ; i<srcobj->data.fdata.chunks ; i++) {
		if (srcobj->data.fdata.chunktab[i]!=0) {
			srcchunks = i+1;
		}
	}
	if (srcchunks==0) {
		return STATUS_OK;
	}
	dstchunks=0;
	for (i=0 ; i<dstobj->data.fdata.chunks ; i++) {
		if (dstobj->data.fdata.chunktab[i]!=0) {
			dstchunks = i+1;
		}
	}
	i = srcchunks+dstchunks-1;	// last new chunk pos
	if (i>MAX_INDEX) {	// chain too long
		return ERROR_INDEXTOOBIG;
	}
	fsnodes_get_stats(dstobj,&psr);
	if (i>=dstobj->data.fdata.chunks) {
		uint32_t newsize;
		if (i<8) {
			newsize=i+1;
		} else if (i<64) {
			newsize=(i&0xFFFFFFF8)+8;
		} else {
			newsize = (i&0xFFFFFFC0)+64;
		}
		if (dstobj->data.fdata.chunktab==NULL) {
			dstobj->data.fdata.chunktab = (uint64_t*)malloc(sizeof(uint64_t)*newsize);
		} else {
			dstobj->data.fdata.chunktab = (uint64_t*)realloc(dstobj->data.fdata.chunktab,sizeof(uint64_t)*newsize);
		}
		for (i=dstobj->data.fdata.chunks ; i<newsize ; i++) {
			dstobj->data.fdata.chunktab[i]=0;
		}
		dstobj->data.fdata.chunks = newsize;
	}

	for (i=0 ; i<srcchunks ; i++) {
		chunkid = srcobj->data.fdata.chunktab[i];
		dstobj->data.fdata.chunktab[i+dstchunks] = chunkid;
		if (chunkid>0) {
			if (chunk_add_file(chunkid,dstobj->id,i+dstchunks,dstobj->goal)!=STATUS_OK) {
				log_structure_error %= LOG_COUNT;
				if (log_structure_error++ == 0){
					 MFSLOG(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,srcobj->id,i);
				}
			}
		}
	}

	length = (((uint64_t)dstchunks)<<26)+srcobj->data.fdata.length;
	if (dstobj->type==TYPE_TRASH) {
		trashspace -= dstobj->data.fdata.length;
		trashspace += length;
	} else if (dstobj->type==TYPE_RESERVED) {
		reservedspace -= dstobj->data.fdata.length;
		reservedspace += length;
	}
	dstobj->data.fdata.length = length;
	fsnodes_get_stats(dstobj,&nsr);
	for (e=dstobj->parents ; e ; e=e->nextparent) {
		fsnodes_add_sub_stats(e->parent,&nsr,&psr);
	}
	dstobj->mtime = ts;
	dstobj->atime = ts;
	srcobj->atime = ts;
	return STATUS_OK;
}

static inline void fsnodes_changefilegoal(fsnode *obj,uint8_t goal) {
	uint32_t i;
	statsrecord psr,nsr;
	fsedge *e;

	fsnodes_get_stats(obj,&psr);
	nsr = psr;
	nsr.realsize = goal * nsr.size;
	for (e=obj->parents ; e ; e=e->nextparent) {
		fsnodes_add_sub_stats(e->parent,&nsr,&psr);
	}
	obj->goal = goal;
	for (i=0 ; i<obj->data.fdata.chunks ; i++) {
		if (obj->data.fdata.chunktab[i]>0) {
			chunk_set_file_goal(obj->data.fdata.chunktab[i],obj->id,i,goal);
		}
	}
}

static inline void shadow_fsnodes_setlength(fsnode *obj,uint64_t length) {
        uint32_t i,chunks;
        uint64_t chunkid;
        fsedge *e;
        statsrecord psr,nsr;
        fsnodes_get_stats(obj,&psr);
        if (obj->type==TYPE_TRASH) {
                trashspace -= obj->data.fdata.length;
                trashspace += length;
        } else if (obj->type==TYPE_RESERVED) {
                reservedspace -= obj->data.fdata.length;
                reservedspace += length;
        }       
        obj->data.fdata.length = length;
        if (length>0) {
                chunks = ((length-1)>>26)+1;
        } else {
                chunks = 0;
        }
        for (i=chunks ; i<obj->data.fdata.chunks ; i++) { 
                chunkid = obj->data.fdata.chunktab[i];
                if (chunkid>0) {
                        if (shadow_chunk_delete_file(chunkid,obj->id,i)!=STATUS_OK) {
                                log_structure_error %= LOG_COUNT;
                                if (log_structure_error++ == 0){
                                        MFSLOG(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,obj->id,i);
                                }
                        }
                }
                obj->data.fdata.chunktab[i]=0;  // raczej zbedne bo ponizej jest realloc, ale na wszelki wypadek niech bedzie
        }
        if (chunks>0) {
                if (chunks<obj->data.fdata.chunks && obj->data.fdata.chunktab) {
                        obj->data.fdata.chunktab = (uint64_t*)realloc(obj->data.fdata.chunktab,sizeof(uint64_t)*chunks);
                        obj->data.fdata.chunks = chunks;
                }       
        } else {
                if (obj->data.fdata.chunks>0 && obj->data.fdata.chunktab) {
                        free(obj->data.fdata.chunktab);
                        obj->data.fdata.chunktab = NULL;
                        obj->data.fdata.chunks = 0;
                }
        }
        fsnodes_get_stats(obj,&nsr);
        for (e=obj->parents ; e ; e=e->nextparent) {
                fsnodes_add_sub_stats(e->parent,&nsr,&psr);
        }
} 

static inline void fsnodes_setlength(fsnode *obj,uint64_t length) {
	uint32_t i,chunks;
	uint64_t chunkid;
	fsedge *e;
	statsrecord psr,nsr;
	fsnodes_get_stats(obj,&psr);
	if (obj->type==TYPE_TRASH) {
		trashspace -= obj->data.fdata.length;
		trashspace += length;
	} else if (obj->type==TYPE_RESERVED) {
		reservedspace -= obj->data.fdata.length;
		reservedspace += length;
	}
	obj->data.fdata.length = length;
	if (length>0) {
		chunks = ((length-1)>>26)+1;
	} else {
		chunks = 0;
	}
	for (i=chunks ; i<obj->data.fdata.chunks ; i++) {
		chunkid = obj->data.fdata.chunktab[i];
		if (chunkid>0) {
			if (chunk_delete_file(chunkid,obj->id,i)!=STATUS_OK) {
				log_structure_error %= LOG_COUNT;
                                if (log_structure_error++ == 0){
					MFSLOG(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,obj->id,i);
				}
			}
		}
		obj->data.fdata.chunktab[i]=0;	// raczej zbedne bo ponizej jest realloc, ale na wszelki wypadek niech bedzie
	}
	if (chunks>0) {
		if (chunks<obj->data.fdata.chunks && obj->data.fdata.chunktab) {
			obj->data.fdata.chunktab = (uint64_t*)realloc(obj->data.fdata.chunktab,sizeof(uint64_t)*chunks);
			obj->data.fdata.chunks = chunks;
		}
	} else {
		if (obj->data.fdata.chunks>0 && obj->data.fdata.chunktab) {
			free(obj->data.fdata.chunktab);
			obj->data.fdata.chunktab = NULL;
			obj->data.fdata.chunks = 0;
		}
	}
	fsnodes_get_stats(obj,&nsr);
	for (e=obj->parents ; e ; e=e->nextparent) {
		fsnodes_add_sub_stats(e->parent,&nsr,&psr);
	}
}

static inline void shadow_fsnodes_remove_node(uint32_t ts,fsnode *toremove) {
        uint32_t nodepos;
        fsnode **ptr;
        if (toremove->parents!=NULL) {
                return;
        }
// remove from idhash
        nodepos = NODEHASHPOS(toremove->id);
        ptr = &(nodehash[nodepos]);
        while (*ptr) {
                if (*ptr==toremove) {
                        *ptr=toremove->next;
                        break;
                }
                ptr = &((*ptr)->next);
        }       
// and free     
        nodes--;        
        if (toremove->type==TYPE_DIRECTORY) {
                dirnodes--;
                if (toremove->data.ddata.quota) {
                        fsnodes_delete_quotanode(toremove->data.ddata.quota);
                }
                free(toremove->data.ddata.stats);
        }
        if (toremove->type==TYPE_FILE || toremove->type==TYPE_TRASH || toremove->type==TYPE_RESERVED) {
                uint32_t i;
                uint64_t chunkid;
                filenodes--;
                for (i=0 ; i<toremove->data.fdata.chunks ; i++) {
                        chunkid = toremove->data.fdata.chunktab[i];
                        if (chunkid>0) {
                                if (shadow_chunk_delete_file(chunkid,toremove->id,i)!=STATUS_OK) {
                                        log_structure_error %= LOG_COUNT;
                                        if (log_structure_error++ == 0){
                                                MFSLOG(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,toremove->id,i);
                                        }
                                }
                        }
                }
                if (toremove->data.fdata.chunktab!=NULL) {
                        free(toremove->data.fdata.chunktab);
                }       
        }       
        if (toremove->type==TYPE_SYMLINK) {
                free(toremove->data.sdata.path);
        }
        fsnodes_free_id(toremove->id,ts);
        dcm_modify(toremove->id,0);
        free(toremove);
}

static inline void fsnodes_remove_node(uint32_t ts,fsnode *toremove) {
	uint32_t nodepos;
	fsnode **ptr;
	if (toremove->parents!=NULL) {
		return;
	}
// remove from idhash
	nodepos = NODEHASHPOS(toremove->id);
	ptr = &(nodehash[nodepos]);
	while (*ptr) {
		if (*ptr==toremove) {
			*ptr=toremove->next;
			break;
		}
		ptr = &((*ptr)->next);
	}
// and free
	nodes--;
	if (toremove->type==TYPE_DIRECTORY) {
		dirnodes--;
		if (toremove->data.ddata.quota) {
			fsnodes_delete_quotanode(toremove->data.ddata.quota);
		}
		free(toremove->data.ddata.stats);
	}
	if (toremove->type==TYPE_FILE || toremove->type==TYPE_TRASH || toremove->type==TYPE_RESERVED) {
		uint32_t i;
		uint64_t chunkid;
		filenodes--;
		for (i=0 ; i<toremove->data.fdata.chunks ; i++) {
			chunkid = toremove->data.fdata.chunktab[i];
			if (chunkid>0) {
				if (chunk_delete_file(chunkid,toremove->id,i)!=STATUS_OK) {
					log_structure_error %= LOG_COUNT;
                                	if (log_structure_error++ == 0){
						MFSLOG(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,toremove->id,i);
					}
				}
			}
		}
		if (toremove->data.fdata.chunktab!=NULL) {
			free(toremove->data.fdata.chunktab);
		}
	}
	if (toremove->type==TYPE_SYMLINK) {
		free(toremove->data.sdata.path);
	}
	fsnodes_free_id(toremove->id,ts);
	dcm_modify(toremove->id,0);
	free(toremove);
}


static inline void shadow_fsnodes_unlink(uint32_t ts,fsedge *e) {
	fsnode *child;
	uint16_t pleng=0;
	uint8_t *path=NULL;

	child = e->child;
	if (child->parents->nextparent==NULL) { // last link
		if (child->type==TYPE_FILE && (child->trashtime>0 || child->data.fdata.sessionids!=NULL)) {	// go to trash or reserved ? - get path
			fsnodes_getpath(e,&pleng,&path);
		}
	}
	shadow_fsnodes_remove_edge(ts,e);
	if (child->parents==NULL) {	// last link
		if (child->type == TYPE_FILE) {
			if (child->trashtime>0) {
				child->type = TYPE_TRASH;
				child->ctime = ts;
				e = malloc(sizeof(fsedge));
				e->nleng = pleng;
				e->name = path;
				e->child = child;
				e->parent = NULL;
				e->nextchild = trash;
				e->nextparent = NULL;
				e->prevchild = &trash;
				e->prevparent = &(child->parents);
				if (e->nextchild) {
					e->nextchild->prevchild = &(e->nextchild);
				}
				e->next = NULL;
				e->prev = NULL;
				trash = e;
				child->parents = e;
				trashspace += child->data.fdata.length;
				trashnodes++;
			} else if (child->data.fdata.sessionids!=NULL) {
				child->type = TYPE_RESERVED;
				e = malloc(sizeof(fsedge));
				e->nleng = pleng;
				e->name = path;
				e->child = child;
				e->parent = NULL;
				e->nextchild = reserved;
				e->nextparent = NULL;
				e->prevchild = &reserved;
				e->prevparent = &(child->parents);
				if (e->nextchild) {
					e->nextchild->prevchild = &(e->nextchild);
				}
#ifdef EDGEHASH
				e->next = NULL;
				e->prev = NULL;
#endif
				reserved = e;
				child->parents = e;
				reservedspace += child->data.fdata.length;
				reservednodes++;
			} else {
				shadow_fsnodes_remove_node(ts,child);
			}
		} else {
			shadow_fsnodes_remove_node(ts,child);
		}
	}
}

static inline void fsnodes_unlink(uint32_t ts,fsedge *e) {
	fsnode *child;
	uint16_t pleng=0;
	uint8_t *path=NULL;

	child = e->child;
	if (child->parents->nextparent==NULL) { // last link
		if (child->type==TYPE_FILE && (child->trashtime>0 || child->data.fdata.sessionids!=NULL)) {	// go to trash or reserved ? - get path
			fsnodes_getpath(e,&pleng,&path);
		}
	}
	fsnodes_remove_edge(ts,e);
	if (child->parents==NULL) {	// last link
		if (child->type == TYPE_FILE) {
			if (child->trashtime>0) {
				child->type = TYPE_TRASH;
				child->ctime = ts;
				e = malloc(sizeof(fsedge));
				e->nleng = pleng;
				e->name = path;
				e->child = child;
				e->parent = NULL;
				e->nextchild = trash;
				e->nextparent = NULL;
				e->prevchild = &trash;
				e->prevparent = &(child->parents);
				if (e->nextchild) {
					e->nextchild->prevchild = &(e->nextchild);
				}
				e->next = NULL;
				e->prev = NULL;
				trash = e;
				child->parents = e;
				trashspace += child->data.fdata.length;
				trashnodes++;
			} else if (child->data.fdata.sessionids!=NULL) {
				child->type = TYPE_RESERVED;
				e = malloc(sizeof(fsedge));
				e->nleng = pleng;
				e->name = path;
				e->child = child;
				e->parent = NULL;
				e->nextchild = reserved;
				e->nextparent = NULL;
				e->prevchild = &reserved;
				e->prevparent = &(child->parents);
				if (e->nextchild) {
					e->nextchild->prevchild = &(e->nextchild);
				}
#ifdef EDGEHASH
				e->next = NULL;
				e->prev = NULL;
#endif
				reserved = e;
				child->parents = e;
				reservedspace += child->data.fdata.length;
				reservednodes++;
			} else {
				fsnodes_remove_node(ts,child);
			}
		} else {
			fsnodes_remove_node(ts,child);
		}
	}
}

static inline int shadow_fsnodes_purge(uint32_t ts,fsnode *p) {
        fsedge *e;
        e = p->parents;

        if (p->type==TYPE_TRASH) {
                trashspace -= p->data.fdata.length;
                trashnodes--;
                if (p->data.fdata.sessionids!=NULL) {
                        p->type = TYPE_RESERVED;
                        reservedspace += p->data.fdata.length;
                        reservednodes++;
                        *(e->prevchild) = e->nextchild;
                        if (e->nextchild) {
                                e->nextchild->prevchild = e->prevchild;
                        }
                        e->nextchild = reserved;
                        e->prevchild = &(reserved);
                        if (e->nextchild) {
                                e->nextchild->prevchild = &(e->nextchild);
                        }
                        reserved = e;
                        return 0;
                } else {
                        shadow_fsnodes_remove_edge(ts,e);
                        shadow_fsnodes_remove_node(ts,p);
                        return 1;
                }
        } else if (p->type==TYPE_RESERVED) {
                reservedspace -= p->data.fdata.length;
                reservednodes--;
                shadow_fsnodes_remove_edge(ts,e);
                shadow_fsnodes_remove_node(ts,p);
                return 1;
        }
        return -1;
}

static inline int fsnodes_purge(uint32_t ts,fsnode *p) {
	fsedge *e;
	e = p->parents;

	if (p->type==TYPE_TRASH) {
		trashspace -= p->data.fdata.length;
		trashnodes--;
		if (p->data.fdata.sessionids!=NULL) {
			p->type = TYPE_RESERVED;
			reservedspace += p->data.fdata.length;
			reservednodes++;
			*(e->prevchild) = e->nextchild;
			if (e->nextchild) {
				e->nextchild->prevchild = e->prevchild;
			}
			e->nextchild = reserved;
			e->prevchild = &(reserved);
			if (e->nextchild) {
				e->nextchild->prevchild = &(e->nextchild);
			}
			reserved = e;
			return 0;
		} else {
			fsnodes_remove_edge(ts,e);
			fsnodes_remove_node(ts,p);
			return 1;
		}
	} else if (p->type==TYPE_RESERVED) {
		reservedspace -= p->data.fdata.length;
		reservednodes--;
		fsnodes_remove_edge(ts,e);
		fsnodes_remove_node(ts,p);
		return 1;
	}
	return -1;
}

static inline uint8_t fsnodes_undel(uint32_t ts,fsnode *node) {
	uint16_t pleng;
	const uint8_t *path;
	uint8_t new;
	uint32_t i,partleng,dots;
	fsedge *e,*pe;
	fsnode *p,*n;

/* check path */
	e = node->parents;
	pleng = e->nleng;
	path = e->name;

	if (path==NULL) {
		return ERROR_CANTCREATEPATH;
	}
	while (*path=='/' && pleng>0) {
		path++;
		pleng--;
	}
	if (pleng==0) {
		return ERROR_CANTCREATEPATH;
	}
	partleng=0;
	dots=0;
	for (i=0 ; i<pleng ; i++) {
		if (path[i]==0) {	// incorrect name character
			return ERROR_CANTCREATEPATH;
		} else if (path[i]=='/') {
			if (partleng==0) {	// "//" in path
				return ERROR_CANTCREATEPATH;
			}
			if (partleng==dots && partleng<=2) {	// '.' or '..' in path
				return ERROR_CANTCREATEPATH;
			}
			partleng=0;
			dots=0;
		} else {
			if (path[i]=='.') {
				dots++;
			}
			partleng++;
			if (partleng>MAXFNAMELENG) {
				return ERROR_CANTCREATEPATH;
			}
		}
	}
	if (partleng==0) {	// last part canot be empty - it's the name of undeleted file
		return ERROR_CANTCREATEPATH;
	}
	if (partleng==dots && partleng<=2) {	// '.' or '..' in path
		return ERROR_CANTCREATEPATH;
	}

/* create path */
	n = NULL;
	p = root;
	new = 0;
	for (;;) {
		if (p->data.ddata.quota && p->data.ddata.quota->exceeded) {
			return ERROR_QUOTA;
		}
		partleng=0;
		while (path[partleng]!='/' && partleng<pleng) {
			partleng++;
		}
		if (partleng==pleng) {	// last name
			if (fsnodes_nameisused(p,partleng,path)) {
				return ERROR_EEXIST;
			}
			// remove from trash and link to new parent
			fsnodes_link(ts,p,node,partleng,path);
			fsnodes_remove_edge(ts,e);
			node->type = TYPE_FILE;
			node->ctime = ts;
			trashspace -= node->data.fdata.length;
			trashnodes--;
			return STATUS_OK;
		} else {
			if (new==0) {
				pe = fsnodes_lookup(p,partleng,path);
				if (pe==NULL) {
					new=1;
				} else {
					n = pe->child;
					if (n->type!=TYPE_DIRECTORY) {
						return ERROR_CANTCREATEPATH;
					}
				}
			}
			if (new==1) {
				n = fsnodes_create_node(ts,p,partleng,path,TYPE_DIRECTORY,0755,0,0);
			}
			p = n;
		}
		path+=partleng+1;
		pleng-=partleng+1;
	}
}


/*
void fsnodes_get_file_stats(fsnode *node,uint32_t *undergoalfiles,uint32_t *missingfiles,uint32_t *chunks,uint32_t *undergoalchunks,uint32_t *missingchunks,uint64_t *length,uint64_t *size,uint64_t *gsize) {
	uint32_t i,ug,m,lastchunk,lastchunksize;
	uint8_t cnt;
	//assert(node->type==TYPE_FILE);
	(*length)+=node->data.fdata.length;
	if (node->data.fdata.length>0) {
		lastchunk = (node->data.fdata.length-1)>>26;
		lastchunksize = ((((node->data.fdata.length-1)&0x3FFFFFF)+0x10000)&0x7FFF0000)+0x1400;
	} else {
		lastchunk = 0;
		lastchunksize = 0x1400;
	}
	ug=0;
	m=0;
	for (i=0 ; i<node->data.fdata.chunks ; i++) {
		if (node->data.fdata.chunktab[i]>0) {
			chunk_get_validcopies(node->data.fdata.chunktab[i],&cnt);
			if (cnt<node->goal) {
				if (cnt==0) {
					m=1;
					(*missingchunks)++;
				} else {
					ug=1;
					//(*undergoalchunks)+=((node->goal)-cnt);
					(*undergoalchunks)++;
				}
			}
			if (i<lastchunk) {
				(*size)+=0x4001400UL;
				(*gsize)+=cnt*0x4001400UL;
			} else if (i==lastchunk) {
				(*size)+=lastchunksize;
				(*gsize)+=cnt*lastchunksize;
			}
			//(*chunks)+=cnt;
			(*chunks)++;
		}
	}
	(*undergoalfiles) += ug;
	(*missingfiles) += m;
}

void fsnodes_get_dir_stats(fsnode *node,uint32_t *inodes,uint32_t *dirs,uint32_t *files,uint32_t *undergoalfiles,uint32_t *missingfiles,uint32_t *chunks,uint32_t *undergoalchunks,uint32_t *missingchunks,uint64_t *length,uint64_t *size,uint64_t *gsize) {
	uint32_t i,ug,m,lastchunk,lastchunksize;
	uint8_t cnt;
	fsedge *e;
	fsnode *n;
	//assert(node->type==TYPE_DIRECTORY);
	for (e = node->data.ddata.children ; e ; e=e->nextchild) {
		n=e->child;
		(*inodes)++;
		if (n->type==TYPE_FILE) {
			(*length)+=n->data.fdata.length;
			if (n->data.fdata.length>0) {
				lastchunk = (n->data.fdata.length-1)>>26;
				lastchunksize = ((((n->data.fdata.length-1)&0x3FFFFFF)+0x10000)&0x7FFF0000)+0x1400;
			} else {
				lastchunk = 0;
				lastchunksize = 0x1400;
			}
			ug=0;
			m=0;
			for (i=0 ; i<n->data.fdata.chunks ; i++) {
				if (n->data.fdata.chunktab[i]>0) {
					chunk_get_validcopies(n->data.fdata.chunktab[i],&cnt);
					if (cnt<n->goal) {
						if (cnt==0) {
							m=1;
							(*missingchunks)++;
						} else {
							ug=1;
							//(*undergoalchunks)+=((ptr->goal)-cnt);
							(*undergoalchunks)++;
						}
					}
					if (i<lastchunk) {
						(*size)+=0x4001400UL;
						(*gsize)+=cnt*0x4001400UL;
					} else if (i==lastchunk) {
						(*size)+=lastchunksize;
						(*gsize)+=cnt*lastchunksize;
					}
					//(*chunks)+=cnt;
					(*chunks)++;
				}
			}
			(*undergoalfiles) += ug;
			(*missingfiles) += m;
			(*files)++;
		} else if (n->type==TYPE_DIRECTORY) {
			fsnodes_get_dir_stats(n,inodes,dirs,files,undergoalfiles,missingfiles,chunks,undergoalchunks,missingchunks,length,size,gsize);
			(*dirs)++;
		}
	}
}
*/

static inline void fsnodes_getgoal_recursive(fsnode *node,uint8_t gmode,uint32_t fgtab[10],uint32_t dgtab[10]) {
	fsedge *e;

	if (node->type==TYPE_FILE || node->type==TYPE_TRASH || node->type==TYPE_RESERVED) {
		if (node->goal>9) {
			MFSLOG(LOG_WARNING,"inode %"PRIu32": goal>9 !!! - fixing",node->id);
			fsnodes_changefilegoal(node,9);
		} else if (node->goal<1) {
			MFSLOG(LOG_WARNING,"inode %"PRIu32": goal<1 !!! - fixing",node->id);
			fsnodes_changefilegoal(node,1);
		}
		fgtab[node->goal]++;
	} else if (node->type==TYPE_DIRECTORY) {
		if (node->goal>9) {
			MFSLOG(LOG_WARNING,"inode %"PRIu32": goal>9 !!! - fixing",node->id);
			node->goal=9;
		} else if (node->goal<1) {
			MFSLOG(LOG_WARNING,"inode %"PRIu32": goal<1 !!! - fixing",node->id);
			node->goal=1;
		}
		dgtab[node->goal]++;
		if (gmode==GMODE_RECURSIVE) {
			for (e = node->data.ddata.children ; e ; e=e->nextchild) {
				fsnodes_getgoal_recursive(e->child,gmode,fgtab,dgtab);
			}
		}
	}
}

static inline void fsnodes_bst_add(bstnode **n,uint32_t val) {
	while (*n) {
		if (val<(*n)->val) {
			n = &((*n)->left);
		} else if (val>(*n)->val) {
			n = &((*n)->right);
		} else {
			(*n)->count++;
			return;
		}
	}
	(*n)=malloc(sizeof(bstnode));
	(*n)->val = val;
	(*n)->count = 1;
	(*n)->left = NULL;
	(*n)->right = NULL;
}

static inline uint32_t fsnodes_bst_nodes(bstnode *n) {
	if (n) {
		return 1+fsnodes_bst_nodes(n->left)+fsnodes_bst_nodes(n->right);
	} else {
		return 0;
	}
}

static inline void fsnodes_bst_storedata(bstnode *n,uint8_t **ptr) {
	if (n) {
		fsnodes_bst_storedata(n->left,ptr);
		put32bit(&*ptr,n->val);
		put32bit(&*ptr,n->count);
		fsnodes_bst_storedata(n->right,ptr);
	}
}

static inline void fsnodes_bst_free(bstnode *n) {
	if (n) {
		fsnodes_bst_free(n->left);
		fsnodes_bst_free(n->right);
		free(n);
	}
}

static inline void fsnodes_gettrashtime_recursive(fsnode *node,uint8_t gmode,bstnode **bstrootfiles,bstnode **bstrootdirs) {
	fsedge *e;

	if (node->type==TYPE_FILE || node->type==TYPE_TRASH || node->type==TYPE_RESERVED) {
		fsnodes_bst_add(bstrootfiles,node->trashtime);
	} else if (node->type==TYPE_DIRECTORY) {
		fsnodes_bst_add(bstrootdirs,node->trashtime);
		if (gmode==GMODE_RECURSIVE) {
			for (e = node->data.ddata.children ; e ; e=e->nextchild) {
				fsnodes_gettrashtime_recursive(e->child,gmode,bstrootfiles,bstrootdirs);
			}
		}
	}
}

static inline void fsnodes_geteattr_recursive(fsnode *node,uint8_t gmode,uint32_t feattrtab[16],uint32_t deattrtab[16]) {
	fsedge *e;

	if (node->type!=TYPE_DIRECTORY) {
		feattrtab[(node->mode>>12)&(EATTR_NOOWNER|EATTR_NOACACHE|EATTR_NODATACACHE)]++;
	} else {
		deattrtab[(node->mode>>12)]++;
		if (gmode==GMODE_RECURSIVE) {
			for (e = node->data.ddata.children ; e ; e=e->nextchild) {
				fsnodes_geteattr_recursive(e->child,gmode,feattrtab,deattrtab);
			}
		}
	}
}


#if VERSMID==7
#warning uncomment quota check
#endif
static inline void fsnodes_setgoal_recursive(fsnode *node,uint32_t ts,uint32_t uid/*,uint8_t quota*/,uint8_t goal,uint8_t smode,uint32_t *sinodes,uint32_t *ncinodes,uint32_t *nsinodes/*,uint32_t *qeinodes*/) {
	fsedge *e;
	uint8_t set;

	if (node->type==TYPE_FILE || node->type==TYPE_DIRECTORY || node->type==TYPE_TRASH || node->type==TYPE_RESERVED) {
		if ((node->mode&(EATTR_NOOWNER<<12))==0 && uid!=0 && node->uid!=uid) {
			(*nsinodes)++;
		} else {
			set=0;
			switch (smode&SMODE_TMASK) {
			case SMODE_SET:
				if (node->goal!=goal) {
					set=1;
				}
				break;
			case SMODE_INCREASE:
				if (node->goal<goal) {
					set=1;
				}
				break;
			case SMODE_DECREASE:
				if (node->goal>goal) {
					set=1;
				}
				break;
			}
			if (set) {
				if (node->type!=TYPE_DIRECTORY) {
//					if (quota && goal>node->goal) {
//						(*qenodes)++;
//					} else {
						fsnodes_changefilegoal(node,goal);
						(*sinodes)++;
//					}
				} else {
					node->goal=goal;
					(*sinodes)++;
				}
			} else {
				(*ncinodes)++;
			}
			node->ctime = ts;
		}
		if (node->type==TYPE_DIRECTORY && (smode&SMODE_RMASK)) {
//			if (quota==0 && node->data.ddata.quota && node->data.ddata.quota->exceeded) {
//				quota=1;
//			}
			for (e = node->data.ddata.children ; e ; e=e->nextchild) {
				fsnodes_setgoal_recursive(e->child,ts,uid/*,quota*/,goal,smode,sinodes,ncinodes,nsinodes/*,qenodes*/);
			}
		}
	}
}

static inline void fsnodes_settrashtime_recursive(fsnode *node,uint32_t ts,uint32_t uid,uint32_t trashtime,uint8_t smode,uint32_t *sinodes,uint32_t *ncinodes,uint32_t *nsinodes) {
	fsedge *e;
	uint8_t set;

	if (node->type==TYPE_FILE || node->type==TYPE_DIRECTORY || node->type==TYPE_TRASH || node->type==TYPE_RESERVED) {
		if ((node->mode&(EATTR_NOOWNER<<12))==0 && uid!=0 && node->uid!=uid) {
			(*nsinodes)++;
		} else {
			set=0;
			switch (smode&SMODE_TMASK) {
			case SMODE_SET:
				if (node->trashtime!=trashtime) {
					node->trashtime=trashtime;
					set=1;
				}
				break;
			case SMODE_INCREASE:
				if (node->trashtime<trashtime) {
					node->trashtime=trashtime;
					set=1;
				}
				break;
			case SMODE_DECREASE:
				if (node->trashtime>trashtime) {
					node->trashtime=trashtime;
					set=1;
				}
				break;
			}
			if (set) {
				(*sinodes)++;
			} else {
				(*ncinodes)++;
			}
			node->ctime = ts;
		}
		if (node->type==TYPE_DIRECTORY && (smode&SMODE_RMASK)) {
			for (e = node->data.ddata.children ; e ; e=e->nextchild) {
				fsnodes_settrashtime_recursive(e->child,ts,uid,trashtime,smode,sinodes,ncinodes,nsinodes);
			}
		}
	}
}

static inline void fsnodes_seteattr_recursive(fsnode *node,uint32_t ts,uint32_t uid,uint8_t eattr,uint8_t smode,uint32_t *sinodes,uint32_t *ncinodes,uint32_t *nsinodes) {
	fsedge *e;
	uint8_t neweattr,seattr;

	if ((node->mode&(EATTR_NOOWNER<<12))==0 && uid!=0 && node->uid!=uid) {
		(*nsinodes)++;
	} else {
		seattr = eattr;
		if (node->type!=TYPE_DIRECTORY) {
			node->mode &= ~(EATTR_NOECACHE<<12);
			seattr &= ~(EATTR_NOECACHE);
		}
		neweattr = (node->mode>>12);
		switch (smode&SMODE_TMASK) {
			case SMODE_SET:
				neweattr = seattr;
				break;
			case SMODE_INCREASE:
				neweattr |= seattr;
				break;
			case SMODE_DECREASE:
				neweattr &= ~seattr;
				break;
		}
		if (neweattr!=(node->mode>>12)) {
			node->mode = (node->mode&0xFFF) | (((uint16_t)neweattr)<<12);
			(*sinodes)++;
		} else {
			(*ncinodes)++;
		}
		node->ctime = ts;
	}
	if (node->type==TYPE_DIRECTORY && (smode&SMODE_RMASK)) {
		for (e = node->data.ddata.children ; e ; e=e->nextchild) {
			fsnodes_seteattr_recursive(e->child,ts,uid,eattr,smode,sinodes,ncinodes,nsinodes);
		}
	}
}


static inline void shadow_fsnodes_snapshot(uint32_t ts,fsnode *srcnode,fsnode *parentnode,uint32_t nleng,const uint8_t *name) {
	fsedge *e;
	fsnode *dstnode;
	uint32_t i;
	uint64_t chunkid;
	if ((e=fsnodes_lookup(parentnode,nleng,name))) {
		dstnode = e->child;
		if (srcnode->type==TYPE_DIRECTORY) {
			for (e = srcnode->data.ddata.children ; e ; e=e->nextchild) {
				shadow_fsnodes_snapshot(ts,e->child,dstnode,e->nleng,e->name);
			}
		} else if (srcnode->type==TYPE_FILE) {
			uint8_t same;
			if (dstnode->data.fdata.length==srcnode->data.fdata.length && dstnode->data.fdata.chunks==srcnode->data.fdata.chunks) {
				same=1;
				for (i=0 ; i<srcnode->data.fdata.chunks && same ; i++) {
					if (srcnode->data.fdata.chunktab[i]!=dstnode->data.fdata.chunktab[i]) {
						same=0;
					}
				}
			} else {
				same=0;
			}
			if (same==0) {
				statsrecord psr,nsr;
				shadow_fsnodes_unlink(ts,e);
				dstnode = fsnodes_create_node(ts,parentnode,nleng,name,TYPE_FILE,srcnode->mode,srcnode->uid,srcnode->gid);
				fsnodes_get_stats(dstnode,&psr);
				dstnode->goal = srcnode->goal;
				dstnode->trashtime = srcnode->trashtime;
//				dstnode->mode = srcnode->mode;
//				dstnode->atime = srcnode->atime;
//				dstnode->mtime = srcnode->mtime;
				dstnode->data.fdata.chunktab = (uint64_t*)malloc(sizeof(uint64_t)*(srcnode->data.fdata.chunks));
				dstnode->data.fdata.chunks = srcnode->data.fdata.chunks;
				for (i=0 ; i<srcnode->data.fdata.chunks ; i++) {
					chunkid = srcnode->data.fdata.chunktab[i];
					dstnode->data.fdata.chunktab[i] = chunkid;
					if (chunkid>0) {
						if (shadow_chunk_add_file(chunkid,dstnode->id,i,dstnode->goal)!=STATUS_OK) {
                                			log_structure_error %= LOG_COUNT;
                                			if (log_structure_error++ == 0){	
								MFSLOG(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,srcnode->id,i);
							}
						}
					}
				}
				dstnode->data.fdata.length = srcnode->data.fdata.length;
				fsnodes_get_stats(dstnode,&nsr);
				fsnodes_add_sub_stats(parentnode,&nsr,&psr);
			}
		} else if (srcnode->type==TYPE_SYMLINK) {
			if (dstnode->data.sdata.pleng!=srcnode->data.sdata.pleng) {
				statsrecord sr;
				memset(&sr,0,sizeof(statsrecord));
				sr.length = dstnode->data.sdata.pleng-srcnode->data.sdata.pleng;
				fsnodes_add_stats(parentnode,&sr);
			}
			if (dstnode->data.sdata.path) {
				free(dstnode->data.sdata.path);
			}
			if (srcnode->data.sdata.pleng>0) {
				dstnode->data.sdata.path = malloc(srcnode->data.sdata.pleng);
				if (dstnode->data.sdata.path!=NULL) {
					memcpy(dstnode->data.sdata.path,srcnode->data.sdata.path,srcnode->data.sdata.pleng);
					dstnode->data.sdata.pleng = srcnode->data.sdata.pleng;
				} else {
					dstnode->data.sdata.pleng=0;
				}
			} else {
				dstnode->data.sdata.path=NULL;
				dstnode->data.sdata.pleng=0;
			}
		} else if (srcnode->type==TYPE_BLOCKDEV || srcnode->type==TYPE_CHARDEV) {
			dstnode->data.rdev = srcnode->data.rdev;
		}
		dstnode->mode = srcnode->mode;
		dstnode->uid = srcnode->uid;
		dstnode->gid = srcnode->gid;
		dstnode->atime = srcnode->atime;
		dstnode->mtime = srcnode->mtime;
		dstnode->ctime = ts;
	} else {
		if (srcnode->type==TYPE_FILE || srcnode->type==TYPE_DIRECTORY || srcnode->type==TYPE_SYMLINK || srcnode->type==TYPE_BLOCKDEV || srcnode->type==TYPE_CHARDEV || srcnode->type==TYPE_SOCKET || srcnode->type==TYPE_FIFO) {
			statsrecord psr,nsr;
			dstnode = fsnodes_create_node(ts,parentnode,nleng,name,srcnode->type,srcnode->mode,srcnode->uid,srcnode->gid);
			fsnodes_get_stats(dstnode,&psr);
			dstnode->goal = srcnode->goal;
			dstnode->trashtime = srcnode->trashtime;
			dstnode->mode = srcnode->mode;
			dstnode->atime = srcnode->atime;
			dstnode->mtime = srcnode->mtime;
			if (srcnode->type==TYPE_DIRECTORY) {
				for (e = srcnode->data.ddata.children ; e ; e=e->nextchild) {
					shadow_fsnodes_snapshot(ts,e->child,dstnode,e->nleng,e->name);
				}
			} else if (srcnode->type==TYPE_FILE) {
				dstnode->data.fdata.chunktab = (uint64_t*)malloc(sizeof(uint64_t)*(srcnode->data.fdata.chunks));
				dstnode->data.fdata.chunks = srcnode->data.fdata.chunks;
				for (i=0 ; i<srcnode->data.fdata.chunks ; i++) {
					chunkid = srcnode->data.fdata.chunktab[i];
					dstnode->data.fdata.chunktab[i] = chunkid;
					if (chunkid>0) {
						if (shadow_chunk_add_file(chunkid,dstnode->id,i,dstnode->goal)!=STATUS_OK) {
							log_structure_error %= LOG_COUNT;
                                			if (log_structure_error++ == 0){
								MFSLOG(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,srcnode->id,i);
							}
						}
					}
				}
				dstnode->data.fdata.length = srcnode->data.fdata.length;
				fsnodes_get_stats(dstnode,&nsr);
				fsnodes_add_sub_stats(parentnode,&nsr,&psr);
			} else if (srcnode->type==TYPE_SYMLINK) {
				if (srcnode->data.sdata.pleng>0) {
					dstnode->data.sdata.path = malloc(srcnode->data.sdata.pleng);
					if (dstnode->data.sdata.path!=NULL) {
						memcpy(dstnode->data.sdata.path,srcnode->data.sdata.path,srcnode->data.sdata.pleng);
						dstnode->data.sdata.pleng = srcnode->data.sdata.pleng;
					}
				}
				fsnodes_get_stats(dstnode,&nsr);
				fsnodes_add_sub_stats(parentnode,&nsr,&psr);
			} else if (srcnode->type==TYPE_BLOCKDEV || srcnode->type==TYPE_CHARDEV) {
				dstnode->data.rdev = srcnode->data.rdev;
			}
		}
	}
}

static inline void fsnodes_snapshot(uint32_t ts,fsnode *srcnode,fsnode *parentnode,uint32_t nleng,const uint8_t *name) {
	fsedge *e;
	fsnode *dstnode;
	uint32_t i;
	uint64_t chunkid;
	if ((e=fsnodes_lookup(parentnode,nleng,name))) {
		dstnode = e->child;
		if (srcnode->type==TYPE_DIRECTORY) {
			for (e = srcnode->data.ddata.children ; e ; e=e->nextchild) {
				fsnodes_snapshot(ts,e->child,dstnode,e->nleng,e->name);
			}
		} else if (srcnode->type==TYPE_FILE) {
			uint8_t same;
			if (dstnode->data.fdata.length==srcnode->data.fdata.length && dstnode->data.fdata.chunks==srcnode->data.fdata.chunks) {
				same=1;
				for (i=0 ; i<srcnode->data.fdata.chunks && same ; i++) {
					if (srcnode->data.fdata.chunktab[i]!=dstnode->data.fdata.chunktab[i]) {
						same=0;
					}
				}
			} else {
				same=0;
			}
			if (same==0) {
				statsrecord psr,nsr;
				fsnodes_unlink(ts,e);
				dstnode = fsnodes_create_node(ts,parentnode,nleng,name,TYPE_FILE,srcnode->mode,srcnode->uid,srcnode->gid);
				fsnodes_get_stats(dstnode,&psr);
				dstnode->goal = srcnode->goal;
				dstnode->trashtime = srcnode->trashtime;
//				dstnode->mode = srcnode->mode;
//				dstnode->atime = srcnode->atime;
//				dstnode->mtime = srcnode->mtime;
				dstnode->data.fdata.chunktab = (uint64_t*)malloc(sizeof(uint64_t)*(srcnode->data.fdata.chunks));
				dstnode->data.fdata.chunks = srcnode->data.fdata.chunks;
				for (i=0 ; i<srcnode->data.fdata.chunks ; i++) {
					chunkid = srcnode->data.fdata.chunktab[i];
					dstnode->data.fdata.chunktab[i] = chunkid;
					if (chunkid>0) {
						if (chunk_add_file(chunkid,dstnode->id,i,dstnode->goal)!=STATUS_OK) {
                                			log_structure_error %= LOG_COUNT;
                                			if (log_structure_error++ == 0){	
								MFSLOG(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,srcnode->id,i);
							}
						}
					}
				}
				dstnode->data.fdata.length = srcnode->data.fdata.length;
				fsnodes_get_stats(dstnode,&nsr);
				fsnodes_add_sub_stats(parentnode,&nsr,&psr);
			}
		} else if (srcnode->type==TYPE_SYMLINK) {
			if (dstnode->data.sdata.pleng!=srcnode->data.sdata.pleng) {
				statsrecord sr;
				memset(&sr,0,sizeof(statsrecord));
				sr.length = dstnode->data.sdata.pleng-srcnode->data.sdata.pleng;
				fsnodes_add_stats(parentnode,&sr);
			}
			if (dstnode->data.sdata.path) {
				free(dstnode->data.sdata.path);
			}
			if (srcnode->data.sdata.pleng>0) {
				dstnode->data.sdata.path = malloc(srcnode->data.sdata.pleng);
				if (dstnode->data.sdata.path!=NULL) {
					memcpy(dstnode->data.sdata.path,srcnode->data.sdata.path,srcnode->data.sdata.pleng);
					dstnode->data.sdata.pleng = srcnode->data.sdata.pleng;
				} else {
					dstnode->data.sdata.pleng=0;
				}
			} else {
				dstnode->data.sdata.path=NULL;
				dstnode->data.sdata.pleng=0;
			}
		} else if (srcnode->type==TYPE_BLOCKDEV || srcnode->type==TYPE_CHARDEV) {
			dstnode->data.rdev = srcnode->data.rdev;
		}
		dstnode->mode = srcnode->mode;
		dstnode->uid = srcnode->uid;
		dstnode->gid = srcnode->gid;
		dstnode->atime = srcnode->atime;
		dstnode->mtime = srcnode->mtime;
		dstnode->ctime = ts;
	} else {
		if (srcnode->type==TYPE_FILE || srcnode->type==TYPE_DIRECTORY || srcnode->type==TYPE_SYMLINK || srcnode->type==TYPE_BLOCKDEV || srcnode->type==TYPE_CHARDEV || srcnode->type==TYPE_SOCKET || srcnode->type==TYPE_FIFO) {
			statsrecord psr,nsr;
			dstnode = fsnodes_create_node(ts,parentnode,nleng,name,srcnode->type,srcnode->mode,srcnode->uid,srcnode->gid);
			fsnodes_get_stats(dstnode,&psr);
			dstnode->goal = srcnode->goal;
			dstnode->trashtime = srcnode->trashtime;
			dstnode->mode = srcnode->mode;
			dstnode->atime = srcnode->atime;
			dstnode->mtime = srcnode->mtime;
			if (srcnode->type==TYPE_DIRECTORY) {
				for (e = srcnode->data.ddata.children ; e ; e=e->nextchild) {
					fsnodes_snapshot(ts,e->child,dstnode,e->nleng,e->name);
				}
			} else if (srcnode->type==TYPE_FILE) {
				dstnode->data.fdata.chunktab = (uint64_t*)malloc(sizeof(uint64_t)*(srcnode->data.fdata.chunks));
				dstnode->data.fdata.chunks = srcnode->data.fdata.chunks;
				for (i=0 ; i<srcnode->data.fdata.chunks ; i++) {
					chunkid = srcnode->data.fdata.chunktab[i];
					dstnode->data.fdata.chunktab[i] = chunkid;
					if (chunkid>0) {
						if (chunk_add_file(chunkid,dstnode->id,i,dstnode->goal)!=STATUS_OK) {
							log_structure_error %= LOG_COUNT;
                                			if (log_structure_error++ == 0){
								MFSLOG(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,srcnode->id,i);
							}
						}
					}
				}
				dstnode->data.fdata.length = srcnode->data.fdata.length;
				fsnodes_get_stats(dstnode,&nsr);
				fsnodes_add_sub_stats(parentnode,&nsr,&psr);
			} else if (srcnode->type==TYPE_SYMLINK) {
				if (srcnode->data.sdata.pleng>0) {
					dstnode->data.sdata.path = malloc(srcnode->data.sdata.pleng);
					if (dstnode->data.sdata.path!=NULL) {
						memcpy(dstnode->data.sdata.path,srcnode->data.sdata.path,srcnode->data.sdata.pleng);
						dstnode->data.sdata.pleng = srcnode->data.sdata.pleng;
					}
				}
				fsnodes_get_stats(dstnode,&nsr);
				fsnodes_add_sub_stats(parentnode,&nsr,&psr);
			} else if (srcnode->type==TYPE_BLOCKDEV || srcnode->type==TYPE_CHARDEV) {
				dstnode->data.rdev = srcnode->data.rdev;
			}
		}
	}
}

static inline uint8_t fsnodes_snapshot_test(fsnode *origsrcnode,fsnode *srcnode,fsnode *parentnode,uint32_t nleng,const uint8_t *name,uint8_t canoverwrite) {
	fsedge *e;
	fsnode *dstnode;
	uint8_t status;
	if ((e=fsnodes_lookup(parentnode,nleng,name))) {
		dstnode = e->child;
		if (dstnode==origsrcnode) {
			return ERROR_EINVAL;
		}
		if (dstnode->type!=srcnode->type) {
			return ERROR_EPERM;
		}
		if (srcnode->type==TYPE_TRASH || srcnode->type==TYPE_RESERVED) {
			return ERROR_EPERM;
		}
		if (srcnode->type==TYPE_DIRECTORY) {
			for (e = srcnode->data.ddata.children ; e ; e=e->nextchild) {
				status = fsnodes_snapshot_test(origsrcnode,e->child,dstnode,e->nleng,e->name,canoverwrite);
				if (status!=STATUS_OK) {
					return status;
				}
			}
		} else if (canoverwrite==0) {
			return ERROR_EEXIST;
		}
	}
	return STATUS_OK;
}

static inline int fsnodes_namecheck(uint32_t nleng,const uint8_t *name) {
	uint32_t i;
	if (nleng==0 || nleng>MAXFNAMELENG) {
		return -1;
	}
	if (name[0]=='.') {
		if (nleng==1) {
			return -1;
		}
		if (nleng==2 && name[1]=='.') {
			return -1;
		}
	}
	for (i=0 ; i<nleng ; i++) {
		if (name[i]=='\0' || name[i]=='/') {
			return -1;
		}
	}
	return 0;
}

static inline int fsnodes_access(fsnode *node,uint32_t uid,uint32_t gid,uint8_t modemask,uint8_t sesflags) {
	uint8_t nodemode;
	if (uid==0) {
		return 1;
	}
	if (uid==node->uid || (node->mode&(EATTR_NOOWNER<<12))) {
		nodemode = ((node->mode)>>6) & 7;
	} else if (sesflags&SESFLAG_IGNOREGID) {
		nodemode = (((node->mode)>>3) | (node->mode)) & 7;
	} else if (gid==node->gid) {
		nodemode = ((node->mode)>>3) & 7;
	} else {
		nodemode = (node->mode & 7);
	}
	if ((nodemode & modemask) == modemask) {
		return 1;
	}
	return 0;
}

static inline int fsnodes_sticky_access(fsnode *parent,fsnode *node,uint32_t uid) {
	if (uid==0 || (parent->mode&01000)==0) {	// super user or sticky bit is not set
		return 1;
	}
	if (uid==parent->uid || (parent->mode&(EATTR_NOOWNER<<12)) || uid==node->uid || (node->mode&(EATTR_NOOWNER<<12))) {
		return 1;
	}
	return 0;
}

/* master <-> fuse operations */


uint8_t shadow_fs_access(uint32_t ts,uint32_t inode) {
	fsnode *p;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	p->atime = ts;
	version++;
	return STATUS_OK;
}

uint8_t fs_readreserved_size(uint32_t rootinode,uint8_t sesflags,uint32_t *dbuffsize) {
	if (rootinode!=0) {
		return ERROR_EPERM;
	}
	(void)sesflags;
	*dbuffsize = fsnodes_getdetachedsize(reserved);
	return STATUS_OK;
}

void fs_readreserved_data(uint32_t rootinode,uint8_t sesflags,uint8_t *dbuff) {
	(void)rootinode;
	(void)sesflags;
	fsnodes_getdetacheddata(reserved,dbuff);
}


uint8_t fs_readtrash_size(uint32_t rootinode,uint8_t sesflags,uint32_t *dbuffsize) {
	if (rootinode!=0) {
		return ERROR_EPERM;
	}
	(void)sesflags;
	*dbuffsize = fsnodes_getdetachedsize(trash);
	return STATUS_OK;
}

void fs_readtrash_data(uint32_t rootinode,uint8_t sesflags,uint8_t *dbuff) {
	(void)rootinode;
	(void)sesflags;
	fsnodes_getdetacheddata(trash,dbuff);
}

/* common procedure for trash and reserved files */
uint8_t fs_getdetachedattr(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint8_t attr[35],uint8_t dtype) {
	fsnode *p;
	memset(attr,0,35);
	if (rootinode!=0) {
		return ERROR_EPERM;
	}
	(void)sesflags;
	if (!DTYPE_ISVALID(dtype)) {
		return ERROR_EINVAL;
	}
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_ENOENT;
	}
	if (dtype==DTYPE_TRASH && p->type==TYPE_RESERVED) {
		return ERROR_ENOENT;
	}
	if (dtype==DTYPE_RESERVED && p->type==TYPE_TRASH) {
		return ERROR_ENOENT;
	}
	fsnodes_fill_attr(p,NULL,p->uid,p->gid,p->uid,p->gid,sesflags,attr);
	return STATUS_OK;
}

uint8_t fs_gettrashpath(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t *pleng,uint8_t **path) {
	fsnode *p;
	*pleng = 0;
	*path = NULL;
	if (rootinode!=0) {
		return ERROR_EPERM;
	}
	(void)sesflags;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_TRASH) {
		return ERROR_ENOENT;
	}
	*pleng = p->parents->nleng;
	*path = p->parents->name;
	return STATUS_OK;
}

uint8_t shadow_fs_setpath(uint32_t inode,const uint8_t *path) {
	uint32_t pleng;
	fsnode *p;
	uint8_t *newpath;
	pleng = strlen((char*)path);
	p = fsnodes_id_to_node(inode);
        if (!p) {
                return ERROR_ENOENT;
        }
        if (p->type!=TYPE_TRASH) {
                return ERROR_ENOENT;
        }
        newpath = malloc(pleng);
        if (newpath==NULL) {
                return ERROR_EINVAL;
        }
        free(p->parents->name);
        memcpy(newpath,path,pleng);
        p->parents->name = newpath;
        p->parents->nleng = pleng;
	version++;
	return STATUS_OK;
}

uint8_t fs_settrashpath(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t pleng,const uint8_t *path) {
	fsnode *p;
	uint8_t *newpath;
	uint32_t i;
	if (rootinode!=0) {
		return ERROR_EPERM;
	}
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (pleng==0) {
		return ERROR_EINVAL;
	}
	for (i=0 ; i<pleng ; i++) {
		if (path[i]==0) {
			return ERROR_EINVAL;
		}
	}
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_TRASH) {
		return ERROR_ENOENT;
	}
	newpath = malloc(pleng);
	if (newpath==NULL) {
		return ERROR_EINVAL;	// no mem ?
	}
	free(p->parents->name);
	memcpy(newpath,path,pleng);
	p->parents->name = newpath;
	p->parents->nleng = pleng;
	//changelog(version++,"%"PRIu32"|SETPATH(%"PRIu32",%s)",(uint32_t)get_current_time(),inode,fsnodes_escape_name(pleng,newpath));
	return STATUS_OK;
}

uint8_t shadow_fs_undel(uint32_t ts,uint32_t inode) {
        fsnode *p;
        uint8_t status;
	p = fsnodes_id_to_node(inode);
        if (!p) {
                return ERROR_ENOENT;
        }
        if (p->type!=TYPE_TRASH) {
                return ERROR_ENOENT;
        }
        status = fsnodes_undel(ts,p);
	version++;
	return STATUS_OK;
}

uint8_t fs_undel(uint32_t rootinode,uint8_t sesflags,uint32_t inode) {
	uint32_t ts;
	fsnode *p;
	uint8_t status;
	if (rootinode!=0) {
		return ERROR_EPERM;
	}
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	ts = get_current_time();
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_TRASH) {
		return ERROR_ENOENT;
	}
	status = fsnodes_undel(ts,p);
	if (status==STATUS_OK) {
		//changelog(version++,"%"PRIu32"|UNDEL(%"PRIu32")",ts,inode);
	}
	return status;
}

uint8_t shadow_fs_purge(uint32_t ts,uint32_t inode) {
	fsnode *p;
	p = fsnodes_id_to_node(inode);
        if (!p) {
                return ERROR_ENOENT;
        }
        if (p->type!=TYPE_TRASH) {
                return ERROR_ENOENT;
        }
        shadow_fsnodes_purge(ts,p);
	version++;
	return STATUS_OK;
}

uint8_t fs_purge(uint32_t rootinode,uint8_t sesflags,uint32_t inode) {
	uint32_t ts;
	fsnode *p;
	if (rootinode!=0) {
		return ERROR_EPERM;
	}
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	ts = get_current_time();
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_TRASH) {
		return ERROR_ENOENT;
	}
	fsnodes_purge(ts,p);
	//changelog(version++,"%"PRIu32"|PURGE(%"PRIu32")",ts,inode);
	return STATUS_OK;
}

void fs_info(uint64_t *totalspace,uint64_t *availspace,uint64_t *trspace,uint32_t *trnodes,uint64_t *respace,uint32_t *renodes,uint32_t *inodes,uint32_t *dnodes,uint32_t *fnodes) {
	matocsserv_getspace(totalspace,availspace);
	*trspace = trashspace;
	*trnodes = trashnodes;
	*respace = reservedspace;
	*renodes = reservednodes;
	*inodes = nodes;
	*dnodes = dirnodes;
	*fnodes = filenodes;
}

uint8_t fs_getrootinode(uint32_t *rootinode,const uint8_t *path) {
	uint32_t nleng;
	const uint8_t *name;
	fsnode *p;
	fsedge *e;

	name = path;
	p = root;
	for (;;) {
		while (*name=='/') {
			name++;
		}
		if (*name=='\0') {
			*rootinode = p->id;
			return STATUS_OK;
		}
		nleng=0;
		while (name[nleng] && name[nleng]!='/') {
			nleng++;
		}
		if (fsnodes_namecheck(nleng,name)<0) {
			return ERROR_EINVAL;
		}
		e = fsnodes_lookup(p,nleng,name);
		if (!e) {
			return ERROR_ENOENT;
		}
		p = e->child;
		if (p->type!=TYPE_DIRECTORY) {
			return ERROR_ENOTDIR;
		}
		name += nleng;
	}
}

void fs_statfs(uint32_t rootinode,uint8_t sesflags,uint64_t *totalspace,uint64_t *availspace,uint64_t *trspace,uint64_t *respace,uint32_t *inodes) {
	fsnode *rn;
	quotanode *qn;
	statsrecord sr;
	(void)sesflags;
//	matocsserv_getspace(totalspace,availspace);
//	*inodes = nodes;
//	*trspace = trashspace;
//	*respace = reservedspace;
	if (rootinode==MFS_ROOT_ID) {
		*trspace = trashspace;
		*respace = reservedspace;
		rn = root;
	} else {
		*trspace = 0;
		*respace = 0;
		rn = fsnodes_id_to_node(rootinode);
	}
	if (!rn || rn->type!=TYPE_DIRECTORY) {
		*totalspace = 0;
		*availspace = 0;
		*inodes = 0;
	} else {
		matocsserv_getspace(totalspace,availspace);
		fsnodes_get_stats(rn,&sr);
		*inodes = sr.inodes;
		qn = rn->data.ddata.quota;
		if (qn && (qn->flags&QUOTA_FLAG_HREALSIZE)) {
			if (sr.realsize>=qn->hrealsize) {
				*availspace = 0;
			} else if (*availspace > qn->hrealsize - sr.realsize) {
				*availspace = qn->hrealsize - sr.realsize;
			}
			if (*totalspace > qn->hrealsize) {
				*totalspace = qn->hrealsize;
			}
		}
		if (sr.realsize + *availspace < *totalspace) {
			*totalspace = sr.realsize + *availspace;
		}
	}
	stats_statfs++;
}

uint8_t fs_access(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t uid,uint32_t gid,int modemask) {
	fsnode *p,*rn;
	if ((sesflags&SESFLAG_READONLY) && (modemask&MODE_MASK_W)) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	return fsnodes_access(p,uid,gid,modemask,sesflags)?STATUS_OK:ERROR_EACCES;
}

uint8_t fs_lookup(uint32_t rootinode,uint8_t sesflags,uint32_t parent,uint16_t nleng,const uint8_t *name,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint32_t *inode,uint8_t attr[35]) {
	fsnode *wd,*rn;
	fsedge *e;
	*inode = 0;
	memset(attr,0,35);
	if (rootinode==MFS_ROOT_ID) {
		rn = root;
		wd = fsnodes_id_to_node(parent);
		if (!wd) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (parent==MFS_ROOT_ID) {
			parent = rootinode;
			wd = rn;
		} else {
			wd = fsnodes_id_to_node(parent);
			if (!wd) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,wd)) {
				return ERROR_EPERM;
			}
		}
	}
	if (wd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (!fsnodes_access(wd,uid,gid,MODE_MASK_X,sesflags)) {
		return ERROR_EACCES;
	}
	if (name[0]=='.') {
		if (nleng==1) {	// self
			if (parent==rootinode) {
				*inode = MFS_ROOT_ID;
			} else {
				*inode = wd->id;
			}
			fsnodes_fill_attr(wd,wd,uid,gid,auid,agid,sesflags,attr);
			stats_lookup++;
			return STATUS_OK;
		}
		if (nleng==2 && name[1]=='.') {	// parent
			if (parent==rootinode) {
				*inode = MFS_ROOT_ID;
				fsnodes_fill_attr(wd,wd,uid,gid,auid,agid,sesflags,attr);
			} else {
				if (wd->parents) {
					if (wd->parents->parent->id==rootinode) {
						*inode = MFS_ROOT_ID;
					} else {
						*inode = wd->parents->parent->id;
					}
					fsnodes_fill_attr(wd->parents->parent,wd,uid,gid,auid,agid,sesflags,attr);
				} else {
					*inode=MFS_ROOT_ID; // rn->id;
					fsnodes_fill_attr(rn,wd,uid,gid,auid,agid,sesflags,attr);
				}
			}
			stats_lookup++;
			return STATUS_OK;
		}
	}
	if (fsnodes_namecheck(nleng,name)<0) {
		return ERROR_EINVAL;
	}
	e = fsnodes_lookup(wd,nleng,name);
	if (!e) {
		return ERROR_ENOENT;
	}
	*inode = e->child->id;
	fsnodes_fill_attr(e->child,wd,uid,gid,auid,agid,sesflags,attr);
	stats_lookup++;
	return STATUS_OK;
}

uint8_t fs_getattr(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint8_t attr[35]) {
	fsnode *p,*rn;
	(void)sesflags;
	memset(attr,0,35);
	if (rootinode==MFS_ROOT_ID) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	fsnodes_fill_attr(p,NULL,uid,gid,auid,agid,sesflags,attr);
	stats_getattr++;
	return STATUS_OK;
}

uint8_t fs_try_setlength(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint8_t opened,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint64_t length,uint8_t attr[35],uint64_t *chunkid) {
	fsnode *p,*rn;
	memset(attr,0,35);
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (opened==0) {
		if (!fsnodes_access(p,uid,gid,MODE_MASK_W,sesflags)) {
			return ERROR_EACCES;
		}
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	if (length>p->data.fdata.length) {
		if (fsnodes_test_quota(p)) {
			return ERROR_QUOTA;
		}
	}
	if (length&0x3FFFFFF) {
		uint32_t indx = (length>>26);
		if (indx<p->data.fdata.chunks) {
			uint64_t ochunkid = p->data.fdata.chunktab[indx];
			if (ochunkid>0) {
				uint8_t status;
				uint64_t nchunkid;
				status = chunk_multi_truncate(&nchunkid,ochunkid,length&0x3FFFFFF,inode,indx,p->goal);
				if (status!=STATUS_OK) {
					return status;
				}
				p->data.fdata.chunktab[indx] = nchunkid;
				*chunkid = nchunkid;
				//changelog(version++,"%"PRIu32"|TRUNC(%"PRIu32",%"PRIu32"):%"PRIu64,(uint32_t)get_current_time(),inode,indx,nchunkid);
				return ERROR_DELAYED;
			}
		}
	}
	fsnodes_fill_attr(p,NULL,uid,gid,auid,agid,sesflags,attr);
	stats_setattr++;
	return STATUS_OK;
}

uint8_t shadow_fs_trunc(uint32_t ts,uint32_t inode,uint32_t indx,uint64_t chunkid) {
	uint64_t ochunkid,nchunkid;
	uint8_t status;
	fsnode *p;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EINVAL;
	}
	if (indx>MAX_INDEX) {
		return ERROR_INDEXTOOBIG;
	}
	if (indx>=p->data.fdata.chunks) {
		return ERROR_EINVAL;
	}
	ochunkid = p->data.fdata.chunktab[indx];
	status = shadow_chunk_multi_truncate(ts,&nchunkid,ochunkid,inode,indx,p->goal);
	if (status!=STATUS_OK) {
		return status;
	}
	if (chunkid!=nchunkid) {
		return ERROR_MISMATCH;
	}
	p->data.fdata.chunktab[indx] = nchunkid;
	version++;
	return STATUS_OK;
}

uint8_t fs_end_setlength(uint64_t chunkid) {
	//changelog(version++,"%"PRIu32"|UNLOCK(%"PRIu64")",(uint32_t)get_current_time(),chunkid);
	return chunk_unlock(chunkid);
}
	
uint8_t shadow_fs_unlock(uint64_t chunkid) {
	version++;
	return chunk_unlock(chunkid);
}

uint8_t fs_do_setlength(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint64_t length,uint8_t attr[35]) {
	fsnode *p,*rn;
	memset(attr,0,35);
	if (rootinode==MFS_ROOT_ID || rootinode==0) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (rootinode==0 && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	fsnodes_setlength(p,length);
	//changelog(version++,"%"PRIu32"|LENGTH(%"PRIu32",%"PRIu64")",(uint32_t)get_current_time(),inode,p->data.fdata.length);
	p->ctime = p->mtime = get_current_time();
	fsnodes_fill_attr(p,NULL,uid,gid,auid,agid,sesflags,attr);
	stats_setattr++;
	return STATUS_OK;
}


uint8_t fs_setattr(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint8_t setmask,uint16_t attrmode,uint32_t attruid,uint32_t attrgid,uint32_t attratime,uint32_t attrmtime,uint8_t attr[35]) {
	fsnode *p,*rn;

	memset(attr,0,35);
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
//	if (setmask==0) {
//		return ERROR_EINVAL;
//	}
	if (rootinode==MFS_ROOT_ID) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
//	changeids = fsnodes_changeids(p);
//	if ((setmask&(SET_MODE_FLAG|SET_UID_FLAG|SET_GID_FLAG|SET_ATIME_FLAG|SET_MTIME_FLAG))==0) {
//		fsnodes_fill_attr(p,NULL,uid,gid,auid,agid,sesflags,attr);
//		stats_setattr++;
//		return STATUS_OK;
//	}
	if (uid!=0 && (sesflags&SESFLAG_MAPALL) && (setmask&(SET_UID_FLAG|SET_GID_FLAG))) {
		return ERROR_EPERM;
	}
	if ((p->mode&(EATTR_NOOWNER<<12))==0) {
		if (uid!=0 && uid!=p->uid && (setmask&(SET_MODE_FLAG|SET_UID_FLAG|SET_GID_FLAG|SET_ATIME_FLAG|SET_MTIME_FLAG))) {
			return ERROR_EPERM;
		}
	}
	if (uid!=0 && uid!=attruid && (setmask&SET_UID_FLAG)) {
		return ERROR_EPERM;
	}
	if ((sesflags&SESFLAG_IGNOREGID)==0) {
		if (uid!=0 && gid!=attrgid && (setmask&SET_GID_FLAG)) {
			return ERROR_EPERM;
		}
	}
// for safety reason always clear suid and sgid flags during chown operation
	if ((setmask&(SET_UID_FLAG|SET_GID_FLAG)) && (p->mode & 06000)) {
		p->mode &= 0171777;	// safe approach - delete both suid and sgid
		attrmode &= 01777;
	}
	if (setmask&SET_MODE_FLAG) {
		p->mode = (attrmode & 07777) | (p->mode & 0xF000);
	}
	if (setmask&SET_UID_FLAG) {
		p->uid = attruid;
	}
	if (setmask&SET_GID_FLAG) {
		p->gid = attrgid;
	}
// 
	if (setmask&SET_ATIME_FLAG) {
		p->atime = attratime;
	}
	if (setmask&SET_MTIME_FLAG) {
		p->mtime = attrmtime;
	}
	//changelog(version++,"%"PRIu32"|ATTR(%"PRIu32",%"PRIu16",%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32")",get_current_time(),inode,p->mode & 07777,p->uid,p->gid,p->atime,p->mtime);
	p->ctime = get_current_time();
	fsnodes_fill_attr(p,NULL,uid,gid,auid,agid,sesflags,attr);
	stats_setattr++;
	return STATUS_OK;
}


uint8_t shadow_fs_attr(uint32_t ts,uint32_t inode,uint32_t mode,uint32_t uid,uint32_t gid,uint32_t atime,uint32_t mtime) {
	fsnode *p;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (mode>07777) {
		return ERROR_EINVAL;
	}
	p->mode = mode | (p->mode & 0xF000);
	p->uid = uid;
	p->gid = gid;
	p->atime = atime;
	p->mtime = mtime;
	p->ctime = ts;
	version++;
	return STATUS_OK;
}

uint8_t shadow_fs_length(uint32_t ts,uint32_t inode,uint64_t length) {
	fsnode *p;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EINVAL;
	}
	shadow_fsnodes_setlength(p,length);
	p->mtime = ts;
	p->ctime = ts;
	version++;
	return STATUS_OK;
}


uint8_t fs_readlink(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t *pleng,uint8_t **path) {
	fsnode *p,*rn;
	(void)sesflags;
	*pleng = 0;
	*path = NULL;
	if (rootinode==MFS_ROOT_ID) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_SYMLINK) {
		return ERROR_EINVAL;
	}
	*pleng = p->data.sdata.pleng;
	*path = p->data.sdata.path;
	p->atime = get_current_time();
	//changelog(version++,"%"PRIu32"|ACCESS(%"PRIu32")",(uint32_t)get_current_time(),inode);
	stats_readlink++;
	return STATUS_OK;
}

uint8_t shadow_fs_symlink(uint32_t ts,uint32_t parent,uint32_t nleng,const uint8_t *name,const uint8_t *path,uint32_t uid,uint32_t gid,uint32_t inode) {
	uint32_t pleng;
        fsnode *wd,*p;
        uint8_t *newpath;
	
	pleng = strlen((const char*)path);
	wd = fsnodes_id_to_node(parent);
	if (!wd) {
		return ERROR_ENOENT;
	}
        if (wd->type!=TYPE_DIRECTORY) {
                return ERROR_ENOTDIR;
        }
        if (fsnodes_namecheck(nleng,name)<0) {
                return ERROR_EINVAL;
        }
        if (fsnodes_nameisused(wd,nleng,name)) {
                return ERROR_EEXIST;
        }
	newpath = malloc(pleng);
        if (newpath==NULL) {
                return ERROR_EINVAL;    // no mem ?
        }
	p = fsnodes_create_node(ts,wd,nleng,name,TYPE_SYMLINK,0777,uid,gid);
        memcpy(newpath,path,pleng);
        p->data.sdata.path = newpath;
        p->data.sdata.pleng = pleng;
        if (inode!=p->id) {
                return ERROR_MISMATCH;
        }       
        version++;
	return STATUS_OK;
}	

uint8_t fs_symlink(uint32_t rootinode,uint8_t sesflags,uint32_t parent,uint16_t nleng,const uint8_t *name,uint32_t pleng,const uint8_t *path,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint32_t *inode,uint8_t attr[35]) {
	fsnode *wd,*p;
	uint8_t *newpath;
	
	fsnode *rn;
	statsrecord sr;
	uint32_t i;
	*inode = 0;
	memset(attr,0,35);
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (pleng==0) {
		return ERROR_EINVAL;
	}
	for (i=0 ; i<pleng ; i++) {
		if (path[i]==0) {
			return ERROR_EINVAL;
		}
	}
	if (rootinode==MFS_ROOT_ID) {
		wd = fsnodes_id_to_node(parent);
		if (!wd) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (parent==MFS_ROOT_ID) {
			parent = rootinode;
			wd = rn;
		} else {
			wd = fsnodes_id_to_node(parent);
			if (!wd) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,wd)) {
				return ERROR_EPERM;
			}
		}
	}
	
	if (wd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (!fsnodes_access(wd,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	if (fsnodes_namecheck(nleng,name)<0) {
		return ERROR_EINVAL;
	}
	if (fsnodes_nameisused(wd,nleng,name)) {
		return ERROR_EEXIST;
	}
	if (fsnodes_test_quota(wd)) {
		return ERROR_QUOTA;
	}
	newpath = malloc(pleng);
	if (newpath==NULL) {
		return ERROR_EINVAL;	// no mem ?
	}
	p = fsnodes_create_node(get_current_time(),wd,nleng,name,TYPE_SYMLINK,0777,uid,gid);
	memcpy(newpath,path,pleng);
	p->data.sdata.path = newpath;
	p->data.sdata.pleng = pleng;

	memset(&sr,0,sizeof(statsrecord));
	sr.length = pleng;
	fsnodes_add_stats(wd,&sr);

	*inode = p->id;
	fsnodes_fill_attr(p,wd,uid,gid,auid,agid,sesflags,attr);
	//changelog(version++,"%"PRIu32"|SYMLINK(%"PRIu32",%s,%s,%"PRIu32",%"PRIu32"):%"PRIu32,(uint32_t)get_current_time(),parent,fsnodes_escape_name(nleng,name),fsnodes_escape_name(pleng,newpath),uid,gid,p->id);
	stats_symlink++;
	return STATUS_OK;
}

uint8_t fs_mknod(uint32_t rootinode,uint8_t sesflags,uint32_t parent,uint16_t nleng,const uint8_t *name,uint8_t type,uint16_t mode,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint32_t rdev,uint32_t *inode,uint8_t attr[35]) {
	fsnode *wd,*p,*rn;
	*inode = 0;
	memset(attr,0,35);
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (type!=TYPE_FILE && type!=TYPE_SOCKET && type!=TYPE_FIFO && type!=TYPE_BLOCKDEV && type!=TYPE_CHARDEV) {
		return ERROR_EINVAL;
	}
	if (rootinode==MFS_ROOT_ID) {
		wd = fsnodes_id_to_node(parent);
		if (!wd) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (parent==MFS_ROOT_ID) {
			parent = rootinode;
			wd = rn;
		} else {
			wd = fsnodes_id_to_node(parent);
			if (!wd) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,wd)) {
				return ERROR_EPERM;
			}
		}
	}
	if (wd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (!fsnodes_access(wd,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	if (fsnodes_namecheck(nleng,name)<0) {
		return ERROR_EINVAL;
	}
	if (fsnodes_nameisused(wd,nleng,name)) {
		return ERROR_EEXIST;
	}
	if (fsnodes_test_quota(wd)) {
		return ERROR_QUOTA;
	}
	p = fsnodes_create_node(get_current_time(),wd,nleng,name,type,mode,uid,gid);
	if (type==TYPE_BLOCKDEV || type==TYPE_CHARDEV) {
		p->data.rdev = rdev;
	}
	*inode = p->id;
	fsnodes_fill_attr(p,wd,uid,gid,auid,agid,sesflags,attr);
	//changelog(version++,"%"PRIu32"|CREATE(%"PRIu32",%s,%c,%"PRIu16",%"PRIu32",%"PRIu32",%"PRIu32"):%"PRIu32,(uint32_t)get_current_time(),parent,fsnodes_escape_name(nleng,name),type,mode,uid,gid,rdev,p->id);
	stats_mknod++;
	return STATUS_OK;
}

uint8_t fs_mkdir(uint32_t rootinode,uint8_t sesflags,uint32_t parent,uint16_t nleng,const uint8_t *name,uint16_t mode,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint32_t *inode,uint8_t attr[35]) {
	fsnode *wd,*p,*rn;
	*inode = 0;
	memset(attr,0,35);
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID) {
		wd = fsnodes_id_to_node(parent);
		if (!wd) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (parent==MFS_ROOT_ID) {
			parent = rootinode;
			wd = rn;
		} else {
			wd = fsnodes_id_to_node(parent);
			if (!wd) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,wd)) {
				return ERROR_EPERM;
			}
		}
	}
	if (wd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (!fsnodes_access(wd,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	if (fsnodes_namecheck(nleng,name)<0) {
		return ERROR_EINVAL;
	}
	if (fsnodes_nameisused(wd,nleng,name)) {
		return ERROR_EEXIST;
	}
	if (fsnodes_test_quota(wd)) {
		return ERROR_QUOTA;
	}
	p = fsnodes_create_node(get_current_time(),wd,nleng,name,TYPE_DIRECTORY,mode,uid,gid);
	*inode = p->id;
	fsnodes_fill_attr(p,wd,uid,gid,auid,agid,sesflags,attr);
	//changelog(version++,"%"PRIu32"|CREATE(%"PRIu32",%s,%c,%"PRIu16",%"PRIu32",%"PRIu32",%"PRIu32"):%"PRIu32,(uint32_t)get_current_time(),parent,fsnodes_escape_name(nleng,name),TYPE_DIRECTORY,mode,uid,gid,0,p->id);
	stats_mkdir++;
	return STATUS_OK;
}

uint8_t shadow_fs_create(uint32_t ts,uint32_t parent,uint32_t nleng,const uint8_t *name,uint8_t type,uint32_t mode,uint32_t uid,uint32_t gid,uint32_t rdev,uint32_t inode) {
	fsnode *wd,*p;
	if (type!=TYPE_FILE && type!=TYPE_SOCKET && type!=TYPE_FIFO && type!=TYPE_BLOCKDEV && type!=TYPE_CHARDEV && type!=TYPE_DIRECTORY) {
		return ERROR_EINVAL;
	}
	wd = fsnodes_id_to_node(parent);
	if (!wd) {
		return ERROR_ENOENT;
	}
	if (wd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (fsnodes_nameisused(wd,nleng,name)) {
		return ERROR_EEXIST;
	}
	p = fsnodes_create_node(ts,wd,nleng,name,type,mode,uid,gid);
	if (type==TYPE_BLOCKDEV || type==TYPE_CHARDEV) {
		p->data.rdev = rdev;
	}
	if (inode!=p->id) {
		return ERROR_MISMATCH;
	}
	version++;
	return STATUS_OK;
}

uint8_t fs_unlink(uint32_t rootinode,uint8_t sesflags,uint32_t parent,uint16_t nleng,const uint8_t *name,uint32_t uid,uint32_t gid) {
	uint32_t ts;
	fsnode *wd,*rn;
	fsedge *e;
	ts = get_current_time();
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID) {
		wd = fsnodes_id_to_node(parent);
		if (!wd) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (parent==MFS_ROOT_ID) {
			parent = rootinode;
			wd = rn;
		} else {
			wd = fsnodes_id_to_node(parent);
			if (!wd) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,wd)) {
				return ERROR_EPERM;
			}
		}
	}
	if (wd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (!fsnodes_access(wd,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	if (fsnodes_namecheck(nleng,name)<0) {
		return ERROR_EINVAL;
	}
	e = fsnodes_lookup(wd,nleng,name);
	if (!e) {
		return ERROR_ENOENT;
	}
	if (!fsnodes_sticky_access(wd,e->child,uid)) {
		return ERROR_EPERM;
	}
	if (e->child->type==TYPE_DIRECTORY) {
		return ERROR_EPERM;
	}
	//changelog(version++,"%"PRIu32"|UNLINK(%"PRIu32",%s):%"PRIu32,ts,parent,fsnodes_escape_name(nleng,name),e->child->id);
	fsnodes_unlink(ts,e);
	stats_unlink++;
	return STATUS_OK;

}

uint8_t fs_rmdir(uint32_t rootinode,uint8_t sesflags,uint32_t parent,uint16_t nleng,const uint8_t *name,uint32_t uid,uint32_t gid) {
	uint32_t ts;
	fsnode *wd,*rn;
	fsedge *e;
	ts = get_current_time();
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID) {
		wd = fsnodes_id_to_node(parent);
		if (!wd) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (parent==MFS_ROOT_ID) {
			parent = rootinode;
			wd = rn;
		} else {
			wd = fsnodes_id_to_node(parent);
			if (!wd) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,wd)) {
				return ERROR_EPERM;
			}
		}
	}
	if (wd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (!fsnodes_access(wd,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	if (fsnodes_namecheck(nleng,name)<0) {
		return ERROR_EINVAL;
	}
	e = fsnodes_lookup(wd,nleng,name);
	if (!e) {
		return ERROR_ENOENT;
	}
	if (!fsnodes_sticky_access(wd,e->child,uid)) {
		return ERROR_EPERM;
	}
	if (e->child->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (e->child->data.ddata.children!=NULL) {
		return ERROR_ENOTEMPTY;
	}
	//changelog(version++,"%"PRIu32"|UNLINK(%"PRIu32",%s):%"PRIu32,ts,parent,fsnodes_escape_name(nleng,name),e->child->id);
	fsnodes_unlink(ts,e);
	stats_rmdir++;
	return STATUS_OK;
}

uint8_t shadow_fs_unlink(uint32_t ts,uint32_t parent,uint32_t nleng,const uint8_t *name,uint32_t inode) {
	fsnode *wd;
	fsedge *e;
	wd = fsnodes_id_to_node(parent);
	if (!wd) {
		return ERROR_ENOENT;
	}
	if (wd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	e = fsnodes_lookup(wd,nleng,name);
	if (!e) {
		return ERROR_ENOENT;
	}
	if (e->child->id!=inode) {
		return ERROR_MISMATCH;
	}
	if (e->child->type==TYPE_DIRECTORY && e->child->data.ddata.children!=NULL) {
		return ERROR_ENOTEMPTY;
	}
	shadow_fsnodes_unlink(ts,e);
	version++;
	return STATUS_OK;
}

uint8_t shadow_fs_move(uint32_t ts,uint32_t parent_src,uint32_t nleng_src,const uint8_t *name_src,uint32_t parent_dst,uint32_t nleng_dst,const uint8_t *name_dst,uint32_t inode) {
	fsnode *swd;
        fsedge *se;
        fsnode *dwd;
        fsedge *de;
        fsnode *node;

	swd = fsnodes_id_to_node(parent_src);
        if (!swd) {
                return ERROR_ENOENT;
        }
        dwd = fsnodes_id_to_node(parent_dst);
        if (!dwd) {
                return ERROR_ENOENT;
        }
        if (swd->type!=TYPE_DIRECTORY) {
                return ERROR_ENOTDIR;
        }
	if (fsnodes_namecheck(nleng_src,name_src)<0) {
                return ERROR_EINVAL;
        }
        se = fsnodes_lookup(swd,nleng_src,name_src);
        if (!se) {
                return ERROR_ENOENT;
        }
        node = se->child;
        if (node->id!=inode) {
                return ERROR_MISMATCH;
        }
        if (dwd->type!=TYPE_DIRECTORY) {
                return ERROR_ENOTDIR;
        }
        if (se->child->type==TYPE_DIRECTORY) {
                if (fsnodes_isancestor(se->child,dwd)) {
                        return ERROR_EINVAL;
                }
        }
        if (fsnodes_namecheck(nleng_dst,name_dst)<0) {
                return ERROR_EINVAL;
//              name_dst = fp->name;
        }
        de = fsnodes_lookup(dwd,nleng_dst,name_dst);
        if (de) {
                if (de->child->type==TYPE_DIRECTORY && de->child->data.ddata.children!=NULL) {
                        return ERROR_ENOTEMPTY;
                }
        	shadow_fsnodes_unlink(ts,de);
        }
        shadow_fsnodes_remove_edge(ts,se);
        fsnodes_link(ts,dwd,node,nleng_dst,name_dst);	
	version++;
	return STATUS_OK;
}

uint8_t fs_rename(uint32_t rootinode,uint8_t sesflags,uint32_t parent_src,uint16_t nleng_src,const uint8_t *name_src,uint32_t parent_dst,uint16_t nleng_dst,const uint8_t *name_dst,uint32_t uid,uint32_t gid) {
	uint32_t ts;
	fsnode *swd;
	fsedge *se;
	fsnode *dwd;
	fsedge *de;
	fsnode *node;
	
	fsnode *rn;
	ts = get_current_time();
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID) {
		swd = fsnodes_id_to_node(parent_src);
		if (!swd) {
			return ERROR_ENOENT;
		}
		dwd = fsnodes_id_to_node(parent_dst);
		if (!dwd) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (parent_src==MFS_ROOT_ID) {
			parent_src = rootinode;
			swd = rn;
		} else {
			swd = fsnodes_id_to_node(parent_src);
			if (!swd) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,swd)) {
				return ERROR_EPERM;
			}
		}
		if (parent_dst==MFS_ROOT_ID) {
			parent_dst = rootinode;
			dwd = rn;
		} else {
			dwd = fsnodes_id_to_node(parent_dst);
			if (!dwd) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,dwd)) {
				return ERROR_EPERM;
			}
		}
	}
	if (swd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (!fsnodes_access(swd,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	if (fsnodes_namecheck(nleng_src,name_src)<0) {
		return ERROR_EINVAL;
	}
	se = fsnodes_lookup(swd,nleng_src,name_src);
	if (!se) {
		return ERROR_ENOENT;
	}
	node = se->child;
	if (!fsnodes_sticky_access(swd,node,uid)) {
		return ERROR_EPERM;
	}
	if (dwd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (!fsnodes_access(dwd,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	if (se->child->type==TYPE_DIRECTORY) {
		if (fsnodes_isancestor(se->child,dwd)) {
			return ERROR_EINVAL;
		}
	}
	if (fsnodes_namecheck(nleng_dst,name_dst)<0) {
		return ERROR_EINVAL;
//		name_dst = fp->name;
	}
	if (fsnodes_test_quota(dwd)) {
		return ERROR_QUOTA;
	}
	de = fsnodes_lookup(dwd,nleng_dst,name_dst);
	if (de) {
		if (de->child->type==TYPE_DIRECTORY && de->child->data.ddata.children!=NULL) {
			return ERROR_ENOTEMPTY;
		}
		if (!fsnodes_sticky_access(dwd,de->child,uid)) {
			return ERROR_EPERM;
		}
		fsnodes_unlink(ts,de);
	}
	fsnodes_remove_edge(ts,se);
	fsnodes_link(ts,dwd,node,nleng_dst,name_dst);
	//changelog(version++,"%"PRIu32"|MOVE(%"PRIu32",%s,%"PRIu32",%s):%"PRIu32,(uint32_t)get_current_time(),parent_src,fsnodes_escape_name(nleng_src,name_src),parent_dst,fsnodes_escape_name(nleng_dst,name_dst),node->id);
	stats_rename++;
	return STATUS_OK;
}

uint8_t shadow_fs_link(uint32_t ts,uint32_t inode_src,uint32_t parent_dst,uint32_t nleng_dst,uint8_t *name_dst) {
	fsnode *sp;
        fsnode *dwd;

        sp = fsnodes_id_to_node(inode_src);
        if (!sp) {
                return ERROR_ENOENT;
        }
        dwd = fsnodes_id_to_node(parent_dst);
        if (!dwd) {
                return ERROR_ENOENT;
        }
        if (sp->type==TYPE_TRASH || sp->type==TYPE_RESERVED) {
                return ERROR_ENOENT;
        }
        if (sp->type==TYPE_DIRECTORY) {
                return ERROR_EPERM;
        }
        if (dwd->type!=TYPE_DIRECTORY) {
                return ERROR_ENOTDIR;
        }
        if (fsnodes_namecheck(nleng_dst,name_dst)<0) {
                return ERROR_EINVAL;
        }
        if (fsnodes_nameisused(dwd,nleng_dst,name_dst)) {
                return ERROR_EEXIST;
        }
        fsnodes_link(ts,dwd,sp,nleng_dst,name_dst);
	version++;
	return STATUS_OK;
}	

uint8_t fs_link(uint32_t rootinode,uint8_t sesflags,uint32_t inode_src,uint32_t parent_dst,uint16_t nleng_dst,const uint8_t *name_dst,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint32_t *inode,uint8_t attr[35]) {
	uint32_t ts;
	fsnode *sp;
	fsnode *dwd;
	fsnode *rn;
	ts = get_current_time();
	*inode = 0;
	memset(attr,0,35);
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID) {
		sp = fsnodes_id_to_node(inode_src);
		if (!sp) {
			return ERROR_ENOENT;
		}
		dwd = fsnodes_id_to_node(parent_dst);
		if (!dwd) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode_src==MFS_ROOT_ID) {
			inode_src = rootinode;
			sp = rn;
		} else {
			sp = fsnodes_id_to_node(inode_src);
			if (!sp) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,sp)) {
				return ERROR_EPERM;
			}
		}
		if (parent_dst==MFS_ROOT_ID) {
			parent_dst = rootinode;
			dwd = rn;
		} else {
			dwd = fsnodes_id_to_node(parent_dst);
			if (!dwd) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,dwd)) {
				return ERROR_EPERM;
			}
		}
	}
	if (sp->type==TYPE_TRASH || sp->type==TYPE_RESERVED) {
		return ERROR_ENOENT;
	}
	if (sp->type==TYPE_DIRECTORY) {
		return ERROR_EPERM;
	}
	if (dwd->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (!fsnodes_access(dwd,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	if (fsnodes_namecheck(nleng_dst,name_dst)<0) {
		return ERROR_EINVAL;
	}
	if (fsnodes_nameisused(dwd,nleng_dst,name_dst)) {
		return ERROR_EEXIST;
	}
	if (fsnodes_test_quota(dwd)) {
		return ERROR_QUOTA;
	}
	fsnodes_link(ts,dwd,sp,nleng_dst,name_dst);
	*inode = inode_src;
	fsnodes_fill_attr(sp,dwd,uid,gid,auid,agid,sesflags,attr);
	//changelog(version++,"%"PRIu32"|LINK(%"PRIu32",%"PRIu32",%s)",(uint32_t)get_current_time(),inode_src,parent_dst,fsnodes_escape_name(nleng_dst,name_dst));
	stats_link++;
	return STATUS_OK;
}

uint8_t shadow_fs_snapshot(uint32_t ts,uint32_t inode_src,uint32_t parent_dst,uint16_t nleng_dst,uint8_t *name_dst,uint8_t canoverwrite) {
        //fsnode *rn;
        fsnode *sp;
        fsnode *dwd;
        uint8_t status;
	sp = fsnodes_id_to_node(inode_src);
        if (!sp) {
                return ERROR_ENOENT;
        }
        dwd = fsnodes_id_to_node(parent_dst);
        if (!dwd) {
                return ERROR_ENOENT;
        }
        if (dwd->type!=TYPE_DIRECTORY) {
                return ERROR_EPERM;
        }
        if (sp->type==TYPE_DIRECTORY) {
                if (sp==dwd || fsnodes_isancestor(sp,dwd)) {
                        return ERROR_EINVAL;
                }
        }
        status = fsnodes_snapshot_test(sp,sp,dwd,nleng_dst,name_dst,canoverwrite);
        if (status!=STATUS_OK) {
                return status;
        }
	shadow_fsnodes_snapshot(ts,sp,dwd,nleng_dst,name_dst);
	version++;
	return STATUS_OK;
}	

uint8_t fs_snapshot(uint32_t rootinode,uint8_t sesflags,uint32_t inode_src,uint32_t parent_dst,uint16_t nleng_dst,const uint8_t *name_dst,uint32_t uid,uint32_t gid,uint8_t canoverwrite) {
	uint32_t ts;
	fsnode *rn;
	fsnode *sp;
	fsnode *dwd;
	uint8_t status;
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID) {
		sp = fsnodes_id_to_node(inode_src);
		if (!sp) {
			return ERROR_ENOENT;
		}
		dwd = fsnodes_id_to_node(parent_dst);
		if (!dwd) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode_src==MFS_ROOT_ID) {
			inode_src = rootinode;
			sp = rn;
		} else {
			sp = fsnodes_id_to_node(inode_src);
			if (!sp) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,sp)) {
				return ERROR_EPERM;
			}
		}
		if (parent_dst==MFS_ROOT_ID) {
			parent_dst = rootinode;
			dwd = rn;
		} else {
			dwd = fsnodes_id_to_node(parent_dst);
			if (!dwd) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,dwd)) {
				return ERROR_EPERM;
			}
		}
	}
	if (!fsnodes_access(sp,uid,gid,MODE_MASK_R,sesflags)) {
		return ERROR_EACCES;
	}
	if (dwd->type!=TYPE_DIRECTORY) {
		return ERROR_EPERM;
	}
	if (sp->type==TYPE_DIRECTORY) {
		if (sp==dwd || fsnodes_isancestor(sp,dwd)) {
			return ERROR_EINVAL;
		}
	}
	if (!fsnodes_access(dwd,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	if (fsnodes_test_quota(dwd)) {
		return ERROR_QUOTA;
	}
	status = fsnodes_snapshot_test(sp,sp,dwd,nleng_dst,name_dst,canoverwrite);
	if (status!=STATUS_OK) {
		return status;
	}
	ts = get_current_time();
	fsnodes_snapshot(ts,sp,dwd,nleng_dst,name_dst);
	//changelog(version++,"%"PRIu32"|SNAPSHOT(%"PRIu32",%"PRIu32",%s,%"PRIu8")",ts,inode_src,parent_dst,fsnodes_escape_name(nleng_dst,name_dst),canoverwrite);
	return STATUS_OK;
}

uint8_t shadow_fs_append(uint32_t ts,uint32_t inode,uint32_t inode_src) {
	uint8_t status;
        fsnode *p,*sp;
        if (inode==inode_src) {
                return ERROR_EINVAL;
        }
        sp = fsnodes_id_to_node(inode_src);
        if (!sp) {
                return ERROR_ENOENT;
        }
        p = fsnodes_id_to_node(inode);
        if (!p) {
                return ERROR_ENOENT;
        }
        if (sp->type!=TYPE_FILE && sp->type!=TYPE_TRASH && sp->type!=TYPE_RESERVED) {
                return ERROR_EPERM;
        }
        if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
                return ERROR_EPERM;
        }
        status = shadow_fsnodes_appendchunks(ts,p,sp);
        if (status!=STATUS_OK) {
                return status;
        }
	version++;
	return STATUS_OK;
}

uint8_t fs_append(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t inode_src,uint32_t uid,uint32_t gid) {
	uint32_t ts;
	fsnode *rn;
	uint8_t status;
	fsnode *p,*sp;
	if (inode==inode_src) {
		return ERROR_EINVAL;
	}
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID) {
		sp = fsnodes_id_to_node(inode_src);
		if (!sp) {
			return ERROR_ENOENT;
		}
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode_src==MFS_ROOT_ID) {
			inode_src = rootinode;
			sp = rn;
		} else {
			sp = fsnodes_id_to_node(inode_src);
			if (!sp) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,sp)) {
				return ERROR_EPERM;
			}
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (sp->type!=TYPE_FILE && sp->type!=TYPE_TRASH && sp->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	if (!fsnodes_access(sp,uid,gid,MODE_MASK_R,sesflags)) {
		return ERROR_EACCES;
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	if (!fsnodes_access(p,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	if (fsnodes_test_quota(p)) {
		return ERROR_QUOTA;
	}
	ts = get_current_time();
	status = fsnodes_appendchunks(ts,p,sp);
	if (status!=STATUS_OK) {
		return status;
	}
	//changelog(version++,"%"PRIu32"|APPEND(%"PRIu32",%"PRIu32")",ts,inode,inode_src);
	return STATUS_OK;
}

uint8_t fs_readdir_size(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t uid,uint32_t gid,uint8_t flags,void **dnode,uint32_t *dbuffsize) {
	fsnode *p,*rn;
	*dnode = NULL;
	*dbuffsize = 0;
	if (rootinode==MFS_ROOT_ID) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_DIRECTORY) {
		return ERROR_ENOTDIR;
	}
	if (!fsnodes_access(p,uid,gid,MODE_MASK_R,sesflags)) {
		return ERROR_EACCES;
	}
	*dnode = p;
	*dbuffsize = fsnodes_getdirsize(p,flags&GETDIR_FLAG_WITHATTR);
	return STATUS_OK;
}

void fs_readdir_data(uint32_t rootinode,uint8_t sesflags,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint8_t flags,void *dnode,uint8_t *dbuff) {
	fsnode *p = (fsnode*)dnode;
	//changelog(version++,"%"PRIu32"|ACCESS(%"PRIu32")",(uint32_t)get_current_time(),p->id);
	fsnodes_getdirdata(get_current_time(),rootinode,uid,gid,auid,agid,sesflags,p,dbuff,flags&GETDIR_FLAG_WITHATTR);
	stats_readdir++;
}


uint8_t fs_checkfile(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint16_t chunkcount[256]) {
	fsnode *p,*rn;
	(void)sesflags;
	if (rootinode==MFS_ROOT_ID || rootinode==0) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (rootinode==0 && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	fsnodes_checkfile(p,chunkcount);
	return STATUS_OK;
}

uint8_t fs_opencheck(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t uid,uint32_t gid,uint32_t auid,uint32_t agid,uint8_t flags,uint8_t attr[35]) {
	fsnode *p,*rn;
	if ((sesflags&SESFLAG_READONLY) && (flags&WANT_WRITE)) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	if ((flags&AFTER_CREATE)==0) {
		uint8_t modemask=0;
		if (flags&WANT_READ) {
			modemask|=MODE_MASK_R;
		}
		if (flags&WANT_WRITE) {
			modemask|=MODE_MASK_W;
		}
		if (!fsnodes_access(p,uid,gid,modemask,sesflags)) {
			return ERROR_EACCES;
		}
	}
	fsnodes_fill_attr(p,NULL,uid,gid,auid,agid,sesflags,attr);
	stats_open++;
	return STATUS_OK;
}

uint8_t shadow_fs_aquire(uint32_t inode,uint32_t sessionid) {
        fsnode *p;
        sessionidrec *cr;
        p = fsnodes_id_to_node(inode);
        if (!p) {
                return ERROR_ENOENT;
        }
        if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
                return ERROR_EPERM;
        }
        for (cr=p->data.fdata.sessionids ; cr ; cr=cr->next) {
                if (cr->sessionid==sessionid) {
                        return ERROR_EINVAL;
                }
        }
        cr = sessionidrec_malloc();
        cr->sessionid = sessionid;
        cr->next = p->data.fdata.sessionids;
        p->data.fdata.sessionids = cr;
	version++;
	return STATUS_OK;
}

//the shadow side's connection forbid reference count
uint8_t fs_aquire(uint32_t inode,uint32_t sessionid) {
    NOT_USED(inode);
    NOT_USED(sessionid);
//	fsnode *p;
//	sessionidrec *cr;
//	p = fsnodes_id_to_node(inode);
//	if (!p) {
//		return ERROR_ENOENT;
//	}
//	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
//		return ERROR_EPERM;
//	}
//	for (cr=p->data.fdata.sessionids ; cr ; cr=cr->next) {
//		if (cr->sessionid==sessionid) {
//			return ERROR_EINVAL;
//		}
//	}
//	cr = sessionidrec_malloc();
//	cr->sessionid = sessionid;
//	cr->next = p->data.fdata.sessionids;
//	p->data.fdata.sessionids = cr;
	//changelog(version++,"%"PRIu32"|AQUIRE(%"PRIu32",%"PRIu32")",(uint32_t)get_current_time(),inode,sessionid);
	return STATUS_OK;
}

uint8_t shadow_fs_release(uint32_t inode,uint32_t sessionid) {
        fsnode *p;
        sessionidrec *cr,**crp;
        p = fsnodes_id_to_node(inode);
        if (!p) {
                return ERROR_ENOENT;
        }
        if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
                return ERROR_EPERM;
        }
        crp = &(p->data.fdata.sessionids);
        while ((cr=*crp)) {
                if (cr->sessionid==sessionid) {
                        *crp = cr->next;
                        sessionidrec_free(cr);
			version++;
 			return STATUS_OK;
                } else {
                        crp = &(cr->next);
                }
        }
	return ERROR_EINVAL;
}

// the shadow side connection forbid reference connection
uint8_t fs_release(uint32_t inode,uint32_t sessionid) {
    NOT_USED(inode);
    NOT_USED(sessionid);
//	fsnode *p;
//	sessionidrec *cr,**crp;
//	p = fsnodes_id_to_node(inode);
//	if (!p) {
//		return ERROR_ENOENT;
//	}
//	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
//		return ERROR_EPERM;
//	}
//	crp = &(p->data.fdata.sessionids);
//	while ((cr=*crp)) {
//		if (cr->sessionid==sessionid) {
//			*crp = cr->next;
//			sessionidrec_free(cr);
//			//changelog(version++,"%"PRIu32"|RELEASE(%"PRIu32",%"PRIu32")",(uint32_t)get_current_time(),inode,sessionid);
//			return STATUS_OK;
//		} else {
//			crp = &(cr->next);
//		}
//	}
//	syslog(LOG_WARNING,"release: session not found");
//	return ERROR_EINVAL;
	return STATUS_OK;
}

uint8_t shadow_fs_session(uint32_t sessionid) {
//	if (sessionid!=nextsessionid) {
//		return ERROR_MISMATCH;
//	}
    NOT_USED(sessionid);
	version++;
//	nextsessionid++;
	return STATUS_OK;
}

uint32_t fs_newsessionid(void) {
	//changelog(version++,"%"PRIu32"|SESSION():%"PRIu32,(uint32_t)get_current_time(),nextsessionid);
	return nextsessionid++;
}


uint8_t fs_readchunk(uint32_t inode,uint32_t indx,uint64_t *chunkid,uint64_t *length) {
	fsnode *p;
	*chunkid = 0;
	*length = 0;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	if (indx>MAX_INDEX) {
		return ERROR_INDEXTOOBIG;
	}
	if (indx<p->data.fdata.chunks) {
		*chunkid = p->data.fdata.chunktab[indx];
	}
	*length = p->data.fdata.length;
	p->atime = get_current_time();
	//changelog(version++,"%"PRIu32"|ACCESS(%"PRIu32")",(uint32_t)get_current_time(),inode);
	stats_read++;
	return STATUS_OK;
}

uint8_t fs_writechunk(uint32_t inode,uint32_t indx,uint64_t *chunkid,uint64_t *length,uint8_t *opflag) {
	int status;
	uint32_t i;
	uint64_t ochunkid,nchunkid;
	statsrecord psr,nsr;
	fsedge *e;
	fsnode *p;

	*chunkid = 0;
	*length = 0;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	if (fsnodes_test_quota(p)) {
		return ERROR_QUOTA;
	}
	if (indx>MAX_INDEX) {
		return ERROR_INDEXTOOBIG;
	}
	fsnodes_get_stats(p,&psr);
	/* resize chunks structure */
	if (indx>=p->data.fdata.chunks) {
		uint32_t newsize;
		if (indx<8) {
			newsize=indx+1;
		} else if (indx<64) {
			newsize=(indx&0xFFFFFFF8)+8;
		} else {
			newsize = (indx&0xFFFFFFC0)+64;
		}
		if (p->data.fdata.chunktab==NULL) {
			p->data.fdata.chunktab = (uint64_t*)malloc(sizeof(uint64_t)*newsize);
		} else {
			p->data.fdata.chunktab = (uint64_t*)realloc(p->data.fdata.chunktab,sizeof(uint64_t)*newsize);
		}
		for (i=p->data.fdata.chunks ; i<newsize ; i++) {
			p->data.fdata.chunktab[i]=0;
		}
		p->data.fdata.chunks = newsize;
	}
	ochunkid = p->data.fdata.chunktab[indx];
	status = chunk_multi_modify(&nchunkid,ochunkid,inode,indx,p->goal,opflag);
/* zapis bez zwiekszania wersji
	if (nchunkid==ochunkid && status==255) {
		*chunkid = nchunkid;
		*length = p->data.fdata.length;
		stats_write++;
		return 255;
	}
*/
	if (status!=STATUS_OK) {
		return status;
	}
	p->data.fdata.chunktab[indx] = nchunkid;
	fsnodes_get_stats(p,&nsr);
	for (e=p->parents ; e ; e=e->nextparent) {
		fsnodes_add_sub_stats(e->parent,&nsr,&psr);
	}
	*chunkid = nchunkid;
	*length = p->data.fdata.length;
	//changelog(version++,"%"PRIu32"|WRITE(%"PRIu32",%"PRIu32",%"PRIu8"):%"PRIu64,(uint32_t)get_current_time(),inode,indx,*opflag,nchunkid);
	p->mtime = p->ctime = get_current_time();
	stats_write++;
	return STATUS_OK;
}

uint8_t shadow_fs_write(uint32_t ts,uint32_t inode,uint32_t indx,uint8_t opflag,uint64_t chunkid) {
	int status;
	uint32_t i;
	uint64_t ochunkid,nchunkid;
	fsnode *p;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	if (indx>MAX_INDEX) {
		return ERROR_INDEXTOOBIG;
	}
	/* resize chunks structure */
	if (indx>=p->data.fdata.chunks) {
		uint32_t newsize;
		if (indx<8) {
			newsize=indx+1;
		} else if (indx<64) {
			newsize=(indx&0xFFFFFFF8)+8;
		} else {
			newsize = (indx&0xFFFFFFC0)+64;
		}
		if (p->data.fdata.chunktab==NULL) {
			p->data.fdata.chunktab = (uint64_t*)malloc(sizeof(uint64_t)*newsize);
		} else {
			p->data.fdata.chunktab = (uint64_t*)realloc(p->data.fdata.chunktab,sizeof(uint64_t)*newsize);
		}
		for (i=p->data.fdata.chunks ; i<newsize ; i++) {
			p->data.fdata.chunktab[i]=0;
		}
		p->data.fdata.chunks = newsize;
	}
	ochunkid = p->data.fdata.chunktab[indx];
	status = shadow_chunk_multi_modify(ts,&nchunkid,ochunkid,inode,indx,p->goal,opflag);
	if (status!=STATUS_OK) {
		return status;
	}
	if (nchunkid!=chunkid) {
		return ERROR_MISMATCH;
	}
	p->data.fdata.chunktab[indx] = nchunkid;
	version++;
	p->mtime = p->ctime = ts;
	return STATUS_OK;
}


uint8_t fs_writeend(uint32_t inode,uint64_t length,uint64_t chunkid) {
	if (length>0) {
		fsnode *p;
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
		if (length>p->data.fdata.length) {
			fsnodes_setlength(p,length);
			p->mtime = p->ctime = get_current_time();
			//changelog(version++,"%"PRIu32"|LENGTH(%"PRIu32",%"PRIu64")",(uint32_t)get_current_time(),inode,length);
		}
	}
	//changelog(version++,"%"PRIu32"|UNLOCK(%"PRIu64")",(uint32_t)get_current_time(),chunkid);
	return chunk_unlock(chunkid);
}

void fs_incversion(uint64_t chunkid) {
    NOT_USED(chunkid);
	//changelog(version++,"%"PRIu32"|INCVERSION(%"PRIu64")",(uint32_t)get_current_time(),chunkid);
}

uint8_t shadow_fs_incversion(uint64_t chunkid) {
	version++;
	return chunk_increase_version(chunkid);
}


uint8_t fs_repair(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t uid,uint32_t gid,uint32_t *notchanged,uint32_t *erased,uint32_t *repaired) {
	uint32_t nversion,indx;
	statsrecord psr,nsr;
	fsedge *e;
	fsnode *p,*rn;

	*notchanged = 0;
	*erased = 0;
	*repaired = 0;
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID || rootinode==0) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (rootinode==0 && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	if (!fsnodes_access(p,uid,gid,MODE_MASK_W,sesflags)) {
		return ERROR_EACCES;
	}
	fsnodes_get_stats(p,&psr);
	for (indx=0 ; indx<p->data.fdata.chunks ; indx++) {
		if (chunk_repair(inode,indx,p->data.fdata.chunktab[indx],&nversion)) {
			//changelog(version++,"%"PRIu32"|REPAIR(%"PRIu32",%"PRIu32"):%"PRIu32,(uint32_t)get_current_time(),inode,indx,nversion);
			if (nversion>0) {
				(*repaired)++;
			} else {
				p->data.fdata.chunktab[indx] = 0;
				(*erased)++;
			}
		} else {
			(*notchanged)++;
		}
	}
	fsnodes_get_stats(p,&nsr);
	for (e=p->parents ; e ; e=e->nextparent) {
		fsnodes_add_sub_stats(e->parent,&nsr,&psr);
	}
	p->mtime = p->ctime = get_current_time();
	return STATUS_OK;
}

uint8_t shadow_fs_repair(uint32_t ts,uint32_t inode,uint32_t indx,uint32_t nversion) {
	fsnode *p;
	uint8_t status;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return ERROR_ENOENT;
	}
	if (p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	if (indx>MAX_INDEX) {
		return ERROR_INDEXTOOBIG;
	}
	if (indx>=p->data.fdata.chunks) {
		return ERROR_NOCHUNK;
	}
	if (p->data.fdata.chunktab[indx]==0) {
		return ERROR_NOCHUNK;
	}
	if (nversion==0) {
		status = shadow_chunk_delete_file(p->data.fdata.chunktab[indx],inode,indx);
		p->data.fdata.chunktab[indx]=0;
	} else {
		status = shadow_chunk_set_version(p->data.fdata.chunktab[indx],nversion);
	}
	version++;
	p->mtime = p->ctime = ts;
	return status;
}

uint8_t fs_getgoal(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint8_t gmode,uint32_t fgtab[10],uint32_t dgtab[10]) {
	fsnode *p,*rn;
	(void)sesflags;
	memset(fgtab,0,10*sizeof(uint32_t));
	memset(dgtab,0,10*sizeof(uint32_t));
	if (!GMODE_ISVALID(gmode)) {
		return ERROR_EINVAL;
	}
	if (rootinode==MFS_ROOT_ID || rootinode==0) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (rootinode==0 && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_DIRECTORY && p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	fsnodes_getgoal_recursive(p,gmode,fgtab,dgtab);
	return STATUS_OK;
}

uint8_t fs_gettrashtime_prepare(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint8_t gmode,void **fptr,void **dptr,uint32_t *fnodes,uint32_t *dnodes) {
	fsnode *p,*rn;
	bstnode *froot,*droot;
	(void)sesflags;
	froot = NULL;
	droot = NULL;
	*fptr = NULL;
	*dptr = NULL;
	*fnodes = 0;
	*dnodes = 0;
	if (!GMODE_ISVALID(gmode)) {
		return ERROR_EINVAL;
	}
	if (rootinode==MFS_ROOT_ID || rootinode==0) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (rootinode==0 && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_DIRECTORY && p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	fsnodes_gettrashtime_recursive(p,gmode,&froot,&droot);
	*fptr = froot;
	*dptr = droot;
	*fnodes = fsnodes_bst_nodes(froot);
	*dnodes = fsnodes_bst_nodes(droot);
	return STATUS_OK;
}

void fs_gettrashtime_store(void *fptr,void *dptr,uint8_t *buff) {
	bstnode *froot,*droot;
	froot = (bstnode*)fptr;
	droot = (bstnode*)dptr;
	fsnodes_bst_storedata(froot,&buff);
	fsnodes_bst_storedata(droot,&buff);
	fsnodes_bst_free(froot);
	fsnodes_bst_free(droot);
}

uint8_t fs_geteattr(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint8_t gmode,uint32_t feattrtab[16],uint32_t deattrtab[16]) {
	fsnode *p,*rn;
	(void)sesflags;
	memset(feattrtab,0,16*sizeof(uint32_t));
	memset(deattrtab,0,16*sizeof(uint32_t));
	if (!GMODE_ISVALID(gmode)) {
		return ERROR_EINVAL;
	}
	if (rootinode==MFS_ROOT_ID || rootinode==0) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (rootinode==0 && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	fsnodes_geteattr_recursive(p,gmode,feattrtab,deattrtab);
	return STATUS_OK;
}


#if VERSMID==7
#warning uncomment quota check
#endif

uint8_t shadow_fs_setgoal(uint32_t ts,uint32_t inode,uint32_t uid,uint8_t goal,uint8_t smode,uint32_t sinodes,uint32_t ncinodes,uint32_t nsinodes/*,uint32_t qeinodes*/) {
	uint32_t si,nci,nsi;
	fsnode *p;

	si = 0;
        nci = 0;
        nsi = 0;
        if (!SMODE_ISVALID(smode) || goal>9 || goal<1) {
                return ERROR_EINVAL;
        }
        p = fsnodes_id_to_node(inode);
        if (!p) {
                return ERROR_ENOENT;
        }
	if (p->type!=TYPE_DIRECTORY && p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
                return ERROR_EPERM;
        }
        fsnodes_setgoal_recursive(p,ts,uid/*,quota*/,goal,smode,&si,&nci,&nsi/*,&qei*/);
        version++;
        if (sinodes!=si || ncinodes!=nci || nsinodes!=nsi/* || qeinodes!=qei*/) {
                return ERROR_MISMATCH;
        }
        return STATUS_OK;
}

uint8_t fs_setgoal(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t uid,uint8_t goal,uint8_t smode,uint32_t *sinodes,uint32_t *ncinodes,uint32_t *nsinodes/*,uint32_t *qeinodes*/) {
	uint32_t ts;
	fsnode *rn;
//	uint8_t quota;
	fsnode *p;

	(void)sesflags;
	ts = get_current_time();
	*sinodes = 0;
	*ncinodes = 0;
	*nsinodes = 0;
	if (!SMODE_ISVALID(smode) || goal>9 || goal<1) {
		return ERROR_EINVAL;
	}
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID || rootinode==0) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (rootinode==0 && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_DIRECTORY && p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}

//	quota = fsnodes_test_quota(p);
	fsnodes_setgoal_recursive(p,ts,uid/*,quota*/,goal,smode,sinodes,ncinodes,nsinodes/*,qeinodes*/);
	if ((smode&SMODE_RMASK)==0 && *nsinodes>0 && *sinodes==0 && *ncinodes==0) {
		return ERROR_EPERM;
	}
	//changelog(version++,"%"PRIu32"|SETGOAL(%"PRIu32",%"PRIu32",%"PRIu8",%"PRIu8"):%"PRIu32",%"PRIu32",%"PRIu32/*",%"PRIu32*/,ts,inode,uid,goal,smode,*sinodes,*ncinodes,*nsinodes/*,*qeinodes*/);
	return STATUS_OK;
}

uint8_t shadow_fs_settrashtime(uint32_t ts,uint32_t inode,uint32_t uid,uint32_t trashtime,uint8_t smode,uint32_t sinodes,uint32_t ncinodes,uint32_t nsinodes) {
	uint32_t si,nci,nsi;
        fsnode *p;
	
	si = 0;
        nci = 0;
        nsi = 0;
	
	if (!SMODE_ISVALID(smode)) {
                return ERROR_EINVAL;
        }
	p = fsnodes_id_to_node(inode);
        if (!p) {
                return ERROR_ENOENT;
        }
        if (p->type!=TYPE_DIRECTORY && p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
                return ERROR_EPERM;
        }
	fsnodes_settrashtime_recursive(p,ts,uid,trashtime,smode,&si,&nci,&nsi);
        version++;
        if (sinodes!=si || ncinodes!=nci || nsinodes!=nsi) {
                return ERROR_MISMATCH;
        }
        return STATUS_OK;
}
	
uint8_t fs_settrashtime(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t uid,uint32_t trashtime,uint8_t smode,uint32_t *sinodes,uint32_t *ncinodes,uint32_t *nsinodes) {
	uint32_t ts;
	fsnode *rn;
	fsnode *p;

	(void)sesflags;
	ts = get_current_time();
	*sinodes = 0;
	*ncinodes = 0;
	*nsinodes = 0;
	if (!SMODE_ISVALID(smode)) {
		return ERROR_EINVAL;
	}
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID || rootinode==0) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (rootinode==0 && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_DIRECTORY && p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}

	fsnodes_settrashtime_recursive(p,ts,uid,trashtime,smode,sinodes,ncinodes,nsinodes);
	if ((smode&SMODE_RMASK)==0 && *nsinodes>0 && *sinodes==0 && *ncinodes==0) {
		return ERROR_EPERM;
	}
	//changelog(version++,"%"PRIu32"|SETTRASHTIME(%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu8"):%"PRIu32",%"PRIu32",%"PRIu32,ts,inode,uid,trashtime,smode,*sinodes,*ncinodes,*nsinodes);
	return STATUS_OK;
}

uint8_t shadow_fs_seteattr(uint32_t ts,uint32_t inode,uint32_t uid,uint8_t eattr,uint8_t smode,uint32_t sinodes,uint32_t ncinodes,uint32_t nsinodes) {
	uint32_t si,nci,nsi;
	fsnode *p;
	si = 0;
        nci = 0;
        nsi = 0;
        if (!SMODE_ISVALID(smode) || (eattr&(~(EATTR_NOOWNER|EATTR_NOACACHE|EATTR_NOECACHE|EATTR_NODATACACHE)))) {
                return ERROR_EINVAL;
        }
        p = fsnodes_id_to_node(inode);
        if (!p) {
                return ERROR_ENOENT;
        }
	fsnodes_seteattr_recursive(p,ts,uid,eattr,smode,&si,&nci,&nsi);
        version++;
        if (sinodes!=si || ncinodes!=nci || nsinodes!=nsi/* || qeinodes!=qei*/) {
                return ERROR_MISMATCH;
        }
        return STATUS_OK;
}
	
uint8_t fs_seteattr(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t uid,uint8_t eattr,uint8_t smode,uint32_t *sinodes,uint32_t *ncinodes,uint32_t *nsinodes) {
	uint32_t ts;
	fsnode *rn;
	fsnode *p;

	(void)sesflags;
	ts = get_current_time();
	*sinodes = 0;
	*ncinodes = 0;
	*nsinodes = 0;
	if (!SMODE_ISVALID(smode) || (eattr&(~(EATTR_NOOWNER|EATTR_NOACACHE|EATTR_NOECACHE|EATTR_NODATACACHE)))) {
		return ERROR_EINVAL;
	}
	if (sesflags&SESFLAG_READONLY) {
		return ERROR_EROFS;
	}
	if (rootinode==MFS_ROOT_ID || rootinode==0) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (rootinode==0 && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}

	fsnodes_seteattr_recursive(p,ts,uid,eattr,smode,sinodes,ncinodes,nsinodes);
	if ((smode&SMODE_RMASK)==0 && *nsinodes>0 && *sinodes==0 && *ncinodes==0) {
		return ERROR_EPERM;
	}

	//changelog(version++,"%"PRIu32"|SETEATTR(%"PRIu32",%"PRIu32",%"PRIu8",%"PRIu8"):%"PRIu32",%"PRIu32",%"PRIu32/*",%"PRIu32*/,ts,inode,uid,eattr,smode,*sinodes,*ncinodes,*nsinodes/*,*qeinodes*/);
	return STATUS_OK;
}

uint8_t fs_quotacontrol(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint8_t delflag,uint8_t *flags,uint32_t *sinodes,uint64_t *slength,uint64_t *ssize,uint64_t *srealsize,uint32_t *hinodes,uint64_t *hlength,uint64_t *hsize,uint64_t *hrealsize,uint32_t *curinodes,uint64_t *curlength,uint64_t *cursize,uint64_t *currealsize) {
	fsnode *p,*rn;
	quotanode *qn;
	statsrecord *psr;

	if (*flags) {
		if (sesflags&SESFLAG_READONLY) {
			return ERROR_EROFS;
		}
		if ((sesflags&SESFLAG_CANCHANGEQUOTA)==0) {
			return ERROR_EPERM;
		}
	}
	if (rootinode==0) {
		return ERROR_EPERM;
	}
	if (rootinode==MFS_ROOT_ID) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_DIRECTORY) {
		return ERROR_EPERM;
	}
	qn = p->data.ddata.quota;
	if (delflag) {
		if (qn) {
			qn->flags &= ~(*flags);
			if (qn->flags==0) {
				fsnodes_delete_quotanode(qn);
				p->data.ddata.quota = NULL;
				qn=NULL;
			}
		}
	} else {
		if (qn==NULL && (*flags)!=0) {
			p->data.ddata.quota = fsnodes_new_quotanode();
			qn = p->data.ddata.quota;
			qn->node = p;
		}
		qn->flags |= *flags;
		if ((*flags)&QUOTA_FLAG_SINODES) {
			qn->sinodes = *sinodes;
		}
		if ((*flags)&QUOTA_FLAG_SLENGTH) {
			qn->slength = *slength;
		}
		if ((*flags)&QUOTA_FLAG_SSIZE) {
			qn->ssize = *ssize;
		}
		if ((*flags)&QUOTA_FLAG_SREALSIZE) {
			qn->srealsize = *srealsize;
		}
		if ((*flags)&QUOTA_FLAG_HINODES) {
			qn->hinodes = *hinodes;
		}
		if ((*flags)&QUOTA_FLAG_HLENGTH) {
			qn->hlength = *hlength;
		}
		if ((*flags)&QUOTA_FLAG_HSIZE) {
			qn->hsize = *hsize;
		}
		if ((*flags)&QUOTA_FLAG_HREALSIZE) {
			qn->hrealsize = *hrealsize;
		}
	}
	if (qn) {
		*flags = qn->flags;
		*sinodes = qn->sinodes;
		*slength = qn->slength;
		*ssize = qn->ssize;
		*srealsize = qn->srealsize;
		*hinodes = qn->hinodes;
		*hlength = qn->hlength;
		*hsize = qn->hsize;
		*hrealsize = qn->hrealsize;
	} else {
		*flags = 0;
		*sinodes = 0;
		*slength = 0;
		*ssize = 0;
		*srealsize = 0;
		*hinodes = 0;
		*hlength = 0;
		*hsize = 0;
		*hrealsize = 0;
	}
	psr = p->data.ddata.stats;
	*curinodes = psr->inodes;
	*curlength = psr->length;
	*cursize = psr->size;
	*currealsize = psr->realsize;
	return STATUS_OK;
}

uint32_t fs_getquotainfo_size() {
	quotanode *qn;
	uint32_t s=0,size;
//	s=4;	// QuotaTimeLimit
	for (qn=quotahead ; qn ; qn=qn->next) {
		size=fsnodes_getpath_size(qn->node->parents);
		s+=4+4+1+1+4+3*(4+8+8+8)+1+size;
	}
	return s;
}

void fs_getquotainfo_data(uint8_t * buff) {
	quotanode *qn;
	statsrecord *psr;
	uint32_t size;
	uint32_t ts = get_current_time();
//	put32bit(&buff,QuotaTimeLimit);
	for (qn=quotahead ; qn ; qn=qn->next) {
		psr = qn->node->data.ddata.stats;
		put32bit(&buff,qn->node->id);
		size=fsnodes_getpath_size(qn->node->parents);
		put32bit(&buff,size+1);
		put8bit(&buff,'/');
		fsnodes_getpath_data(qn->node->parents,buff,size);
		buff+=size;
		put8bit(&buff,qn->exceeded);
		put8bit(&buff,qn->flags);
		if (qn->stimestamp==0) {					// soft quota not exceeded
			put32bit(&buff,0xFFFFFFFF); 				// time to block = INF
		} else if (qn->stimestamp+QuotaTimeLimit<ts) {			// soft quota timed out
			put32bit(&buff,0);					// time to block = 0 (blocked)
		} else {							// soft quota exceeded, but not timed out
			put32bit(&buff,qn->stimestamp+QuotaTimeLimit-ts);
		}
		if (qn->flags&QUOTA_FLAG_SINODES) {
			put32bit(&buff,qn->sinodes);
		} else {
			put32bit(&buff,0);
		}
		if (qn->flags&QUOTA_FLAG_SLENGTH) {
			put64bit(&buff,qn->slength);
		} else {
			put64bit(&buff,0);
		}
		if (qn->flags&QUOTA_FLAG_SSIZE) {
			put64bit(&buff,qn->ssize);
		} else {
			put64bit(&buff,0);
		}
		if (qn->flags&QUOTA_FLAG_SREALSIZE) {
			put64bit(&buff,qn->srealsize);
		} else {
			put64bit(&buff,0);
		}
		if (qn->flags&QUOTA_FLAG_HINODES) {
			put32bit(&buff,qn->hinodes);
		} else {
			put32bit(&buff,0);
		}
		if (qn->flags&QUOTA_FLAG_HLENGTH) {
			put64bit(&buff,qn->hlength);
		} else {
			put64bit(&buff,0);
		}
		if (qn->flags&QUOTA_FLAG_HSIZE) {
			put64bit(&buff,qn->hsize);
		} else {
			put64bit(&buff,0);
		}
		if (qn->flags&QUOTA_FLAG_HREALSIZE) {
			put64bit(&buff,qn->hrealsize);
		} else {
			put64bit(&buff,0);
		}
		put32bit(&buff,psr->inodes);
		put64bit(&buff,psr->length);
		put64bit(&buff,psr->size);
		put64bit(&buff,psr->realsize);
	}
}

uint32_t fs_getdirpath_size(uint32_t inode) {
	fsnode *node;
	node = fsnodes_id_to_node(inode);
	if (node) {
		if (node->type!=TYPE_DIRECTORY) {
			return 15; // "(not directory)"
		} else {
			return 1+fsnodes_getpath_size(node->parents);
		}
	} else {
		return 11; // "(not found)"
	}
	return 0;
}

void fs_getdirpath_data(uint32_t inode,uint8_t *buff,uint32_t size) {
	fsnode *node;
	node = fsnodes_id_to_node(inode);
	if (node) {
		if (node->type!=TYPE_DIRECTORY) {
			if (size>=15) {
				memcpy(buff,"(not directory)",15);
				return;
			}
		} else {
			if (size>0) {
				buff[0]='/';
				fsnodes_getpath_data(node->parents,buff+1,size-1);
				return;
			}
		}
	} else {
		if (size>=11) {
			memcpy(buff,"(not found)",11);
			return;
		}
	}
}

uint8_t fs_get_dir_stats(uint32_t rootinode,uint8_t sesflags,uint32_t inode,uint32_t *inodes,uint32_t *dirs,uint32_t *files,uint32_t *chunks,uint64_t *length,uint64_t *size,uint64_t *rsize) {
	fsnode *p,*rn;
	statsrecord sr;
	(void)sesflags;
	if (rootinode==MFS_ROOT_ID || rootinode==0) {
		p = fsnodes_id_to_node(inode);
		if (!p) {
			return ERROR_ENOENT;
		}
		if (rootinode==0 && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
			return ERROR_EPERM;
		}
	} else {
		rn = fsnodes_id_to_node(rootinode);
		if (!rn || rn->type!=TYPE_DIRECTORY) {
			return ERROR_ENOENT;
		}
		if (inode==MFS_ROOT_ID) {
			inode = rootinode;
			p = rn;
		} else {
			p = fsnodes_id_to_node(inode);
			if (!p) {
				return ERROR_ENOENT;
			}
			if (!fsnodes_isancestor(rn,p)) {
				return ERROR_EPERM;
			}
		}
	}
	if (p->type!=TYPE_DIRECTORY && p->type!=TYPE_FILE && p->type!=TYPE_TRASH && p->type!=TYPE_RESERVED) {
		return ERROR_EPERM;
	}
	fsnodes_get_stats(p,&sr);
	*inodes = sr.inodes;
	*dirs = sr.dirs;
	*files = sr.files;
	*chunks = sr.chunks;
	*length = sr.length;
	*size = sr.size;
	*rsize = sr.realsize;
	MFSLOG(LOG_NOTICE,"using fast stats");
	return STATUS_OK;
}

void fs_add_files_to_chunks() {
	uint32_t i,j;
	uint64_t chunkid;
	fsnode *f;
	for (i=0 ; i<NODEHASHSIZE ; i++) {
		for (f=nodehash[i] ; f ; f=f->next) {
			if (f->type==TYPE_FILE || f->type==TYPE_TRASH || f->type==TYPE_RESERVED) {
				for (j=0 ; j<f->data.fdata.chunks ; j++) {
					chunkid = f->data.fdata.chunktab[j];
					if (chunkid>0) {
						chunk_add_file(chunkid,f->id,j,f->goal);
					}
				}
			}
		}
	}
}


void fs_test_getdata(uint32_t *loopstart,uint32_t *loopend,uint32_t *files,uint32_t *ugfiles,uint32_t *mfiles,uint32_t *chunks,uint32_t *ugchunks,uint32_t *mchunks,char **msgbuff,uint32_t *msgbuffleng) {
	*loopstart = fsinfo_loopstart;
	*loopend = fsinfo_loopend;
	*files = fsinfo_files;
	*ugfiles = fsinfo_ugfiles;
	*mfiles = fsinfo_mfiles;
	*chunks = fsinfo_chunks;
	*ugchunks = fsinfo_ugchunks;
	*mchunks = fsinfo_mchunks;
	*msgbuff = fsinfo_msgbuff;
	*msgbuffleng = fsinfo_msgbuffleng;
}

uint32_t fs_test_log_inconsistency(fsedge *e,const char *iname,char *buff,uint32_t size) {
	uint32_t leng;
	leng=0;
	if (e->parent) {
	        log_structure_error %= LOG_COUNT;
                if (log_structure_error++ == 0){	
//			syslog(LOG_ERR,"structure error - %s inconsistency (edge: %"PRIu32",%s -> %"PRIu32")",iname,e->parent->id,fsnodes_escape_name(e->nleng,e->name),e->child->id);
		}
		if (leng<size) {
			leng += snprintf(buff+leng,size-leng,"structure error - %s inconsistency (edge: %"PRIu32",%s -> %"PRIu32")\n",iname,e->parent->id,fsnodes_escape_name(e->nleng,e->name),e->child->id);
		}
	} else {
		if (e->child->type==TYPE_TRASH) {
		        log_structure_error %= LOG_COUNT;
                        if (log_structure_error++ == 0){
//				syslog(LOG_ERR,"structure error - %s inconsistency (edge: TRASH,%s -> %"PRIu32")",iname,fsnodes_escape_name(e->nleng,e->name),e->child->id);
			}
			if (leng<size) {
				leng += snprintf(buff+leng,size-leng,"structure error - %s inconsistency (edge: TRASH,%s -> %"PRIu32")\n",iname,fsnodes_escape_name(e->nleng,e->name),e->child->id);
			}
		} else if (e->child->type==TYPE_RESERVED) {
			log_structure_error %= LOG_COUNT;
                        if (log_structure_error++ == 0){
//				syslog(LOG_ERR,"structure error - %s inconsistency (edge: RESERVED,%s -> %"PRIu32")",iname,fsnodes_escape_name(e->nleng,e->name),e->child->id);
			}
			if (leng<size) {
				leng += snprintf(buff+leng,size-leng,"structure error - %s inconsistency (edge: RESERVED,%s -> %"PRIu32")\n",iname,fsnodes_escape_name(e->nleng,e->name),e->child->id);
			}
		} else {
                       	log_structure_error %= LOG_COUNT;
                       	if (log_structure_error++ == 0){
//				syslog(LOG_ERR,"structure error - %s inconsistency (edge: NULL,%s -> %"PRIu32")",iname,fsnodes_escape_name(e->nleng,e->name),e->child->id);
			}
			if (leng<size) {
				leng += snprintf(buff+leng,size-leng,"structure error - %s inconsistency (edge: NULL,%s -> %"PRIu32")\n",iname,fsnodes_escape_name(e->nleng,e->name),e->child->id);
			}
		}
	}
	return leng;
}
void log_print_control_fs(void) {
	log_structure_error=0;
	log_test_files=0;	
}

void fs_test_files() {
	static uint32_t i=0;
	uint32_t j;
	uint32_t k;
	uint64_t chunkid;
	uint8_t vc,valid,ugflag;
	static uint32_t files=0;
	static uint32_t ugfiles=0;
	static uint32_t mfiles=0;
	static uint32_t chunks=0;
	static uint32_t ugchunks=0;
	static uint32_t mchunks=0;
	static uint32_t errors=0;
	static uint32_t notfoundchunks=0;
	static uint32_t unavailchunks=0;
	static uint32_t unavailfiles=0;
	static uint32_t unavailtrashfiles=0;
	static uint32_t unavailreservedfiles=0;
	static char *msgbuff=NULL,*tmp;
	static uint32_t leng=0;
	fsnode *f;
	fsedge *e;

	if ((uint32_t)(get_current_time())<=starttime+900) {
		return;
	}
	if (i>=NODEHASHSIZE) {
		MFSLOG(LOG_NOTICE,"structure check loop");
		i=0;
		errors=0;
	}
	if (i==0) {
		if (errors==ERRORS_LOG_MAX) {
			log_test_files %= LOG_COUNT;
                        if (log_test_files++ == 0){
//				syslog(LOG_ERR,"only first %u errors (unavailable chunks/files) were logged",ERRORS_LOG_MAX);
			}	
			if (leng<MSGBUFFSIZE) {
				leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"only first %u errors (unavailable chunks/files) were logged\n",ERRORS_LOG_MAX);
			}
		}
		if (notfoundchunks>0) {
			log_test_files %= LOG_COUNT;
                        if (log_test_files++ == 0){
//				syslog(LOG_ERR,"unknown chunks: %"PRIu32,notfoundchunks);
			}
			if (leng<MSGBUFFSIZE) {
				leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"unknown chunks: %"PRIu32"\n",notfoundchunks);
			}
			notfoundchunks=0;
		}
		if (unavailchunks>0) {
			log_test_files %= LOG_COUNT;
                        if (log_test_files++ == 0){
//				syslog(LOG_ERR,"unavailable chunks: %"PRIu32,unavailchunks);
			}
			if (leng<MSGBUFFSIZE) {
				leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"unavailable chunks: %"PRIu32"\n",unavailchunks);
			}
			unavailchunks=0;
		}
		if (unavailtrashfiles>0) {
			log_test_files %= LOG_COUNT;
                        if (log_test_files++ == 0){
//				syslog(LOG_ERR,"unavailable trash files: %"PRIu32,unavailtrashfiles);
			}	
			if (leng<MSGBUFFSIZE) {
				leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"unavailable trash files: %"PRIu32"\n",unavailtrashfiles);
			}
			unavailtrashfiles=0;
		}
		if (unavailreservedfiles>0) {
			log_test_files %= LOG_COUNT;
                        if (log_test_files++ == 0){
//				syslog(LOG_ERR,"unavailable reserved files: %"PRIu32,unavailreservedfiles);
			}
			if (leng<MSGBUFFSIZE) {
				leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"unavailable reserved files: %"PRIu32"\n",unavailreservedfiles);
			}
			unavailreservedfiles=0;
		}
		if (unavailfiles>0) {
			log_test_files %= LOG_COUNT;
                        if (log_test_files++ == 0){
//				syslog(LOG_ERR,"unavailable files: %"PRIu32,unavailfiles);
			}
			if (leng<MSGBUFFSIZE) {
				leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"unavailable files: %"PRIu32"\n",unavailfiles);
			}
			unavailfiles=0;
		}
		fsinfo_files=files;
		fsinfo_ugfiles=ugfiles;
		fsinfo_mfiles=mfiles;
		fsinfo_chunks=chunks;
		fsinfo_ugchunks=ugchunks;
		fsinfo_mchunks=mchunks;
		files=0;
		ugfiles=0;
		mfiles=0;
		chunks=0;
		ugchunks=0;
		mchunks=0;

		if (fsinfo_msgbuff==NULL) {
			fsinfo_msgbuff=malloc(MSGBUFFSIZE);
		}
		tmp = fsinfo_msgbuff;
		fsinfo_msgbuff=msgbuff;
		msgbuff = tmp;
		if (leng>MSGBUFFSIZE) {
			fsinfo_msgbuffleng=MSGBUFFSIZE;
		} else {
			fsinfo_msgbuffleng=leng;
		}
		leng=0;

		fsinfo_loopstart = fsinfo_loopend;
		fsinfo_loopend = get_current_time();
	}
	for (k=0 ; k<(NODEHASHSIZE/14400) && i<NODEHASHSIZE ; k++,i++) {
		for (f=nodehash[i] ; f ; f=f->next) {
			if (f->type==TYPE_FILE || f->type==TYPE_TRASH || f->type==TYPE_RESERVED) {
				valid = 1;
				ugflag = 0;
				for (j=0 ; j<f->data.fdata.chunks ; j++) {
					chunkid = f->data.fdata.chunktab[j];
					if (chunkid>0) {
						if (chunk_get_validcopies(chunkid,&vc)!=STATUS_OK) {
							if (errors<ERRORS_LOG_MAX) {
					                        log_test_files %= LOG_COUNT;
                        					if (log_test_files++ == 0){
//									syslog(LOG_ERR,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,f->id,j);
								}
								if (leng<MSGBUFFSIZE) {
									leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"structure error - chunk %016"PRIX64" not found (inode: %"PRIu32" ; index: %"PRIu32")\n",chunkid,f->id,j);
								}
								errors++;
							}
							notfoundchunks++;
							if ((notfoundchunks%1000)==0) {
//								syslog(LOG_ERR,"unknown chunks: %"PRIu32" ...",notfoundchunks);
							}
							valid =0;
							mchunks++;
						} else if (vc==0) {
							if (errors<ERRORS_LOG_MAX) {
					                        log_test_files %= LOG_COUNT;
                        					if (log_test_files++ == 0){
//									syslog(LOG_ERR,"currently unavailable chunk %016"PRIX64" (inode: %"PRIu32" ; index: %"PRIu32")",chunkid,f->id,j);
								}
								if (leng<MSGBUFFSIZE) {
									leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"currently unavailable chunk %016"PRIX64" (inode: %"PRIu32" ; index: %"PRIu32")\n",chunkid,f->id,j);
								}
								errors++;
							}
							unavailchunks++;
							if ((unavailchunks%1000)==0) {
//								syslog(LOG_ERR,"unavailable chunks: %"PRIu32" ...",unavailchunks);
							}
							valid = 0;
							mchunks++;
						} else if (vc<f->goal) {
							ugflag = 1;
							ugchunks++;
						}
						chunks++;
					}
				}
				if (valid==0) {
					mfiles++;
					if (f->type==TYPE_TRASH) {
						if (errors<ERRORS_LOG_MAX) {
				                        log_test_files %= LOG_COUNT;
                        				if (log_test_files++ == 0){
//								syslog(LOG_ERR,"- currently unavailable file in trash %"PRIu32": %s",f->id,fsnodes_escape_name(f->parents->nleng,f->parents->name));
							}
							if (leng<MSGBUFFSIZE) {
								leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"- currently unavailable file in trash %"PRIu32": %s\n",f->id,fsnodes_escape_name(f->parents->nleng,f->parents->name));
							}
							errors++;
							unavailtrashfiles++;
							if ((unavailtrashfiles%1000)==0) {
//								syslog(LOG_ERR,"unavailable trash files: %"PRIu32" ...",unavailtrashfiles);
							}
						}
					} else if (f->type==TYPE_RESERVED) {
						if (errors<ERRORS_LOG_MAX) {
				                        log_test_files %= LOG_COUNT;
                        				if (log_test_files++ == 0){
//								syslog(LOG_ERR,"+ currently unavailable reserved file %"PRIu32": %s",f->id,fsnodes_escape_name(f->parents->nleng,f->parents->name));
							}
							if (leng<MSGBUFFSIZE) {
								leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"+ currently unavailable reserved file %"PRIu32": %s\n",f->id,fsnodes_escape_name(f->parents->nleng,f->parents->name));
							}
							errors++;
							unavailreservedfiles++;
							if ((unavailreservedfiles%1000)==0) {
//								syslog(LOG_ERR,"unavailable reserved files: %"PRIu32" ...",unavailreservedfiles);
							}
						}
					} else {
						uint8_t *path;
						uint16_t pleng;
						for (e=f->parents ; e ; e=e->nextparent) {
							if (errors<ERRORS_LOG_MAX) {
								fsnodes_getpath(e,&pleng,&path);
//								syslog(LOG_ERR,"* currently unavailable file %"PRIu32": %s",f->id,fsnodes_escape_name(pleng,path));
								if (leng<MSGBUFFSIZE) {
									leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"* currently unavailable file %"PRIu32": %s\n",f->id,fsnodes_escape_name(pleng,path));
								}
								free(path);
								errors++;
							}
							unavailfiles++;
							if ((unavailfiles%1000)==0) {
//								syslog(LOG_ERR,"unavailable files: %"PRIu32" ...",unavailfiles);
							}
						}
					}
				} else if (ugflag) {
					ugfiles++;
				}
				files++;
			}
			for (e=f->parents ; e ; e=e->nextparent) {
				if (e->child != f) {
					if (e->parent) {
						log_test_files %= LOG_COUNT;
                        			if (log_test_files++ == 0){
//							syslog(LOG_ERR,"structure error - edge->child/child->edges (node: %"PRIu32" ; edge: %"PRIu32",%s -> %"PRIu32")",f->id,e->parent->id,fsnodes_escape_name(e->nleng,e->name),e->child->id);
						}
						if (leng<MSGBUFFSIZE) {
							leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"structure error - edge->child/child->edges (node: %"PRIu32" ; edge: %"PRIu32",%s -> %"PRIu32")\n",f->id,e->parent->id,fsnodes_escape_name(e->nleng,e->name),e->child->id);
						}
					} else {
				                log_test_files %= LOG_COUNT;
                        			if (log_test_files++ == 0){
//							syslog(LOG_ERR,"structure error - edge->child/child->edges (node: %"PRIu32" ; edge: NULL,%s -> %"PRIu32")",f->id,fsnodes_escape_name(e->nleng,e->name),e->child->id);
						}
						if (leng<MSGBUFFSIZE) {
							leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"structure error - edge->child/child->edges (node: %"PRIu32" ; edge: NULL,%s -> %"PRIu32")\n",f->id,fsnodes_escape_name(e->nleng,e->name),e->child->id);
						}
					}
				} else if (e->nextchild) {
					if (e->nextchild->prevchild != &(e->nextchild)) {
						if (leng<MSGBUFFSIZE) {
							leng += fs_test_log_inconsistency(e,"nextchild/prevchild",msgbuff+leng,MSGBUFFSIZE-leng);
						} else {
							fs_test_log_inconsistency(e,"nextchild/prevchild",NULL,0);
						}
					}
				} else if (e->nextparent) {
					if (e->nextparent->prevparent != &(e->nextparent)) {
						if (leng<MSGBUFFSIZE) {
							leng += fs_test_log_inconsistency(e,"nextparent/prevparent",msgbuff+leng,MSGBUFFSIZE-leng);
						} else {
							fs_test_log_inconsistency(e,"nextparent/prevparent",NULL,0);
						}
					}
#ifdef EDGEHASH
				} else if (e->next) {
					if (e->next->prev != &(e->next)) {
						if (leng<MSGBUFFSIZE) {
							leng += fs_test_log_inconsistency(e,"nexthash/prevhash",msgbuff+leng,MSGBUFFSIZE-leng);
						} else {
							fs_test_log_inconsistency(e,"nexthash/prevhash",NULL,0);
						}
					}
#endif
				}
			}
			if (f->type == TYPE_DIRECTORY) {
				for (e=f->data.ddata.children ; e ; e=e->nextchild) {
					if (e->parent != f) {
						if (e->parent) {
							log_test_files %= LOG_COUNT;
                        				if (log_test_files++ == 0){
//								syslog(LOG_ERR,"structure error - edge->parent/parent->edges (node: %"PRIu32" ; edge: %"PRIu32",%s -> %"PRIu32")",f->id,e->parent->id,fsnodes_escape_name(e->nleng,e->name),e->child->id);
							}
							if (leng<MSGBUFFSIZE) {
								leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"structure error - edge->parent/parent->edges (node: %"PRIu32" ; edge: %"PRIu32",%s -> %"PRIu32")\n",f->id,e->parent->id,fsnodes_escape_name(e->nleng,e->name),e->child->id);
							}
						} else {
							log_test_files %= LOG_COUNT;
                        				if (log_test_files++ == 0){
//								syslog(LOG_ERR,"structure error - edge->parent/parent->edges (node: %"PRIu32" ; edge: NULL,%s -> %"PRIu32")",f->id,fsnodes_escape_name(e->nleng,e->name),e->child->id);
							}
							if (leng<MSGBUFFSIZE) {
								leng += snprintf(msgbuff+leng,MSGBUFFSIZE-leng,"structure error - edge->parent/parent->edges (node: %"PRIu32" ; edge: NULL,%s -> %"PRIu32")\n",f->id,fsnodes_escape_name(e->nleng,e->name),e->child->id);
							}
						}
					} else if (e->nextchild) {
						if (e->nextchild->prevchild != &(e->nextchild)) {
							if (leng<MSGBUFFSIZE) {
								leng += fs_test_log_inconsistency(e,"nextchild/prevchild",msgbuff+leng,MSGBUFFSIZE-leng);
							} else {
								fs_test_log_inconsistency(e,"nextchild/prevchild",NULL,0);
							}
						}
					} else if (e->nextparent) {
						if (e->nextparent->prevparent != &(e->nextparent)) {
							if (leng<MSGBUFFSIZE) {
								leng += fs_test_log_inconsistency(e,"nextparent/prevparent",msgbuff+leng,MSGBUFFSIZE-leng);
							} else {
								fs_test_log_inconsistency(e,"nextparent/prevparent",NULL,0);
							}
						}
#ifdef EDGEHASH
					} else if (e->next) {
						if (e->next->prev != &(e->next)) {
							if (leng<MSGBUFFSIZE) {
								leng += fs_test_log_inconsistency(e,"nexthash/prevhash",msgbuff+leng,MSGBUFFSIZE-leng);
							} else {
								fs_test_log_inconsistency(e,"nexthash/prevhash",NULL,0);
							}
						}
#endif
					}
				}
			}
		}
	}
}

//shadowmaster interface
uint8_t shadow_fs_emptytrash(uint32_t ts,uint32_t freeinodes,uint32_t reservedinodes) {
        uint32_t fi,ri;
        fsedge *e;
        fsnode *p;
        
	fi=0;
        ri=0;
        e = trash;
        while (e) {
                p = e->child;
                e = e->nextchild;
                if (((uint64_t)(p->atime) + (uint64_t)(p->trashtime) < (uint64_t)ts) && ((uint64_t)(p->mtime) + (uint64_t)(p->trashtime) < (uint64_t)ts) && ((uint64_t)(p->ctime) + (uint64_t)(p->trashtime) < (uint64_t)ts)) {
                        if (shadow_fsnodes_purge(ts,p)) {
                                fi++;
                        } else {
                                ri++;
                        }
                }
        }
	version++;
        if (freeinodes!=fi || reservedinodes!=ri) {
                return ERROR_MISMATCH;
        }
        return STATUS_OK;
}

uint8_t shadow_fs_emptyreserved(uint32_t ts,uint32_t freeinodes) {
	fsedge *e;
        fsnode *p;
        uint32_t fi;
	fi =0;
        e = reserved;
        while (e) {
                p = e->child;
                e = e->nextchild;
                if (p->data.fdata.sessionids==NULL) {
                        shadow_fsnodes_purge(ts,p);
                        fi++;
                }
        }
	version++;
	if (freeinodes!=fi) {
//		syslog(LOG_NOTICE,"the freeinodes != fi,freeinodes:%d,fi:%d",freeinodes,fi);
                return ERROR_MISMATCH;
        }
        return STATUS_OK;
}

uint64_t shadow_fs_getversion() {
	return version;
}


enum {FLAG_TREE,FLAG_TRASH,FLAG_RESERVED};

/* DUMP */

void fs_dumpedge(fsedge *e) {
	if (e->parent==NULL) {
		if (e->child->type==TYPE_TRASH) {
			printf("E|p:     TRASH|c:%10"PRIu32"|n:%s\n",e->child->id,fsnodes_escape_name(e->nleng,e->name));
		} else if (e->child->type==TYPE_RESERVED) {
			printf("E|p:  RESERVED|c:%10"PRIu32"|n:%s\n",e->child->id,fsnodes_escape_name(e->nleng,e->name));
		} else {
			printf("E|p:      NULL|c:%10"PRIu32"|n:%s\n",e->child->id,fsnodes_escape_name(e->nleng,e->name));
		}
	} else {
		printf("E|p:%10"PRIu32"|c:%10"PRIu32"|n:%s\n",e->parent->id,e->child->id,fsnodes_escape_name(e->nleng,e->name));
	}
}

void fs_dumpnode(fsnode *f) {
	char c;
	uint32_t i,ch;
	sessionidrec *sessionidptr;

	c='?';
	switch (f->type) {
	case TYPE_DIRECTORY:
		c='D';
		break;
	case TYPE_SOCKET:
		c='S';
		break;
	case TYPE_FIFO:
		c='F';
		break;
	case TYPE_BLOCKDEV:
		c='B';
		break;
	case TYPE_CHARDEV:
		c='C';
		break;
	case TYPE_SYMLINK:
		c='L';
		break;
	case TYPE_FILE:
		c='-';
		break;
	case TYPE_TRASH:
		c='T';
		break;
	case TYPE_RESERVED:
		c='R';
		break;
	}
//	if (flag==FLAG_TRASH) {
//		c='T';
//	} else if (flag==FLAG_RESERVED) {
//		c='R';
//	}

	printf("%c|i:%10"PRIu32"|#:%"PRIu8"|e:%1"PRIX16"|m:%04"PRIo16"|u:%10"PRIu32"|g:%10"PRIu32"|a:%10"PRIu32",m:%10"PRIu32",c:%10"PRIu32"|t:%10"PRIu32,c,f->id,f->goal,f->mode>>12,f->mode&0xFFF,f->uid,f->gid,f->atime,f->mtime,f->ctime,f->trashtime);

	if (f->type==TYPE_BLOCKDEV || f->type==TYPE_CHARDEV) {
		printf("|d:%5"PRIu32",%5"PRIu32"\n",f->data.rdev>>16,f->data.rdev&0xFFFF);
	} else if (f->type==TYPE_SYMLINK) {
		printf("|p:%s\n",fsnodes_escape_name(f->data.sdata.pleng,f->data.sdata.path));
	} else if (f->type==TYPE_FILE || f->type==TYPE_TRASH || f->type==TYPE_RESERVED) {
		printf("|l:%20"PRIu64"|c:(",f->data.fdata.length);
		ch = 0;
		for (i=0 ; i<f->data.fdata.chunks ; i++) {
			if (f->data.fdata.chunktab[i]!=0) {
				ch=i+1;
			}
		}
		for (i=0 ; i<ch ; i++) {
			if (f->data.fdata.chunktab[i]!=0) {
				printf("%016"PRIX64,f->data.fdata.chunktab[i]);
			} else {
				printf("N");
			}
			if (i+1<ch) {
				printf(",");
			}
		}
		printf(")|r:(");
		for (sessionidptr=f->data.fdata.sessionids ; sessionidptr ; sessionidptr=sessionidptr->next) {
			printf("%"PRIu32,sessionidptr->sessionid);
			if (sessionidptr->next) {
				printf(",");
			}
		}
		printf(")\n");
	} else {
		printf("\n");
	}
}

void fs_dumpnodes() {
	uint32_t i;
	fsnode *p;
	for (i=0 ; i<NODEHASHSIZE ; i++) {
		for (p=nodehash[i] ; p ; p=p->next) {
			fs_dumpnode(p);
		}
	}
}

void fs_dumpedgelist(fsedge *e) {
	while (e) {
		fs_dumpedge(e);
		e=e->nextchild;
	}
}

void fs_dumpedges(fsnode *f) {
	fsedge *e;
	fs_dumpedgelist(f->data.ddata.children);
	for (e=f->data.ddata.children ; e ; e=e->nextchild) {
		if (e->child->type==TYPE_DIRECTORY) {
			fs_dumpedges(e->child);
		}
	}
}

void fs_dumpfree() {
	freenode *n;
	for (n=freelist ; n ; n=n->next) {
		printf("I|i:%10"PRIu32"|f:%10"PRIu32"\n",n->id,n->ftime);
	}
}

void fs_dump(void) {
	fs_dumpnodes();
	fs_dumpedges(root);
	fs_dumpedgelist(trash);
	fs_dumpedgelist(reserved);
	fs_dumpfree();
}


void fs_storeedge(fsedge *e,FILE *fd) {
	uint8_t uedgebuff[4+4+2+65535];
	uint8_t *ptr;
	if (e==NULL) {	// last edge
		memset(uedgebuff,0,4+4+2);
		fwrite(uedgebuff,1,4+4+2,fd);
		return;
	}
	ptr = uedgebuff;
	if (e->parent==NULL) {
		put32bit(&ptr,0);
	} else {
		put32bit(&ptr,e->parent->id);
	}
	put32bit(&ptr,e->child->id);
	put16bit(&ptr,e->nleng);
	memcpy(ptr,e->name,e->nleng);
	fwrite(uedgebuff,1,4+4+2+e->nleng,fd);
}

int fs_loadedge(FILE *fd) {
	uint8_t uedgebuff[4+4+2];
	const uint8_t *ptr;
	uint32_t parent_id;
	uint32_t child_id;
#ifdef EDGEHASH
	uint32_t hpos;
#endif
	fsedge *e;
	statsrecord sr;

	if (fread(uedgebuff,1,4+4+2,fd)!=4+4+2) {
		MFSLOG(LOG_ERR,"loading edge: read error: %m");
		return -1;
	}
	ptr = uedgebuff;
	parent_id = get32bit(&ptr);
	child_id = get32bit(&ptr);
	if (parent_id==0 && child_id==0) {	// last edge
		return 1;
	}
	e = malloc(sizeof(fsedge));
	if (e==NULL) {
		MFSLOG(LOG_ERR,"loading edge: edge alloc: out of memory");
		return -1;
	}
	e->nleng = get16bit(&ptr);
	e->name = malloc(e->nleng);
	if (e->name==NULL) {
		MFSLOG(LOG_ERR,"loading edge: name alloc: out of memory");
		free(e);
		return -1;
	}
	if (fread(e->name,1,e->nleng,fd)!=e->nleng) {
		MFSLOG(LOG_ERR,"loading edge: read error: %m");
		free(e->name);
		free(e);
		return -1;
	}
	e->child = fsnodes_id_to_node(child_id);
	if (e->child==NULL) {
		MFSLOG(LOG_ERR,"loading edge: %"PRIu32",%s->%"PRIu32" error: child not found",parent_id,fsnodes_escape_name(e->nleng,e->name),child_id);
		free(e->name);
		free(e);
		return -1;
	}
	if (parent_id==0) {
		if (e->child->type==TYPE_TRASH) {
			e->parent = NULL;
			e->nextchild = trash;
			if (e->nextchild) {
				e->nextchild->prevchild = &(e->nextchild);
			}
			trash = e;
			e->prevchild = &trash;
#ifdef EDGEHASH
			e->next = NULL;
			e->prev = NULL;
#endif
			trashspace += e->child->data.fdata.length;
			trashnodes++;
		} else if (e->child->type==TYPE_RESERVED) {
			e->parent = NULL;
			e->nextchild = reserved;
			if (e->nextchild) {
				e->nextchild->prevchild = &(e->nextchild);
			}
			reserved = e;
			e->prevchild = &reserved;
#ifdef EDGEHASH
			e->next = NULL;
			e->prev = NULL;
#endif
			reservedspace += e->child->data.fdata.length;
			reservednodes++;
		} else {
			MFSLOG(LOG_ERR,"loading edge: %"PRIu32",%s->%"PRIu32" error: bad child type (%c)",parent_id,fsnodes_escape_name(e->nleng,e->name),child_id,e->child->type);
			free(e->name);
			free(e);
			return -1;
		}
	} else {
		e->parent = fsnodes_id_to_node(parent_id);
		if (e->parent==NULL) {
			MFSLOG(LOG_ERR,"loading edge: %"PRIu32",%s->%"PRIu32" error: parent not found",parent_id,fsnodes_escape_name(e->nleng,e->name),child_id);
			free(e->name);
			free(e);
			return -1;
		}
		if (e->parent->type!=TYPE_DIRECTORY) {
			MFSLOG(LOG_ERR,"loading edge: %"PRIu32",%s->%"PRIu32" error: bad parent type (%c)",parent_id,fsnodes_escape_name(e->nleng,e->name),child_id,e->parent->type);
			free(e->name);
			free(e);
			return -1;
		}
		e->nextchild = e->parent->data.ddata.children;
		if (e->nextchild) {
			e->nextchild->prevchild = &(e->nextchild);
		}
		e->parent->data.ddata.children = e;
		e->prevchild = &(e->parent->data.ddata.children);
		e->parent->data.ddata.elements++;
		if (e->child->type==TYPE_DIRECTORY) {
			e->parent->data.ddata.nlink++;
		}
#ifdef EDGEHASH
		hpos = EDGEHASHPOS(fsnodes_hash(e->parent->id,e->nleng,e->name));
		e->next = edgehash[hpos];
		if (e->next) {
			e->next->prev = &(e->next);
		}
		edgehash[hpos] = e;
		e->prev = &(edgehash[hpos]);
#endif
	}
	e->nextparent = e->child->parents;
	if (e->nextparent) {
		e->nextparent->prevparent = &(e->nextparent);
	}
	e->child->parents = e;
	e->prevparent = &(e->child->parents);
	if (e->parent) {
		fsnodes_get_stats(e->child,&sr);
		fsnodes_add_stats(e->parent,&sr);
	}
	return 0;
}

void fs_storenode(fsnode *f,FILE *fd) {
	uint8_t unodebuff[1+4+1+2+4+4+4+4+4+4+8+4+2+8*MAX_CHUNKS_PER_FILE+4*65536+4];
	uint8_t *ptr;
	uint32_t indx,ch,sessionids;
	sessionidrec *sessionidptr;

	if (f==NULL) {	// last node
		fputc(0,fd);
		return;
	}
	ptr = unodebuff;
	put8bit(&ptr,f->type);
	put32bit(&ptr,f->id);
	put8bit(&ptr,f->goal);
	put16bit(&ptr,f->mode);
	put32bit(&ptr,f->uid);
	put32bit(&ptr,f->gid);
	put32bit(&ptr,f->atime);
	put32bit(&ptr,f->mtime);
	put32bit(&ptr,f->ctime);
	put32bit(&ptr,f->trashtime);
	switch (f->type) {
	case TYPE_DIRECTORY:
	case TYPE_SOCKET:
	case TYPE_FIFO:
		fwrite(unodebuff,1,1+4+1+2+4+4+4+4+4+4,fd);
		break;
	case TYPE_BLOCKDEV:
	case TYPE_CHARDEV:
		put32bit(&ptr,f->data.rdev);
		fwrite(unodebuff,1,1+4+1+2+4+4+4+4+4+4+4,fd);
		break;
	case TYPE_SYMLINK:
		put32bit(&ptr,f->data.sdata.pleng);
		fwrite(unodebuff,1,1+4+1+2+4+4+4+4+4+4+4,fd);
		fwrite(f->data.sdata.path,1,f->data.sdata.pleng,fd);
		break;
	case TYPE_FILE:
	case TYPE_TRASH:
	case TYPE_RESERVED:
		put64bit(&ptr,f->data.fdata.length);
		ch = 0;
		for (indx=0 ; indx<f->data.fdata.chunks ; indx++) {
			if (f->data.fdata.chunktab[indx]!=0) {
				ch=indx+1;
			}
		}
		put32bit(&ptr,ch);
		sessionids=0;
		for (sessionidptr=f->data.fdata.sessionids ; sessionidptr && sessionids<65535; sessionidptr=sessionidptr->next) {
			sessionids++;
		}
		put16bit(&ptr,sessionids);

		for (indx=0 ; indx<ch ; indx++) {
			put64bit(&ptr,f->data.fdata.chunktab[indx]);
		}

		sessionids=0;
		for (sessionidptr=f->data.fdata.sessionids ; sessionidptr && sessionids<65535; sessionidptr=sessionidptr->next) {
			put32bit(&ptr,sessionidptr->sessionid);
			sessionids++;
		}

		fwrite(unodebuff,1,1+4+1+2+4+4+4+4+4+4+8+4+2+8*ch+4*sessionids,fd);
	}
}

int fs_loadnode(FILE *fd) {
	uint8_t unodebuff[4+1+2+4+4+4+4+4+4+8+4+2+8*MAX_CHUNKS_PER_FILE+4*65536+4];
	const uint8_t *ptr;
	uint8_t type;
	uint32_t indx,pleng,ch,sessionids,sessionid;
	fsnode *p;
	sessionidrec *sessionidptr;
	uint32_t nodepos;
	statsrecord *sr;

	type = fgetc(fd);
	if (type==0) {	// last node
		return 1;
	}
	p = malloc(sizeof(fsnode));
	if (p==NULL) {
		MFSLOG(LOG_ERR,"loading node: node alloc: out of memory");
		return -1;
	}
	p->type = type;
	switch (type) {
	case TYPE_DIRECTORY:
	case TYPE_FIFO:
	case TYPE_SOCKET:
		if (fread(unodebuff,1,4+1+2+4+4+4+4+4+4,fd)!=4+1+2+4+4+4+4+4+4) {
			MFSLOG(LOG_ERR,"loading node: read error: %m");
			free(p);
			return -1;
		}
		break;
	case TYPE_BLOCKDEV:
	case TYPE_CHARDEV:
	case TYPE_SYMLINK:
		if (fread(unodebuff,1,4+1+2+4+4+4+4+4+4+4,fd)!=4+1+2+4+4+4+4+4+4+4) {
			MFSLOG(LOG_ERR,"loading node: read error: %m");
			free(p);
			return -1;
		}
		break;
	case TYPE_FILE:
	case TYPE_TRASH:
	case TYPE_RESERVED:
		if (fread(unodebuff,1,4+1+2+4+4+4+4+4+4+8+4+2,fd)!=4+1+2+4+4+4+4+4+4+8+4+2) {
			MFSLOG(LOG_ERR,"loading node: read error: %m");
			free(p);
			return -1;
		}
		break;
	default:
		MFSLOG(LOG_ERR,"loading node: unrecognized node type: %c",type);
		free(p);
		return -1;
	}
	ptr = unodebuff;
	p->id = get32bit(&ptr);
	p->goal = get8bit(&ptr);
	p->mode = get16bit(&ptr);
	p->uid = get32bit(&ptr);
	p->gid = get32bit(&ptr);
	p->atime = get32bit(&ptr);
	p->mtime = get32bit(&ptr);
	p->ctime = get32bit(&ptr);
	p->trashtime = get32bit(&ptr);
	switch (type) {
	case TYPE_DIRECTORY:
		sr = malloc(sizeof(statsrecord));
		memset(sr,0,sizeof(statsrecord));
		p->data.ddata.stats = sr;
		p->data.ddata.quota = NULL;
		p->data.ddata.children = NULL;
		p->data.ddata.nlink = 2;
		p->data.ddata.elements = 0;
	case TYPE_SOCKET:
	case TYPE_FIFO:
		break;
	case TYPE_BLOCKDEV:
	case TYPE_CHARDEV:
		p->data.rdev = get32bit(&ptr);
		break;
	case TYPE_SYMLINK:
		pleng = get32bit(&ptr);
		p->data.sdata.pleng = pleng;
		if (pleng>0) {
			p->data.sdata.path = malloc(pleng);
			if (p->data.sdata.path==NULL) {
				MFSLOG(LOG_ERR,"loading node: path alloc: out of memory");
				free(p);
				return -1;
			}
			if (fread(p->data.sdata.path,1,pleng,fd)!=pleng) {
				MFSLOG(LOG_ERR,"loading node: read error: %m");
				free(p->data.sdata.path);
				free(p);
				return -1;
			}
		} else {
			p->data.sdata.path = NULL;
		}
		break;
	case TYPE_FILE:
	case TYPE_TRASH:
	case TYPE_RESERVED:
		p->data.fdata.length = get64bit(&ptr);
		ch = get32bit(&ptr);
		p->data.fdata.chunks = ch;
		sessionids = get16bit(&ptr);
		if (ch>0) {
			p->data.fdata.chunktab = malloc(sizeof(uint64_t)*ch);
			if (p->data.fdata.chunktab==NULL) {
				MFSLOG(LOG_ERR,"loading node: chunktab alloc: out of memory");
				free(p);
				return -1;
			}
		} else {
			p->data.fdata.chunktab = NULL;
		}
		if (fread((uint8_t*)ptr,1,8*ch+4*sessionids,fd)!=8*ch+4*sessionids) {
			MFSLOG(LOG_ERR,"loading node: read error: %m");
			if (p->data.fdata.chunktab) {
				free(p->data.fdata.chunktab);
			}
			free(p);
			return -1;
		}
		for (indx=0 ; indx<ch ; indx++) {
			p->data.fdata.chunktab[indx] = get64bit(&ptr);;
		}
		p->data.fdata.sessionids=NULL;
		while (sessionids) {
			sessionid = get32bit(&ptr);
			sessionidptr = sessionidrec_malloc();
			if (sessionidptr==NULL) {
				MFSLOG(LOG_ERR,"loading node: sessionidrec alloc: out of memory");
				if (p->data.fdata.chunktab) {
					free(p->data.fdata.chunktab);
				}
				free(p);
				return -1;
			}
			sessionidptr->sessionid = sessionid;
			sessionidptr->next = p->data.fdata.sessionids;
			p->data.fdata.sessionids = sessionidptr;
			matocuserv_init_sessions(sessionid,p->id);
			sessionids--;
		}
	}
	p->parents = NULL;
	nodepos = NODEHASHPOS(p->id);
	p->next = nodehash[nodepos];
	nodehash[nodepos] = p;
	fsnodes_used_inode(p->id);
	nodes++;
	if (type==TYPE_DIRECTORY) {
		dirnodes++;
	}
	if (type==TYPE_FILE || type==TYPE_TRASH || type==TYPE_RESERVED) {
		filenodes++;
	}
	return 0;
}

void fs_storenodes(FILE *fd) {
	uint32_t i;
	fsnode *p;
	for (i=0 ; i<NODEHASHSIZE ; i++) {
		for (p=nodehash[i] ; p ; p=p->next) {
			fs_storenode(p,fd);
		}
	}
	fs_storenode(NULL,fd);	// end marker
}

void fs_storeedgelist(fsedge *e,FILE *fd) {
	while (e) {
		fs_storeedge(e,fd);
		e=e->nextchild;
	}
}

void fs_storeedges_rec(fsnode *f,FILE *fd) {
	fsedge *e;
	fs_storeedgelist(f->data.ddata.children,fd);
	for (e=f->data.ddata.children ; e ; e=e->nextchild) {
		if (e->child->type==TYPE_DIRECTORY) {
			fs_storeedges_rec(e->child,fd);
		}
	}
}

void fs_storeedges(FILE *fd) {
	fs_storeedges_rec(root,fd);
	fs_storeedgelist(trash,fd);
	fs_storeedgelist(reserved,fd);
	fs_storeedge(NULL,fd);	// end marker
}

int fs_lostnode(fsnode *p) {
	uint8_t artname[40];
	uint32_t i,l;
	i=0;
	do {
		if (i==0) {
			l = snprintf((char*)artname,40,"lost_node_%"PRIu32,p->id);
		} else {
			l = snprintf((char*)artname,40,"lost_node_%"PRIu32".%"PRIu32,p->id,i);
		}
		if (!fsnodes_nameisused(root,l,artname)) {
			fsnodes_link(0,root,p,l,artname);
			return 1;
		}
		i++;
	} while (i);
	return -1;
}

int fs_checknodes() {
	uint32_t i;
	fsnode *p;
	for (i=0 ; i<NODEHASHSIZE ; i++) {
		for (p=nodehash[i] ; p ; p=p->next) {
			if (p->parents==NULL && p!=root) {
				MFSLOG(LOG_ERR,"fschk: found lost inode: %"PRIu32,p->id);
				if (fs_lostnode(p)<0) {
					return -1;
				}
			}
		}
	}
	return 1;
}

int fs_loadnodes(FILE *fd) {
	int s;
	do {
		s = fs_loadnode(fd);
		if (s<0) {
			return -1;
		}
	} while (s==0);
	return 0;
}

int fs_loadedges(FILE *fd) {
	int s;
	do {
		s = fs_loadedge(fd);
		if (s<0) {
			return -1;
		}
	} while (s==0);
	return 0;
}

void fs_storefree(FILE *fd) {
	uint8_t wbuff[8*1024],*ptr;
	freenode *n;
	uint32_t l;
	l=0;
	for (n=freelist ; n ; n=n->next) {
		l++;
	}
	ptr = wbuff;
	put32bit(&ptr,l);
	fwrite(wbuff,1,4,fd);
	l=0;
	ptr=wbuff;
	for (n=freelist ; n ; n=n->next) {
		if (l==1024) {
			fwrite(wbuff,1,8*1024,fd);
			l=0;
			ptr=wbuff;
		}
		put32bit(&ptr,n->id);
		put32bit(&ptr,n->ftime);
		l++;
	}
	if (l>0) {
		fwrite(wbuff,1,8*l,fd);
	}
}

int fs_loadfree(FILE *fd) {
	uint8_t rbuff[8*1024];
	const uint8_t *ptr;
	freenode *n;
	uint32_t l,t;
	if (fread(rbuff,1,4,fd)!=4) {
		return -1;
	}
	ptr=rbuff;
	t = get32bit(&ptr);
	freelist = NULL;
	freetail = &(freelist);
	l=0;
	while (t>0) {
		if (l==0) {
			if (t>1024) {
				if (fread(rbuff,1,8*1024,fd)!=8*1024) {
					return -1;
				}
				l=1024;
			} else {
				if (fread(rbuff,1,8*t,fd)!=8*t) {
					return -1;
				}
				l=t;
			}
			ptr = rbuff;
		}
		n = freenode_malloc();
		if (n==NULL) {
			return -1;
		}
		n->id = get32bit(&ptr);
		n->ftime = get32bit(&ptr);
		n->next = NULL;
		*freetail = n;
		freetail = &(n->next);
		fsnodes_used_inode(n->id);
		l--;
		t--;
	}
	return 0;
}

void fs_store(FILE *fd) {
	uint8_t hdr[16];
	uint8_t *ptr;
	ptr = hdr;
	put32bit(&ptr,maxnodeid);
	put64bit(&ptr,version);
	put32bit(&ptr,nextsessionid);
	fwrite(hdr,1,16,fd);
	fs_storenodes(fd);
	fs_storeedges(fd);
	fs_storefree(fd);
}

uint64_t fs_loadversion(FILE *fd) {
	uint8_t hdr[12];
	const uint8_t *ptr;
	uint64_t fversion;

	if (fread(hdr,1,12,fd)!=12) {
		return 0;
	}
	ptr = hdr+4;
//	maxnodeid = get32bit(&ptr);
	fversion = get64bit(&ptr);
//	nextsessionid = get32bit(&ptr);
	return fversion;
}

int fs_load(FILE *fd) {
	uint8_t hdr[16];
	const uint8_t *ptr;
	uint64_t loadversion;
	//uint32_t loadsessionid;	
	
	if (fread(hdr,1,16,fd)!=16) {
		MFSLOG(LOG_NOTICE,"error loading header");
		return -1;
	}
	ptr = hdr;
	maxnodeid = get32bit(&ptr);

	/*
	  * as when we load meta, the master generate changelog which version is bigger than 
	  * version in meta file, we should skip load the version in this situation in case version
	  * confict
	  * Dongyang Zhang
	  */
	loadversion = get64bit(&ptr);
	if(loadversion > version) {
		version = loadversion;
	} else {
		MFSLOG(LOG_NOTICE, "loadversion :%lu is smaller than version:%lu skip it\n",
			loadversion, version);
	}

	/**
	 *  as the shadow master has it's own session different witch master, we should not load
	 *  the metadata file
	 * 
	 *  Dongyang Zhang
	 */
	/*
		loadsessionid = get32bit(&ptr);
		MFSLOG(LOG_NOTICE, "loadsessionid:%u nextsessionid:%u\n", loadsessionid, nextsessionid);
		if(loadsessionid > nextsessionid) {
			nextsessionid = loadsessionid;
	}
	*/
	
	fsnodes_init_freebitmask();
	MFSLOG(LOG_NOTICE,"loading objects (files,directories,etc.) ... ");
	if (fs_loadnodes(fd)<0) {
		MFSLOG(LOG_ERR,"error reading metadata (node)");
		return -1;
	}
	MFSLOG(LOG_NOTICE,"ok");
	MFSLOG(LOG_NOTICE,"loading names ... ");
	if (fs_loadedges(fd)<0) {
		MFSLOG(LOG_ERR,"error reading metadata (edge)");
		return -1;
	}
	MFSLOG(LOG_NOTICE,"ok");
	MFSLOG(LOG_NOTICE,"loading deletion timestamps ... ");
	if (fs_loadfree(fd)<0) {
		MFSLOG(LOG_ERR,"error reading metadata (free)");
		return -1;
	}
	MFSLOG(LOG_NOTICE,"ok");
	MFSLOG(LOG_NOTICE,"checking filesystem consistency ... ");
	root = fsnodes_id_to_node(MFS_ROOT_ID);
	if (root==NULL) {
		MFSLOG(LOG_ERR,"error reading metadata (no root)");
		return -1;
	}
	if (fs_checknodes()<0) {
		MFSLOG(LOG_NOTICE,"error");
		return -1;
	}
	MFSLOG(LOG_NOTICE,"ok");
	return 0;
}

void fs_new(void) {
	uint32_t nodepos;
	statsrecord *sr;
	maxnodeid = MFS_ROOT_ID;
	version = 0;
	nextsessionid = 1;
	fsnodes_init_freebitmask();
	root = malloc(sizeof(fsnode));
	root->id = MFS_ROOT_ID;
	root->type = TYPE_DIRECTORY;
	root->ctime = root->mtime = root->atime = get_current_time();
	root->goal = DEFAULT_GOAL;
	root->trashtime = DEFAULT_TRASHTIME;
	root->mode = 0777;
	root->uid = 0;
	root->gid = 0;
	sr = malloc(sizeof(statsrecord));
	memset(sr,0,sizeof(statsrecord));
	root->data.ddata.stats = sr;
	root->data.ddata.quota = NULL;
	root->data.ddata.children = NULL;
	root->data.ddata.elements = 0;
	root->data.ddata.nlink = 2;
	root->parents = NULL;
	nodepos = NODEHASHPOS(root->id);
	root->next = nodehash[nodepos];
	nodehash[nodepos] = root;
	fsnodes_used_inode(root->id);
	chunk_newfs();
	nodes=1;
	dirnodes=1;
	filenodes=0;
}

int fs_emergency_storeall(const char *fname) {
	FILE *fd;
	fd = fopen(fname,"w");
	if (fd==NULL) {
		return -1;
	}
	fwrite("MFSM 1.5",1,8,fd);
	fs_store(fd);
	chunk_store(fd);
	if (ferror(fd)!=0) {
		fclose(fd);
		return -1;
	}
	fclose(fd);
	MFSLOG(LOG_WARNING,"metadata were stored to emergency file: %s - please copy this file to your default location as 'metadata.mfs'",fname);
	return 0;
}

int fs_emergency_saves() {
#if defined(HAVE_PWD_H) && defined(HAVE_GETPWUID)
	struct passwd *p;
#endif
	if (fs_emergency_storeall("metadata.mfs.emergency")==0) {
		return 0;
	}
#if defined(HAVE_PWD_H) && defined(HAVE_GETPWUID)
	p = getpwuid(getuid());
	if (p) {
		char *fname;
		int l;
		l = strlen(p->pw_dir);
		fname = malloc(l+24);
		if (fname) {
			memcpy(fname,p->pw_dir,l);
			fname[l]='/';
			memcpy(fname+l+1,"metadata.mfs.emergency",22);
			fname[l+23]=0;
			if (fs_emergency_storeall(fname)==0) {
				free(fname);
				return 0;
			}
			free(fname);
		}
	}
#endif
	if (fs_emergency_storeall("/metadata.mfs.emergency")==0) {
		return 0;
	}
	if (fs_emergency_storeall("/tmp/metadata.mfs.emergency")==0) {
		return 0;
	}
	if (fs_emergency_storeall("/var/metadata.mfs.emergency")==0) {
		return 0;
	}
	if (fs_emergency_storeall("/usr/metadata.mfs.emergency")==0) {
		return 0;
	}
	if (fs_emergency_storeall("/usr/share/metadata.mfs.emergency")==0) {
		return 0;
	}
	if (fs_emergency_storeall("/usr/local/metadata.mfs.emergency")==0) {
		return 0;
	}
	if (fs_emergency_storeall("/usr/local/var/metadata.mfs.emergency")==0) {
		return 0;
	}
	if (fs_emergency_storeall("/usr/local/share/metadata.mfs.emergency")==0) {
		return 0;
	}
	return -1;
}

int fs_storeall(int bg) {
	FILE *fd;

#ifdef BACKGROUND_METASTORE
	int i;
	struct stat sb;
	if (stat("metadata.mfs.back.tmp",&sb)==0) {
		return -1;
	}
#else
	(void)bg;
#endif
//	changelog_rotate();
#ifdef BACKGROUND_METASTORE
	if (bg) {
		i = fork();
	} else {
		i = -1;
	}
	// if fork returned -1 (fork error) store metadata in foreground !!!
	if (i<=0) {
	       if(i < 0) {
		   	MFSLOG(LOG_NOTICE, "i:%d errrno:%d\n", i,  errno);
	       }
#endif
			
		if (rename("metadata.mfs.back","metadata.mfs.back.tmp")<0) {
			if (errno!=ENOENT) {
				MFSLOG(LOG_ERR,"can't rename metadata.mfs.back -> metadata.mfs.back.tmp (%m)");
#ifdef BACKGROUND_METASTORE
				if (i==0) {
					exit(0);
				}
#endif
				return 0;
			}
		}
		fd = fopen("metadata.mfs.back","w");
		if (fd==NULL) {
			MFSLOG(LOG_ERR,"can't open metadata file");
#ifdef BACKGROUND_METASTORE
			if (i==0) {
				exit(0);
			}
#endif
			return 0;
		}
		fwrite("MFSM 1.5",1,8,fd);
		fs_store(fd);
		chunk_store(fd);
		if (ferror(fd)!=0) {
			MFSLOG(LOG_ERR,"can't write metadata");
		}
		fclose(fd);
		unlink("metadata.mfs.back.tmp");
		unlink("metadata.mfs");
#ifdef BACKGROUND_METASTORE
		if (i==0) {
			exit(0);
		}
	}
#endif
	
	return 1;
}

void fs_dostoreall(void) {
        fs_storeall(1); // ignore error
}

void fs_waitchild(void) {
        int statloc;

        /* wait the fs_storeall child to exit */
        wait3(&statloc, WNOHANG, NULL);
}    

//void fs_term(void) {
//        int u;
//        for (u=0 ; u<3 ; u++) {
//                if (fs_storeall(0)==1) {
//                        if (rename("metadata.mfs.back","metadata.mfs")<0) {
//                                syslog(LOG_WARNING,"can't rename metadata.mfs.back -> metadata.mfs (%m)");
//                        }
//                        return ;
//                }
//                sleep(5);
//        }
//        while (fs_emergency_saves()<0) {
//                syslog(LOG_ERR,"can't store metadata - try to make more space on your hdd or change privieleges");
//                sleep(10);
//        }
//}

int fs_loadall() {
	FILE *fd;
	uint8_t hdr[8];
	uint8_t bhdr[8];
	uint64_t backversion;
	int converted=0;

	backversion = 0;
	fd = fopen("metadata.mfs.back","r");
	if (fd!=NULL) {
		if (fread(bhdr,1,8,fd)==8) {
//			if (memcmp(bhdr,"MFSM 1.4",8)==0) {
//				backversion = fs_loadversion_1_4(fd);
//			} else
			if (memcmp(bhdr,"MFSM 1.5",8)==0) {
				backversion = fs_loadversion(fd);
			}
		}
		fclose(fd);
	}

	fd = fopen("metadata.mfs","r");
	if (fd==NULL) {
		MFSLOG(LOG_NOTICE,"can't open metadata file\n");
		{
#if defined(HAVE_GETCWD)
#ifndef PATH_MAX
#define PATH_MAX 10000
#endif
			char cwdbuf[PATH_MAX+1];
			int cwdlen;
			if (getcwd(cwdbuf,PATH_MAX)==NULL) {
				cwdbuf[0]=0;
			} else {
				cwdlen = strlen(cwdbuf);
				if (cwdlen>0 && cwdlen<PATH_MAX-1 && cwdbuf[cwdlen-1]!='/') {
					cwdbuf[cwdlen]='/';
					cwdbuf[cwdlen+1]=0;
				} else {
					cwdbuf[0]=0;
				}
			}

#else
			char cwdbuf[1];
			cwdbuf[0]=0;
#endif
			if (cwdbuf[0]) {
				MFSLOG(LOG_NOTICE,"if this is new instalation then rename %smetadata.mfs.empty as %smetadata.mfs",cwdbuf,cwdbuf);
			} else {
				MFSLOG(LOG_NOTICE,"if this is new instalation then rename metadata.mfs.empty as metadata.mfs (in current working directory)");
			}
		}
		MFSLOG(LOG_ERR,"can't open metadata file");
		return -1;
	}
	if (fread(hdr,1,8,fd)!=8) {
		MFSLOG(LOG_ERR,"can't read metadata header");
		return -1;
	}
	if (memcmp(hdr,"MFSM NEW",8)==0) {	// special case - create new file system
		fclose(fd);
		if (backversion>0) {
			MFSLOG(LOG_ERR,"backup file is newer than current file - please check it manually - propably you should run metarestore");
			return -1;
		}
		if (rename("metadata.mfs","metadata.mfs.back")<0) {
			MFSLOG(LOG_ERR,"can't rename metadata.mfs -> metadata.mfs.back (%m)");
			return -1;
		}
		MFSLOG(LOG_NOTICE,"create new empty filesystem");
		fs_new();
		fs_storeall(0);	// after creating new filesystem always create "back" file for using in metarestore
		return 0;
	}
/*
	if (memcmp(hdr,"MFSM 1.4",8)==0) {
		converted=1;
		if (fs_load_1_4(fd)<0) {
			fprintf(msgfd,"error reading metadata (structure)\n");
			syslog(LOG_ERR,"error reading metadata (structure)");
			fclose(fd);
			return -1;
		}
		if (chunk_load(fd)<0) {
			fprintf(msgfd,"error reading metadata (chunks)\n");
			syslog(LOG_ERR,"error reading metadata (chunks)");
			fclose(fd);
			return -1;
		}
	} else
*/
	if (memcmp(hdr,"MFSM 1.5",8)==0) {
		if (fs_load(fd)<0) {
			MFSLOG(LOG_ERR,"error reading metadata (structure)");
			fclose(fd);
			return -1;
		}
		if (chunk_load(fd)<0) {
			MFSLOG(LOG_ERR,"error reading metadata (chunks)");
			fclose(fd);
			return -1;
		}
	} else {
		MFSLOG(LOG_ERR,"wrong metadata header");
		fclose(fd);
		return -1;
	}
	if (ferror(fd)!=0) {
		MFSLOG(LOG_ERR,"error reading metadata");
		fclose(fd);
		return -1;
	}
	fclose(fd);
	if (backversion>version) {
		MFSLOG(LOG_ERR,"backup file is newer than current file - please check it manually - propably you should run metarestore");
		return -1;
	}
	if (converted==1) {
		if (rename("metadata.mfs","metadata.mfs.back.1.4")<0) {
			MFSLOG(LOG_ERR,"can't rename metadata.mfs -> metadata.mfs.back.1.4 (%m)");
			return -1;
		}
		fs_storeall(0);	// after conversion always create new version of "back" file for using in proper version of metarestore
//	} else if (converted==2) {
//		rename("metadata.mfs","metadata.mfs.back.1.3");
//		fs_storeall(0);	// after conversion always create new version of "back" file for using in proper version of metarestore
	} else {
		if (rename("metadata.mfs","metadata.mfs.back")<0) {
			MFSLOG(LOG_ERR,"can't rename metadata.mfs -> metadata.mfs.back (%m)");
			return -1;
		}
	}
	fs_add_files_to_chunks();
	MFSLOG(LOG_NOTICE,"ok");
	MFSLOG(LOG_NOTICE,"all inodes: %"PRIu32"",nodes);
	MFSLOG(LOG_NOTICE,"directory inodes: %"PRIu32"",dirnodes);
	MFSLOG(LOG_NOTICE,"file inodes: %"PRIu32"",filenodes);
	MFSLOG(LOG_NOTICE,"chunks: %"PRIu32"",chunk_count());
	return 0;
}

void fs_strinit(void) {
	uint32_t i;
	root = NULL;
	trash = NULL;
	reserved = NULL;
	trashspace = 0;
	reservedspace = 0;
	trashnodes = 0;
	reservednodes = 0;
	quotahead = NULL;
	for (i=0 ; i<NODEHASHSIZE ; i++) {
		nodehash[i]=NULL;
	}
#ifdef EDGEHASH
	for (i=0 ; i<EDGEHASHSIZE ; i++) {
		edgehash[i]=NULL;
	}
#endif
}

int fs_init() {
	LOG_COUNT = cfg_getuint32("LOG_PRINT_FREQUENCY",1000);
	fprintf(msgfd,"the log print frequency is %d\n",LOG_COUNT);
	fprintf(msgfd,"loading metadata ...\n");
//	fs_strinit();
//	chunk_strinit();
//	starttime = get_current_time();
//	if (fs_loadall()<0) {
//		return -1;
//	}
//	fprintf(msgfd,"metadata file has been loaded\n");
#if VERSMID==7
#warning uncomment quota time limit
#endif
//	config_getuint32("QUOTA_TIME_LIMIT",7*86400,&QuotaTimeLimit);
	QuotaTimeLimit = 7*86400;	// for tests
//	main_timeregister(TIMEMODE_RUNONCE,1,0,fs_test_files);
	main_timeregister(TIMEMODE_RUNONCE,3600,0,fs_dostoreall);
       main_timeregister(TIMEMODE_RUNONCE, 60, 0, fs_waitchild);  
	main_timeregister(TIMEMODE_RUNONCE,60,0,log_print_control_fs);
	//main_destructregister(fs_term);
	return 0;
}

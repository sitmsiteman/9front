#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <bio.h>
#include <ctype.h>
#include <ndb.h>
#include <ip.h>

enum
{
	Nreply=			20,
	Maxreply=		256,
	Maxrequest=		128,
	Maxpath=		128,
	Maxfdata=		8192,
	Maxhost=		64,		/* maximum host name size */
	Maxservice=		64,		/* maximum service name size */
	Maxactive=		200,		/* maximum number of active slave procs */

	Qdir=			0,
	Qcs=			1,
};

typedef struct Mfile	Mfile;
typedef struct Mlist	Mlist;
typedef struct Network	Network;
typedef struct Flushreq	Flushreq;
typedef struct Job	Job;

int vers;		/* incremented each clone/attach */

struct Mfile
{
	int		busy;	/* fid in use */
	int		ref;	/* cleanup when drops to zero */

	char		*user;
	Qid		qid;
	int		fid;

	/*
	 *  current request
	 */
	char		*net;
	char		*host;
	char		*serv;
	char		*rem;

	/*
	 *  result of the last lookup
	 */
	Network		*nextnet;
	int		nreply;
	char		*reply[Nreply];
	int		replylen[Nreply];
};

struct Mlist
{
	Mlist	*next;
	Mfile	mf;
};


/*
 *  active requests
 */
struct Job
{
	Job	*next;
	int	flushed;
	Fcall	request;
	Fcall	reply;
};
QLock	joblock;
Job	*joblist;

Mlist	*mlist;
int	mfd[2];
int	debug;

jmp_buf	masterjmp;	/* return through here after a slave process has been created */
int	*isslave;	/* *isslave non-zero means this is a slave process */
long	active;		/* number of active slaves */
char	*dbfile;
Ndb	*db, *netdb;
char	*csuser;

void	rversion(Job*);
void	rflush(Job*);
void	rattach(Job*, Mfile*);
char*	rwalk(Job*, Mfile*);
void	ropen(Job*, Mfile*);
void	rcreate(Job*, Mfile*);
void	rread(Job*, Mfile*);
void	rwrite(Job*, Mfile*);
void	rclunk(Job*, Mfile*);
void	rremove(Job*, Mfile*);
void	rstat(Job*, Mfile*);
void	rwstat(Job*, Mfile*);
void	rauth(Job*);
void	sendmsg(Job*, char*);
void	error(char*);
void	mountinit(char*, char*);
void	io(void);
void	ndbinit(void);
void	netinit(int);
void	netadd(char*);
char	*genquery(Mfile*, char*);
char*	ipinfoquery(Mfile*, char**, int);
int	needproto(Network*, Ndbtuple*);
int	lookup(Mfile*);
void	ipid(void);
void	readipinterfaces(void);
void*	emalloc(int);
char*	estrdup(char*);
Job*	newjob(void);
void	freejob(Job*);
void	setext(char*, int, char*);
void	cleanmf(Mfile*);

QLock	dblock;		/* mutex on database operations */
QLock	netlock;	/* mutex for netinit() */

char	*logfile = "cs";

char	mntpt[Maxpath];
char	netndb[Maxpath];

/*
 *  Network specific translators
 */
Ndbtuple*	iplookup(Network*, char*, char*);
char*		iptrans(Ndbtuple*, Network*, char*, char*, int);
Ndbtuple*	telcolookup(Network*, char*, char*);
char*		telcotrans(Ndbtuple*, Network*, char*, char*, int);

Ndbtuple*	dnsiplookup(char*, Ndbs*, int);
Ndbtuple*	myipinfo(Ndb *db, char **list, int n);

struct Network
{
	char		*net;
	Ndbtuple	*(*lookup)(Network*, char*, char*);
	char		*(*trans)(Ndbtuple*, Network*, char*, char*, int);

	char		considered;		/* flag: ignored for "net!"? */
	char		fasttimeout;		/* flag. was for IL */
	char		ipvers;			/* flag: V4, V6 */

	Network		*next;
};

enum {
	Ntcp = 1,

	V4 = 1,
	V6 = 2,
};

/*
 *  net doesn't apply to (r)udp, icmp(v6), or telco (for speed).
 */
Network network[] = {
	{ "il",		iplookup,	iptrans,	0, 1, V4,	},
	{ "tcp",	iplookup,	iptrans,	0, 0, V4|V6,	},
	{ "il",		iplookup,	iptrans,	0, 0, V4,	},
	{ "udp",	iplookup,	iptrans,	1, 0, V4|V6,	},
	{ "icmp",	iplookup,	iptrans,	1, 0, V4,	},
	{ "icmpv6",	iplookup,	iptrans,	1, 0, V6,	},
	{ "rudp",	iplookup,	iptrans,	1, 0, V4,	},
	{ "ssh",	iplookup,	iptrans,	1, 0, V4|V6,	},
	{ "telco",	telcolookup,	telcotrans,	1, 0, 0,	},
	{ 0 },
};

QLock ipifclock;
Ipifc *ipifcs;
int confipvers;
int dnsipvers;
int lookipvers = V4|V6;

char *mysysname;

Network *netlist;		/* networks ordered by preference */
Network *last;

#pragma varargck type "$" char*
#pragma varargck type "N" Ndbtuple*

static int
ndblinefmt(Fmt *f)
{
	Ndbtuple *t;

	for(t = va_arg(f->args, Ndbtuple*); t != nil; t = t->entry) {
		fmtprint(f, "%s=%$ ", t->attr, t->val);
		if(t->line != t->entry)
			break;
	}
	return 0;
}

static void
nstrcpy(char *to, char *from, int len)
{
	strncpy(to, from, len);
	to[len-1] = 0;
}

void
usage(void)
{
	fprint(2, "usage: %s [-46dn] [-f ndb-file] [-x netmtpt]\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	int justsetname;
	char ext[Maxpath], servefile[Maxpath];

	justsetname = 0;
	setnetmtpt(mntpt, sizeof(mntpt), nil);
	ext[0] = 0;
	ARGBEGIN{
	case '4':
		lookipvers = V4;
		break;
	case '6':
		lookipvers = V6;
		break;
	case 'd':
		debug = 1;
		break;
	case 'f':
		dbfile = EARGF(usage());
		break;
	case 'n':
		justsetname = 1;
		break;
	case 'x':
		setnetmtpt(mntpt, sizeof(mntpt), EARGF(usage()));
		setext(ext, sizeof(ext), mntpt);
		break;
	}ARGEND
	USED(argc);
	USED(argv);

	snprint(netndb, sizeof(netndb), "%s/ndb", mntpt);

	fmtinstall('E', eipfmt);
	fmtinstall('I', eipfmt);
	fmtinstall('M', eipfmt);
	fmtinstall('F', fcallfmt);

	fmtinstall('$', ndbvalfmt);
	fmtinstall('N', ndblinefmt);

	ndbinit();
	netinit(0);

	if(!justsetname){
		snprint(servefile, sizeof(servefile), "/srv/cs%s", ext);
		unmount(servefile, mntpt);
		remove(servefile);

		rfork(RFREND|RFNOTEG);
		csuser = estrdup(getuser());
		mountinit(servefile, mntpt);
		io();
	}
	exits(0);
}

/*
 *  if a mount point is specified, set the cs extention to be the mount point
 *  with '_'s replacing '/'s
 */
void
setext(char *ext, int n, char *p)
{
	int i, c;

	n--;
	for(i = 0; i < n; i++){
		c = p[i];
		if(c == 0)
			break;
		if(c == '/')
			c = '_';
		ext[i] = c;
	}
	ext[i] = 0;
}

void
mountinit(char *service, char *mntpt)
{
	int f;
	int p[2];
	char buf[32];

	if(pipe(p) < 0)
		error("pipe failed");

	/*
	 *  make a /srv/cs
	 */
	f = create(service, OWRITE|ORCLOSE, 0666);
	if(f < 0)
		error(service);
	snprint(buf, sizeof(buf), "%d", p[1]);
	if(write(f, buf, strlen(buf)) != strlen(buf))
		error("write /srv/cs");

	switch(rfork(RFFDG|RFPROC|RFNAMEG)){
	case 0:
		close(p[1]);
		procsetname("%s", mntpt);
		break;
	case -1:
		error("fork failed");
	default:
		/*
		 *  put ourselves into the file system
		 */
		close(p[0]);
		if(mount(p[1], -1, mntpt, MAFTER, "") == -1)
			error("mount failed");
		_exits(0);
	}
	mfd[0] = mfd[1] = p[0];
}

void
ndbinit(void)
{
	db = ndbopen(dbfile);
	if(db == nil)
		error("can't open network database");

	for(netdb = db; netdb != nil; netdb = netdb->next)
		if(strcmp(netdb->file, netndb) == 0)
			return;

	netdb = ndbopen(netndb);
	if(netdb != nil){
		netdb->nohash = 1;
		db = ndbcat(netdb, db);
	}
}

Mfile*
newfid(int fid)
{
	Mlist *f, *ff;
	Mfile *mf;

	ff = nil;
	for(f = mlist; f != nil; f = f->next)
		if(f->mf.busy && f->mf.fid == fid)
			return &f->mf;
		else if(ff == nil && !f->mf.busy && !f->mf.ref)
			ff = f;
	if(ff == nil){
		ff = emalloc(sizeof *f);
		ff->next = mlist;
		mlist = ff;
	}
	mf = &ff->mf;
	memset(mf, 0, sizeof *mf);
	mf->fid = fid;
	return mf;
}

Job*
newjob(void)
{
	Job *job;

	job = emalloc(sizeof *job);
	qlock(&joblock);
	job->next = joblist;
	joblist = job;
	job->request.tag = -1;
	qunlock(&joblock);
	return job;
}

void
freejob(Job *job)
{
	Job **l;

	qlock(&joblock);
	for(l = &joblist; *l != nil; l = &(*l)->next){
		if((*l) == job){
			*l = job->next;
			break;
		}
	}
	qunlock(&joblock);
	free(job);
}

void
flushjob(int tag)
{
	Job *job;

	qlock(&joblock);
	for(job = joblist; job != nil; job = job->next){
		if(job->request.tag == tag && job->request.type != Tflush){
			job->flushed = 1;
			break;
		}
	}
	qunlock(&joblock);
}

void
io(void)
{
	long n;
	Mfile *mf;
	int slaveflag;
	uchar mdata[IOHDRSZ + Maxfdata];
	Job *job;

	/*
	 *  if we ask dns to fulfill requests,
	 *  a slave process is created to wait for replies.  The
	 *  master process returns immediately via a longjmp
	 *  through 'masterjmp'.
	 *
	 *  *isslave is a pointer into the call stack to a variable
	 *  that tells whether or not the current process is a slave.
	 */
	slaveflag = 0;		/* init slave variable */
	isslave = &slaveflag;
	setjmp(masterjmp);

	for(;;){
		n = read9pmsg(mfd[0], mdata, sizeof mdata);
		if(n < 0)
			error("mount read");
		if(n == 0)
			break;
		job = newjob();
		if(convM2S(mdata, n, &job->request) != n){
			syslog(1, logfile, "format error %ux %ux %ux %ux %ux",
				mdata[0], mdata[1], mdata[2], mdata[3], mdata[4]);
			freejob(job);
			break;
		}
		qlock(&dblock);
		mf = newfid(job->request.fid);
		if(debug)
			syslog(0, logfile, "%F", &job->request);

		switch(job->request.type){
		default:
			syslog(1, logfile, "unknown request type %d", job->request.type);
			break;
		case Tversion:
			rversion(job);
			break;
		case Tauth:
			rauth(job);
			break;
		case Tflush:
			rflush(job);
			break;
		case Tattach:
			rattach(job, mf);
			break;
		case Twalk:
			rwalk(job, mf);
			break;
		case Topen:
			ropen(job, mf);
			break;
		case Tcreate:
			rcreate(job, mf);
			break;
		case Tread:
			rread(job, mf);
			break;
		case Twrite:
			rwrite(job, mf);
			break;
		case Tclunk:
			rclunk(job, mf);
			break;
		case Tremove:
			rremove(job, mf);
			break;
		case Tstat:
			rstat(job, mf);
			break;
		case Twstat:
			rwstat(job, mf);
			break;
		}
		qunlock(&dblock);

		freejob(job);

		/*
		 *  slave processes die after replying
		 */
		if(*isslave){
			if(debug)
				syslog(0, logfile, "slave death %d", getpid());
			adec(&active);
			_exits(0);
		}
	}
}

void
rversion(Job *job)
{
	if(job->request.msize > IOHDRSZ + Maxfdata)
		job->reply.msize = IOHDRSZ + Maxfdata;
	else
		job->reply.msize = job->request.msize;
	job->reply.version = "9P2000";
	if(strncmp(job->request.version, "9P", 2) != 0)
		job->reply.version = "unknown";
	sendmsg(job, nil);
}

void
rauth(Job *job)
{
	sendmsg(job, "cs: authentication not required");
}

/*
 *  don't flush till all the slaves are done
 */
void
rflush(Job *job)
{
	flushjob(job->request.oldtag);
	sendmsg(job, nil);
}

void
rattach(Job *job, Mfile *mf)
{
	if(mf->busy == 0){
		mf->busy = 1;
		mf->user = estrdup(job->request.uname);
	}
	mf->qid.vers = vers++;
	mf->qid.type = QTDIR;
	mf->qid.path = 0LL;
	job->reply.qid = mf->qid;
	sendmsg(job, nil);
}


char*
rwalk(Job *job, Mfile *mf)
{
	char *err;
	char **elems;
	int nelems;
	int i;
	Mfile *nmf;
	Qid qid;

	err = nil;
	nmf = nil;
	elems = job->request.wname;
	nelems = job->request.nwname;
	job->reply.nwqid = 0;

	if(job->request.newfid != job->request.fid){
		/* clone fid */
		nmf = newfid(job->request.newfid);
		if(nmf->busy){
			nmf = nil;
			err = "clone to used channel";
			goto send;
		}
		*nmf = *mf;
		nmf->user = estrdup(mf->user);
		nmf->fid = job->request.newfid;
		nmf->qid.vers = vers++;
		mf = nmf;
	}
	/* else nmf will be nil */

	qid = mf->qid;
	if(nelems > 0){
		/* walk fid */
		for(i=0; i<nelems && i<MAXWELEM; i++){
			if((qid.type & QTDIR) == 0){
				err = "not a directory";
				break;
			}
			if(strcmp(elems[i], "..") == 0 || strcmp(elems[i], ".") == 0){
				qid.type = QTDIR;
				qid.path = Qdir;
    Found:
				job->reply.wqid[i] = qid;
				job->reply.nwqid++;
				continue;
			}
			if(strcmp(elems[i], "cs") == 0){
				qid.type = QTFILE;
				qid.path = Qcs;
				goto Found;
			}
			err = "file does not exist";
			break;
		}
	}

    send:
	if(nmf != nil && (err!=nil || job->reply.nwqid<nelems)){
		cleanmf(nmf);
		free(nmf->user);
		nmf->user = nil;
		nmf->busy = 0;
		nmf->fid = 0;
	}
	if(err == nil)
		mf->qid = qid;
	sendmsg(job, err);
	return err;
}

void
ropen(Job *job, Mfile *mf)
{
	int mode;
	char *err;

	err = nil;
	mode = job->request.mode;
	if(mf->qid.type & QTDIR){
		if(mode)
			err = "permission denied";
	}
	job->reply.qid = mf->qid;
	job->reply.iounit = 0;
	sendmsg(job, err);
}

void
rcreate(Job *job, Mfile *mf)
{
	USED(mf);
	sendmsg(job, "creation permission denied");
}

void
rread(Job *job, Mfile *mf)
{
	int i, n, cnt;
	long off, toff, clock;
	Dir dir;
	uchar buf[Maxfdata];
	char *err;

	n = 0;
	err = nil;
	off = job->request.offset;
	cnt = job->request.count;
	mf->ref++;

	if(mf->qid.type & QTDIR){
		clock = time(0);
		if(off == 0){
			memset(&dir, 0, sizeof dir);
			dir.name = "cs";
			dir.qid.type = QTFILE;
			dir.qid.vers = vers;
			dir.qid.path = Qcs;
			dir.mode = 0666;
			dir.length = 0;
			dir.uid = mf->user;
			dir.gid = mf->user;
			dir.muid = mf->user;
			dir.atime = clock;	/* wrong */
			dir.mtime = clock;	/* wrong */
			n = convD2M(&dir, buf, sizeof buf);
		}
		job->reply.data = (char*)buf;
		goto send;
	}

	for(;;){
		/* look for an answer at the right offset */
		toff = 0;
		for(i = 0; mf->reply[i] != nil && i < mf->nreply; i++){
			n = mf->replylen[i];
			if(off < toff + n)
				break;
			toff += n;
		}
		if(i < mf->nreply)
			break;		/* got something to return */

		/* try looking up more answers */
		if(lookup(mf) == 0 || job->flushed){
			/* no more */
			n = 0;
			goto send;
		}
	}

	/* give back a single reply (or part of one) */
	job->reply.data = mf->reply[i] + (off - toff);
	if(cnt > toff - off + n)
		n = toff - off + n;
	else
		n = cnt;

send:
	job->reply.count = n;
	sendmsg(job, err);

	if(--mf->ref == 0 && mf->busy == 0)
		cleanmf(mf);
}

void
cleanmf(Mfile *mf)
{
	int i;

	if(mf->net != nil){
		free(mf->net);
		mf->net = nil;
	}
	if(mf->host != nil){
		free(mf->host);
		mf->host = nil;
	}
	if(mf->serv != nil){
		free(mf->serv);
		mf->serv = nil;
	}
	if(mf->rem != nil){
		free(mf->rem);
		mf->rem = nil;
	}
	for(i = 0; i < mf->nreply; i++){
		free(mf->reply[i]);
		mf->reply[i] = nil;
		mf->replylen[i] = 0;
	}
	mf->nreply = 0;
	mf->nextnet = netlist;
}

void
rwrite(Job *job, Mfile *mf)
{
	int cnt, n;
	char *err;
	char *field[4];
	char curerr[64];

	err = nil;
	cnt = job->request.count;
	if(mf->qid.type & QTDIR){
		err = "can't write directory";
		goto send;
	}
	if(cnt >= Maxrequest){
		err = "request too long";
		goto send;
	}
	job->request.data[cnt] = 0;

	if(strcmp(mf->user, "none") == 0 || strcmp(mf->user, csuser) != 0)
		goto query;	/* skip special commands if not owner */

	/*
	 *  toggle debugging
	 */
	if(strncmp(job->request.data, "debug", 5)==0){
		debug ^= 1;
		syslog(1, logfile, "debug %d", debug);
		goto send;
	}

	/*
	 *  toggle ipv4 lookups
	 */
	if(strncmp(job->request.data, "ipv4", 4)==0){
		lookipvers ^= V4;
		syslog(1, logfile, "ipv4lookups %d", (lookipvers & V4) != 0);
		goto send;
	}

	/*
	 *  toggle ipv6 lookups
	 */
	if(strncmp(job->request.data, "ipv6", 4)==0){
		lookipvers ^= V6;
		syslog(1, logfile, "ipv6lookups %d", (lookipvers & V6) != 0);
		goto send;
	}

	/*
	 *  add networks to the default list
	 */
	if(strncmp(job->request.data, "add ", 4)==0){
		if(job->request.data[cnt-1] == '\n')
			job->request.data[cnt-1] = 0;
		netadd(job->request.data+4);
		readipinterfaces();
		goto send;
	}

	/*
	 *  refresh all state
	 */
	if(strncmp(job->request.data, "refresh", 7)==0){
		netinit(1);
		goto send;
	}

query:
	if(mf->ref){
		err = "query already in progress";
		goto send;
	}
	mf->ref++;

	/* start transaction with a clean slate */
	cleanmf(mf);

	/*
	 *  look for a general query
	 */
	if(*job->request.data == '!'){
		err = genquery(mf, job->request.data+1);
		goto done;
	}

	if(debug)
		syslog(0, logfile, "write %s", job->request.data);
	/*
	 *  break up name
	 */
	n = getfields(job->request.data, field, 4, 1, "!");
	switch(n){
	case 1:
		mf->net = estrdup("net");
		mf->host = estrdup(field[0]);
		break;
	case 4:
		mf->rem = estrdup(field[3]);
		/* fall through */
	case 3:
		mf->serv = estrdup(field[2]);
		/* fall through */
	case 2:
		mf->host = estrdup(field[1]);
		mf->net = estrdup(field[0]);
		break;
	}

	/*
	 *  do the first net worth of lookup
	 */
	if(lookup(mf) == 0){
		rerrstr(curerr, sizeof curerr);
		err = curerr;
	}

done:
	if(--mf->ref == 0 && mf->busy == 0)
		cleanmf(mf);

send:
	job->reply.count = cnt;
	sendmsg(job, err);
}

void
rclunk(Job *job, Mfile *mf)
{
	if(mf->ref == 0)
		cleanmf(mf);
	free(mf->user);
	mf->user = nil;
	mf->fid = 0;
	mf->busy = 0;
	sendmsg(job, nil);
}

void
rremove(Job *job, Mfile *mf)
{
	USED(mf);
	sendmsg(job, "remove permission denied");
}

void
rstat(Job *job, Mfile *mf)
{
	Dir dir;
	uchar buf[IOHDRSZ+Maxfdata];

	memset(&dir, 0, sizeof dir);
	if(mf->qid.type & QTDIR){
		dir.name = ".";
		dir.mode = DMDIR|0555;
	} else {
		dir.name = "cs";
		dir.mode = 0666;
	}
	dir.qid = mf->qid;
	dir.length = 0;
	dir.uid = mf->user;
	dir.gid = mf->user;
	dir.muid = mf->user;
	dir.atime = dir.mtime = time(0);
	job->reply.nstat = convD2M(&dir, buf, sizeof buf);
	job->reply.stat = buf;
	sendmsg(job, nil);
}

void
rwstat(Job *job, Mfile *mf)
{
	USED(mf);
	sendmsg(job, "wstat permission denied");
}

void
sendmsg(Job *job, char *err)
{
	int n;
	uchar mdata[IOHDRSZ + Maxfdata];
	char ename[ERRMAX];

	if(err){
		job->reply.type = Rerror;
		snprint(ename, sizeof(ename), "cs: %s", err);
		job->reply.ename = ename;
	}else{
		job->reply.type = job->request.type+1;
	}
	job->reply.tag = job->request.tag;
	n = convS2M(&job->reply, mdata, sizeof mdata);
	if(n == 0){
		syslog(1, logfile, "sendmsg convS2M of %F returns 0", &job->reply);
		abort();
	}
	qlock(&joblock);
	if(job->flushed == 0)
		if(write(mfd[1], mdata, n)!=n)
			error("mount write");
	qunlock(&joblock);
	if(debug)
		syslog(0, logfile, "%F %d", &job->reply, n);
}

void
error(char *s)
{
	syslog(1, logfile, "%s: %r", s);
	_exits(0);
}

static uchar loopbacknet[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0xff, 0xff,
	127, 0, 0, 0
};
static uchar loopbackmask[IPaddrlen] = {
	0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff,
	0xff, 0, 0, 0
};
static uchar loopback6[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 1
};

void
readipinterfaces(void)
{
	uchar mynet[IPaddrlen];
	Ipifc *ifc;
	Iplifc *lifc;
	int conf, dns;

	conf = dns = 0;
	qlock(&ipifclock);
	ipifcs = readipifc(mntpt, ipifcs, -1);
	for(ifc = ipifcs; ifc != nil; ifc = ifc->next){
		for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
			if(ipcmp(lifc->ip, IPnoaddr) == 0)
				continue;
			if(isv4(lifc->ip)){
				conf |= V4;
				maskip(lifc->ip, loopbackmask, mynet);
				if(ipcmp(mynet, loopbacknet) == 0)
					continue;
				dns |= V4;
			} else {
				conf |= V6;
				if(ISIPV6LINKLOCAL(lifc->ip))
					continue;
				if(ipcmp(lifc->ip, loopback6) == 0)
					continue;
				dns |= V6;
			}
		}
	}
	qunlock(&ipifclock);
	confipvers = conf;
	dnsipvers = dns;
}

/*
 *  get the system name
 */
void
ipid(void)
{
	char eaddr[16], buf[Maxpath];
	uchar addr[6];
	Ndbtuple *t, *tt;
	char *p, *attr;
	Ndbs s;
	int f, n;
	Dir *d;

	if(mysysname != nil)
		return;

	/*
	 *  environment has priority.
	 *
	 *  on the sgi power the default system name
	 *  is the ip address.  ignore that.
	 *
	 */
	p = getenv("sysname");
	if(p != nil && *p != 0){
		attr = ipattr(p);
		if(strcmp(attr, "ip") != 0) {
			mysysname = p;
			goto setsys;
		}
		free(p);
	}

	/* try configured interfaces */
	attr = "sys";
	t = s.t = myipinfo(db, &attr, 1);
	if(t != nil)
		goto found;

	/* try ethernet interfaces */
	n = 0;
	d = nil;
	f = open(mntpt, OREAD);
	if(f >= 0){
		n = dirreadall(f, &d);
		close(f);
	}
	for(f = 0; f < n; f++){
		if((d[f].mode & DMDIR) == 0 || strncmp(d[f].name, "ether", 5) != 0)
			continue;
		snprint(buf, sizeof buf, "%s/%s", mntpt, d[f].name);
		if(myetheraddr(addr, buf) >= 0){
			snprint(eaddr, sizeof(eaddr), "%E", addr);
			free(ndbgetvalue(db, &s, "ether", eaddr, "sys", &t));
			if(t != nil){
				free(d);
				goto found;
			}
		}
	}
	free(d);

	/* nothing else worked, use ip address */
	attr = "ip";
	t = s.t = myipinfo(db, &attr, 1);
	if(t == nil)
		return;
	
found:
	/* found in database */
	if((tt = ndbfindattr(t, s.t, "sys")) != nil)
		mysysname = estrdup(tt->val);
	else if((tt = ndbfindattr(t, s.t, "ip")) != nil)
		mysysname = estrdup(tt->val);
	ndbfree(t);

	if(mysysname == nil)
		return;

setsys:
	/* set /dev/sysname if we now know it */
	f = open("/dev/sysname", OWRITE);
	if(f >= 0){
		write(f, mysysname, strlen(mysysname));
		close(f);
	}
}

/*
 *  Set up a list of default networks by looking for
 *  /net/^*^/clone.
 */
void
netinit(int background)
{
	char clone[Maxpath];
	Network *np;

	if(background){
		if(rfork(RFPROC|RFNOTEG|RFMEM|RFNOWAIT) != 0)
			return;
		qlock(&netlock);
	}

	/* add the mounted networks to the default list */
	for(np = network; np->net != nil; np++){
		if(np->considered)
			continue;
		snprint(clone, sizeof(clone), "%s/%s/clone", mntpt, np->net);
		if(access(clone, AEXIST) < 0)
			continue;
		if(netlist != nil)
			last->next = np;
		else
			netlist = np;
		last = np;
		np->next = nil;
		np->considered = 1;
	}

	/* find out what our ip addresses are */
	readipinterfaces();

	/* set the system name if we need to, these days ip is all we have */
	ipid();

	if(debug)
		syslog(0, logfile, "mysysname %s", mysysname?mysysname:"???");

	if(background){
		qunlock(&netlock);
		_exits(0);
	}
}

/*
 *  add networks to the standard list
 */
void
netadd(char *p)
{
	Network *np;
	char *field[12];
	int i, n;

	n = getfields(p, field, 12, 1, " ");
	for(i = 0; i < n; i++){
		for(np = network; np->net != nil; np++){
			if(strcmp(field[i], np->net) != 0)
				continue;
			if(np->considered)
				break;
			if(netlist != nil)
				last->next = np;
			else
				netlist = np;
			last = np;
			np->next = nil;
			np->considered = 1;
		}
	}
}

int
lookforproto(Ndbtuple *t, char *proto)
{
	for(; t != nil; t = t->entry)
		if(strcmp(t->attr, "proto") == 0 && strcmp(t->val, proto) == 0)
			return 1;
	return 0;
}

/*
 *  lookup a request.  the network "net" means we should pick the
 *  best network to get there.
 */
int
lookup(Mfile *mf)
{
	Network *np;
	char *cp;
	Ndbtuple *nt, *t;
	char reply[Maxreply];
	int i, rv, fasttimeout;

	/* open up the standard db files */
	if(db == nil)
		ndbinit();
	if(db == nil)
		error("can't open mf->network database\n");

	if(mf->net == nil)
		return 0;	/* must have been a genquery */

	rv = 0;
	if(strcmp(mf->net, "net") == 0){
		/*
		 *  go through set of default nets
		 */
		for(np = mf->nextnet; np != nil && rv == 0; np = np->next){
			nt = (*np->lookup)(np, mf->host, mf->serv);
			if(nt == nil)
				continue;
			fasttimeout = np->fasttimeout && !lookforproto(nt, np->net);
			for(t = nt; mf->nreply < Nreply && t != nil; t = t->entry){
				cp = (*np->trans)(t, np, mf->serv, mf->rem, fasttimeout);
				if(cp != nil){
					/* avoid duplicates */
					for(i = 0; i < mf->nreply; i++)
						if(strcmp(mf->reply[i], cp) == 0)
							break;
					if(i == mf->nreply){
						/* save the reply */
						mf->replylen[mf->nreply] = strlen(cp);
						mf->reply[mf->nreply++] = cp;
						rv++;
					} else
						free(cp);
				}
			}
			ndbfree(nt);
		}
		mf->nextnet = np;
		return rv;
	}

	/*
	 *  if not /net, we only get one lookup
	 */
	if(mf->nreply != 0)
		return 0;

	/*
	 *  look for a specific network
	 */
	for(np = network; np->net != nil; np++){
		if(np->fasttimeout)
			continue;
		if(strcmp(np->net, mf->net) == 0)
			break;
	}

	if(np->net != nil){
		/*
		 *  known network
		 */
		nt = (*np->lookup)(np, mf->host, mf->serv);
		for(t = nt; mf->nreply < Nreply && t != nil; t = t->entry){
			cp = (*np->trans)(t, np, mf->serv, mf->rem, 0);
			if(cp != nil){
				mf->replylen[mf->nreply] = strlen(cp);
				mf->reply[mf->nreply++] = cp;
				rv++;
			}
		}
		ndbfree(nt);
		return rv;
	} else {
		/*
		 *  not a known network, don't translate host or service
		 */
		if(mf->serv != nil)
			snprint(reply, sizeof(reply), "%s/%s/clone %s!%s",
				mntpt, mf->net, mf->host, mf->serv);
		else
			snprint(reply, sizeof(reply), "%s/%s/clone %s",
				mntpt, mf->net, mf->host);
		mf->reply[0] = estrdup(reply);
		mf->replylen[0] = strlen(reply);
		mf->nreply = 1;
		return 1;
	}
}

/*
 *  translate an ip service name into a port number.  If it's a numeric port
 *  number, look for restricted access.
 *
 *  the service '*' needs no translation.
 */
char*
ipserv(Network *np, char *name, char *buf, int blen)
{
	char *p;
	int alpha = 0;
	int restr = 0;
	Ndbtuple *t, *nt;
	Ndbs s;

	/* '*' means any service */
	if(strcmp(name, "*") == 0){
		nstrcpy(buf, name, blen);
		return buf;
	}

	/*  see if it's numeric or symbolic */
	for(p = name; *p; p++){
		if(isdigit(*p))
			{}
		else if(isalpha(*p) || *p == '-' || *p == '$')
			alpha = 1;
		else
			return nil;
	}
	t = nil;
	p = nil;
	if(alpha){
		p = ndbgetvalue(db, &s, np->net, name, "port", &t);
		if(p == nil)
			return nil;
	} else {
		/* look up only for tcp ports < 1024 to get the restricted
		 * attribute
		 */
		if(atoi(name) < 1024 && strcmp(np->net, "tcp") == 0)
			p = ndbgetvalue(db, &s, "port", name, "port", &t);
		if(p == nil)
			p = estrdup(name);
	}

	if(t){
		for(nt = t; nt != nil; nt = nt->entry)
			if(strcmp(nt->attr, "restricted") == 0)
				restr = 1;
		ndbfree(t);
	}
	snprint(buf, blen, "%s%s", p, restr ? "!r" : "");
	free(p);

	return buf;
}

Ndbtuple*
myipinfo(Ndb *db, char **list, int n)
{
	Ndbtuple *t, *nt;
	char ip[64];
	Ipifc *ifc;
	Iplifc *lifc;

	t = nil;
	qlock(&ipifclock);
	for(ifc = ipifcs; ifc != nil; ifc = ifc->next){
		for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
			snprint(ip, sizeof(ip), "%I", lifc->ip);
			nt = ndbipinfo(db, "ip", ip, list, n);
			t = ndbconcatenate(t, nt);
		}
	}
	qunlock(&ipifclock);

	return ndbdedup(t);
}

/*
 * reorder according to our interfaces
 */
static Ndbtuple*
ipreorder(Ndbtuple *t)
{
	Ndbtuple *nt;
	uchar ip[IPaddrlen];
	uchar net[IPaddrlen];
	uchar tnet[IPaddrlen];
	Ipifc *ifc;
	Iplifc *lifc;

	if(t == nil)
		return nil;

	qlock(&ipifclock);
	for(ifc = ipifcs; ifc != nil; ifc = ifc->next){
		for(lifc = ifc->lifc; lifc != nil; lifc = lifc->next){
			maskip(lifc->ip, lifc->mask, net);
			for(nt = t; nt != nil; nt = nt->entry){
				if(strcmp(nt->attr, "ip") != 0)
					continue;
				if(parseip(ip, nt->val) == -1)
					continue;
				maskip(ip, lifc->mask, tnet);
				if(memcmp(net, tnet, IPaddrlen) == 0){
					qunlock(&ipifclock);
					return ndbreorder(t, nt);
				}
			}
		}
	}
	qunlock(&ipifclock);

	return t;
}

static Ndbtuple*
ndbline(Ndbtuple *t)
{
	Ndbtuple *nt;

	for(nt = t; nt != nil; nt = nt->entry){
		if(nt->entry == nil)
			nt->line = t;
		else
			nt->line = nt->entry;
	}
	return t;
}

/*
 *  lookup an ip destination
 */
static Ndbtuple*
iplookuphost(Network *np, char *host)
{
	char *attr, *dnsname;
	Ndbtuple *t, *nt;
	Ndbs s;

	/*
	 *  turn '[ip address]' into just 'ip address'
	 */
	if(*host == '['){
		char tmp[Maxhost], *x;

		nstrcpy(tmp, host, sizeof tmp);
		host = tmp;
		if((x = strchr(++host, ']')) != nil)
			*x = 0;
	}

	/* for dial strings with no host */
	if(strcmp(host, "*") == 0)
		return ndbline(ndbnew("ip", "*"));

	/*
	 *  hack till we go v6 :: = 0.0.0.0
	 */
	if(strcmp("::", host) == 0)
		return ndbline(ndbnew("ip", "*"));

	/*
	 *  just accept addresses
	 */
	attr = ipattr(host);
	if(strcmp(attr, "ip") == 0)
		return ndbline(ndbnew("ip", host));

	/*
	 *  give the domain name server the first opportunity to
	 *  resolve domain names.  if that fails try the database.
	 */
	t = nil;
	if(strcmp(attr, "dom") == 0)
		t = dnsiplookup(host, &s, np->ipvers);
	if(t == nil){
		for(nt = ndbsearch(db, &s, attr, host); nt != nil; nt = ndbsnext(&s, attr, host)){
			if(ndbfindattr(nt, s.t, "ip") == nil){
				ndbfree(nt);
				continue;
			}
			t = ndbconcatenate(t, ndbreorder(nt, s.t));
		}
		s.t = t;
	}
	if(t == nil){
		if(strcmp(attr, "dom") != 0){
			dnsname = ndbgetvalue(db, &s, attr, host, "dom", nil);
			if(dnsname != nil){
				t = dnsiplookup(dnsname, &s, np->ipvers);
				free(dnsname);
			}
		}
		if(t == nil)
			t = dnsiplookup(host, &s, np->ipvers);
	}
	if(t == nil)
		return nil;

	/*
	 *  reorder the tuple to have the matched line first and
	 *  save that in the request structure.
	 */
	return ndbreorder(t, s.t);
}


Ndbtuple*
iplookup(Network *np, char *host, char *serv)
{
	Ndbtuple *l, *t, *nt;
	char ts[Maxservice], *attr;

	/*
	 *  start with the service since it's the most likely to fail
	 *  and costs the least
	 */
	if(serv == nil || ipserv(np, serv, ts, sizeof ts) == nil){
		werrstr("can't translate service");
		return nil;
	}

	/*
	 *  '$' means the rest of the name is an attribute that we
	 *  need to search for
	 */
 	werrstr("can't translate address");
	if(*host == '$'){
		t = nil;
		attr = host+1;
		l = myipinfo(db, &attr, 1);
		for(nt = l; nt != nil; nt = nt->entry){
			if(strcmp(nt->attr, attr) == 0)
				t = ndbconcatenate(t, iplookuphost(np, nt->val));
		}
		ndbfree(l);
	} else
		t = iplookuphost(np, host);

	return ipreorder(t);
}


/*
 *  translate an ip address
 */
char*
iptrans(Ndbtuple *t, Network *np, char *serv, char *rem, int fasttimeout)
{
	char ts[Maxservice];
	char reply[Maxreply];
	char x[Maxservice];
	uchar ip[IPaddrlen];

	if(strcmp(t->attr, "ip") != 0)
		return nil;

	if(serv == nil || ipserv(np, serv, ts, sizeof ts) == nil){
		werrstr("can't translate service");
		return nil;
	}

	if(rem != nil)
		snprint(x, sizeof(x), "!%s", rem);
	else
		*x = 0;

	if(*t->val == '*')
		snprint(reply, sizeof(reply), "%s/%s/clone %s%s",
			mntpt, np->net, ts, x);
	else {
		if(parseip(ip, t->val) == -1)
			return nil;
		if((np->ipvers & confipvers & (isv4(ip) ? V4 : V6)) == 0)
			return nil;
		snprint(reply, sizeof(reply), "%s/%s/clone %I!%s%s%s",
			mntpt, np->net, ip, ts, x, fasttimeout? "!fasttimeout": "");
	}

	return estrdup(reply);
}

/*
 *  lookup a telephone number
 */
Ndbtuple*
telcolookup(Network *np, char *host, char *serv)
{
	Ndbtuple *t;
	Ndbs s;

	USED(np, serv);

	werrstr("can't translate address");
	free(ndbgetvalue(db, &s, "sys", host, "telco", &t));
	if(t == nil)
		return ndbline(ndbnew("telco", host));

	return ndbreorder(t, s.t);
}

/*
 *  translate a telephone address
 */
char*
telcotrans(Ndbtuple *t, Network *np, char *serv, char *rem, int)
{
	char reply[Maxreply];
	char x[Maxservice];

	if(strcmp(t->attr, "telco") != 0)
		return nil;

	if(rem != nil)
		snprint(x, sizeof(x), "!%s", rem);
	else
		*x = 0;
	if(serv != nil)
		snprint(reply, sizeof(reply), "%s/%s/clone %s!%s%s", mntpt, np->net,
			t->val, serv, x);
	else
		snprint(reply, sizeof(reply), "%s/%s/clone %s%s", mntpt, np->net,
			t->val, x);
	return estrdup(reply);
}

/*
 *  create a slave process to handle a request to avoid one request blocking
 *  another.  parent returns to job loop.
 */
void
slave(char *host)
{
	if(*isslave)
		return;		/* we're already a slave process */
	if(ainc(&active) >= Maxactive){
		adec(&active);
		return;
	}
	switch(rfork(RFPROC|RFNOTEG|RFMEM|RFNOWAIT)){
	case -1:
		adec(&active);
		break;
	case 0:
		*isslave = 1;
		if(debug)
			syslog(0, logfile, "slave %d", getpid());
		procsetname("%s", host);
		break;
	default:
		longjmp(masterjmp, 1);
	}

}

static int
mountdns(void)
{
	static QLock mountlock;
	static int mounted;
	char buf[128], *p;
	int fd;

	if(mounted)
		return 0;

	qlock(&mountlock);
	snprint(buf, sizeof(buf), "%s/dns", mntpt);
	if(access(buf, AEXIST) == 0)
		goto done;
	if(strcmp(mntpt, "/net") == 0)
		snprint(buf, sizeof(buf), "/srv/dns");
	else {
		snprint(buf, sizeof(buf), "/srv/dns%s", mntpt);
		while((p = strchr(buf+8, '/')) != nil)
			*p = '_';
	}
	if((fd = open(buf, ORDWR)) < 0){
err:
		qunlock(&mountlock);
		return -1;	
	}
	if(mount(fd, -1, mntpt, MAFTER, "") == -1){
		close(fd);
		goto err;
	}
done:
	mounted = 1;
	qunlock(&mountlock);
	return 0;
}

static Ndbtuple*
dnsip6lookup(char *mntpt, char *buf, Ndbtuple *t)
{
	Ndbtuple *t6, *tt;

	t6 = dnsquery(mntpt, buf, "ipv6");	/* lookup AAAA dns RRs */
	if (t6 == nil)
		return t;

	/* convert ipv6 attr to ip */
	for (tt = t6; tt != nil; tt = tt->entry)
		if (strcmp(tt->attr, "ipv6") == 0)
			strcpy(tt->attr, "ip");

	/* append t6 list to t list */
	return ndbconcatenate(t, t6);
}

/*
 *  call the dns process and have it try to translate a name
 */
Ndbtuple*
dnsiplookup(char *host, Ndbs *s, int ipvers)
{
	char buf[Maxreply];
	Ndbtuple *t;

	ipvers &= dnsipvers & lookipvers;
	if(ipvers == 0){
		werrstr("no ip address");
		return nil;
	}
	qunlock(&dblock);
	slave(host);
	if(*isslave == 0){
		qlock(&dblock);
		werrstr("too much activity");
		return nil;
	}

	if(mountdns() < 0){
		qlock(&dblock);
		return nil;
	}

	if(strcmp(ipattr(host), "ip") == 0)
		t = dnsquery(mntpt, host, "ptr");
	else {
		t = nil;
		if(ipvers & V4)
			t = dnsquery(mntpt, host, "ip");
		if(ipvers & V6)
			t = dnsip6lookup(mntpt, host, t);
	}
	s->t = t;

	if(t == nil){
		rerrstr(buf, sizeof buf);
		if(strstr(buf, "exist") != nil)
			werrstr("can't translate address: %s", buf);
		else if(strstr(buf, "dns failure") != nil)
			werrstr("temporary problem: %s", buf);
	}

	qlock(&dblock);
	return t;
}

int
qmatch(Ndbtuple *t, char **attr, char **val, int n)
{
	int i, found;
	Ndbtuple *nt;

	for(i = 1; i < n; i++){
		found = 0;
		for(nt = t; nt != nil; nt = nt->entry)
			if(strcmp(attr[i], nt->attr) == 0)
				if(strcmp(val[i], "*") == 0
				|| strcmp(val[i], nt->val) == 0){
					found = 1;
					break;
				}
		if(found == 0)
			break;
	}
	return i == n;
}

void
qreply(Mfile *mf, Ndbtuple *t)
{
	while(mf->nreply < Nreply && t != nil) {
		char *line = smprint("%N", t);
		if(line == nil)
			break;
		mf->reply[mf->nreply] = line;
		mf->replylen[mf->nreply++] = strlen(line);

		/* skip to next line */
		do {
			Ndbtuple *l = t->line;
			t = t->entry;
			if(t != l)
				break;
		} while(t != nil);
	}
}

enum
{
	Maxattr=	32,
};

/*
 *  generic query lookup.  The query is of one of the following
 *  forms:
 *
 *  attr1=val1 attr2=val2 attr3=val3 ...
 *
 *  returns the matching tuple
 *
 *  ipinfo attr=val attr1 attr2 attr3 ...
 *
 *  is like ipinfo and returns the attr{1-n}
 *  associated with the ip address.
 */
char*
genquery(Mfile *mf, char *query)
{
	int i, n;
	char *p;
	char *attr[Maxattr];
	char *val[Maxattr];
	Ndbtuple *t;
	Ndbs s;

	n = getfields(query, attr, nelem(attr), 1, " ");
	if(n == 0)
		return "bad query";

	if(strcmp(attr[0], "ipinfo") == 0)
		return ipinfoquery(mf, attr, n);

	/* parse pairs */
	for(i = 0; i < n; i++){
		p = strchr(attr[i], '=');
		if(p == nil)
			return "bad query";
		*p++ = 0;
		val[i] = p;
	}

	/* give dns a chance */
	if((strcmp(attr[0], "dom") == 0 || strcmp(attr[0], "ip") == 0) && val[0]){
		t = dnsiplookup(val[0], &s, lookipvers);
		if(t != nil){
			if(qmatch(t, attr, val, n)){
				qreply(mf, t);
				ndbfree(t);
				return nil;
			}
			ndbfree(t);
		}
	}

	/* first pair is always the key.  It can't be a '*' */
	t = ndbsearch(db, &s, attr[0], val[0]);

	/* search is the and of all the pairs */
	while(t != nil){
		if(qmatch(t, attr, val, n)){
			qreply(mf, t);
			ndbfree(t);
			return nil;
		}

		ndbfree(t);
		t = ndbsnext(&s, attr[0], val[0]);
	}

	return "no match";
}

/*
 *  resolve an ip address
 */
static Ndbtuple*
ipresolve(char *attr, char *host)
{
	Ndbtuple *t, *nt, **l;

	t = iplookup(&network[Ntcp], host, "*");
	for(l = &t; *l != nil; ){
		nt = *l;
		if(strcmp(nt->attr, "ip") != 0){
			*l = nt->entry;
			nt->entry = nil;
			ndbfree(nt);
			continue;
		}
		nstrcpy(nt->attr, attr, sizeof(nt->attr));
		l = &nt->entry;
	}
	return t;
}

char*
ipinfoquery(Mfile *mf, char **list, int n)
{
	int i, nresolve;
	uchar resolve[Maxattr];
	Ndbtuple *t, *nt, **l;
	char *attr, *val;

	/* skip 'ipinfo' */
	list++; n--;

	if(n < 1)
		return "bad query";

	/* get search attribute=value, or assume myip */
	attr = *list;
	if((val = strchr(attr, '=')) != nil){
		*val++ = 0;
		list++;
		n--;
	}else{
		attr = nil;
		val = nil;
	}
	if(n < 1)
		return "bad query";


	/*
	 *  don't let ndbipinfo resolve the addresses, we're
	 *  better at it.
	 */
	nresolve = 0;
	for(i = 0; i < n; i++)
		if(*list[i] == '@'){		/* @attr=val ? */
			list[i]++;
			resolve[i] = 1;		/* we'll resolve it */
			nresolve++;
		} else
			resolve[i] = 0;

	if(attr == nil)
		t = myipinfo(db, list, n);
	else
		t = ndbipinfo(db, attr, val, list, n);

	if(t == nil)
		return "no match";

	if(nresolve != 0){
		for(l = &t; *l != nil;){
			nt = *l;

			/* already an address? */
			if(strcmp(ipattr(nt->val), "ip") == 0){
				l = &(*l)->entry;
				continue;
			}

			/* user wants it resolved? */
			for(i = 0; i < n; i++)
				if(strcmp(list[i], nt->attr) == 0)
					break;
			if(i >= n || resolve[i] == 0){
				l = &(*l)->entry;
				continue;
			}

			/* resolve address and replace entry */
			*l = ipresolve(nt->attr, nt->val);
			while(*l != nil)
				l = &(*l)->entry;
			*l = nt->entry;

			nt->entry = nil;
			ndbfree(nt);
		}

		t = ndbdedup(t);
	}

	/* make it all one line */
	t = ndbline(t);

	qreply(mf, t);

	return nil;
}

void*
emalloc(int size)
{
	void *x;

	x = malloc(size);
	if(x == nil)
		error("out of memory");
	memset(x, 0, size);
	return x;
}

char*
estrdup(char *s)
{
	int size;
	char *p;

	size = strlen(s);
	p = malloc(size+1);
	if(p == nil)
		error("out of memory");
	memmove(p, s, size);
	p[size] = 0;
	return p;
}

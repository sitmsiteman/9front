#include <u.h>
#include <libc.h>

#include "git.h"

#pragma	varargck	type	"P"	void

int fullpath;
int changes;
int reverse;
char *path[128];
int npath;

int
Pfmt(Fmt *f)
{
	int i, n;

	n = 0;
	for(i = 0; i < npath; i++)
		n += fmtprint(f, "%s/", path[i]);
	return n;
}

void
showdir(Hash dh, char *dname, char m)
{
	Dirent *p, *e;
	Object *d;


	path[npath++] = dname;
	if((d = readobject(dh)) == nil)
		sysfatal("bad hash %H", dh);
	assert(d->type == GTree);
	p = d->tree->ent;
	e = p + d->tree->nent;
	for(; p != e; p++){
		if(p->ismod)
			continue;
		if(p->mode & DMDIR)
			showdir(p->h, p->name, m);
		else
			print("%c %P%s\n", m, p->name);
	}
	print("%c %P\n", m);
	unref(d);
	npath--;
}

void
show(Dirent *e, char m)
{
	if(e->mode & DMDIR)
		showdir(e->h, e->name, m);
	else
		print("%c %P%s\n", m, e->name);
}

void
difftrees(Object *a, Object *b)
{
	Dirent *ap, *bp, *ae, *be;
	int c;

	ap = ae = nil;
	bp = be = nil;
	if(a != nil){
		if(a->type != GTree)
			return;
		ap = a->tree->ent;
		ae = ap + a->tree->nent;
	}
	if(b != nil){
		if(b->type != GTree)
			return;
		bp = b->tree->ent;
		be = bp + b->tree->nent;
	}
	while(ap != ae && bp != be){
		c = entcmp(ap, bp);
		if(c == 0){
			if(ap->mode == bp->mode && hasheq(&ap->h, &bp->h))
				goto next;
			if(ap->mode != bp->mode)
				print("! %P%s\n", ap->name);
			else if(!(ap->mode & DMDIR) || !(bp->mode & DMDIR))
				print("@ %P%s\n", ap->name);
			if((ap->mode & DMDIR) && (bp->mode & DMDIR)){
				if(npath >= nelem(path))
					sysfatal("path too deep");
				path[npath++] = ap->name;
				if((a = readobject(ap->h)) == nil)
					sysfatal("bad hash %H", ap->h);
				if((b = readobject(bp->h)) == nil)
					sysfatal("bad hash %H", bp->h);
				difftrees(a, b);
				unref(a);
				unref(b);
				npath--;
			}
next:
			ap++;
			bp++;
		}else if(c < 0) {
			show(ap, '-');
			ap++;
		}else if(c > 0){
			show(bp, '+');
			bp++;
		}
	}
	for(; ap != ae; ap++)
		show(ap, '-');
	for(; bp != be; bp++)
		show(bp, '+');
}

void
diffcommits(Hash ah, Hash bh)
{
	Object *a, *b, *at, *bt;

	at = nil;
	bt = nil;
	if(!hasheq(&ah, &Zhash) && (a = readobject(ah)) != nil){
		if(a->type != GCommit)
			sysfatal("not commit: %H", ah);
		if((at = readobject(a->commit->tree)) == nil)
			sysfatal("bad hash %H", a->commit->tree);
		unref(a);
	}
	if(!hasheq(&bh, &Zhash) && (b = readobject(bh)) != nil){
		if(b->type != GCommit)
			sysfatal("not commit: %H", ah);
		if((bt = readobject(b->commit->tree)) == nil)
			sysfatal("bad hash %H", b->commit->tree);
		unref(b);
	}
	difftrees(at, bt);
	unref(at);
	unref(bt);
}

void
usage(void)
{
	fprint(2, "usage: %s [-pcr] query\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *query, repo[512];
	char *p, *e, *objpfx;
	int i, j, n, nrel;
	Hash *h;

	ARGBEGIN{
	case 'd':	chattygit++;	break;
	case 'p':	fullpath++;	break;
	case 'c':	changes++;	break;
	case 'r':	reverse ^= 1;	break;
	default:	usage();	break;
	}ARGEND;

	gitinit();
	fmtinstall('P', Pfmt);

	if(argc == 0)
		usage();
	if(findrepo(repo, sizeof(repo), &nrel) == -1)
		sysfatal("find root: %r");
	if(chdir(repo) == -1)
		sysfatal("chdir: %r");
	if((objpfx = smprint("%s/.git/fs/object/", repo)) == nil)
		sysfatal("smprint: %r");
	for(i = 0, n = 0; i < argc; i++)
		n += strlen(argv[i]) + 1;
	query = emalloc(n+1);
	p = query;
	e = query + n;
	for(i = 0; i < argc; i++)
		p = seprint(p, e, "%s ", argv[i]);
	n = resolverefs(&h, query);
	free(query);
	if(n == -1)
		sysfatal("resolve: %r");
	if(changes){
		for(i = 1; i < n; i++)
			diffcommits(h[0], h[i]);
	}else{
		p = (fullpath ? objpfx : "");
		for(j = 0; j < n; j++)
			print("%s%H\n", p, h[reverse ? n - 1 - j : j]);
	}
	exits(nil);
}


#include	"mk.h"

Arc *
newarc(Node *n, Rule *r, char *stem, Resub *match)
{
	Arc *a;

	a = (Arc *)Malloc(sizeof(Arc));
	a->n = n;
	a->r = r;
	a->stem = Strdup(stem);
	rcopy(a->match, match, NREGEXP);
	a->next = 0;
	a->flag = 0;
	a->prog = r->prog;
	return(a);
}

void
freearc(Arc *a)
{
	free(a->stem);
	free(a);
}

void
dumpa(char *s, Arc *a)
{
	char buf[1024];

	Bprint(&bout, "%sArc@%p: n=%p r=%p flag=0x%x stem='%s'",
		s, a, a->n, a->r, a->flag, a->stem);
	if(a->prog)
		Bprint(&bout, " prog='%s'", a->prog);
	Bprint(&bout, "\n");

	if(a->n){
		snprint(buf, sizeof(buf), "%s    ", (*s == ' ')? s:"");
		dumpn(buf, a->n);
	}
}

void
nrep(void)
{
	Word *w;

	if(!empty(w = getvar("NREP")))
		nreps = atoi(w->s);
	if(nreps < 1)
		nreps = 1;
	if(DEBUG(D_GRAPH))
		Bprint(&bout, "nreps = %d\n", nreps);
}

// SPDX-License-Identifier: LicenseRef-Public-Domain
// Utah Energy Balance model, Tarboton, Luce, You, Chowdhury and Jackson.
// The upstream authors place these US-government-funded sources in the public domain.
// Double-precision translation of official snowdgt.f (1997-04-24) and snowdgtv22.f.
// Sources: https://hydrology.usu.edu/dtarb/snow/{snow,UEB22}.zip
// Archive SHA256: 6254b2015a88f23583b40ec1332a5fee266d5514ec0cb9be66f51526f6b3b8e1
//                 500c21d72696ad15d1ae289ed3aad3613afe210df3790397b28abb74c54f404b
// f2c 20240504 -f -a -r8 -R; declarations reordered; unused historical solvers omitted.
// The small C++ adapter replaces Fortran IO with diagnostics, STOP with an exception,
// and common-block scratch with thread-local storage. No Fortran runtime dependency.
// Adapter changes: configurable snow ageing/albedo and optional prescribed precipitation split,
// duration-weighted 24h means, corrected
// water*latent-heat and rate*duration terms in two v2.2 fallback branches, and no
// unbooked 10 C bare-soil energy cap. Reference tests isolate these corrections.
#include "UebReference.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>
namespace openswmm::surface::ueb_reference {
namespace {
using doublereal=double; using integer=int; using ftnlen=int;
struct cilist {int cierr,ciunit,ciend; const char* format; int record;};
thread_local int diagnostics=0;
thread_local double age_dust=.3,new_snow=.01,mean_surface=0,mean_bulk=0;
thread_local double visible_age=.2,infrared_age=.5,snow_fraction=-1;
integer s_wsle(cilist*) {++diagnostics;return 0;}
integer do_lio(integer*,integer*,const char*,ftnlen) {return 0;}
integer e_wsle() {return 0;}
int s_stop(const char*,ftnlen) {throw std::runtime_error("UEB reference rejected a negative melt rate.");}
double pow_dd(double* a,double* b) {return std::pow(*a,*b);}
double pow_di(double* a,int* b) {return std::pow(*a,*b);}
using std::log;using std::exp;using std::sqrt;using std::abs;
template<class A,class B> auto min(A a,B b) {using T=std::common_type_t<A,B>;return std::min(T(a),T(b));}
template<class A,class B> auto max(A a,B b) {using T=std::common_type_t<A,B>;return std::max(T(a),T(b));}
namespace equilibrium {

// Namespace declarations give every translated helper internal linkage before
// its block-scope extern declarations are seen by MSVC (including with LTCG).
int predicorr_(doublereal *dt, doublereal *ub, doublereal *w, doublereal *a,
    doublereal *ta, doublereal *prain, doublereal *ps, doublereal *ws,
    doublereal *rh, doublereal *qsi, doublereal *qli, integer *iradfl,
    doublereal *rkn, doublereal *qnetob, doublereal *cdh, doublereal *cde,
    doublereal *rid, doublereal *param, doublereal *sitev, doublereal *qh,
    doublereal *qe, doublereal *e, doublereal *mr, doublereal *qm,
    doublereal *q, doublereal *fm, doublereal *tsurf, doublereal *qnet);
int qfm_(doublereal *ub, doublereal *w, doublereal *a, doublereal *ta,
    doublereal *prain, doublereal *ps, doublereal *ws, doublereal *rh,
    doublereal *qsi, doublereal *qli, doublereal *rkn, integer *iradfl,
    doublereal *qnetob, doublereal *cdh, doublereal *cde, doublereal *rid,
    doublereal *param, doublereal *sitev, doublereal *fm, doublereal *q,
    doublereal *qm, doublereal *mr, doublereal *qe, doublereal *e,
    doublereal *tsurf, doublereal *qh, doublereal *qnet);
doublereal tavg_(doublereal *ub, doublereal *w, doublereal *rhow, doublereal *cs,
    doublereal *to, doublereal *rhog, doublereal *de, doublereal *cg,
    doublereal *hf);
doublereal srftmp_(doublereal *qsi, doublereal *a, doublereal *qli, doublereal *qpin,
    doublereal *ea, doublereal *ta, doublereal *tave, doublereal *tk,
    doublereal *pr, doublereal *ra, doublereal *cp, doublereal *rho,
    doublereal *rkn, doublereal *hneu, doublereal *es, doublereal *sbc,
    doublereal *cs, doublereal *rs, doublereal *w, doublereal *qnetob,
    integer *iradfl, doublereal *ws, doublereal *z__, doublereal *g,
    doublereal *fc, doublereal *fstab);
doublereal surfeb_(doublereal *ts, doublereal *rkn, doublereal *ws, doublereal *tak,
    doublereal *z__, doublereal *g, doublereal *fstab, doublereal *qp,
    doublereal *densa, doublereal *cp, doublereal *hneu, doublereal *pr,
    doublereal *ea, doublereal *tk, doublereal *dens, doublereal *cs,
    doublereal *rs, doublereal *tavek, doublereal *qsn, doublereal *qli,
    doublereal *fc, doublereal *es, doublereal *sbc, doublereal *qnetob,
    integer *iradfl);
doublereal rkinst_(doublereal *rkn, doublereal *ws, doublereal *ta, doublereal *ts,
    doublereal *z__, doublereal *g, doublereal *fstab);
doublereal partsnow_(doublereal *p, doublereal *ta, doublereal *tr, doublereal *ts);
doublereal qpf_(doublereal *pr, doublereal *ta, doublereal *to, doublereal *ps,
    doublereal *rhow, doublereal *hf, doublereal *cw, doublereal *cs);
int turbflux_(doublereal *pr, doublereal *ra, doublereal *ta, doublereal *tk,
    doublereal *ts, doublereal *z__, doublereal *g, doublereal *cp,
    doublereal *rkn, doublereal *ws, doublereal *ea, doublereal *rhow,
    doublereal *hneu, doublereal *qh, doublereal *qe, doublereal *e,
    doublereal *fstab);
doublereal fmelt_(doublereal *ub, doublereal *rhow, doublereal *w, doublereal *hf,
    doublereal *lc, doublereal *rid, doublereal *ks, doublereal *prain);
doublereal svp_(doublereal *t);
doublereal svpi_(doublereal *t);
doublereal svpw_(doublereal *t);
int prehelp_(doublereal *w1, doublereal *w, doublereal *dt, doublereal *fm,
    doublereal *fm1, doublereal *fac, doublereal *ps, doublereal *prain,
    doublereal *e, doublereal *rhow, doublereal *hf, doublereal *q,
    doublereal *qm, doublereal *mr, doublereal *qe, doublereal *hneu);
doublereal albedo_(doublereal *tausn, doublereal *coszen, doublereal *d__, doublereal *aep,
    doublereal *abg, doublereal *avo, doublereal *airo);
int agesn_(doublereal *tausn, doublereal *dt, doublereal *ps, doublereal *tsurf,
    doublereal *tk);



static thread_local integer c__5 = 5;
static thread_local integer c__1 = 1;
static thread_local doublereal c_b42 = 0.;
static thread_local doublereal c_b43 = 1.;
static thread_local doublereal c_b44 = 2.;
static thread_local integer c__9 = 9;

 int snow_(doublereal *dt, integer *nt, doublereal *input,
	doublereal *sitev, doublereal *statev, doublereal *param, integer *
	iflag, doublereal *cump, doublereal *cume, doublereal *cummr,
	doublereal *outv)
{

    integer i__1{};
    doublereal d__1{};

    extern doublereal partsnow_(doublereal *, doublereal *, doublereal *,
	    doublereal *);
    doublereal a{}, e{}, g{};
    integer i__{};
    doublereal k{}, p{}, q{}, w{}, z__{};
    extern  int predicorr_(doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, integer *, doublereal *, doublereal *, doublereal *,
	     doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *);
    doublereal cd{}, fc{}, df{}, cg{}, de{}, hf{}, lc{}, cp{}, ra{}, ta{}, cs{}, ub{}, qg{}, es{}, cw{}, rh{}, qh{}, ks{}, mr{}, tk{}, qe{}, pr{}, to{}, rs{}, tr{}, ts{}, ps{}, zo{}, ws{}, qm{}, fm{}, abg{}, cde{}, cdh{}, hff{}, aep{}, sbc{}, rid{}, avo{}, qli{}, rho{}, rkn{}, qsi{}, hneu{}, rhoi{}, rhog{}, tave{};
    extern doublereal tavg_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *);
    doublereal qnet{}, rrho{}, rhow{}, anir0{};
    integer pflag{};
    extern  int agesn_(doublereal *, doublereal *, doublereal
	    *, doublereal *, doublereal *);
    doublereal fstab{}, prain{}, rrhoi{};
    integer ounit__{};
    doublereal tsurf{};
    extern doublereal albedo_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *);
    integer iradfl{};
    doublereal qnetob{}, coszen{};

    static thread_local cilist io___70 = { 0, 0, 0, 0, 0 };

    input -= 8;
    --sitev;
    --statev;
    --param;
    --iflag;
    --outv;

    ub = statev[1];
    w = statev[2];
    fc = sitev[1];
    df = sitev[2];
    pr = sitev[3];
    qg = sitev[4];
    aep = sitev[5];
    tr = param[1];
    ts = param[2];
    to = param[3];
    tk = param[4];
    es = param[5];
    sbc = param[6];
    hf = param[7];
    hneu = param[8];
    cw = param[9];
    cs = param[10];
    cg = param[11];
    cp = param[12];
    ra = param[13];
    k = param[14];
    z__ = param[15];
    zo = param[16];
    hff = param[17];
    rhoi = param[18];
    rhow = param[19];
    rho = param[20];
    rhog = param[21];
    lc = param[22];
    ks = param[23];
    de = param[24];
    rs = param[25];
    g = param[26];
    abg = param[27];
    avo = param[28];
    anir0 = param[29];
    fstab = param[30];
    iradfl = iflag[1];
    pflag = iflag[2];
    ounit__ = iflag[3];

    d__1 = log(z__ / zo);
    cd = k * k * hff / (d__1 * d__1) * (1 - fc * .8);
    rrhoi = rhoi / rhow;
    rrho = rho / rhow;
    rid = 1. / rrho - 1. / rrhoi;
    i__1 = *nt;
    for (i__ = 1; i__ <= i__1; ++i__) {
	ta = input[i__ * 7 + 1];
	p = input[i__ * 7 + 2];
	ws = input[i__ * 7 + 3];
	rh = input[i__ * 7 + 4];
	if (iradfl == 0) {
	    qsi = input[i__ * 7 + 5];
	    qli = input[i__ * 7 + 6];
	} else {
	    qnetob = input[i__ * 7 + 5];
	}
	coszen = input[i__ * 7 + 7];
	ps = partsnow_(&p, &ta, &tr, &ts);
	prain = p - ps;
	ps *= df;
	if (iflag[4] == 1) {
	    d__1 = w / rrho;
	    a = albedo_(&statev[3], &coszen, &d__1, &aep, &abg, &avo, &anir0);
	} else {
	    a = statev[3];
	}
	rkn = cd * ws;
	qsi *= 1. - fc;
	qli *= 1. - fc;
	qnetob *= 1. - fc;
	predicorr_(dt, &ub, &w, &a, &ta, &prain, &ps, &ws, &rh, &qsi, &qli, &
		iradfl, &rkn, &qnetob, &cdh, &cde, &rid, &param[1], &sitev[1],
		 &qh, &qe, &e, &mr, &qm, &q, &fm, &tsurf, &qnet);
	if (iflag[4] == 1) {
	    agesn_(&statev[3], dt, &ps, &tsurf, &tk);
	}
	*cump += (ps + prain) * *dt;
	*cume += e * *dt;
	*cummr += mr * *dt;
	tave = tavg_(&ub, &w, &rhow, &cs, &to, &rhog, &de, &cg, &hf);
	if (pflag == 1) {
	    io___70.ciunit = ounit__;
	    s_wsle(&io___70);
	    do_lio(&c__5, &c__1, (char *)&ub, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&w, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&statev[3], (ftnlen)sizeof(
		    doublereal));
	    do_lio(&c__5, &c__1, (char *)&prain, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&ps, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&a, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&qh, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&qe, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&e, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&mr, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&qm, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&q, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&fm, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&tave, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&tsurf, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&(*cump), (ftnlen)sizeof(doublereal))
		    ;
	    do_lio(&c__5, &c__1, (char *)&(*cume), (ftnlen)sizeof(doublereal))
		    ;
	    do_lio(&c__5, &c__1, (char *)&(*cummr), (ftnlen)sizeof(doublereal)
		    );
	    do_lio(&c__5, &c__1, (char *)&qnet, (ftnlen)sizeof(doublereal));
	    e_wsle();
	}

    }
    statev[1] = ub;
    statev[2] = w;
    outv[1] = prain;
    outv[2] = ps;
    outv[3] = a;
    outv[4] = qh;
    outv[5] = qe;
    outv[6] = e;
    outv[7] = mr;
    outv[8] = qm;
    outv[9] = q;
    outv[10] = fm;
    outv[11] = tave;
    outv[12] = tsurf;
    outv[13] = qnet;
    return 0;
}

 int predicorr_(doublereal *dt, doublereal *ub, doublereal *w,
	 doublereal *a, doublereal *ta, doublereal *prain, doublereal *ps,
	doublereal *ws, doublereal *rh, doublereal *qsi, doublereal *qli,
	integer *iradfl, doublereal *rkn, doublereal *qnetob, doublereal *cdh,
	 doublereal *cde, doublereal *rid, doublereal *param, doublereal *
	sitev, doublereal *qh, doublereal *qe, doublereal *e, doublereal *mr,
	doublereal *qm, doublereal *q, doublereal *fm, doublereal *tsurf,
	doublereal *qnet)
{

    static thread_local doublereal wtol = .025;
    static thread_local doublereal utol = 2e3;
    static thread_local integer ncall = 0;

    doublereal d__1{}, d__2{};

    doublereal r__{}, e1{}, e2{}, q1{}, w1{}, w2{}, ae{}, hf{}, fm1{}, qe1{}, ub1{}, ub2{}, qh1{}, qm1{}, mr1{};
    extern  int qfm_(doublereal *, doublereal *, doublereal *,
	     doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *, integer *,
	     doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *);
    integer imax{};
    doublereal hneu{}, rhow{}, qnet1{};
    integer niter{};
    doublereal tsurf1{};
    extern  int prehelp_(doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *);

    --sitev;
    --param;

    ++ncall;
    hf = param[7];
    rhow = param[19];
    hneu = param[8];
    qfm_(ub, w, a, ta, prain, ps, ws, rh, qsi, qli, rkn, iradfl, qnetob, cdh,
	    cde, rid, &param[1], &sitev[1], fm, q, qm, mr, qe, e, tsurf, qh,
	    qnet);
    w1 = *w + *dt * *fm;
    if (w1 < 0.) {
	w1 = 0.;
	prehelp_(&w1, w, dt, fm, &c_b42, &c_b43, ps, prain, e, &rhow, &hf, q,
		qm, mr, qe, &hneu);
    }
    ub1 = *ub + *dt * *q;
    q1 = *q;
    fm1 = *fm;
    qh1 = *qh;
    qe1 = *qe;
    e1 = *e;
    mr1 = *mr;
    qm1 = *qm;
    tsurf1 = *tsurf;
    qnet1 = *qnet;
    qfm_(&ub1, &w1, a, ta, prain, ps, ws, rh, qsi, qli, rkn, iradfl, qnetob,
	    cdh, cde, rid, &param[1], &sitev[1], fm, q, qm, mr, qe, e, tsurf,
	    qh, qnet);
    w2 = *w + *dt / 2. * (fm1 + *fm);
    if (w2 < 0.) {
	w2 = 0.;
	prehelp_(&w2, w, dt, fm, &fm1, &c_b44, ps, prain, e, &rhow, &hf, q,
		qm, mr, qe, &hneu);
    }
    ub2 = *ub + *dt / 2. * (q1 + *q);
    niter = -3;
    imax = 1;
L1:
    if (((d__1 = w2 - w1, abs(d__1)) > wtol || (d__2 = ub2 - ub1, abs(d__2))
	    > utol) && niter < imax) {
	w1 = w2;
	ub1 = ub2;
	qfm_(&ub1, &w1, a, ta, prain, ps, ws, rh, qsi, qli, rkn, iradfl,
		qnetob, cdh, cde, rid, &param[1], &sitev[1], fm, q, qm, mr,
		qe, e, tsurf, qh, qnet);
	w2 = *w + *dt / 2. * (fm1 + *fm);
	if (w2 < 0.) {
	    w2 = 0.;
	    prehelp_(&w2, w, dt, fm, &fm1, &c_b44, ps, prain, e, &rhow, &hf,
		    q, qm, mr, qe, &hneu);
	}
	ub2 = *ub + *dt / 2. * (q1 + *q);
	++niter;
	if (niter >= 1) {
	    ae = (q1 + *q + qm1 + *qm) * .5 * *dt;
	    if (*ub > 0. && ae > 0.) {
		r__ = *w / *ub;
		e2 = (*e + e1) * .5;
		ub2 = (rhow * hf * (*w + (*ps + *prain - e2) * *dt) - ae - *
			ub) / (rhow * hf * r__ - 1);
		w2 = r__ * ub2;
		*qm = (ae + *ub - ub2) / *dt;
		*mr = *qm / (rhow * hf);
		*q = ae / *dt - *qm;
		qm1 = *qm;
		mr1 = *mr;
		q1 = *q;
	    }
	}
	goto L1;
    }
    *w = w2;
    *ub = ub2;
    *qh = (*qh + qh1) * .5;
    *qe = (*qe + qe1) * .5;
    *e = (*e + e1) * .5;
    *mr = (*mr + mr1) * .5;
    *qm = (*qm + qm1) * .5;
    *tsurf = (*tsurf + tsurf1) * .5;
    *qnet = (*qnet + qnet1) * .5;
    *q = (*q + q1) * .5;
    return 0;
}

 int qfm_(doublereal *ub, doublereal *w, doublereal *a,
	doublereal *ta, doublereal *prain, doublereal *ps, doublereal *ws,
	doublereal *rh, doublereal *qsi, doublereal *qli, doublereal *rkn,
	integer *iradfl, doublereal *qnetob, doublereal *cdh, doublereal *cde,
	 doublereal *rid, doublereal *param, doublereal *sitev, doublereal *
	fm, doublereal *q, doublereal *qm, doublereal *mr, doublereal *qe,
	doublereal *e, doublereal *tsurf, doublereal *qh, doublereal *qnet)
{

    doublereal d__1{};

    extern  int turbflux_(doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *);
    doublereal g{}, k{}, z__{}, ea{}, fc{}, cg{}, de{}, hf{}, lc{}, cp{}, qg{}, es{}, cw{}, cs{}, ks{}, tk{}, pr{}, to{}, ra{}, rs{}, qp{}, zo{}, hff{}, sbc{}, qle{};
    extern doublereal qpf_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *);
    doublereal rho{}, hneu{}, rhog{}, rhoi{}, tave{};
    extern doublereal tavg_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *);
    doublereal rhow{};
    extern doublereal svpw_(doublereal *);
    doublereal fstab{};
    extern doublereal fmelt_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *);
    doublereal qlnet{};
    extern doublereal srftmp_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, integer *, doublereal *, doublereal *, doublereal *,
	     doublereal *, doublereal *);

    --sitev;
    --param;

    fc = sitev[1];
    pr = sitev[3];
    qg = sitev[4];
    to = param[3];
    tk = param[4];
    es = param[5];
    sbc = param[6];
    hf = param[7];
    hneu = param[8];
    cw = param[9];
    cs = param[10];
    cg = param[11];
    cp = param[12];
    ra = param[13];
    k = param[14];
    z__ = param[15];
    zo = param[16];
    hff = param[17];
    rhoi = param[18];
    rhow = param[19];
    rho = param[20];
    rhog = param[21];
    lc = param[22];
    ks = param[23];
    de = param[24];
    rs = param[25];
    g = param[26];
    fstab = param[30];
    ea = svpw_(ta) * *rh;
    qp = qpf_(prain, ta, &to, ps, &rhow, &hf, &cw, &cs);
    tave = tavg_(ub, w, &rhow, &cs, &to, &rhog, &de, &cg, &hf);
    *tsurf = srftmp_(qsi, a, qli, &qp, &ea, ta, &tave, &tk, &pr, &ra, &cp, &
	    rho, rkn, &hneu, &es, &sbc, &cs, &rs, w, qnetob, iradfl, ws, &z__,
	     &g, &fc, &fstab);

    d__1 = *tsurf + tk, d__1 *= d__1;
    qle = (1 - fc) * es * sbc * (d__1 * d__1);
    qlnet = *qli - qle;
    turbflux_(&pr, &ra, ta, &tk, tsurf, &z__, &g, &cp, rkn, ws, &ea, &rhow, &
	    hneu, qh, qe, e, &fstab);
    *mr = fmelt_(ub, &rhow, w, &hf, &lc, rid, &ks, prain);
    *qm = *mr * rhow * (hf + (tave - to) * cw);
    if (*iradfl == 0) {
	*qnet = *qsi * (1. - *a) + qlnet;
    } else {
	*qnet = *qnetob;
    }
    *q = *qnet + qp + qg + *qh + *qe - *qm;
    *fm = *prain + *ps - *mr - *e;
    return 0;
}

doublereal tavg_(doublereal *ub, doublereal *w, doublereal *rhow, doublereal *
	cs, doublereal *to, doublereal *rhog, doublereal *de, doublereal *cg,
	doublereal *hf)
{

    doublereal ret_val{};

    doublereal al{}, ts{}, chc{}, shc{}, snhc{};

    snhc = *rhow * *w * *cs;
    shc = *rhog * *de * *cg;
    chc = snhc + shc;
    if (*ub <= 0.) {
	ts = *ub / chc;
    } else {
	al = *ub / (*rhow * *hf);
	if (*w > al) {
	    ts = *to;
	} else {
	    ts = (*ub - *w * *rhow * *hf) / chc;
	}
    }
    ret_val = ts;
    return ret_val;
}

doublereal srftmp_(doublereal *qsi, doublereal *a, doublereal *qli,
	doublereal *qpin, doublereal *ea, doublereal *ta, doublereal *tave,
	doublereal *tk, doublereal *pr, doublereal *ra, doublereal *cp,
	doublereal *rho, doublereal *rkn, doublereal *hneu, doublereal *es,
	doublereal *sbc, doublereal *cs, doublereal *rs, doublereal *w,
	doublereal *qnetob, integer *iradfl, doublereal *ws, doublereal *z__,
	doublereal *g, doublereal *fc, doublereal *fstab)
{

    static thread_local doublereal fff = .999;
    static thread_local doublereal tol = 1e-4;

    doublereal ret_val{}, d__1{};

    doublereal f1{}, f2{}, er{}, qp{}, ts{}, tak{}, qsn{}, dens{}, densa{}, tavek{};
    integer niter{};
    extern doublereal surfeb_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, integer *);
    doublereal tslast{};

    static thread_local cilist io___147 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___148 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___149 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___150 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___151 = { 0, 6, 0, 0, 0 };

    qsn = *qsi * (1. - *a);
    tak = *ta + *tk;
    tavek = *tave + *tk;
    densa = *pr / (*ra * tak);
    dens = *rho;
    qp = *qpin;
    if (*w <= 0. && qp > 0.) {
	qp = 0.;
    }
    ts = tak;
    er = 1.;
    niter = 0;
L1:
    if (er > tol && niter < 20) {
	tslast = ts;
	f1 = surfeb_(&ts, rkn, ws, &tak, z__, g, fstab, &qp, &densa, cp, hneu,
		 pr, ea, tk, &dens, cs, rs, &tavek, &qsn, qli, fc, es, sbc,
		qnetob, iradfl);
	d__1 = fff * ts;
	f2 = surfeb_(&d__1, rkn, ws, &tak, z__, g, fstab, &qp, &densa, cp,
		hneu, pr, ea, tk, &dens, cs, rs, &tavek, &qsn, qli, fc, es,
		sbc, qnetob, iradfl);
	ts -= (1. - fff) * ts * f1 / (f1 - f2);
	er = (d__1 = ts - tslast, abs(d__1));
	++niter;
	goto L1;
    }
    ts -= *tk;
    if (*w > 0. && ts > 0.) {
	ret_val = 0.;
    } else {
	ret_val = ts;
    }
    if (niter >= 20) {
	s_wsle(&io___147);
	do_lio(&c__9, &c__1, "More than 20 iterations in srftmp", (ftnlen)33);
	e_wsle();
	s_wsle(&io___148);
	do_lio(&c__9, &c__1, "This may not be a critical problem", (ftnlen)34)
		;
	e_wsle();
	s_wsle(&io___149);
	do_lio(&c__9, &c__1, "Snow surface temperature T = ", (ftnlen)29);
	do_lio(&c__5, &c__1, (char *)&ret_val, (ftnlen)sizeof(doublereal));
	e_wsle();
	s_wsle(&io___150);
	do_lio(&c__9, &c__1, "If this is happening a lot and looks ", (ftnlen)
		37);
	e_wsle();
	s_wsle(&io___151);
	do_lio(&c__9, &c__1, "unreasonable DO (Yes YOU) something about it.",
		(ftnlen)45);
	e_wsle();
    }
    return ret_val;
}

doublereal surfeb_(doublereal *ts, doublereal *rkn, doublereal *ws,
	doublereal *tak, doublereal *z__, doublereal *g, doublereal *fstab,
	doublereal *qp, doublereal *densa, doublereal *cp, doublereal *hneu,
	doublereal *pr, doublereal *ea, doublereal *tk, doublereal *dens,
	doublereal *cs, doublereal *rs, doublereal *tavek, doublereal *qsn,
	doublereal *qli, doublereal *fc, doublereal *es, doublereal *sbc,
	doublereal *qnetob, integer *iradfl)
{

    doublereal ret_val{}, d__1{};

    extern doublereal svp_(doublereal *);
    doublereal rkin{};
    extern doublereal rkinst_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *);

    rkin = rkinst_(rkn, ws, tak, ts, z__, g, fstab);
    d__1 = *ts - *tk;
    ret_val = *qp + rkin * *densa * *cp * (*tak - *ts) + *hneu * *densa *
	    .622 * rkin / *pr * (*ea - svp_(&d__1)) - *dens * *cs * *rs * (*
	    ts - *tavek);
    if (*iradfl == 0) {

	d__1 = *ts, d__1 *= d__1;
	ret_val = ret_val + *qsn + *qli - (1. - *fc) * *es * *sbc * (d__1 *
		d__1);
    } else {
	ret_val += *qnetob;
    }
    return ret_val;
}

doublereal rkinst_(doublereal *rkn, doublereal *ws, doublereal *ta,
	doublereal *ts, doublereal *z__, doublereal *g, doublereal *fstab)
{

    doublereal ret_val{};

    doublereal rich{};

    if (*ws <= 0.) {
	ret_val = 0.;
    } else {
	rich = *g * (*ta - *ts) * *z__ / (*ws * *ws * *ta);
	if (rich > 0.) {
	    ret_val = *rkn / (rich * 10. + 1.);
	} else {
	    ret_val = *rkn * (1 - rich * 10.);
	}
    }
    ret_val = *rkn + *fstab * (ret_val - *rkn);
    return ret_val;
}

doublereal partsnow_(doublereal *p, doublereal *ta, doublereal *tr,
	doublereal *ts)
{

    doublereal ret_val{};

    if (snow_fraction >= 0) return *p * snow_fraction;
    if (*ta < *ts) {
	ret_val = *p;
    } else if (*ta > *tr) {
	ret_val = 0.;
    } else {
	ret_val = *p * (*tr - *ta) / (*tr - *ts);
    }
    return ret_val;
}

doublereal qpf_(doublereal *pr, doublereal *ta, doublereal *to, doublereal *
	ps, doublereal *rhow, doublereal *hf, doublereal *cw, doublereal *cs)
{

    doublereal ret_val{};

    doublereal train{}, tsnow{};

    if (*ta > *to) {
	train = *ta;
	tsnow = *to;
    } else {
	train = *to;
	tsnow = *ta;
    }
    ret_val = *pr * *rhow * (*hf + *cw * (train - *to)) + *ps * *rhow * *cs *
	    (tsnow - *to);
    return ret_val;
}

 int turbflux_(doublereal *pr, doublereal *ra, doublereal *ta,
	 doublereal *tk, doublereal *ts, doublereal *z__, doublereal *g,
	doublereal *cp, doublereal *rkn, doublereal *ws, doublereal *ea,
	doublereal *rhow, doublereal *hneu, doublereal *qh, doublereal *qe,
	doublereal *e, doublereal *fstab)
{
    doublereal es{}, tak{}, tsk{}, rhoa{}, rkin{};
    extern doublereal svpi_(doublereal *), rkinst_(doublereal *, doublereal *,
	     doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *);

    tak = *ta + *tk;
    tsk = *ts + *tk;
    rkin = rkinst_(rkn, ws, &tak, &tsk, z__, g, fstab);
    rhoa = *pr / (*ra * tak);
    *qh = rhoa * (*ta - *ts) * *cp * rkin;
    es = svpi_(ts);
    *qe = *hneu * .622 / (*ra * tak) * rkin * (*ea - es);
    *e = -(*qe) / (*rhow * *hneu);
    return 0;
}

doublereal fmelt_(doublereal *ub, doublereal *rhow, doublereal *w, doublereal
	*hf, doublereal *lc, doublereal *rid, doublereal *ks, doublereal *
	prain)
{

    doublereal ret_val{}, d__1{};

    doublereal ss{}, uu{};

    static thread_local cilist io___163 = { 0, 6, 0, 0, 0 };

    uu = 0.;
    if (*ub < 0.) {
	ret_val = 0.;
    } else if (*w <= 0.) {
	ret_val = *prain;
	if (*prain <= 0.) {
	    ret_val = 0.;
	}
    } else {
	uu = *ub / (*rhow * *w * *hf);
	if (uu > .99) {
	    uu = .99;
	}
	if (uu / (1 - uu) <= *lc) {
	    ss = 0.;
	} else {
	    ss = (uu / ((1 - uu) * *rid) - *lc / *rid) / (1 - *lc / *rid);
	}

	d__1 = ss;
	ret_val = *ks * (d__1 * (d__1 * d__1));
    }
    if (ret_val < 0.) {
	s_wsle(&io___163);
	do_lio(&c__9, &c__1, "FMELT is NEGATIVE!", (ftnlen)18);
	e_wsle();
	s_stop("", (ftnlen)0);
    }
    return ret_val;
}

doublereal svp_(doublereal *t)
{

    doublereal ret_val{};

    extern doublereal svpi_(doublereal *), svpw_(doublereal *);

    if (*t >= 0.) {
	ret_val = svpw_(t);
    } else {
	ret_val = svpi_(t);
    }
    return ret_val;
}

doublereal svpi_(doublereal *t)
{

    doublereal ret_val{};

    ret_val = *t * (*t * (*t * (*t * (*t * (*t * 1.838826904e-10 +
	    4.838803174e-8) + 5.82472028e-6) + 4.176223716e-4) + .01886013408)
	     + .503469897) + 6.109177956;
    ret_val *= 100;
    return ret_val;
}

doublereal svpw_(doublereal *t)
{

    doublereal ret_val{};

    ret_val = *t * (*t * (*t * (*t * (*t * (*t * 6.136820929e-11 +
	    2.034080948e-8) + 3.031240936e-6) + 2.650648471e-4) +
	    .01428945805) + .4436518521) + 6.107799961;
    ret_val *= 100;
    return ret_val;
}

 int prehelp_(doublereal *w1, doublereal *w, doublereal *dt,
	doublereal *fm, doublereal *fm1, doublereal *fac, doublereal *ps,
	doublereal *prain, doublereal *e, doublereal *rhow, doublereal *hf,
	doublereal *q, doublereal *qm, doublereal *mr, doublereal *qe,
	doublereal *hneu)
{

    doublereal d__1{}, d__2{};

    doublereal qother{};

    *fm = (*w1 - *w) / *dt * *fac - *fm1;

    d__1 = 0., d__2 = *ps + *prain - *fm - *e;
    *mr = max(d__1,d__2);
    *e = *ps + *prain - *fm - *mr;
    qother = *q + *qm - *qe;
    *qm = *mr * *rhow * *hf;
    *qe = -(*e) * *rhow * *hneu;
    *q = qother - *qm + *qe;
    return 0;
}

doublereal albedo_(doublereal *tausn, doublereal *coszen, doublereal *d__,
	doublereal *aep, doublereal *abg, doublereal *avo, doublereal *airo)
{

    doublereal ret_val{};

    doublereal b{}, cn{}, cs{}, rr{}, avd{}, fage{}, aird{}, anir{}, fzen{}, avis{};

    b = 2.;
    cs = visible_age;
    cn = infrared_age;
    fage = *tausn / (*tausn + 1.);
    if (*coszen < .5) {
	fzen = 1. / b * ((b + 1.) / (b * 2. * *coszen + 1.) - 1.);
    } else {
	fzen = 0.;
    }
    avd = (1. - cs * fage) * *avo;
    avis = avd + fzen * .4 * (1. - avd);
    aird = (1. - cn * fage) * *airo;
    anir = aird + fzen * .4 * (1. - aird);
    ret_val = (avis + anir) / 2.;
    if (*d__ < *aep) {
	rr = (1. - *d__ / *aep) * exp(-(*d__) * .5 / *aep);
	ret_val = rr * *abg + (1. - rr) * ret_val;
    }
    return ret_val;
}

 int agesn_(doublereal *tausn, doublereal *dt, doublereal *ps,
	 doublereal *tsurf, doublereal *tk)
{

    doublereal d__1{}, d__2{};

    doublereal r1{}, r2{}, r3{}, tsk{};

    tsk = *tsurf + *tk;
    r1 = exp((1. / *tk - 1. / tsk) * 5e3);

    d__1 = r1, d__1 *= d__1, d__2 = d__1, d__1 *= d__1;
    r2 = d__2 * (d__1 * d__1);
    if (r2 > 1.) {
	r2 = 1.;
    }
    r3 = age_dust;

    d__1 = (*tausn + (r1 + r2 + r3) * .0036 * *dt) * (1. - *ps * 100. * *dt);
    *tausn = max(d__1,0.);
    return 0;
}


}
namespace force_restore {

// Namespace declarations give every translated helper internal linkage before
// its block-scope extern declarations are seen by MSVC (including with LTCG).
int predicorr_(doublereal *dt, doublereal *ub, doublereal *w, doublereal *a,
    doublereal *ta, doublereal *prain, doublereal *ps, doublereal *ws,
    doublereal *rh, doublereal *qsi, doublereal *qli, integer *iradfl,
    doublereal *rkn, doublereal *qnetob, doublereal *cdh, doublereal *cde,
    doublereal *rid, doublereal *param, doublereal *sitev, integer *itsmethod,
    doublereal *mtime, doublereal *qh, doublereal *qe, doublereal *e,
    doublereal *mr, doublereal *qm, doublereal *q, doublereal *fm,
    doublereal *tsurf, doublereal *tave, doublereal *qnet, doublereal *refdepth,
    doublereal *totalrefdepth, doublereal *smelt, doublereal *gsurf);
int qfm_(doublereal *ub, doublereal *w, doublereal *a, doublereal *ta,
    doublereal *prain, doublereal *ps, doublereal *ws, doublereal *rh,
    doublereal *qsi, doublereal *qli, doublereal *rkn, integer *iradfl,
    doublereal *qnetob, doublereal *cdh, doublereal *cde, doublereal *rid,
    doublereal *param, doublereal *sitev, integer *itsmethod, doublereal *mtime,
    doublereal *fm, doublereal *q, doublereal *qm, doublereal *mr,
    doublereal *qe, doublereal *e, doublereal *tsurf, doublereal *tave,
    doublereal *qh, doublereal *qnet, doublereal *dt, doublereal *refdepth,
    doublereal *totalrefdepth, doublereal *smelt);
doublereal tavg_(doublereal *ub, doublereal *w, doublereal *rhow, doublereal *cs,
    doublereal *to, doublereal *rhog, doublereal *de, doublereal *cg,
    doublereal *hf);
doublereal srftmp_(doublereal *qsi, doublereal *a, doublereal *qli, doublereal *qpin,
    doublereal *ea, doublereal *ta, doublereal *tave, doublereal *tk,
    doublereal *pr, doublereal *ra, doublereal *cp, doublereal *rho,
    doublereal *rkn, doublereal *hneu, doublereal *es, doublereal *sbc,
    doublereal *cs, doublereal *rs, doublereal *w, doublereal *qnetob,
    integer *iradfl, doublereal *ws, doublereal *z__, doublereal *g,
    doublereal *fc, doublereal *fstab, doublereal *mtime, doublereal *param,
    integer *itsmethod, doublereal *dt, doublereal *ub, doublereal *refdepth,
    doublereal *smelt);
doublereal surfeb_(doublereal *ts, doublereal *rkn, doublereal *ws, doublereal *tak,
    doublereal *z__, doublereal *g, doublereal *fstab, doublereal *qp,
    doublereal *densa, doublereal *cp, doublereal *hneu, doublereal *pr,
    doublereal *ea, doublereal *tk, doublereal *dens, doublereal *cs,
    doublereal *rs, doublereal *tavek, doublereal *qsn, doublereal *qli,
    doublereal *fc, doublereal *es, doublereal *sbc, doublereal *qnetob,
    integer *iradfl, doublereal *mtime, doublereal *param, integer *itsmethod,
    doublereal *w, doublereal *dt, doublereal *ub, doublereal *refdepth);
doublereal qcest_(doublereal *ts, doublereal *rkn, doublereal *ws, doublereal *tak,
    doublereal *z__, doublereal *g, doublereal *fstab, doublereal *qp,
    doublereal *densa, doublereal *cp, doublereal *hneu, doublereal *pr,
    doublereal *ea, doublereal *tk, doublereal *dens, doublereal *cs,
    doublereal *rs, doublereal *tavek, doublereal *qsn, doublereal *qli,
    doublereal *fc, doublereal *es, doublereal *sbc, doublereal *qnetob,
    integer *iradfl, doublereal *mtime, doublereal *param, integer *itsmethod,
    doublereal *w, doublereal *dt);
doublereal rkinst_(doublereal *rkn, doublereal *ws, doublereal *ta, doublereal *ts,
    doublereal *z__, doublereal *g, doublereal *fstab);
doublereal partsnow_(doublereal *p, doublereal *ta, doublereal *tr, doublereal *ts);
doublereal qpf_(doublereal *pr, doublereal *ta, doublereal *to, doublereal *ps,
    doublereal *rhow, doublereal *hf, doublereal *cw, doublereal *cs);
int turbflux_(doublereal *pr, doublereal *ra, doublereal *ta, doublereal *tk,
    doublereal *ts, doublereal *z__, doublereal *g, doublereal *cp,
    doublereal *rkn, doublereal *ws, doublereal *ea, doublereal *rhow,
    doublereal *hneu, doublereal *qh, doublereal *qe, doublereal *e,
    doublereal *fstab);
doublereal fmelt_(doublereal *ub, doublereal *rhow, doublereal *w, doublereal *hf,
    doublereal *lc, doublereal *rid, doublereal *ks, doublereal *prain);
doublereal svp_(doublereal *t);
doublereal svpi_(doublereal *t);
doublereal svpw_(doublereal *t);
int prehelp_(doublereal *w1, doublereal *w, doublereal *dt, doublereal *fm,
    doublereal *fm1, doublereal *fac, doublereal *ps, doublereal *prain,
    doublereal *e, doublereal *rhow, doublereal *hf, doublereal *q,
    doublereal *qm, doublereal *mr, doublereal *qe, doublereal *hneu);
doublereal albedo_(doublereal *tausn, doublereal *coszen, doublereal *d__, doublereal *aep,
    doublereal *abg, doublereal *avo, doublereal *airo);
int agesn_(doublereal *tausn, doublereal *dt, doublereal *ps, doublereal *tsurf,
    doublereal *tk, doublereal *dnews);
doublereal lane_(doublereal *lans, doublereal *lang, doublereal *zs, doublereal *rho,
    doublereal *rhog, doublereal *cs, doublereal *cg, doublereal *r__,
    doublereal *ze, doublereal *w1day);
doublereal refdep_(doublereal *flans, doublereal *a, doublereal *b, doublereal *hf,
    doublereal *rhom, doublereal *dt, doublereal *x1);
int grad_(doublereal *qc1, doublereal *qc2, doublereal *t1, doublereal *t2,
    doublereal *a, doublereal *b);
doublereal daily_ave__(doublereal *backup, integer *n, doublereal *a);



struct {
    doublereal ts_old__{}, tave_old__{}, ts_ave__{}, tave_ave__{};
} thread_local ts_save__;

#define ts_save__1 ts_save__

static thread_local doublereal c_b3 = -100.;
static thread_local integer c__9 = 9;
static thread_local integer c__1 = 1;
static thread_local integer c__5 = 5;
static thread_local doublereal c_b83 = 0.;
static thread_local doublereal c_b84 = 1.;
static thread_local doublereal c_b85 = 2.;
static thread_local doublereal c_b96 = -.01;
static thread_local integer c__3 = 3;
static thread_local doublereal c_b176 = .75;

 int snowueb2_(doublereal *dt, integer *nt, doublereal *input,
	 doublereal *sitev, doublereal *statev, doublereal *tsprevday,
	doublereal *taveprevday, integer *nstepday, doublereal *param,
	integer *iflag, doublereal *cump, doublereal *cume, doublereal *cummr,
	 doublereal *outv, doublereal *mtime)
{

    static thread_local doublereal to = 0.;
    static thread_local doublereal k = .4;
    static thread_local doublereal hff = 3600.;
    static thread_local doublereal rhoi = 917.;
    static thread_local doublereal rhow = 1e3;
    static thread_local doublereal tk = 273.15;
    static thread_local doublereal hf = 333.5;
    static thread_local doublereal cs = 2.09;

    integer i__1{}, i__2{};
    doublereal d__1{};

    doublereal refdepth{};
    extern doublereal partsnow_(doublereal *, doublereal *, doublereal *,
	    doublereal *);
    doublereal a{}, e{};
    integer i__{};
    doublereal p{}, q{}, w{}, z__{};
    extern doublereal daily_ave__(doublereal *, integer *, doublereal *);
    extern  int predicorr_(doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, integer *, doublereal *, doublereal *, doublereal *,
	     doublereal *, doublereal *, doublereal *, doublereal *, integer *
	    , doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *);
    integer itsmethod{};
    doublereal cd{}, dd{}, de{}, cg{}, fc{}, df{}, lc{};
    integer ii{};
    doublereal ta{}, ub{}, es{}, qg{};
    integer mm{};
    doublereal ks{}, mr{}, hr{}, pr{}, rh{}, tr{}, ts{}, ps{}, zo{}, ws{}, qh{}, qe{}, qm{}, fm{}, yy{}, rd1{}, abg{}, cde{}, cdh{}, aep{}, rid{}, avo{}, qli{}, rho{}, wlf{}, rkn{}, qsi{}, lang{}, lans{}, rhog{}, tref{}, tave{}, rhom{};
    extern doublereal tavg_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *);
    doublereal qnet{}, rrho{}, refdepth_old__{}, anir0{};
    integer pflag{};
    extern  int agesn_(doublereal *, doublereal *, doublereal
	    *, doublereal *, doublereal *, doublereal *);
    doublereal fstab{}, prain{}, dnews{}, rrhoi{}, smelt{}, gsurf{};
    integer ounit__{};
    doublereal tsurf{}, totalrefdepth{};
    extern doublereal albedo_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *);
    integer iradfl{};
    doublereal ub_old__{}, qnetob{}, coszen{};

    static thread_local cilist io___72 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___73 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___74 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___75 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___94 = { 0, 0, 0, 0, 0 };

    input -= 8;
    --sitev;
    --statev;
    --tsprevday;
    --taveprevday;
    --param;
    --iflag;
    --outv;
    --mtime;

    tr = param[1];
    ts = param[2];
    es = param[3];
    cg = param[4];
    z__ = param[5];
    zo = param[6];
    rho = param[7];
    rhog = param[8];
    lc = param[9];
    ks = param[10];
    de = param[11];
    abg = param[12];
    avo = param[13];
    anir0 = param[14];
    lans = param[15];
    lang = param[16];
    wlf = param[17];
    rd1 = param[18];
    fstab = param[19];
    tref = param[20];
    dnews = param[21];
    gsurf = param[22];
    ub = statev[1];
    w = statev[2];
    if (ub <= 0.) {
	refdepth = 0.;
	totalrefdepth = 0.;
    } else {
	refdepth = statev[4];
	totalrefdepth = statev[5];
    }
    ub_old__ = ub;
    refdepth_old__ = refdepth;
    fc = sitev[1];
    df = sitev[2];
    pr = sitev[3];
    qg = sitev[4];
    aep = sitev[5];
    iradfl = iflag[1];
    pflag = iflag[2];
    ounit__ = iflag[3];
    itsmethod = iflag[5];
    yy = mtime[1];
    mm = (integer) mtime[2];
    dd = mtime[3];
    hr = mtime[4];

    d__1 = log(z__ / zo);
    cd = k * k * hff / (d__1 * d__1) * (1 - fc * .8);
    rrhoi = rhoi / rhow;
    rrho = rho / rhow;
    rid = 1. / rrho - 1. / rrhoi;
    rhom = lc * rho;
    i__1 = *nt;
    for (i__ = 1; i__ <= i__1; ++i__) {
	ta = input[i__ * 7 + 1];
	p = input[i__ * 7 + 2];
	ws = input[i__ * 7 + 3];
	rh = input[i__ * 7 + 4];
	if (iradfl == 0) {
	    qsi = input[i__ * 7 + 5];
	    qli = input[i__ * 7 + 6];
	} else {
	    qnetob = input[i__ * 7 + 5];
	}
	coszen = input[i__ * 7 + 7];
	ts_save__1.ts_ave__ = mean_surface +
		tk;
	ts_save__1.tave_ave__ = mean_bulk
		+ tk;
	ts_save__1.ts_old__ = tsprevday[*nstepday] + tk;
	ts_save__1.tave_old__ = taveprevday[*nstepday] + tk;
	if (ts_save__1.ts_old__ < 0.) {
	    s_wsle(&io___72);
	    do_lio(&c__9, &c__1, "Invalid previous time step surface tempera"
		    "ture ", (ftnlen)47);
	    do_lio(&c__5, &c__1, (char *)&ts_save__1.ts_old__, (ftnlen)sizeof(
		    doublereal));
	    do_lio(&c__9, &c__1, " set to 273 K", (ftnlen)13);
	    e_wsle();
	    ts_save__1.ts_old__ = tk;
	}
	if (ts_save__1.tave_old__ < 0.) {
	    s_wsle(&io___73);
	    do_lio(&c__9, &c__1, "Invalid previous time step average tempera"
		    "ture ", (ftnlen)47);
	    do_lio(&c__5, &c__1, (char *)&ts_save__1.tave_old__, (ftnlen)
		    sizeof(doublereal));
	    do_lio(&c__9, &c__1, " set to 273 K", (ftnlen)13);
	    e_wsle();
	    ts_save__1.tave_old__ = tk;
	}
	if (ts_save__1.ts_ave__ < 0.) {
	    s_wsle(&io___74);
	    do_lio(&c__9, &c__1, "Invalid last 24 hr average surface tempera"
		    "ture ", (ftnlen)47);
	    do_lio(&c__5, &c__1, (char *)&ts_save__1.ts_ave__, (ftnlen)sizeof(
		    doublereal));
	    do_lio(&c__9, &c__1, " set to 273 K", (ftnlen)13);
	    e_wsle();
	    ts_save__1.ts_ave__ = tk;
	}
	if (ts_save__1.tave_ave__ < 0.) {
	    s_wsle(&io___75);
	    do_lio(&c__9, &c__1, "Invalid last 24 hr average temperature ", (
		    ftnlen)39);
	    do_lio(&c__5, &c__1, (char *)&ts_save__1.tave_ave__, (ftnlen)
		    sizeof(doublereal));
	    do_lio(&c__9, &c__1, " set to 273 K", (ftnlen)13);
	    e_wsle();
	    ts_save__1.tave_ave__ = tk;
	}
	ps = partsnow_(&p, &ta, &tr, &ts);
	prain = p - ps;
	ps *= df;
	if (iflag[4] == 1) {
	    d__1 = w / rrho;
	    a = albedo_(&statev[3], &coszen, &d__1, &aep, &abg, &avo, &anir0);
	} else {
	    a = statev[3];
	}
	rkn = cd * ws;
	qsi *= 1. - fc;
	qli *= 1. - fc;
	qnetob *= 1. - fc;
	predicorr_(dt, &ub, &w, &a, &ta, &prain, &ps, &ws, &rh, &qsi, &qli, &
		iradfl, &rkn, &qnetob, &cdh, &cde, &rid, &param[1], &sitev[1],
		 &itsmethod, &mtime[1], &qh, &qe, &e, &mr, &qm, &q, &fm, &
		tsurf, &tave, &qnet, &refdepth, &totalrefdepth, &smelt, &
		gsurf);
	tave = tavg_(&ub, &w, &rhow, &cs, &to, &rhog, &de, &cg, &hf);
	if (tave > 0.) {
	    mr += w / *dt;
	    qm += w / *dt * rhow * hf;
	    q -= w / *dt * rhow * hf;
	    ub -= w * rhow * hf; // removed water times latent heat (not a rate)
	    w = 0.;
	}

	if (iflag[4] == 1) {
	    agesn_(&statev[3], dt, &ps, &tsurf, &tk, &dnews);
	}
	*cump += (ps + prain) * *dt;
	*cume += e * *dt;
	*cummr += mr * *dt;
	tave = tavg_(&ub, &w, &rhow, &cs, &to, &rhog, &de, &cg, &hf);
	if (lc > 0.) {
	    if (refdepth > 0.) {
		totalrefdepth = refdepth;
	    } else {
		if (mr > 0. || ub > ub_old__ && ub > 0.) {
		    totalrefdepth = 0.;
		}
	    }
	} else if (mr > 0. || ub > ub_old__ && ub > 0.) {
	    totalrefdepth = 0.;
	}
	if (totalrefdepth < 0.) {
	    totalrefdepth = 0.;
	}
	i__2 = *nstepday - 1;
	for (ii = 1; ii <= i__2; ++ii) {
	    tsprevday[ii] = tsprevday[ii + 1];
	    taveprevday[ii] = taveprevday[ii + 1];

	}
	tsprevday[*nstepday] = tsurf;
	taveprevday[*nstepday] = tave;
	if (pflag == 1) {
	    io___94.ciunit = ounit__;
	    s_wsle(&io___94);
	    do_lio(&c__5, &c__1, (char *)&ub, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&w, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&statev[3], (ftnlen)sizeof(
		    doublereal));
	    do_lio(&c__5, &c__1, (char *)&prain, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&ps, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&a, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&qh, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&qe, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&e, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&mr, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&qm, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&q, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&fm, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&tave, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&tsurf, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&(*cump), (ftnlen)sizeof(doublereal))
		    ;
	    do_lio(&c__5, &c__1, (char *)&(*cume), (ftnlen)sizeof(doublereal))
		    ;
	    do_lio(&c__5, &c__1, (char *)&(*cummr), (ftnlen)sizeof(doublereal)
		    );
	    do_lio(&c__5, &c__1, (char *)&qnet, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&smelt, (ftnlen)sizeof(doublereal));
	    do_lio(&c__5, &c__1, (char *)&refdepth, (ftnlen)sizeof(doublereal)
		    );
	    do_lio(&c__5, &c__1, (char *)&totalrefdepth, (ftnlen)sizeof(
		    doublereal));
	    e_wsle();
	}

    }
    statev[1] = ub;
    statev[2] = w;
    statev[4] = refdepth;
    statev[5] = totalrefdepth;
    outv[1] = prain;
    outv[2] = ps;
    outv[3] = a;
    outv[4] = qh;
    outv[5] = qe;
    outv[6] = e;
    outv[7] = mr;
    outv[8] = qm;
    outv[9] = q;
    outv[10] = fm;
    outv[11] = tave;
    outv[12] = tsurf;
    outv[13] = qnet;
    outv[14] = smelt;
    return 0;
}

 int predicorr_(doublereal *dt, doublereal *ub, doublereal *w,
	 doublereal *a, doublereal *ta, doublereal *prain, doublereal *ps,
	doublereal *ws, doublereal *rh, doublereal *qsi, doublereal *qli,
	integer *iradfl, doublereal *rkn, doublereal *qnetob, doublereal *cdh,
	 doublereal *cde, doublereal *rid, doublereal *param, doublereal *
	sitev, integer *itsmethod, doublereal *mtime, doublereal *qh,
	doublereal *qe, doublereal *e, doublereal *mr, doublereal *qm,
	doublereal *q, doublereal *fm, doublereal *tsurf, doublereal *tave,
	doublereal *qnet, doublereal *refdepth, doublereal *totalrefdepth,
	doublereal *smelt, doublereal *gsurf)
{

    static thread_local doublereal rhow = 1e3;
    static thread_local doublereal wtol = .025;
    static thread_local doublereal utol = 2e3;
    static thread_local integer ncall = 0;
    static thread_local doublereal hf = 333.5;
    static thread_local doublereal hneu = 2834.;

    doublereal d__1{}, d__2{};

    doublereal r__{}, e1{}, e2{}, q1{}, w1{}, w2{}, ae{}, fm1{}, qe1{}, ub1{}, ub2{}, qh1{}, qm1{}, mr1{};
    extern  int qfm_(doublereal *, doublereal *, doublereal *,
	     doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *, integer *,
	     doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, integer *, doublereal *, doublereal *,
	     doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *);
    doublereal rlf{};
    integer imax{};
    doublereal qnet1{};
    integer niter{};
    doublereal smelt1{}, tsurf1{};
    extern  int prehelp_(doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *);

    static thread_local cilist io___134 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___135 = { 0, 6, 0, 0, 0 };

    --mtime;
    --sitev;
    --param;

    ++ncall;
    qfm_(ub, w, a, ta, prain, ps, ws, rh, qsi, qli, rkn, iradfl, qnetob, cdh,
	    cde, rid, &param[1], &sitev[1], itsmethod, &mtime[1], fm, q, qm,
	    mr, qe, e, tsurf, tave, qh, qnet, dt, refdepth, totalrefdepth,
	    smelt);
    w1 = *w + *dt * *fm;
    if (w1 < 0.) {
	w1 = 0.;
	prehelp_(&w1, w, dt, fm, &c_b83, &c_b84, ps, prain, e, &rhow, &hf, q,
		qm, mr, qe, &hneu);
    }
    ub1 = *ub + *dt * *q;
    q1 = *q;
    fm1 = *fm;
    qh1 = *qh;
    qe1 = *qe;
    e1 = *e;
    mr1 = *mr;
    smelt1 = *smelt;
    qm1 = *qm;
    tsurf1 = *tsurf;
    qnet1 = *qnet;
    qfm_(&ub1, &w1, a, ta, prain, ps, ws, rh, qsi, qli, rkn, iradfl, qnetob,
	    cdh, cde, rid, &param[1], &sitev[1], itsmethod, &mtime[1], fm, q,
	    qm, mr, qe, e, tsurf, tave, qh, qnet, dt, refdepth, totalrefdepth,
	     smelt);
    w2 = *w + *dt / 2. * (fm1 + *fm);
    if (w2 < 0.) {
	w2 = 0.;
	prehelp_(&w2, w, dt, fm, &fm1, &c_b85, ps, prain, e, &rhow, &hf, q,
		qm, mr, qe, &hneu);
    }
    ub2 = *ub + *dt / 2. * (q1 + *q);
    niter = -3;
    imax = 1;
L1:
    if (((d__1 = w2 - w1, abs(d__1)) > wtol || (d__2 = ub2 - ub1, abs(d__2))
	    > utol) && niter < imax) {
	w1 = w2;
	ub1 = ub2;
	qfm_(&ub1, &w1, a, ta, prain, ps, ws, rh, qsi, qli, rkn, iradfl,
		qnetob, cdh, cde, rid, &param[1], &sitev[1], itsmethod, &
		mtime[1], fm, q, qm, mr, qe, e, tsurf, tave, qh, qnet, dt,
		refdepth, totalrefdepth, smelt);
	w2 = *w + *dt / 2. * (fm1 + *fm);
	if (w2 < 0.) {
	    w2 = 0.;
	    prehelp_(&w2, w, dt, fm, &fm1, &c_b85, ps, prain, e, &rhow, &hf,
		    q, qm, mr, qe, &hneu);
	}
	ub2 = *ub + *dt / 2. * (q1 + *q);
	++niter;
	if (niter >= 1) {
	    ae = (q1 + *q + qm1 + *qm) * .5 * *dt;
	    if (*ub > 0. && ae > 0. && *w > 0.) {
		e2 = (*e + e1) * .5;
		rlf = (*ub + ae) / (rhow * *w * hf);
		if (rlf >= 1.) {
		    *mr = *w / *dt + (*ps + *prain - e2);
		    if (*mr < 0.) {
			*mr = 0.;
		    }
		    *qm = *mr * rhow * hf;
		    w2 = 0.;
		    ub2 = *ub + ae - *qm * *dt;
		} else {
		    r__ = *w / *ub;
		    ub2 = (rhow * hf * (*w + (*ps + *prain - e2) * *dt) - ae
			    - *ub) / (rhow * hf * r__ - 1);
		    w2 = r__ * ub2;
		    if (w2 < 0.) {
			w2 = 0.;
		    }
		    *mr = (*w - w2) / *dt - e2 + *ps + *prain;
		    if (*mr < 0.) {
			*mr = 0.;
			w2 = *w + (*ps + *prain - e2) * *dt;
			if (w2 < 0.) {
			    w2 = 0.;
			}
		    }
		    *qm = *mr * rhow * hf;
		    ub2 = *ub + ae - *qm * *dt;
		}
		if (*mr < 0.) {
		    s_wsle(&io___134);
		    do_lio(&c__9, &c__1, "Error - negative melt rate in snow",
			     (ftnlen)34);
		    e_wsle();
		}
		if (w2 < 0.) {
		    s_wsle(&io___135);
		    do_lio(&c__9, &c__1, "Error - negative w2 in snow", (
			    ftnlen)27);
		    e_wsle();
		}
		*q = ae / *dt - *qm;
		qm1 = *qm;
		mr1 = *mr;
		q1 = *q;
	    }
	}
	goto L1;
    }
    *w = w2;
    *ub = ub2;
    *qh = (*qh + qh1) * .5;
    *qe = (*qe + qe1) * .5;
    *e = (*e + e1) * .5;
    *mr = (*mr + mr1) * .5;
    *qm = (*qm + qm1) * .5;
    *tsurf = (*tsurf + tsurf1) * .5;
    *qnet = (*qnet + qnet1) * .5;
    *q = (*q + q1) * .5;
    *smelt = (*smelt + smelt1) * .5 / (hf * rhow);
    return 0;
}

 int qfm_(doublereal *ub, doublereal *w, doublereal *a,
	doublereal *ta, doublereal *prain, doublereal *ps, doublereal *ws,
	doublereal *rh, doublereal *qsi, doublereal *qli, doublereal *rkn,
	integer *iradfl, doublereal *qnetob, doublereal *cdh, doublereal *cde,
	 doublereal *rid, doublereal *param, doublereal *sitev, integer *
	itsmethod, doublereal *mtime, doublereal *fm, doublereal *q,
	doublereal *qm, doublereal *mr, doublereal *qe, doublereal *e,
	doublereal *tsurf, doublereal *tave, doublereal *qh, doublereal *qnet,
	 doublereal *dt, doublereal *refdepth, doublereal *totalrefdepth,
	doublereal *smelt)
{

    static thread_local doublereal to = 0.;
    static thread_local doublereal tk = 273.15;
    static thread_local doublereal sbc = 2.0747e-7;
    static thread_local doublereal hf = 333.5;
    static thread_local doublereal hneu = 2834.;
    static thread_local doublereal cw = 4.18;
    static thread_local doublereal cs = 2.09;
    static thread_local doublereal cp = 1.005;
    static thread_local doublereal ra = 287.;
    static thread_local doublereal rhow = 1e3;
    static thread_local doublereal g = 9.81;
    static thread_local doublereal w1day = .261799;

    doublereal d__1{};

    extern  int turbflux_(doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *);
    doublereal z__{}, x1{}, ea{}, de{}, cg{}, fc{}, lc{}, ks{}, es{}, pr{}, qg{}, ds{}, zo{}, rs{}, qp{}, qc1{}, qc2{}, rd1{}, abg{}, tak{}, qle{}, avo{};
    extern doublereal qpf_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *);
    doublereal rho{}, wlf{}, qsn{};
    extern  int grad_(doublereal *, doublereal *, doublereal *
	    , doublereal *, doublereal *, doublereal *);
    doublereal lang{}, dens{}, lans{}, rhog{}, tref{};
    extern doublereal tavg_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *);
    doublereal rhom{};
    extern doublereal svpw_(doublereal *);
    doublereal anir0{}, var_a__{}, var_b__{}, densa{}, fstab{};
    extern doublereal fmelt_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *);
    doublereal tavek{};
    extern doublereal qcest_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, integer *, doublereal *, doublereal *, integer *,
	    doublereal *, doublereal *);
    doublereal dnews{}, qlnet{}, gsurf{};
    extern doublereal refdep_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *), srftmp_(
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *, integer *,
	     doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, integer *, doublereal *,
	     doublereal *, doublereal *, doublereal *);
    doublereal fkappas{};

    --mtime;
    --sitev;
    --param;

    es = param[3];
    cg = param[4];
    z__ = param[5];
    zo = param[6];
    rho = param[7];
    rhog = param[8];
    lc = param[9];
    ks = param[10];
    de = param[11];
    abg = param[12];
    avo = param[13];
    anir0 = param[14];
    lans = param[15];
    lang = param[16];
    wlf = param[17];
    rd1 = param[18];
    fstab = param[19];
    tref = param[20];
    dnews = param[21];
    gsurf = param[22];
    fc = sitev[1];
    pr = sitev[3];
    qg = sitev[4];
    qsn = *qsi * (1. - *a);
    tak = *ta + tk;
    tavek = *tave + tk;
    densa = pr / (ra * tak);
    dens = rho;
    rhom = lc * rho;
    fkappas = lans / (rho * cs);
    ds = sqrt(fkappas * 2 / w1day);
    rs = lans / (ds * rd1 * rho * cs);
    ts_save__1.tave_old__ = *tave;
    ea = svpw_(ta) * *rh;
    qp = qpf_(prain, ta, &to, ps, &rhow, &hf, &cw, &cs);
    *tave = tavg_(ub, w, &rhow, &cs, &to, &rhog, &de, &cg, &hf);
    if (*ub > 0. && *ps <= 0. && *prain <= 0. && *totalrefdepth <= rd1 * ds &&
	     *w > 0.) {
	qc1 = qcest_(&tk, rkn, ws, &tak, &z__, &g, &fstab, &qp, &densa, &cp, &
		hneu, &pr, &ea, &tk, &dens, &cs, &rs, &tavek, &qsn, qli, &fc,
		&es, &sbc, qnetob, iradfl, &mtime[1], &param[1], itsmethod, w,
		 dt);
	d__1 = tk - .01;
	qc2 = qcest_(&d__1, rkn, ws, &tak, &z__, &g, &fstab, &qp, &densa, &cp,
		 &hneu, &pr, &ea, &tk, &dens, &cs, &rs, &tavek, &qsn, qli, &
		fc, &es, &sbc, qnetob, iradfl, &mtime[1], &param[1],
		itsmethod, w, dt);
	grad_(&qc1, &qc2, &c_b83, &c_b96, &var_a__, &var_b__);
	x1 = refdep_(&lans, &var_a__, &var_b__, &hf, &rhom, dt, refdepth);
	*refdepth = x1;
    } else {
	*refdepth = 0.;
    }
    *tsurf = srftmp_(qsi, a, qli, &qp, &ea, ta, tave, &tk, &pr, &ra, &cp, &
	    rho, rkn, &hneu, &es, &sbc, &cs, &rs, w, qnetob, iradfl, ws, &z__,
	     &g, &fc, &fstab, &mtime[1], &param[1], itsmethod, dt, ub,
	    refdepth, smelt);

    d__1 = *tsurf + tk, d__1 *= d__1;
    qle = (1 - fc) * es * sbc * (d__1 * d__1);
    qlnet = *qli - qle;
    turbflux_(&pr, &ra, ta, &tk, tsurf, &z__, &g, &cp, rkn, ws, &ea, &rhow, &
	    hneu, qh, qe, e, &fstab);
    *mr = fmelt_(ub, &rhow, w, &hf, &lc, rid, &ks, prain);
    *qm = *mr * rhow * (hf + (*tave - to) * cw);
    *mr += *smelt / (hf * rhow) * gsurf;
    *qm += *smelt * gsurf;
    if (*iradfl == 0) {
	*qnet = *qsi * (1. - *a) + qlnet;
    } else {
	*qnet = *qnetob;
    }
    *q = *qnet + qp + qg + *qh + *qe - *qm;
    *fm = *prain + *ps - *mr - *e;
    return 0;
}

doublereal tavg_(doublereal *ub, doublereal *w, doublereal *rhow, doublereal *
	cs, doublereal *to, doublereal *rhog, doublereal *de, doublereal *cg,
	doublereal *hf)
{

    doublereal ret_val{};

    doublereal al{}, ts{}, chc{}, shc{}, snhc{};

    snhc = *rhow * *w * *cs;
    shc = *rhog * *de * *cg;
    chc = snhc + shc;
    if (*ub <= 0.) {
	ts = *ub / chc;
    } else {
	al = *ub / (*rhow * *hf);
	if (*w > al) {
	    ts = *to;
	} else {
	    ts = (*ub - *w * *rhow * *hf) / chc;
	}
    }
    ret_val = ts;
    return ret_val;
}

doublereal srftmp_(doublereal *qsi, doublereal *a, doublereal *qli,
	doublereal *qpin, doublereal *ea, doublereal *ta, doublereal *tave,
	doublereal *tk, doublereal *pr, doublereal *ra, doublereal *cp,
	doublereal *rho, doublereal *rkn, doublereal *hneu, doublereal *es,
	doublereal *sbc, doublereal *cs, doublereal *rs, doublereal *w,
	doublereal *qnetob, integer *iradfl, doublereal *ws, doublereal *z__,
	doublereal *g, doublereal *fc, doublereal *fstab, doublereal *mtime,
	doublereal *param, integer *itsmethod, doublereal *dt, doublereal *ub,
	 doublereal *refdepth, doublereal *smelt)
{

    static thread_local doublereal tol = .05;
    static thread_local integer nitermax = 10;

    integer i__1{};
    doublereal ret_val{}, d__1{};

    doublereal f1{}, f2{};
    integer ibtowrite{};
    doublereal er{}, qp{}, ts{}, fff{}, flb{}, fub{}, tak{}, tlb{}, tub{}, qsn{}, dens{};
    integer iter{};
    doublereal densa{}, tavek{};
    integer niter{};
    extern doublereal surfeb_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, integer *, doublereal *, doublereal *, integer *,
	    doublereal *, doublereal *, doublereal *, doublereal *);
    doublereal tslast{};

    static thread_local cilist io___218 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___219 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___220 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___221 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___222 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___223 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___224 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___225 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___226 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___227 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___228 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___230 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___231 = { 0, 6, 0, 0, 0 };
    static thread_local cilist io___232 = { 0, 6, 0, 0, 0 };

    --param;
    --mtime;

    fff = (273 - tol) / 273.;
    qsn = *qsi * (1. - *a);
    tak = *ta + *tk;
    tavek = *tave + *tk;
    densa = *pr / (*ra * tak);
    dens = *rho;
    qp = *qpin;
    if (*w <= 0. && qp > 0.) {
	qp = 0.;
    }
    ts = tak;
    er = tol * 2;
    niter = 0;
L1:
    if (er > tol && niter < nitermax) {
	tslast = ts;
	f1 = surfeb_(&ts, rkn, ws, &tak, z__, g, fstab, &qp, &densa, cp, hneu,
		 pr, ea, tk, &dens, cs, rs, &tavek, &qsn, qli, fc, es, sbc,
		qnetob, iradfl, &mtime[1], &param[1], itsmethod, w, dt, ub,
		refdepth);
	d__1 = fff * ts;
	f2 = surfeb_(&d__1, rkn, ws, &tak, z__, g, fstab, &qp, &densa, cp,
		hneu, pr, ea, tk, &dens, cs, rs, &tavek, &qsn, qli, fc, es,
		sbc, qnetob, iradfl, &mtime[1], &param[1], itsmethod, w, dt,
		ub, refdepth);
	ts -= (1. - fff) * ts * f1 / (f1 - f2);
	if (ts < tak - 50) {
	    goto L11;
	}
	er = (d__1 = ts - tslast, abs(d__1));
	++niter;
	goto L1;
    }
    if (er <= tol) {
	goto L10;
    }
L11:
    tlb = tak - 20.;
    tub = tak + 10.;
    flb = surfeb_(&tlb, rkn, ws, &tak, z__, g, fstab, &qp, &densa, cp, hneu,
	    pr, ea, tk, &dens, cs, rs, &tavek, &qsn, qli, fc, es, sbc, qnetob,
	     iradfl, &mtime[1], &param[1], itsmethod, w, dt, ub, refdepth);
    fub = surfeb_(&tub, rkn, ws, &tak, z__, g, fstab, &qp, &densa, cp, hneu,
	    pr, ea, tk, &dens, cs, rs, &tavek, &qsn, qli, fc, es, sbc, qnetob,
	     iradfl, &mtime[1], &param[1], itsmethod, w, dt, ub, refdepth);
    ibtowrite = 0;
    if (flb * fub > 0.) {
	tlb = tak - 150.;
	tub = tak + 100.;
	flb = surfeb_(&tlb, rkn, ws, &tak, z__, g, fstab, &qp, &densa, cp,
		hneu, pr, ea, tk, &dens, cs, rs, &tavek, &qsn, qli, fc, es,
		sbc, qnetob, iradfl, &mtime[1], &param[1], itsmethod, w, dt,
		ub, refdepth);
	fub = surfeb_(&tub, rkn, ws, &tak, z__, g, fstab, &qp, &densa, cp,
		hneu, pr, ea, tk, &dens, cs, rs, &tavek, &qsn, qli, fc, es,
		sbc, qnetob, iradfl, &mtime[1], &param[1], itsmethod, w, dt,
		ub, refdepth);
	ibtowrite = 1;
	if (flb * fub > 0.) {
	    s_wsle(&io___218);
	    do_lio(&c__9, &c__1, "Bisection surface temperature solution fai"
		    "led with large range", (ftnlen)62);
	    e_wsle();
	    s_wsle(&io___219);
	    do_lio(&c__9, &c__1, "Date: ", (ftnlen)6);
	    do_lio(&c__5, &c__1, (char *)&mtime[1], (ftnlen)sizeof(doublereal)
		    );
	    do_lio(&c__5, &c__1, (char *)&mtime[2], (ftnlen)sizeof(doublereal)
		    );
	    do_lio(&c__5, &c__1, (char *)&mtime[3], (ftnlen)sizeof(doublereal)
		    );
	    e_wsle();
	    s_wsle(&io___220);
	    do_lio(&c__9, &c__1, "Time: ", (ftnlen)6);
	    do_lio(&c__5, &c__1, (char *)&mtime[4], (ftnlen)sizeof(doublereal)
		    );
	    e_wsle();
	    s_wsle(&io___221);
	    do_lio(&c__9, &c__1, "Model element: ", (ftnlen)15);
	    do_lio(&c__5, &c__1, (char *)&mtime[5], (ftnlen)sizeof(doublereal)
		    );
	    e_wsle();
	    s_wsle(&io___222);
	    do_lio(&c__9, &c__1, "A surface temperature of 273 K assumed", (
		    ftnlen)38);
	    e_wsle();
	    ts = *tk;
	    goto L10;
	} else {
	    s_wsle(&io___223);
	    do_lio(&c__9, &c__1, "Bisection surface temperature solution wit"
		    "h large range", (ftnlen)55);
	    e_wsle();
	    s_wsle(&io___224);
	    do_lio(&c__9, &c__1, "Date: ", (ftnlen)6);
	    do_lio(&c__5, &c__1, (char *)&mtime[1], (ftnlen)sizeof(doublereal)
		    );
	    do_lio(&c__5, &c__1, (char *)&mtime[2], (ftnlen)sizeof(doublereal)
		    );
	    do_lio(&c__5, &c__1, (char *)&mtime[3], (ftnlen)sizeof(doublereal)
		    );
	    e_wsle();
	    s_wsle(&io___225);
	    do_lio(&c__9, &c__1, "Time: ", (ftnlen)6);
	    do_lio(&c__5, &c__1, (char *)&mtime[4], (ftnlen)sizeof(doublereal)
		    );
	    e_wsle();
	    s_wsle(&io___226);
	    do_lio(&c__9, &c__1, "Model element: ", (ftnlen)15);
	    do_lio(&c__5, &c__1, (char *)&mtime[5], (ftnlen)sizeof(doublereal)
		    );
	    e_wsle();
	    s_wsle(&io___227);
	    do_lio(&c__9, &c__1, "This is not a critical problem unless it h"
		    "appens frequently", (ftnlen)59);
	    e_wsle();
	    s_wsle(&io___228);
	    do_lio(&c__9, &c__1, "and solution below appears incorrect", (
		    ftnlen)36);
	    e_wsle();
	}
    } else {
    }
    niter = (integer) (log((tub - tlb) / tol) / log(2.));
    i__1 = niter;
    for (iter = 1; iter <= i__1; ++iter) {
	ts = (tub + tlb) * .5;
	f1 = surfeb_(&ts, rkn, ws, &tak, z__, g, fstab, &qp, &densa, cp, hneu,
		 pr, ea, tk, &dens, cs, rs, &tavek, &qsn, qli, fc, es, sbc,
		qnetob, iradfl, &mtime[1], &param[1], itsmethod, w, dt, ub,
		refdepth);
	if (f1 > 0.) {
	    tlb = ts;
	} else {
	    tub = ts;
	}
    }
    if (ibtowrite == 1) {
	s_wsle(&io___230);
	do_lio(&c__9, &c__1, "Surface temperature: ", (ftnlen)21);
	do_lio(&c__5, &c__1, (char *)&ts, (ftnlen)sizeof(doublereal));
	do_lio(&c__9, &c__1, " K", (ftnlen)2);
	e_wsle();
	s_wsle(&io___231);
	do_lio(&c__9, &c__1, "Energy closure: ", (ftnlen)16);
	do_lio(&c__5, &c__1, (char *)&f1, (ftnlen)sizeof(doublereal));
	e_wsle();
	s_wsle(&io___232);
	do_lio(&c__9, &c__1, "Iterations: ", (ftnlen)12);
	do_lio(&c__3, &c__1, (char *)&niter, (ftnlen)sizeof(integer));
	e_wsle();
    }
L10:
    ts -= *tk;
    if (*w > 0. && ts > 0.) {
	ret_val = 0.;
	d__1 = ret_val + *tk;
	*smelt = surfeb_(&d__1, rkn, ws, &tak, z__, g, fstab, &qp, &densa, cp,
		 hneu, pr, ea, tk, &dens, cs, rs, &tavek, &qsn, qli, fc, es,
		sbc, qnetob, iradfl, &mtime[1], &param[1], itsmethod, w, dt,
		ub, refdepth);
    } else {
	ret_val = ts;
	*smelt = 0.;
    }
    return ret_val;
}

doublereal surfeb_(doublereal *ts, doublereal *rkn, doublereal *ws,
	doublereal *tak, doublereal *z__, doublereal *g, doublereal *fstab,
	doublereal *qp, doublereal *densa, doublereal *cp, doublereal *hneu,
	doublereal *pr, doublereal *ea, doublereal *tk, doublereal *dens,
	doublereal *cs, doublereal *rs, doublereal *tavek, doublereal *qsn,
	doublereal *qli, doublereal *fc, doublereal *es, doublereal *sbc,
	doublereal *qnetob, integer *iradfl, doublereal *mtime, doublereal *
	param, integer *itsmethod, doublereal *w, doublereal *dt, doublereal *
	ub, doublereal *refdepth)
{

    static thread_local doublereal rhow = 1e3;
    static thread_local doublereal w1day = .261799;

    doublereal ret_val{}, d__1{};

    doublereal d1{}, cg{}, de{};
    integer lc{}, ks{};
    doublereal ze{}, zo{}, zs{}, de2{}, rd1{}, ze2{}, abg{}, dlf{}, avo{}, qcs{}, rho{}, wlf{};
    extern doublereal svp_(doublereal *), lane_(doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *);
    doublereal lang{}, lans{}, rhog{}, tref{}, rkin{}, anir0{};
    extern doublereal rkinst_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *);
    doublereal lane_de__{}, fkappag{}, lane_ze__{}, fkappas{}, lane_de2__{}, lane_ze2__{};

    --param;
    --mtime;

    *es = param[3];
    cg = param[4];
    *z__ = param[5];
    zo = param[6];
    rho = param[7];
    rhog = param[8];
    lc = (integer) param[9];
    ks = (integer) param[10];
    de = param[11];
    abg = param[12];
    avo = param[13];
    anir0 = param[14];
    lans = param[15];
    lang = param[16];
    wlf = param[17];
    rd1 = param[18];
    *fstab = param[19];
    tref = param[20];
    zs = *w * rhow / rho;
    rkin = rkinst_(rkn, ws, tak, ts, z__, g, fstab);
    if (*itsmethod == 1) {
	qcs = *dens * *cs * *rs * (*ts - *tavek);
    } else if (*itsmethod == 2) {
	fkappas = lans / (rho * *cs);
	fkappag = lang / (rhog * cg);
	d1 = sqrt(fkappas * 2 / w1day);
	if (zs >= rd1 * d1) {
	    lane_ze__ = lans / (rd1 * d1);
	} else {
	    lane_ze__ = lane_(&lans, &lang, &zs, &rho, &rhog, cs, &cg, &rd1, &
		    ze, &w1day);
	    lane_ze__ /= ze;
	}
	qcs = lane_ze__ * (*ts - *tavek);
    } else if (*itsmethod == 3) {
	fkappas = lans / (rho * *cs);
	fkappag = lang / (rhog * cg);
	d1 = sqrt(fkappas * 2 / w1day);
	if (zs >= rd1 * d1) {
	    lane_ze__ = lans / (rd1 * d1);
	    ze = rd1 * d1;
	} else {
	    lane_ze__ = lane_(&lans, &lang, &zs, &rho, &rhog, cs, &cg, &rd1, &
		    ze, &w1day);
	    lane_ze__ /= ze;
	}
	de = ze / rd1;
	lane_de__ = lane_ze__ / de * ze;
	qcs = lane_de__ * (*ts - ts_save__1.ts_old__) / (w1day * *dt) +
		lane_ze__ * (*ts - *tavek);
    } else {
	fkappas = lans / (rho * *cs);
	fkappag = lang / (rhog * cg);
	d1 = sqrt(fkappas * 2 / w1day);
	dlf = sqrt(fkappag * 2 / wlf);
	if (zs >= rd1 * d1) {
	    lane_ze__ = lans / (rd1 * d1);
	    ze = rd1 * d1;
	} else {
	    lane_ze__ = lane_(&lans, &lang, &zs, &rho, &rhog, cs, &cg, &rd1, &
		    ze, &w1day);
	    lane_ze__ /= ze;
	}
	if (zs >= rd1 * dlf) {
	    lane_ze2__ = lans / (rd1 * dlf);
	    ze2 = rd1 * dlf;
	} else {
	    lane_ze2__ = lane_(&lans, &lang, &zs, &rho, &rhog, cs, &cg, &rd1,
		    &ze2, &wlf);
	    lane_ze2__ /= ze2;
	}
	de = ze / rd1;
	lane_de__ = lane_ze__ / de * ze;
	de2 = ze2 / rd1;
	lane_de2__ = lane_ze2__ / de2 * ze2;
	if (*ub <= 0. || *refdepth <= 0.) {
	    qcs = lane_de__ * (*ts - ts_save__1.ts_old__) / (w1day * *dt) +
		    lane_ze__ * (*ts - ts_save__1.ts_ave__) + lane_de2__ * (
		    ts_save__1.ts_ave__ - ts_save__1.tave_ave__);
	} else if (*refdepth > rd1 * d1) {
	    qcs = lane_de__ * (*ts - ts_save__1.ts_old__) / (w1day * *dt) +
		    lane_ze__ * (*ts - ts_save__1.ts_ave__) + lane_de2__ * (
		    ts_save__1.ts_ave__ - ts_save__1.tave_ave__);
	} else {
	    qcs = lane_ze__ * ze * (*ts - *tk) / *refdepth;
	}
    }
    d__1 = *ts - *tk;
    ret_val = *qp + rkin * *densa * *cp * (*tak - *ts) + *hneu * *densa *
	    .622 * rkin / *pr * (*ea - svp_(&d__1)) - qcs;
    if (*iradfl == 0) {

	d__1 = *ts, d__1 *= d__1;
	ret_val = ret_val + *qsn + *qli - (1. - *fc) * *es * *sbc * (d__1 *
		d__1);
    } else {
	ret_val += *qnetob;
    }
    return ret_val;
}

doublereal qcest_(doublereal *ts, doublereal *rkn, doublereal *ws, doublereal
	*tak, doublereal *z__, doublereal *g, doublereal *fstab, doublereal *
	qp, doublereal *densa, doublereal *cp, doublereal *hneu, doublereal *
	pr, doublereal *ea, doublereal *tk, doublereal *dens, doublereal *cs,
	doublereal *rs, doublereal *tavek, doublereal *qsn, doublereal *qli,
	doublereal *fc, doublereal *es, doublereal *sbc, doublereal *qnetob,
	integer *iradfl, doublereal *mtime, doublereal *param, integer *
	itsmethod, doublereal *w, doublereal *dt)
{

    static thread_local doublereal rhow = 1e3;

    doublereal ret_val{}, d__1{};

    doublereal cg{}, de{};
    integer lc{}, ks{};
    doublereal zo{}, zs{}, rd1{}, abg{}, avo{}, rho{}, wlf{};
    extern doublereal svp_(doublereal *);
    doublereal lang{}, lans{}, rhog{}, tref{}, rkin{}, anir0{};
    extern doublereal rkinst_(doublereal *, doublereal *, doublereal *,
	    doublereal *, doublereal *, doublereal *, doublereal *);

    --param;
    --mtime;

    *es = param[3];
    cg = param[4];
    *z__ = param[5];
    zo = param[6];
    rho = param[7];
    rhog = param[8];
    lc = (integer) param[9];
    ks = (integer) param[10];
    de = param[11];
    abg = param[12];
    avo = param[13];
    anir0 = param[14];
    lans = param[15];
    lang = param[16];
    wlf = param[17];
    rd1 = param[18];
    *fstab = param[19];
    tref = param[20];
    zs = *w * rhow / rho;
    rkin = rkinst_(rkn, ws, tak, ts, z__, g, fstab);
    d__1 = *ts - *tk;
    ret_val = *qp + rkin * *densa * *cp * (*tak - *ts) + *hneu * *densa *
	    .622 * rkin / *pr * (*ea - svp_(&d__1));
    if (*iradfl == 0) {

	d__1 = *ts, d__1 *= d__1;
	ret_val = ret_val + *qsn + *qli - (1. - *fc) * *es * *sbc * (d__1 *
		d__1);
    } else {
	ret_val += *qnetob;
    }
    return ret_val;
}

doublereal rkinst_(doublereal *rkn, doublereal *ws, doublereal *ta,
	doublereal *ts, doublereal *z__, doublereal *g, doublereal *fstab)
{

    doublereal ret_val{}, d__1{}, d__2{}, d__3{};

    doublereal rich{};

    if (*ws <= 0.) {
	ret_val = 0.;
    } else {
	rich = *g * (*ta - *ts) * *z__ / (*ws * *ws * *ta);
	if (rich >= 0.) {
	    ret_val = *rkn / (rich * 10. + 1.);
	} else {

	    d__3 = 1 - rich * 16;
	    d__1 = 3., d__2 = pow_dd(&d__3, &c_b176);
	    ret_val = *rkn * min(d__1,d__2);
	}
    }
    ret_val = *rkn + *fstab * (ret_val - *rkn);
    return ret_val;
}

doublereal partsnow_(doublereal *p, doublereal *ta, doublereal *tr,
	doublereal *ts)
{

    doublereal ret_val{};

    if (snow_fraction >= 0) return *p * snow_fraction;
    if (*ta < *ts) {
	ret_val = *p;
    } else if (*ta > *tr) {
	ret_val = 0.;
    } else {
	ret_val = *p * (*tr - *ta) / (*tr - *ts);
    }
    return ret_val;
}

doublereal qpf_(doublereal *pr, doublereal *ta, doublereal *to, doublereal *
	ps, doublereal *rhow, doublereal *hf, doublereal *cw, doublereal *cs)
{

    doublereal ret_val{};

    doublereal train{}, tsnow{};

    if (*ta > *to) {
	train = *ta;
	tsnow = *to;
    } else {
	train = *to;
	tsnow = *ta;
    }
    ret_val = *pr * *rhow * (*hf + *cw * (train - *to)) + *ps * *rhow * *cs *
	    (tsnow - *to);
    return ret_val;
}

 int turbflux_(doublereal *pr, doublereal *ra, doublereal *ta,
	 doublereal *tk, doublereal *ts, doublereal *z__, doublereal *g,
	doublereal *cp, doublereal *rkn, doublereal *ws, doublereal *ea,
	doublereal *rhow, doublereal *hneu, doublereal *qh, doublereal *qe,
	doublereal *e, doublereal *fstab)
{
    doublereal es{}, tak{}, tsk{}, rhoa{}, rkin{};
    extern doublereal svpi_(doublereal *), rkinst_(doublereal *, doublereal *,
	     doublereal *, doublereal *, doublereal *, doublereal *,
	    doublereal *);

    tak = *ta + *tk;
    tsk = *ts + *tk;
    rkin = rkinst_(rkn, ws, &tak, &tsk, z__, g, fstab);
    rhoa = *pr / (*ra * tak);
    *qh = rhoa * (*ta - *ts) * *cp * rkin;
    es = svpi_(ts);
    *qe = *hneu * .622 / (*ra * tak) * rkin * (*ea - es);
    *e = -(*qe) / (*rhow * *hneu);
    return 0;
}

doublereal fmelt_(doublereal *ub, doublereal *rhow, doublereal *w, doublereal
	*hf, doublereal *lc, doublereal *rid, doublereal *ks, doublereal *
	prain)
{

    doublereal ret_val{}, d__1{};

    doublereal ss{}, uu{};

    static thread_local cilist io___309 = { 0, 6, 0, 0, 0 };

    uu = 0.;
    if (*ub < 0.) {
	ret_val = 0.;
    } else if (*w <= 0.) {
	ret_val = *prain;
	if (*prain <= 0.) {
	    ret_val = 0.;
	}
    } else {
	uu = *ub / (*rhow * *w * *hf);
	if (uu > .99) {
	    uu = .99;
	}
	if (uu / (1 - uu) <= *lc) {
	    ss = 0.;
	} else {
	    ss = (uu / ((1 - uu) * *rid) - *lc / *rid) / (1 - *lc / *rid);
	}

	d__1 = ss;
	ret_val = *ks * (d__1 * (d__1 * d__1));
    }
    if (ret_val < 0.) {
	s_wsle(&io___309);
	do_lio(&c__9, &c__1, "FMELT is NEGATIVE!", (ftnlen)18);
	e_wsle();
	s_stop("", (ftnlen)0);
    }
    return ret_val;
}

doublereal svp_(doublereal *t)
{

    doublereal ret_val{};

    extern doublereal svpi_(doublereal *), svpw_(doublereal *);

    if (*t >= 0.) {
	ret_val = svpw_(t);
    } else {
	ret_val = svpi_(t);
    }
    return ret_val;
}

doublereal svpi_(doublereal *t)
{

    doublereal ret_val{};

    ret_val = *t * (*t * (*t * (*t * (*t * (*t * 1.838826904e-10 +
	    4.838803174e-8) + 5.82472028e-6) + 4.176223716e-4) + .01886013408)
	     + .503469897) + 6.109177956;
    ret_val *= 100;
    return ret_val;
}

doublereal svpw_(doublereal *t)
{

    doublereal ret_val{};

    ret_val = *t * (*t * (*t * (*t * (*t * (*t * 6.136820929e-11 +
	    2.034080948e-8) + 3.031240936e-6) + 2.650648471e-4) +
	    .01428945805) + .4436518521) + 6.107799961;
    ret_val *= 100;
    return ret_val;
}

 int prehelp_(doublereal *w1, doublereal *w, doublereal *dt,
	doublereal *fm, doublereal *fm1, doublereal *fac, doublereal *ps,
	doublereal *prain, doublereal *e, doublereal *rhow, doublereal *hf,
	doublereal *q, doublereal *qm, doublereal *mr, doublereal *qe,
	doublereal *hneu)
{

    doublereal d__1{}, d__2{};

    doublereal qother{};

    *fm = (*w1 - *w) / *dt * *fac - *fm1;

    d__1 = 0., d__2 = *ps + *prain - *fm - *e;
    *mr = max(d__1,d__2);
    *e = *ps + *prain - *fm - *mr;
    qother = *q + *qm - *qe;
    *qm = *mr * *rhow * *hf;
    *qe = -(*e) * *rhow * *hneu;
    *q = qother - *qm + *qe;
    return 0;
}

doublereal albedo_(doublereal *tausn, doublereal *coszen, doublereal *d__,
	doublereal *aep, doublereal *abg, doublereal *avo, doublereal *airo)
{

    doublereal ret_val{};

    doublereal b{}, cn{}, cs{}, rr{}, avd{}, fage{}, aird{}, anir{}, fzen{}, avis{};

    b = 2.;
    cs = visible_age;
    cn = infrared_age;
    fage = *tausn / (*tausn + 1.);
    if (*coszen < .5) {
	fzen = 1. / b * ((b + 1.) / (b * 2. * *coszen + 1.) - 1.);
    } else {
	fzen = 0.;
    }
    avd = (1. - cs * fage) * *avo;
    avis = avd + fzen * .4 * (1. - avd);
    aird = (1. - cn * fage) * *airo;
    anir = aird + fzen * .4 * (1. - aird);
    ret_val = (avis + anir) / 2.;
    if (*d__ < *aep) {
	rr = (1. - *d__ / *aep) * exp(-(*d__) * .5 / *aep);
	ret_val = rr * *abg + (1. - rr) * ret_val;
    }
    return ret_val;
}

 int agesn_(doublereal *tausn, doublereal *dt, doublereal *ps,
	 doublereal *tsurf, doublereal *tk, doublereal *dnews)
{

    doublereal d__1{}, d__2{};

    doublereal r1{}, r2{}, r3{}, tsk{};

    tsk = *tsurf + *tk;
    r1 = exp((1. / *tk - 1. / tsk) * 5e3);

    d__1 = r1, d__1 *= d__1, d__2 = d__1, d__1 *= d__1;
    r2 = d__2 * (d__1 * d__1);
    if (r2 > 1.) {
	r2 = 1.;
    }
    r3 = age_dust;
    *tausn += (r1 + r2 + r3) * .0036 * *dt;
    if (*ps > 0.) {
	if (*dnews > 0.) {

	    d__1 = *tausn * (1 - *ps * *dt / *dnews);
	    *tausn = max(d__1,0.);
	} else {
	    *tausn = 0.;
	}
    }
    return 0;
}

doublereal lane_(doublereal *lans, doublereal *lang, doublereal *zs,
	doublereal *rho, doublereal *rhog, doublereal *cs, doublereal *cg,
	doublereal *r__, doublereal *ze, doublereal *w1day)
{

    doublereal ret_val{}, d__1{}, d__2{}, d__3{}, d__4{};

    doublereal d1{}, d2{}, fkappag{}, fkappas{};

    fkappas = *lans / (*rho * *cs);
    fkappag = *lang / (*rhog * *cg);
    d1 = sqrt(fkappas * 2 / *w1day);
    d2 = sqrt(fkappag * 2 / *w1day);

    d__1 = *r__, d__2 = *zs / d1;

    d__3 = 0., d__4 = *r__ - *zs / d1;
    ret_val = min(d__1,d__2) * d1 / *lans + max(d__3,d__4) * d2 / *lang;

    d__1 = *r__, d__2 = *zs / d1;

    d__3 = 0., d__4 = *r__ - *zs / d1;
    *ze = min(d__1,d__2) * d1 + max(d__3,d__4) * d2;
    if (ret_val != 0.) {
	ret_val = 1 / ret_val * *ze;
    }
    return ret_val;
}

doublereal refdep_(doublereal *flans, doublereal *a, doublereal *b,
	doublereal *hf, doublereal *rhom, doublereal *dt, doublereal *x1)
{

    doublereal ret_val{};

    doublereal temp{};

    temp = *flans * *flans + *b * 2 * (-(*a) * *flans * *dt / (*hf * *rhom) +
	    *flans * *x1 + *b * .5 * *x1 * *x1);
    if (temp < 0. || *a > 0. || *b == 0.) {
	ret_val = 0.;
    } else {
	ret_val = (-(*flans) + sqrt(temp)) / *b;
    }
    return ret_val;
}

 int grad_(doublereal *qc1, doublereal *qc2, doublereal *t1,
	doublereal *t2, doublereal *a, doublereal *b)
{
    if (*t2 - *t1 != 0.) {
	*b = (*qc2 - *qc1) / (*t1 - *t2);
	*a = *qc1 + *b * *t1;
    }
    return 0;
}

doublereal daily_ave__(doublereal *backup, integer *n, doublereal *a)
{

    integer i__1{};
    doublereal ret_val{};

    integer i__{};
    doublereal sum{}, count{};

    --backup;

    sum = 0.;
    count = 0.;
    i__1 = *n;
    for (i__ = 1; i__ <= i__1; ++i__) {
	if (backup[i__] > *a) {
	    sum += backup[i__];
	    count += 1.;
	}

    }
    if (count != 0.) {
	ret_val = sum / count;
    } else {
	ret_val = 0.;
    }
    return ret_val;
}


}
} // namespace
int equilibriumStep(double dt,double* input,double* site,double* state,double* parameters,double* output,double dust,double fresh,double cv,double cir,double fraction) {
    age_dust=dust;new_snow=fresh;visible_age=cv;infrared_age=cir;snow_fraction=fraction;
    int count=1,flags[5]{0,0,0,0,1};double p=0,e=0,m=0;diagnostics=0;
    equilibrium::snow_(&dt,&count,input,site,state,parameters,flags+1,&p,&e,&m,output);
    return diagnostics;
}
int forceRestoreStep(double dt,double* input,double* site,double* state,double* temperatures,
    double* averages,int count,double* parameters,double* output,double dust,double surface_mean,double bulk_mean,double cv,double cir,double fraction) {
    age_dust=dust;mean_surface=surface_mean;mean_bulk=bulk_mean;visible_age=cv;infrared_age=cir;snow_fraction=fraction;
    int steps=1,flags[6]{0,0,0,0,1,4};double p=0,e=0,m=0,time[6]{};diagnostics=0;
    force_restore::snowueb2_(&dt,&steps,input,site,state,temperatures,averages,&count,
        parameters,flags+1,&p,&e,&m,output,time+1);
    return diagnostics;
}
} // namespace openswmm::surface::ueb_reference

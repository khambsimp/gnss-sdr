/*!
 * \file rtklib_solution.cc
 * \brief solution functions
 * \authors <ul>
 *          <li> 2007-2013, T. Takasu
 *          <li> 2017, Javier Arribas
 *          <li> 2017, Carles Fernandez
 *          </ul>
 *
 * This is a derived work from RTKLIB http://www.rtklib.com/
 * The original source code at https://github.com/tomojitakasu/RTKLIB is
 * released under the BSD 2-clause license with an additional exclusive clause
 * that does not apply here. This additional clause is reproduced below:
 *
 * " The software package includes some companion executive binaries or shared
 * libraries necessary to execute APs on Windows. These licenses succeed to the
 * original ones of these software. "
 *
 * Neither the executive binaries nor the shared libraries are required by, used
 * or included in GNSS-SDR.
 *
 * -----------------------------------------------------------------------------
 * Copyright (C) 2007-2013, T. Takasu
 * Copyright (C) 2017, Javier Arribas
 * Copyright (C) 2017, Carles Fernandez
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 *
 * -----------------------------------------------------------------------------
 */

#include "rtklib_solution.h"
#include "rtklib_rtkcmn.h"
#include "rtklib_rtksvr.h"
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>


/* constants and macros ------------------------------------------------------*/

#define SQR_SOL(x) ((x) < 0.0 ? -(x) * (x) : (x) * (x))
#define SQRT_SOL(x) ((x) < 0.0 ? 0.0 : std::sqrt(x))

const int MAXFIELD = 64; /* max number of fields in a record */

const double KNOT2M = 0.514444444; /* m/knot */

static const int SOLQ_NMEA[] = {/* nmea quality flags to rtklib sol quality */
    /* nmea 0183 v.2.3 quality flags: */
    /*  0=invalid, 1=gps fix (sps), 2=dgps fix, 3=pps fix, 4=rtk, 5=float rtk */
    /*  6=estimated (dead reckoning), 7=manual input, 8=simulation */
    SOLQ_NONE, SOLQ_SINGLE, SOLQ_DGPS, SOLQ_PPP, SOLQ_FIX,
    SOLQ_FLOAT, SOLQ_DR, SOLQ_NONE, SOLQ_NONE, SOLQ_NONE};


/* solution option to field separator ----------------------------------------*/
const char *opt2sep(const solopt_t *opt)
{
    if (!*opt->sep)
        {
            return " ";
        }
    if (!strcmp(opt->sep, "\\t"))
        {
            return "\t";
        }
    return opt->sep;
}


/* separate fields -----------------------------------------------------------*/
int tonum(char *buff, const char *sep, double *v)
{
    int n;
    int len = static_cast<int>(strlen(sep));
    char *p;
    char *q;

    for (p = buff, n = 0; n < MAXFIELD; p = q + len)
        {
            if ((q = strstr(p, sep)))
                {
                    *q = '\0';
                }
            if (*p)
                {
                    v[n++] = atof(p);
                }
            if (!q)
                {
                    break;
                }
        }
    return n;
}


/* sqrt of covariance --------------------------------------------------------*/
double sqvar(double covar)
{
    return covar < 0.0 ? -sqrt(-covar) : sqrt(covar);
}


/* convert ddmm.mm in nmea format to deg -------------------------------------*/
double dmm2deg(double dmm)
{
    return floor(dmm / 100.0) + fmod(dmm, 100.0) / 60.0;
}


/* convert time in nmea format to time ---------------------------------------*/
void septime(double t, double *t1, double *t2, double *t3)
{
    *t1 = floor(t / 10000.0);
    t -= *t1 * 10000.0;
    *t2 = floor(t / 100.0);
    *t3 = t - *t2 * 100.0;
}


/* solution to covariance ----------------------------------------------------*/
void soltocov(const sol_t *sol, double *P)
{
    P[0] = sol->qr[0];        /* xx or ee */
    P[4] = sol->qr[1];        /* yy or nn */
    P[8] = sol->qr[2];        /* zz or uu */
    P[1] = P[3] = sol->qr[3]; /* xy or en */
    P[5] = P[7] = sol->qr[4]; /* yz or nu */
    P[2] = P[6] = sol->qr[5]; /* zx or ue */
}


/* covariance to solution ----------------------------------------------------*/
void covtosol(const double *P, sol_t *sol)
{
    sol->qr[0] = static_cast<float>(P[0]); /* xx or ee */
    sol->qr[1] = static_cast<float>(P[4]); /* yy or nn */
    sol->qr[2] = static_cast<float>(P[8]); /* zz or uu */
    sol->qr[3] = static_cast<float>(P[1]); /* xy or en */
    sol->qr[4] = static_cast<float>(P[5]); /* yz or nu */
    sol->qr[5] = static_cast<float>(P[2]); /* zx or ue */
}


/* decode nmea gprmc: recommended minimum data for gps -----------------------*/
int decode_nmearmc(char **val, int n, sol_t *sol)
{
    double tod = 0.0;
    double lat = 0.0;
    double lon = 0.0;
    double vel = 0.0;
    double dir = 0.0;
    double date = 0.0;
    double ang = 0.0;
    double ep[6];
    double pos[3] = {0};
    char act = ' ';
    char ns = 'N';
    char ew = 'E';
    char mew = 'E';
    char mode = 'A';
    int i;

    trace(4, "decode_nmearmc: n=%d\n", n);

    for (i = 0; i < n; i++)
        {
            switch (i)
                {
                case 0:
                    tod = atof(val[i]);
                    break; /* time in utc (hhmmss) */
                case 1:
                    act = *val[i];
                    break; /* A=active,V=void */
                case 2:
                    lat = atof(val[i]);
                    break; /* latitude (ddmm.mmm) */
                case 3:
                    ns = *val[i];
                    break; /* N=north,S=south */
                case 4:
                    lon = atof(val[i]);
                    break; /* longitude (dddmm.mmm) */
                case 5:
                    ew = *val[i];
                    break; /* E=east,W=west */
                case 6:
                    vel = atof(val[i]);
                    break; /* speed (knots) */
                case 7:
                    dir = atof(val[i]);
                    break; /* track angle (deg) */
                case 8:
                    date = atof(val[i]);
                    break; /* date (ddmmyy) */
                case 9:
                    ang = atof(val[i]);
                    break; /* magnetic variation */
                case 10:
                    mew = *val[i];
                    break; /* E=east,W=west */
                case 11:
                    mode = *val[i];
                    break; /* mode indicator (>nmea 2) */
                    /* A=autonomous,D=differential */
                    /* E=estimated,N=not valid,S=simulator */
                }
        }
    if ((act != 'A' && act != 'V') || (ns != 'N' && ns != 'S') || (ew != 'E' && ew != 'W'))
        {
            trace(2, "invalid nmea gprmc format\n");
            return 0;
        }
    pos[0] = (ns == 'S' ? -1.0 : 1.0) * dmm2deg(lat) * D2R;
    pos[1] = (ew == 'W' ? -1.0 : 1.0) * dmm2deg(lon) * D2R;
    septime(date, ep + 2, ep + 1, ep);
    septime(tod, ep + 3, ep + 4, ep + 5);
    ep[0] += ep[0] < 80.0 ? 2000.0 : 1900.0;
    sol->time = utc2gpst(epoch2time(ep));
    pos2ecef(pos, sol->rr);
    sol->stat = mode == 'D' ? SOLQ_DGPS : SOLQ_SINGLE;
    sol->ns = 0;

    sol->type = 0; /* position type = xyz */

    trace(5, "decode_nmearmc: %s rr=%.3f %.3f %.3f stat=%d ns=%d vel=%.2f dir=%.0f ang=%.0f mew=%c mode=%c\n",
        time_str(sol->time, 0), sol->rr[0], sol->rr[1], sol->rr[2], sol->stat, sol->ns,
        vel, dir, ang, mew, mode);

    return 1;
}


/* decode nmea gpgga: fix information ----------------------------------------*/
int decode_nmeagga(char **val, int n, sol_t *sol)
{
    gtime_t time;
    double tod = 0.0;
    double lat = 0.0;
    double lon = 0.0;
    double hdop = 0.0;
    double alt = 0.0;
    double msl = 0.0;
    double ep[6];
    double tt;
    double pos[3] = {0};
    char ns = 'N';
    char ew = 'E';
    char ua = ' ';
    char um = ' ';
    int i;
    int solq = 0;
    int nrcv = 0;

    trace(4, "decode_nmeagga: n=%d\n", n);

    for (i = 0; i < n; i++)
        {
            switch (i)
                {
                case 0:
                    tod = atof(val[i]);
                    break; /* time in utc (hhmmss) */
                case 1:
                    lat = atof(val[i]);
                    break; /* latitude (ddmm.mmm) */
                case 2:
                    ns = *val[i];
                    break; /* N=north,S=south */
                case 3:
                    lon = atof(val[i]);
                    break; /* longitude (dddmm.mmm) */
                case 4:
                    ew = *val[i];
                    break; /* E=east,W=west */
                case 5:
                    solq = atoi(val[i]);
                    break; /* fix quality */
                case 6:
                    nrcv = atoi(val[i]);
                    break; /* # of satellite tracked */
                case 7:
                    hdop = atof(val[i]);
                    break; /* hdop */
                case 8:
                    alt = atof(val[i]);
                    break; /* altitude in msl */
                case 9:
                    ua = *val[i];
                    break; /* unit (M) */
                case 10:
                    msl = atof(val[i]);
                    break; /* height of geoid */
                case 11:
                    um = *val[i];
                    break; /* unit (M) */
                }
        }
    if ((ns != 'N' && ns != 'S') || (ew != 'E' && ew != 'W'))
        {
            trace(2, "invalid nmea gpgga format\n");
            return 0;
        }
    if (sol->time.time == 0.0)
        {
            trace(2, "no date info for nmea gpgga\n");
            return 0;
        }
    pos[0] = (ns == 'N' ? 1.0 : -1.0) * dmm2deg(lat) * D2R;
    pos[1] = (ew == 'E' ? 1.0 : -1.0) * dmm2deg(lon) * D2R;
    pos[2] = alt + msl;

    time2epoch(sol->time, ep);
    septime(tod, ep + 3, ep + 4, ep + 5);
    time = utc2gpst(epoch2time(ep));
    tt = timediff(time, sol->time);
    if (tt < -43200.0)
        {
            sol->time = timeadd(time, 86400.0);
        }
    else if (tt > 43200.0)
        {
            sol->time = timeadd(time, -86400.0);
        }
    else
        {
            sol->time = time;
        }
    pos2ecef(pos, sol->rr);
    sol->stat = 0 <= solq && solq <= 8 ? SOLQ_NMEA[solq] : SOLQ_NONE;
    sol->ns = nrcv;

    sol->type = 0; /* position type = xyz */

    trace(5, "decode_nmeagga: %s rr=%.3f %.3f %.3f stat=%d ns=%d hdop=%.1f ua=%c um=%c\n",
        time_str(sol->time, 0), sol->rr[0], sol->rr[1], sol->rr[2], sol->stat, sol->ns,
        hdop, ua, um);

    return 1;
}


/* decode nmea ---------------------------------------------------------------*/
int decode_nmea(char *buff, sol_t *sol)
{
    char *p;
    char *q;
    char *val[MAXFIELD] = {nullptr};
    int n = 0;

    trace(4, "decode_nmea: buff=%s\n", buff);

    /* parse fields */
    for (p = buff; *p && n < MAXFIELD; p = q + 1)
        {
            if ((q = strchr(p, ',')) || (q = strchr(p, '*')))
                {
                    val[n++] = p;
                    *q = '\0';
                }
            else
                {
                    break;
                }
        }
    /* decode nmea sentence */
    if (val[0])
        {
            if (!strcmp(val[0], "$GPRMC"))
                {
                    return decode_nmearmc(val + 1, n - 1, sol);
                }
            if (!strcmp(val[0], "$GPGGA"))
                {
                    return decode_nmeagga(val + 1, n - 1, sol);
                }
        }
    return 0;
}


/* decode solution time ------------------------------------------------------*/
char *decode_soltime(char *buff, const solopt_t *opt, gtime_t *time)
{
    double v[MAXFIELD];
    char *p;
    char *q;
    char s[64] = " ";
    int n;
    int len;

    trace(4, "decode_soltime:\n");

    if (!strcmp(opt->sep, "\\t"))
        {
            std::strncpy(s, "\t", 2);
        }
    else if (*opt->sep)
        {
            std::strncpy(s, opt->sep, 64);
        }
    len = static_cast<int>(strlen(s));

    /* yyyy/mm/dd hh:mm:ss or yyyy mm dd hh:mm:ss */
    if (sscanf(buff, "%lf/%lf/%lf %lf:%lf:%lf", v, v + 1, v + 2, v + 3, v + 4, v + 5) >= 6)
        {
            if (v[0] < 100.0)
                {
                    v[0] += v[0] < 80.0 ? 2000.0 : 1900.0;
                }
            *time = epoch2time(v);
            if (opt->times == TIMES_UTC)
                {
                    *time = utc2gpst(*time);
                }
            else if (opt->times == TIMES_JST)
                {
                    *time = utc2gpst(timeadd(*time, -9 * 3600.0));
                }
            if (!(p = strchr(buff, ':')) || !(p = strchr(p + 1, ':')))
                {
                    return nullptr;
                }
            for (p++; isdigit(static_cast<int>(*p)) || *p == '.';)
                {
                    p++;
                }
            return p + len;
        }
    if (opt->posf == SOLF_GSIF)
        {
            if (sscanf(buff, "%lf %lf %lf %lf:%lf:%lf", v, v + 1, v + 2, v + 3, v + 4, v + 5) < 6)
                {
                    return nullptr;
                }
            *time = timeadd(epoch2time(v), -12.0 * 3600.0);
            if (!(p = strchr(buff, ':')) || !(p = strchr(p + 1, ':')))
                {
                    return nullptr;
                }
            for (p++; isdigit(static_cast<int>(*p)) || *p == '.';)
                {
                    p++;
                }
            return p + len;
        }
    /* wwww ssss */
    for (p = buff, n = 0; n < 2; p = q + len)
        {
            if ((q = strstr(p, s)))
                {
                    *q = '\0';
                }
            if (*p)
                {
                    v[n++] = atof(p);
                }
            if (!q)
                {
                    break;
                }
        }
    if (n >= 2 && 0.0 <= v[0] && v[0] <= 3000.0 && 0.0 <= v[1] && v[1] < 604800.0)
        {
            *time = gpst2time(static_cast<int>(v[0]), v[1]);
            return p;
        }
    return nullptr;
}


/* decode x/y/z-ecef ---------------------------------------------------------*/
int decode_solxyz(char *buff, const solopt_t *opt, sol_t *sol)
{
    double val[MAXFIELD];
    double P[9] = {0};
    int i = 0;
    int j;
    int n;
    const char *sep = opt2sep(opt);

    trace(4, "decode_solxyz:\n");

    if ((n = tonum(buff, sep, val)) < 3)
        {
            return 0;
        }

    for (j = 0; j < 3; j++)
        {
            sol->rr[j] = val[i++]; /* xyz */
        }
    if (i < n)
        {
            sol->stat = static_cast<unsigned char>(val[i++]);
        }
    if (i < n)
        {
            sol->ns = static_cast<unsigned char>(val[i++]);
        }
    if (i + 3 < n)
        {
            P[0] = val[i] * val[i];
            i++; /* sdx */
            P[4] = val[i] * val[i];
            i++; /* sdy */
            P[8] = val[i] * val[i];
            i++; /* sdz */
            if (i + 3 < n)
                {
                    P[1] = P[3] = SQR_SOL(val[i]);
                    i++; /* sdxy */
                    P[5] = P[7] = SQR_SOL(val[i]);
                    i++; /* sdyz */
                    P[2] = P[6] = SQR_SOL(val[i]);
                    i++; /* sdzx */
                }
            covtosol(P, sol);
        }
    if (i < n)
        {
            sol->age = static_cast<float>(val[i++]);
        }
    if (i < n)
        {
            sol->ratio = static_cast<float>(val[i]);
        }

    sol->type = 0; /* position type = xyz */

    if (MAXSOLQ < sol->stat)
        {
            sol->stat = SOLQ_NONE;
        }
    return 1;
}


/* decode lat/lon/height -----------------------------------------------------*/
int decode_solllh(char *buff, const solopt_t *opt, sol_t *sol)
{
    double val[MAXFIELD];
    double pos[3];
    double Q[9] = {0};
    double P[9];
    int i = 0;
    int n;
    const char *sep = opt2sep(opt);

    trace(4, "decode_solllh:\n");

    n = tonum(buff, sep, val);

    if (!opt->degf)
        {
            if (n < 3)
                {
                    return 0;
                }
            pos[0] = val[i++] * D2R; /* lat/lon/hgt (ddd.ddd) */
            pos[1] = val[i++] * D2R;
            pos[2] = val[i++];
        }
    else
        {
            if (n < 7)
                {
                    return 0;
                }
            pos[0] = dms2deg(val) * D2R; /* lat/lon/hgt (ddd mm ss) */
            pos[1] = dms2deg(val + 3) * D2R;
            pos[2] = val[6];
            i += 7;
        }
    pos2ecef(pos, sol->rr);
    if (i < n)
        {
            sol->stat = static_cast<unsigned char>(val[i++]);
        }
    if (i < n)
        {
            sol->ns = static_cast<unsigned char>(val[i++]);
        }
    if (i + 3 < n)
        {
            Q[4] = val[i] * val[i];
            i++; /* sdn */
            Q[0] = val[i] * val[i];
            i++; /* sde */
            Q[8] = val[i] * val[i];
            i++; /* sdu */
            if (i + 3 < n)
                {
                    Q[1] = Q[3] = SQR_SOL(val[i]);
                    i++; /* sdne */
                    Q[2] = Q[6] = SQR_SOL(val[i]);
                    i++; /* sdeu */
                    Q[5] = Q[7] = SQR_SOL(val[i]);
                    i++; /* sdun */
                }
            covecef(pos, Q, P);
            covtosol(P, sol);
        }
    if (i < n)
        {
            sol->age = static_cast<float>(val[i++]);
        }
    if (i < n)
        {
            sol->ratio = static_cast<float>(val[i]);
        }

    sol->type = 0; /* position type = xyz */

    if (MAXSOLQ < sol->stat)
        {
            sol->stat = SOLQ_NONE;
        }
    return 1;
}


/* decode e/n/u-baseline -----------------------------------------------------*/
int decode_solenu(char *buff, const solopt_t *opt, sol_t *sol)
{
    double val[MAXFIELD];
    double Q[9] = {0};
    int i = 0;
    int j;
    int n;
    const char *sep = opt2sep(opt);

    trace(4, "decode_solenu:\n");

    if ((n = tonum(buff, sep, val)) < 3)
        {
            return 0;
        }

    for (j = 0; j < 3; j++)
        {
            sol->rr[j] = val[i++]; /* enu */
        }
    if (i < n)
        {
            sol->stat = static_cast<unsigned char>(val[i++]);
        }
    if (i < n)
        {
            sol->ns = static_cast<unsigned char>(val[i++]);
        }
    if (i + 3 < n)
        {
            Q[0] = val[i] * val[i];
            i++; /* sde */
            Q[4] = val[i] * val[i];
            i++; /* sdn */
            Q[8] = val[i] * val[i];
            i++; /* sdu */
            if (i + 3 < n)
                {
                    Q[1] = Q[3] = SQR_SOL(val[i]);
                    i++; /* sden */
                    Q[5] = Q[7] = SQR_SOL(val[i]);
                    i++; /* sdnu */
                    Q[2] = Q[6] = SQR_SOL(val[i]);
                    i++; /* sdue */
                }
            covtosol(Q, sol);
        }
    if (i < n)
        {
            sol->age = static_cast<float>(val[i++]);
        }
    if (i < n)
        {
            sol->ratio = static_cast<float>(val[i]);
        }

    sol->type = 1; /* position type = enu */

    if (MAXSOLQ < sol->stat)
        {
            sol->stat = SOLQ_NONE;
        }
    return 1;
}


/* decode gsi f solution -----------------------------------------------------*/
int decode_solgsi(char *buff, const solopt_t *opt __attribute((unused)), sol_t *sol)
{
    double val[MAXFIELD];
    int i = 0;
    int j;

    trace(4, "decode_solgsi:\n");

    if (tonum(buff, " ", val) < 3)
        {
            return 0;
        }

    for (j = 0; j < 3; j++)
        {
            sol->rr[j] = val[i++]; /* xyz */
        }
    sol->stat = SOLQ_FIX;
    return 1;
}


/* decode solution position --------------------------------------------------*/
int decode_solpos(char *buff, const solopt_t *opt, sol_t *sol)
{
    sol_t sol0 = {{0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, '0', '0', '0', 0, 0, 0};
    char *p = buff;

    trace(4, "decode_solpos: buff=%s\n", buff);

    *sol = sol0;

    /* decode solution time */
    if (!(p = decode_soltime(p, opt, &sol->time)))
        {
            return 0;
        }
    /* decode solution position */
    switch (opt->posf)
        {
        case SOLF_XYZ:
            return decode_solxyz(p, opt, sol);
        case SOLF_LLH:
            return decode_solllh(p, opt, sol);
        case SOLF_ENU:
            return decode_solenu(p, opt, sol);
        case SOLF_GSIF:
            return decode_solgsi(p, opt, sol);
        }
    return 0;
}


/* decode reference position -------------------------------------------------*/
void decode_refpos(char *buff, const solopt_t *opt, double *rb)
{
    double val[MAXFIELD];
    double pos[3];
    int i;
    int n;
    const char *sep = opt2sep(opt);

    trace(3, "decode_refpos: buff=%s\n", buff);

    if ((n = tonum(buff, sep, val)) < 3)
        {
            return;
        }

    if (opt->posf == SOLF_XYZ)
        { /* xyz */
            for (i = 0; i < 3; i++)
                {
                    rb[i] = val[i];
                }
        }
    else if (opt->degf == 0)
        { /* lat/lon/hgt (ddd.ddd) */
            pos[0] = val[0] * D2R;
            pos[1] = val[1] * D2R;
            pos[2] = val[2];
            pos2ecef(pos, rb);
        }
    else if (opt->degf == 1 && n >= 7)
        { /* lat/lon/hgt (ddd mm ss) */
            pos[0] = dms2deg(val) * D2R;
            pos[1] = dms2deg(val + 3) * D2R;
            pos[2] = val[6];
            pos2ecef(pos, rb);
        }
}


/* decode solution -----------------------------------------------------------*/
int decode_sol(char *buff, const solopt_t *opt, sol_t *sol, double *rb)
{
    char *p;

    trace(4, "decode_sol: buff=%s\n", buff);

    if (!strncmp(buff, COMMENTH, 1))
        { /* reference position */
            if (!strstr(buff, "ref pos") && !strstr(buff, "slave pos"))
                {
                    return 0;
                }
            if (!(p = strchr(buff, ':')))
                {
                    return 0;
                }
            decode_refpos(p + 1, opt, rb);
            return 0;
        }
    if (!strncmp(buff, "$GP", 3))
        { /* decode nmea */
            if (!decode_nmea(buff, sol))
                {
                    return 0;
                }

            /* for time update only */
            if (opt->posf != SOLF_NMEA && !strncmp(buff, "$GPRMC", 6))
                {
                    return 2;
                }
        }
    else
        { /* decode position record */
            if (!decode_solpos(buff, opt, sol))
                {
                    return 0;
                }
        }
    return 1;
}


/* decode solution options ---------------------------------------------------*/
void decode_solopt(char *buff, solopt_t *opt)
{
    char *p;

    trace(4, "decode_solhead: buff=%s\n", buff);

    if (strncmp(buff, COMMENTH, 1) != 0 && strncmp(buff, "+", 1) != 0)
        {
            return;
        }

    if (strstr(buff, "GPST"))
        {
            opt->times = TIMES_GPST;
        }
    else if (strstr(buff, "UTC"))
        {
            opt->times = TIMES_UTC;
        }
    else if (strstr(buff, "JST"))
        {
            opt->times = TIMES_JST;
        }

    if ((p = strstr(buff, "x-ecef(m)")))
        {
            opt->posf = SOLF_XYZ;
            opt->degf = 0;
            strncpy(opt->sep, p + 9, 1);
            opt->sep[1] = '\0';
        }
    else if ((p = strstr(buff, "latitude(d'\")")))
        {
            opt->posf = SOLF_LLH;
            opt->degf = 1;
            strncpy(opt->sep, p + 14, 1);
            opt->sep[1] = '\0';
        }
    else if ((p = strstr(buff, "latitude(deg)")))
        {
            opt->posf = SOLF_LLH;
            opt->degf = 0;
            strncpy(opt->sep, p + 13, 1);
            opt->sep[1] = '\0';
        }
    else if ((p = strstr(buff, "e-baseline(m)")))
        {
            opt->posf = SOLF_ENU;
            opt->degf = 0;
            strncpy(opt->sep, p + 13, 1);
            opt->sep[1] = '\0';
        }
    else if ((p = strstr(buff, "+SITE/INF")))
        { /* gsi f2/f3 solution */
            opt->times = TIMES_GPST;
            opt->posf = SOLF_GSIF;
            opt->degf = 0;
            std::strncpy(opt->sep, " ", 2);
        }
}


/* read solution option ------------------------------------------------------*/
void readsolopt(FILE *fp, solopt_t *opt)
{
    char buff[MAXSOLMSG + 1];
    int i;

    trace(3, "readsolopt:\n");

    for (i = 0; fgets(buff, sizeof(buff), fp) && i < 100; i++)
        { /* only 100 lines */
            /* decode solution options */
            decode_solopt(buff, opt);
        }
}


/* input solution data from stream ---------------------------------------------
 * input solution data from stream
 * args   : unsigned char data I stream data
 *          gtime_t ts       I  start time (ts.time == 0: from start)
 *          gtime_t te       I  end time   (te.time == 0: to end)
 *          double tint      I  time interval (0: all)
 *          int    qflag     I  quality flag  (0: all)
 *          solbuf_t *solbuf IO solution buffer
 * return : status (1:solution received,0:no solution,-1:disconnect received)
 *-----------------------------------------------------------------------------*/
int inputsol(unsigned char data, gtime_t ts, gtime_t te, double tint,
    int qflag, const solopt_t *opt, solbuf_t *solbuf)
{
    sol_t sol = {{0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, '0', '0', '0', 0, 0, 0};
    int stat;

    trace(4, "inputsol: data=0x%02x\n", data);

    sol.time = solbuf->time;

    if (data == '$' || (!isprint(data) && data != '\r' && data != '\n'))
        { /* sync header */
            solbuf->nb = 0;
        }
    solbuf->buff[solbuf->nb++] = data;
    if (data != '\n' && solbuf->nb < MAXSOLMSG)
        {
            return 0; /* sync trailer */
        }

    solbuf->buff[solbuf->nb] = '\0';
    solbuf->nb = 0;

    /* check disconnect message */
    if (!strcmp(reinterpret_cast<char *>(solbuf->buff), MSG_DISCONN))
        {
            trace(3, "disconnect received\n");
            return -1;
        }
    /* decode solution */
    if ((stat = decode_sol(reinterpret_cast<char *>(solbuf->buff), opt, &sol, solbuf->rb)) > 0)
        {
            solbuf->time = sol.time; /* update current time */
        }
    if (stat != 1 || !screent(sol.time, ts, te, tint) || (qflag && sol.stat != qflag))
        {
            return 0;
        }
    /* add solution to solution buffer */
    return addsol(solbuf, &sol);
}


/* read solution data --------------------------------------------------------*/
int readsoldata(FILE *fp, gtime_t ts, gtime_t te, double tint, int qflag,
    const solopt_t *opt, solbuf_t *solbuf)
{
    int c;

    trace(3, "readsoldata:\n");

    while ((c = fgetc(fp)) != EOF)
        {
            /* input solution */
            inputsol(static_cast<unsigned char>(c), ts, te, tint, qflag, opt, solbuf);
        }
    return solbuf->n > 0;
}


/* compare solution data -----------------------------------------------------*/
int cmpsol(const void *p1, const void *p2)
{
    const auto *q1 = static_cast<const sol_t *>(p1);
    const auto *q2 = static_cast<const sol_t *>(p2);
    double tt = timediff(q1->time, q2->time);
    return tt < -0.0 ? -1 : (tt > 0.0 ? 1 : 0);
}


/* sort solution data --------------------------------------------------------*/
int sort_solbuf(solbuf_t *solbuf)
{
    sol_t *solbuf_data;

    trace(4, "sort_solbuf: n=%d\n", solbuf->n);

    if (solbuf->n <= 0)
        {
            return 0;
        }

    if (!(solbuf_data = static_cast<sol_t *>(realloc(solbuf->data, sizeof(sol_t) * solbuf->n))))
        {
            trace(1, "sort_solbuf: memory allocation error\n");
            free(solbuf->data);
            solbuf->data = nullptr;
            solbuf->n = solbuf->nmax = 0;
            return 0;
        }
    solbuf->data = solbuf_data;
    qsort(solbuf->data, solbuf->n, sizeof(sol_t), cmpsol);
    solbuf->nmax = solbuf->n;
    solbuf->start = 0;
    solbuf->end = solbuf->n - 1;
    return 1;
}


/* read solutions data from solution files -------------------------------------
 * read solution data from soluiton files
 * args   : char   *files[]  I  solution files
 *          int    nfile     I  number of files
 *         (gtime_t ts)      I  start time (ts.time == 0: from start)
 *         (gtime_t te)      I  end time   (te.time == 0: to end)
 *         (double tint)     I  time interval (0: all)
 *         (int    qflag)    I  quality flag  (0: all)
 *          solbuf_t *solbuf O  solution buffer
 * return : status (1:ok,0:no data or error)
 *-----------------------------------------------------------------------------*/
int readsolt(char *files[], int nfile, gtime_t ts, gtime_t te,
    double tint, int qflag, solbuf_t *solbuf)
{
    FILE *fp;
    solopt_t opt = SOLOPT_DEFAULT;
    int i;

    trace(3, "readsolt: nfile=%d\n", nfile);

    initsolbuf(solbuf, 0, 0);

    for (i = 0; i < nfile; i++)
        {
            if (!(fp = fopen(files[i], "rbe")))
                {
                    trace(1, "readsolt: file open error %s\n", files[i]);
                    continue;
                }
            /* read solution options in header */
            readsolopt(fp, &opt);
            rewind(fp);

            /* read solution data */
            if (!readsoldata(fp, ts, te, tint, qflag, &opt, solbuf))
                {
                    trace(1, "readsolt: no solution in %s\n", files[i]);
                }
            fclose(fp);
        }
    return sort_solbuf(solbuf);
}


int readsol(char *files[], int nfile, solbuf_t *sol)
{
    gtime_t time = {0, 0.0};

    trace(3, "readsol: nfile=%d\n", nfile);

    return readsolt(files, nfile, time, time, 0.0, 0, sol);
}


/* add solution data to solution buffer ----------------------------------------
 * add solution data to solution buffer
 * args   : solbuf_t *solbuf IO solution buffer
 *          sol_t  *sol      I  solution data
 * return : status (1:ok,0:error)
 *-----------------------------------------------------------------------------*/
int addsol(solbuf_t *solbuf, const sol_t *sol)
{
    sol_t *solbuf_data;

    trace(4, "addsol:\n");

    if (solbuf->cyclic)
        { /* ring buffer */
            if (solbuf->nmax <= 1)
                {
                    return 0;
                }
            solbuf->data[solbuf->end] = *sol;
            if (++solbuf->end >= solbuf->nmax)
                {
                    solbuf->end = 0;
                }
            if (solbuf->start == solbuf->end)
                {
                    if (++solbuf->start >= solbuf->nmax)
                        {
                            solbuf->start = 0;
                        }
                }
            else
                {
                    solbuf->n++;
                }

            return 1;
        }

    if (solbuf->n >= solbuf->nmax)
        {
            solbuf->nmax = solbuf->nmax == 0 ? 8192 : solbuf->nmax * 2;
            if (!(solbuf_data = static_cast<sol_t *>(realloc(solbuf->data, sizeof(sol_t) * solbuf->nmax))))
                {
                    trace(1, "addsol: memory allocation error\n");
                    free(solbuf->data);
                    solbuf->data = nullptr;
                    solbuf->n = solbuf->nmax = 0;
                    return 0;
                }
            solbuf->data = solbuf_data;
        }
    solbuf->data[solbuf->n++] = *sol;
    return 1;
}


/* get solution data from solution buffer --------------------------------------
 * get solution data by index from solution buffer
 * args   : solbuf_t *solbuf I  solution buffer
 *          int    index     I  index of solution (0...)
 * return : solution data pointer (NULL: no solution, out of range)
 *-----------------------------------------------------------------------------*/
sol_t *getsol(solbuf_t *solbuf, int index)
{
    trace(4, "getsol: index=%d\n", index);

    if (index < 0 || solbuf->n <= index)
        {
            return nullptr;
        }
    if ((index = solbuf->start + index) >= solbuf->nmax)
        {
            index -= solbuf->nmax;
        }
    return solbuf->data + index;
}


/* initialize solution buffer --------------------------------------------------
 * initialize position solutions
 * args   : solbuf_t *solbuf I  solution buffer
 *          int    cyclic    I  solution data buffer type (0:linear,1:cyclic)
 *          int    nmax      I  initial number of solution data
 * return : status (1:ok,0:error)
 *-----------------------------------------------------------------------------*/
void initsolbuf(solbuf_t *solbuf, int cyclic, int nmax)
{
    gtime_t time0 = {0, 0.0};

    trace(3, "initsolbuf: cyclic=%d nmax=%d\n", cyclic, nmax);

    solbuf->n = solbuf->nmax = solbuf->start = solbuf->end = 0;
    solbuf->cyclic = cyclic;
    solbuf->time = time0;
    solbuf->data = nullptr;
    if (cyclic)
        {
            if (nmax <= 2)
                {
                    nmax = 2;
                }
            if (!(solbuf->data = static_cast<sol_t *>(malloc(sizeof(sol_t) * nmax))))
                {
                    trace(1, "initsolbuf: memory allocation error\n");
                    return;
                }
            solbuf->nmax = nmax;
        }
}


/* free solution ---------------------------------------------------------------
 * free memory for solution buffer
 * args   : solbuf_t *solbuf I  solution buffer
 * return : none
 *-----------------------------------------------------------------------------*/
void freesolbuf(solbuf_t *solbuf)
{
    trace(3, "freesolbuf: n=%d\n", solbuf->n);

    free(solbuf->data);
    solbuf->n = solbuf->nmax = solbuf->start = solbuf->end = 0;
    solbuf->data = nullptr;
}


void freesolstatbuf(solstatbuf_t *solstatbuf)
{
    trace(3, "freesolstatbuf: n=%d\n", solstatbuf->n);

    solstatbuf->n = solstatbuf->nmax = 0;
    free(solstatbuf->data);
    solstatbuf->data = nullptr;
}


/* compare solution status ---------------------------------------------------*/
int cmpsolstat(const void *p1, const void *p2)
{
    const auto *q1 = static_cast<const solstat_t *>(p1);
    const auto *q2 = static_cast<const solstat_t *>(p2);
    double tt = timediff(q1->time, q2->time);
    return tt < -0.0 ? -1 : (tt > 0.0 ? 1 : 0);
}


/* sort solution data --------------------------------------------------------*/
int sort_solstat(solstatbuf_t *statbuf)
{
    solstat_t *statbuf_data;

    trace(4, "sort_solstat: n=%d\n", statbuf->n);

    if (statbuf->n <= 0)
        {
            return 0;
        }

    if (!(statbuf_data = static_cast<solstat_t *>(realloc(statbuf->data, sizeof(solstat_t) * statbuf->n))))
        {
            trace(1, "sort_solstat: memory allocation error\n");
            free(statbuf->data);
            statbuf->data = nullptr;
            statbuf->n = statbuf->nmax = 0;
            return 0;
        }
    statbuf->data = statbuf_data;
    qsort(statbuf->data, statbuf->n, sizeof(solstat_t), cmpsolstat);
    statbuf->nmax = statbuf->n;
    return 1;
}


/* decode solution status ----------------------------------------------------*/
int decode_solstat(char *buff, solstat_t *stat)
{
    static const solstat_t stat0 = {{0, 0.0}, '0', '0', 0, 0, 0, 0, '0', '0', 0, 0, 0, 0};
    double tow;
    double az;
    double el;
    double resp;
    double resc;
    int n;
    int week;
    int sat;
    int frq;
    int vsat;
    int snr;
    int fix;
    int slip;
    int lock;
    int outc;
    int slipc;
    int rejc;
    char id[32] = "";
    char *p;

    trace(4, "decode_solstat: buff=%s\n", buff);

    if (strstr(buff, "$SAT") != buff)
        {
            return 0;
        }

    for (p = buff; *p; p++)
        {
            if (*p == ',')
                {
                    *p = ' ';
                }
        }

    n = sscanf(buff, "$SAT%d%lf%s%d%lf%lf%lf%lf%d%d%d%d%d%d%d%d",
        &week, &tow, id, &frq, &az, &el, &resp, &resc, &vsat, &snr, &fix, &slip,
        &lock, &outc, &slipc, &rejc);

    if (n < 15)
        {
            trace(2, "invalid format of solution status: %s\n", buff);
            return 0;
        }
    if ((sat = satid2no(id)) <= 0)
        {
            trace(2, "invalid satellite in solution status: %s\n", id);
            return 0;
        }
    *stat = stat0;
    stat->time = gpst2time(week, tow);
    stat->sat = static_cast<unsigned char>(sat);
    stat->frq = static_cast<unsigned char>(frq);
    stat->az = static_cast<float>(az * D2R);
    stat->el = static_cast<float>(el * D2R);
    stat->resp = static_cast<float>(resp);
    stat->resc = static_cast<float>(resc);
    stat->flag = static_cast<unsigned char>((vsat << 5) + (slip << 3) + fix);
    stat->snr = static_cast<unsigned char>(std::lround(snr * 4.0));
    stat->lock = static_cast<uint16_t>(lock);
    stat->outc = static_cast<uint16_t>(outc);
    stat->slipc = static_cast<uint16_t>(slipc);
    stat->rejc = static_cast<uint16_t>(rejc);
    return 1;
}


/* add solution status data --------------------------------------------------*/
void addsolstat(solstatbuf_t *statbuf, const solstat_t *stat)
{
    solstat_t *statbuf_data;

    trace(4, "addsolstat:\n");

    if (statbuf->n >= statbuf->nmax)
        {
            statbuf->nmax = statbuf->nmax == 0 ? 8192 : statbuf->nmax * 2;
            if (!(statbuf_data = static_cast<solstat_t *>(realloc(statbuf->data, sizeof(solstat_t) *
                                                                                     statbuf->nmax))))
                {
                    trace(1, "addsolstat: memory allocation error\n");
                    free(statbuf->data);
                    statbuf->data = nullptr;
                    statbuf->n = statbuf->nmax = 0;
                    return;
                }
            statbuf->data = statbuf_data;
        }
    statbuf->data[statbuf->n++] = *stat;
}


/* read solution status data -------------------------------------------------*/
int readsolstatdata(FILE *fp, gtime_t ts, gtime_t te, double tint,
    solstatbuf_t *statbuf)
{
    solstat_t stat = {{0, 0.0}, '0', '0', 0, 0, 0, 0, '0', '0', 0, 0, 0, 0};
    char buff[MAXSOLMSG + 1];

    trace(3, "readsolstatdata:\n");

    while (fgets(buff, sizeof(buff), fp))
        {
            /* decode solution status */
            if (!decode_solstat(buff, &stat))
                {
                    continue;
                }

            /* add solution to solution buffer */
            if (screent(stat.time, ts, te, tint))
                {
                    addsolstat(statbuf, &stat);
                }
        }
    return statbuf->n > 0;
}


/* read solution status --------------------------------------------------------
 * read solution status from solution status files
 * args   : char   *files[]  I  solution status files
 *          int    nfile     I  number of files
 *         (gtime_t ts)      I  start time (ts.time == 0: from start)
 *         (gtime_t te)      I  end time   (te.time == 0: to end)
 *         (double tint)     I  time interval (0: all)
 *          solstatbuf_t *statbuf O  solution status buffer
 * return : status (1:ok,0:no data or error)
 *-----------------------------------------------------------------------------*/
int readsolstatt(char *files[], int nfile, gtime_t ts, gtime_t te,
    double tint, solstatbuf_t *statbuf)
{
    FILE *fp;
    char path[1024];
    int i;

    trace(3, "readsolstatt: nfile=%d\n", nfile);

    statbuf->n = statbuf->nmax = 0;
    statbuf->data = nullptr;

    for (i = 0; i < nfile; i++)
        {
            std::snprintf(path, sizeof(path), "%s.stat", files[i]);
            if (!(fp = fopen(path, "re")))
                {
                    trace(1, "readsolstatt: file open error %s\n", path);
                    continue;
                }
            /* read solution status data */
            if (!readsolstatdata(fp, ts, te, tint, statbuf))
                {
                    trace(1, "readsolt: no solution in %s\n", path);
                }
            fclose(fp);
        }
    return sort_solstat(statbuf);
}


int readsolstat(char *files[], int nfile, solstatbuf_t *statbuf)
{
    gtime_t time = {0, 0.0};

    trace(3, "readsolstat: nfile=%d\n", nfile);

    return readsolstatt(files, nfile, time, time, 0.0, statbuf);
}


/* output solution as the form of x/y/z-ecef ---------------------------------*/
int outecef(unsigned char *buff, const char *s, const sol_t *sol,
    const solopt_t *opt)
{
    const char *sep = opt2sep(opt);
    char *p = reinterpret_cast<char *>(buff);

    trace(3, "outecef:\n");

    p += std::snprintf(p, MAXSOLBUF, "%s%s%14.4f%s%14.4f%s%14.4f%s%3d%s%3d%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%6.2f%s%6.1f\n",
        s, sep, sol->rr[0], sep, sol->rr[1], sep, sol->rr[2], sep, sol->stat, sep,
        sol->ns, sep, SQRT_SOL(sol->qr[0]), sep, SQRT_SOL(sol->qr[1]), sep, SQRT_SOL(sol->qr[2]),
        sep, sqvar(sol->qr[3]), sep, sqvar(sol->qr[4]), sep, sqvar(sol->qr[5]),
        sep, sol->age, sep, sol->ratio);
    return p - reinterpret_cast<char *>(buff);
}


/* output solution as the form of lat/lon/height -----------------------------*/
int outpos(unsigned char *buff, const char *s, const sol_t *sol,
    const solopt_t *opt)
{
    double pos[3];
    double dms1[3];
    double dms2[3];
    double P[9];
    double Q[9];
    const char *sep = opt2sep(opt);
    char *p = reinterpret_cast<char *>(buff);
    char *start;
    start = p;

    trace(3, "outpos  :\n");

    ecef2pos(sol->rr, pos);
    soltocov(sol, P);
    covenu(pos, P, Q);
    if (opt->height == 1)
        { /* geodetic height */
            // pos[2] -= geoidh(pos);
        }
    if (opt->degf)
        {
            deg2dms(pos[0] * R2D, dms1);
            deg2dms(pos[1] * R2D, dms2);
            p += std::snprintf(p, MAXSOLMSG - (p - start), "%s%s%4.0f%s%02.0f%s%08.5f%s%4.0f%s%02.0f%s%08.5f", s, sep,
                dms1[0], sep, dms1[1], sep, dms1[2], sep, dms2[0], sep, dms2[1], sep,
                dms2[2]);
        }
    else
        {
            p += std::snprintf(p, MAXSOLMSG - (p - start), "%s%s%14.9f%s%14.9f", s, sep, pos[0] * R2D, sep, pos[1] * R2D);
        }
    p += std::snprintf(p, MAXSOLMSG - (p - start), "%s%10.4f%s%3d%s%3d%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%6.2f%s%6.1f\n",
        sep, pos[2], sep, sol->stat, sep, sol->ns, sep, SQRT_SOL(Q[4]), sep,
        SQRT_SOL(Q[0]), sep, SQRT_SOL(Q[8]), sep, sqvar(Q[1]), sep, sqvar(Q[2]),
        sep, sqvar(Q[5]), sep, sol->age, sep, sol->ratio);
    return p - reinterpret_cast<char *>(buff);
}


/* output solution as the form of e/n/u-baseline -----------------------------*/
int outenu(unsigned char *buff, const char *s, const sol_t *sol,
    const double *rb, const solopt_t *opt)
{
    double pos[3];
    double rr[3];
    double enu[3];
    double P[9];
    double Q[9];
    int i;
    const char *sep = opt2sep(opt);
    char *p = reinterpret_cast<char *>(buff);

    trace(3, "outenu  :\n");

    for (i = 0; i < 3; i++)
        {
            rr[i] = sol->rr[i] - rb[i];
        }
    ecef2pos(rb, pos);
    soltocov(sol, P);
    covenu(pos, P, Q);
    ecef2enu(pos, rr, enu);
    p += std::snprintf(p, MAXSOLMSG, "%s%s%14.4f%s%14.4f%s%14.4f%s%3d%s%3d%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%6.2f%s%6.1f\n",
        s, sep, enu[0], sep, enu[1], sep, enu[2], sep, sol->stat, sep, sol->ns, sep,
        SQRT_SOL(Q[0]), sep, SQRT_SOL(Q[4]), sep, SQRT_SOL(Q[8]), sep, sqvar(Q[1]),
        sep, sqvar(Q[5]), sep, sqvar(Q[2]), sep, sol->age, sep, sol->ratio);
    return p - reinterpret_cast<char *>(buff);
}


/* output solution in the form of nmea RMC sentence --------------------------*/
int outnmea_rmc(unsigned char *buff, const sol_t *sol)
{
    static double dirp = 0.0;
    gtime_t time;
    double ep[6];
    double pos[3];
    double enuv[3];
    double dms1[3];
    double dms2[3];
    double vel;
    double dir;
    double amag = 0.0;
    char *p = reinterpret_cast<char *>(buff);
    char *q;
    char sum;
    char *emag = const_cast<char *>("E");
    const int MSG_TAIL = 6;

    trace(3, "outnmea_rmc:\n");

    if (sol->stat <= SOLQ_NONE)
        {
            p += std::snprintf(p, MAXSOLBUF - MSG_TAIL, "$GPRMC,,,,,,,,,,,,");
            for (q = reinterpret_cast<char *>(buff) + 1, sum = 0; *q; q++)
                {
                    sum ^= *q;
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
            return p - reinterpret_cast<char *>(buff);
        }
    time = gpst2utc(sol->time);
    if (time.sec >= 0.995)
        {
            time.time++;
            time.sec = 0.0;
        }
    time2epoch(time, ep);
    ecef2pos(sol->rr, pos);
    ecef2enu(pos, sol->rr + 3, enuv);
    vel = norm_rtk(enuv, 3);
    if (vel >= 1.0)
        {
            dir = atan2(enuv[0], enuv[1]) * R2D;
            if (dir < 0.0)
                {
                    dir += 360.0;
                }
            dirp = dir;
        }
    else
        {
            dir = dirp;
        }
    deg2dms(fabs(pos[0]) * R2D, dms1);
    deg2dms(fabs(pos[1]) * R2D, dms2);
    p += std::snprintf(p, MAXSOLBUF - MSG_TAIL, "$GPRMC,%02.0f%02.0f%05.2f,A,%02.0f%010.7f,%s,%03.0f%010.7f,%s,%4.2f,%4.2f,%02.0f%02.0f%02d,%.1f,%s,%s",
        ep[3], ep[4], ep[5], dms1[0], dms1[1] + dms1[2] / 60.0, pos[0] >= 0 ? "N" : "S",
        dms2[0], dms2[1] + dms2[2] / 60.0, pos[1] >= 0 ? "E" : "W", vel / KNOT2M, dir,
        ep[2], ep[1], static_cast<int>(ep[0]) % 100, amag, emag,
        sol->stat == SOLQ_DGPS || sol->stat == SOLQ_FLOAT || sol->stat == SOLQ_FIX ? "D" : "A");
    for (q = reinterpret_cast<char *>(buff) + 1, sum = 0; *q; q++)
        {
            sum ^= *q; /* check-sum */
        }
    p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
    return p - reinterpret_cast<char *>(buff);
}


/* output solution in the form of nmea GGA sentence --------------------------*/
int outnmea_gga(unsigned char *buff, const sol_t *sol)
{
    gtime_t time;
    double h;
    double ep[6];
    double pos[3];
    double dms1[3];
    double dms2[3];
    double dop = 1.0;
    int solq;
    char *p = reinterpret_cast<char *>(buff);
    char *q;
    char sum;
    const int MSG_TAIL = 6;

    trace(3, "outnmea_gga:\n");

    if (sol->stat <= SOLQ_NONE)
        {
            p += std::snprintf(p, MAXSOLBUF - MSG_TAIL, "$GPGGA,,,,,,,,,,,,,,");
            for (q = reinterpret_cast<char *>(buff) + 1, sum = 0; *q; q++)
                {
                    sum ^= *q;
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
            return p - reinterpret_cast<char *>(buff);
        }
    for (solq = 0; solq < 8; solq++)
        {
            if (SOLQ_NMEA[solq] == sol->stat)
                {
                    break;
                }
        }
    if (solq >= 8)
        {
            solq = 0;
        }
    time = gpst2utc(sol->time);
    if (time.sec >= 0.995)
        {
            time.time++;
            time.sec = 0.0;
        }
    time2epoch(time, ep);
    ecef2pos(sol->rr, pos);
    h = 0;  // geoidh(pos);
    deg2dms(fabs(pos[0]) * R2D, dms1);
    deg2dms(fabs(pos[1]) * R2D, dms2);
    p += std::snprintf(p, MAXSOLBUF - MSG_TAIL, "$GPGGA,%02.0f%02.0f%05.2f,%02.0f%010.7f,%s,%03.0f%010.7f,%s,%d,%02d,%.1f,%.3f,M,%.3f,M,%.1f,",
        ep[3], ep[4], ep[5], dms1[0], dms1[1] + dms1[2] / 60.0, pos[0] >= 0 ? "N" : "S",
        dms2[0], dms2[1] + dms2[2] / 60.0, pos[1] >= 0 ? "E" : "W", solq,
        sol->ns, dop, pos[2] - h, h, sol->age);
    for (q = reinterpret_cast<char *>(buff) + 1, sum = 0; *q; q++)
        {
            sum ^= *q; /* check-sum */
        }
    p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
    return p - reinterpret_cast<char *>(buff);
}


/* output solution in the form of nmea GSA sentences -------------------------*/
int outnmea_gsa(unsigned char *buff, const sol_t *sol,
    const ssat_t *ssat)
{
    double azel[MAXSAT * 2];
    double dop[4];
    int i;
    int sat;
    int sys;
    int nsat;
    std::vector<int> prn(MAXSAT);
    char *p = reinterpret_cast<char *>(buff);
    char *q;
    char *s;
    char sum;
    const int MSG_TAIL = 6;
    const int COMMA_LENGTH = 2;
    const int MAX_LENGTH_INT = 10;

    trace(3, "outnmea_gsa:\n");

    if (sol->stat <= SOLQ_NONE)
        {
            p += std::snprintf(p, MAXSOLBUF - MSG_TAIL, "$GPGSA,A,1,,,,,,,,,,,,,,,");
            for (q = reinterpret_cast<char *>(buff) + 1, sum = 0; *q; q++)
                {
                    sum ^= *q;
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
            return p - reinterpret_cast<char *>(buff);
        }

    /* GPGSA: gps/sbas */
    for (sat = 1, nsat = 0; sat <= MAXSAT && nsat < 12; sat++)
        {
            if (!ssat[sat - 1].vs || ssat[sat - 1].azel[1] <= 0.0)
                {
                    continue;
                }
            sys = satsys(sat, &prn[nsat]);
            if (sys != SYS_GPS && sys != SYS_SBS)
                {
                    continue;
                }
            if (sys == SYS_SBS)
                {
                    prn[nsat] += 33 - MINPRNSBS;
                }
            for (i = 0; i < 2; i++)
                {
                    azel[i + nsat * 2] = ssat[sat - 1].azel[i];
                }
            nsat++;
        }
    if (nsat > 0)
        {
            s = p;
            p += std::snprintf(p, MAXSOLBUF, "$GPGSA,A,%d", sol->stat <= 0 ? 1 : 3);
            for (i = 0; i < 12; i++)
                {
                    if (i < nsat)
                        {
                            p += std::snprintf(p, MAX_LENGTH_INT + 2, ",%02d", prn[i]);
                        }
                    else
                        {
                            p += std::snprintf(p, COMMA_LENGTH, ",");
                        }
                }
            dops(nsat, azel, 0.0, dop);
            p += std::snprintf(p, MAXSOLBUF - (p - s), ",%3.1f,%3.1f,%3.1f,1", dop[1], dop[2], dop[3]);
            for (q = s + 1, sum = 0; *q; q++)
                {
                    sum ^= *q; /* check-sum */
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
        }
    /* GLGSA: glonass */
    for (sat = 1, nsat = 0; sat <= MAXSAT && nsat < 12; sat++)
        {
            if (!ssat[sat - 1].vs || ssat[sat - 1].azel[1] <= 0.0)
                {
                    continue;
                }
            if (satsys(sat, &prn[nsat]) != SYS_GLO)
                {
                    continue;
                }
            for (i = 0; i < 2; i++)
                {
                    azel[i + nsat * 2] = ssat[sat - 1].azel[i];
                }
            nsat++;
        }
    if (nsat > 0)
        {
            s = p;
            const int GLGSA_LENGTH = 11;
            p += std::snprintf(p, GLGSA_LENGTH, "$GLGSA,A,%d", sol->stat <= 0 ? 1 : 3);
            for (i = 0; i < 12; i++)
                {
                    if (i < nsat)
                        {
                            p += std::snprintf(p, MAX_LENGTH_INT + 2, ",%02d", prn[i] + 64);
                        }
                    else
                        {
                            p += std::snprintf(p, COMMA_LENGTH, ",");
                        }
                }
            dops(nsat, azel, 0.0, dop);
            p += std::snprintf(p, MAXSOLBUF - (p - s), ",%3.1f,%3.1f,%3.1f,2", dop[1], dop[2], dop[3]);
            for (q = s + 1, sum = 0; *q; q++)
                {
                    sum ^= *q; /* check-sum */
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
        }
    /* GAGSA: galileo */
    for (sat = 1, nsat = 0; sat <= MAXSAT && nsat < 12; sat++)
        {
            if (!ssat[sat - 1].vs || ssat[sat - 1].azel[1] <= 0.0)
                {
                    continue;
                }
            if (satsys(sat, &prn[nsat]) != SYS_GAL)
                {
                    continue;
                }
            for (i = 0; i < 2; i++)
                {
                    azel[i + nsat * 2] = ssat[sat - 1].azel[i];
                }
            nsat++;
        }
    if (nsat > 0)
        {
            s = p;
            p += std::snprintf(p, MAXSOLBUF, "$GAGSA,A,%d", sol->stat <= 0 ? 1 : 3);
            for (i = 0; i < 12; i++)
                {
                    if (i < nsat)
                        {
                            p += std::snprintf(p, MAX_LENGTH_INT + 2, ",%02d", prn[i]);
                        }
                    else
                        {
                            p += std::snprintf(p, COMMA_LENGTH, ",");
                        }
                }
            dops(nsat, azel, 0.0, dop);
            p += std::snprintf(p, MAXSOLBUF - (p - s), ",%3.1f,%3.1f,%3.1f,3", dop[1], dop[2], dop[3]);
            for (q = s + 1, sum = 0; *q; q++)
                {
                    sum ^= *q; /* check-sum */
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
        }
    /* BDGSA: beidou */
    for (sat = 1, nsat = 0; sat <= MAXSAT && nsat < 12; sat++)
        {
            if (!ssat[sat - 1].vs || ssat[sat - 1].azel[1] <= 0.0)
                {
                    continue;
                }
            if (satsys(sat, &prn[nsat]) != SYS_BDS)
                {
                    continue;
                }
            for (i = 0; i < 2; i++)
                {
                    azel[i + nsat * 2] = ssat[sat - 1].azel[i];
                }
            nsat++;
        }
    if (nsat > 0)
        {
            s = p;
            p += std::snprintf(p, MAXSOLBUF, "$BDGSA,A,%d", sol->stat <= 0 ? 1 : 3);
            for (i = 0; i < 12; i++)
                {
                    if (i < nsat)
                        {
                            p += std::snprintf(p, MAX_LENGTH_INT + 2, ",%02d", prn[i]);
                        }
                    else
                        {
                            p += std::snprintf(p, COMMA_LENGTH, ",");
                        }
                }
            dops(nsat, azel, 0.0, dop);
            p += std::snprintf(p, MAXSOLBUF - (p - s), ",%3.1f,%3.1f,%3.1f,3", dop[1], dop[2], dop[3]);
            for (q = s + 1, sum = 0; *q; q++)
                {
                    sum ^= *q; /* check-sum */
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
        }
    return p - reinterpret_cast<char *>(buff);
}


/* output solution in the form of nmea GSV sentence --------------------------*/
int outnmea_gsv(unsigned char *buff, const sol_t *sol,
    const ssat_t *ssat)
{
    double az;
    double el;
    double snr;
    int i;
    int j;
    int k;
    int n;
    int sat;
    int prn;
    int sys;
    int nmsg;
    std::vector<int> sats(MAXSAT);
    char *p = reinterpret_cast<char *>(buff);
    char *q;
    char *s;
    char sum;
    const int MSG_TAIL = 6;

    trace(3, "outnmea_gsv:\n");

    if (sol->stat <= SOLQ_NONE)
        {
            p += std::snprintf(p, MAXSOLBUF, "$GPGSV,1,1,0,,,,,,,,,,,,,,,,");
            for (q = reinterpret_cast<char *>(buff) + 1, sum = 0; *q; q++)
                {
                    sum ^= *q;
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
            return p - reinterpret_cast<char *>(buff);
        }
    /* GPGSV: gps/sbas */
    for (sat = 1, n = 0; sat < MAXSAT && n < 12; sat++)
        {
            sys = satsys(sat, &prn);
            if (sys != SYS_GPS && sys != SYS_SBS)
                {
                    continue;
                }
            if (ssat[sat - 1].vs && ssat[sat - 1].azel[1] > 0.0)
                {
                    sats[n++] = sat;
                }
        }
    nmsg = n <= 0 ? 0 : (n - 1) / 4 + 1;

    for (i = k = 0; i < nmsg; i++)
        {
            s = p;
            p += std::snprintf(p, MAXSOLBUF, "$GPGSV,%d,%d,%02d", nmsg, i + 1, n);

            for (j = 0; j < 4; j++, k++)
                {
                    if (k < n)
                        {
                            if (satsys(sats[k], &prn) == SYS_SBS)
                                {
                                    prn += 33 - MINPRNSBS;
                                }
                            az = ssat[sats[k] - 1].azel[0] * R2D;
                            if (az < 0.0)
                                {
                                    az += 360.0;
                                }
                            el = ssat[sats[k] - 1].azel[1] * R2D;
                            snr = ssat[sats[k] - 1].snr[0] * 0.25;
                            p += std::snprintf(p, MAXSOLBUF - (s - p), ",%02d,%02.0f,%03.0f,%02.0f", prn, el, az, snr);
                        }
                    else
                        {
                            p += std::snprintf(p, MAXSOLBUF - (s - p), ",,,,");
                        }
                }
            p += std::snprintf(p, MAXSOLBUF - (s - p), ",1"); /* L1C/A */
            for (q = s + 1, sum = 0; *q; q++)
                {
                    sum ^= *q; /* check-sum */
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
        }
    /* GLGSV: glonass */
    for (sat = 1, n = 0; sat < MAXSAT && n < 12; sat++)
        {
            if (satsys(sat, &prn) != SYS_GLO)
                {
                    continue;
                }
            if (ssat[sat - 1].vs && ssat[sat - 1].azel[1] > 0.0)
                {
                    sats[n++] = sat;
                }
        }
    nmsg = n <= 0 ? 0 : (n - 1) / 4 + 1;

    for (i = k = 0; i < nmsg; i++)
        {
            s = p;
            p += std::snprintf(p, MAXSOLBUF, "$GLGSV,%d,%d,%02d", nmsg, i + 1, n);

            for (j = 0; j < 4; j++, k++)
                {
                    if (k < n)
                        {
                            satsys(sats[k], &prn);
                            prn += 64; /* 65-99 */
                            az = ssat[sats[k] - 1].azel[0] * R2D;
                            if (az < 0.0)
                                {
                                    az += 360.0;
                                }
                            el = ssat[sats[k] - 1].azel[1] * R2D;
                            snr = ssat[sats[k] - 1].snr[0] * 0.25;
                            p += std::snprintf(p, MAXSOLBUF - (s - p), ",%02d,%02.0f,%03.0f,%02.0f", prn, el, az, snr);
                        }
                    else
                        {
                            p += std::snprintf(p, MAXSOLBUF - (s - p), ",,,,");
                        }
                }
            p += std::snprintf(p, MAXSOLBUF - (s - p), ",1"); /* L1C/A */
            for (q = s + 1, sum = 0; *q; q++)
                {
                    sum ^= *q; /* check-sum */
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
        }
    /* GAGSV: galileo */
    for (sat = 1, n = 0; sat < MAXSAT && n < 12; sat++)
        {
            if (satsys(sat, &prn) != SYS_GAL)
                {
                    continue;
                }
            if (ssat[sat - 1].vs && ssat[sat - 1].azel[1] > 0.0)
                {
                    sats[n++] = sat;
                }
        }
    nmsg = n <= 0 ? 0 : (n - 1) / 4 + 1;

    for (i = k = 0; i < nmsg; i++)
        {
            s = p;
            p += std::snprintf(p, MAXSOLBUF, "$GAGSV,%d,%d,%02d", nmsg, i + 1, n);

            for (j = 0; j < 4; j++, k++)
                {
                    if (k < n)
                        {
                            satsys(sats[k], &prn); /* 1-36 */
                            az = ssat[sats[k] - 1].azel[0] * R2D;
                            if (az < 0.0)
                                {
                                    az += 360.0;
                                }
                            el = ssat[sats[k] - 1].azel[1] * R2D;
                            snr = ssat[sats[k] - 1].snr[0] * 0.25;
                            p += std::snprintf(p, MAXSOLBUF - (s - p), ",%02d,%02.0f,%03.0f,%02.0f", prn, el, az, snr);
                        }
                    else
                        {
                            p += std::snprintf(p, MAXSOLBUF - (s - p), ",,,,");
                        }
                }
            p += std::snprintf(p, MAXSOLBUF - (s - p), ",7"); /* L1BC */
            for (q = s + 1, sum = 0; *q; q++)
                {
                    sum ^= *q; /* check-sum */
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
        }
    /* BDGSV: beidou */
    for (sat = 1, n = 0; sat < MAXSAT && n < 12; sat++)
        {
            if (satsys(sat, &prn) != SYS_BDS)
                {
                    continue;
                }
            if (ssat[sat - 1].vs && ssat[sat - 1].azel[1] > 0.0)
                {
                    sats[n++] = sat;
                }
        }
    nmsg = n <= 0 ? 0 : (n - 1) / 4 + 1;

    for (i = k = 0; i < nmsg; i++)
        {
            s = p;
            p += std::snprintf(p, MAXSOLBUF, "$BDGSV,%d,%d,%02d", nmsg, i + 1, n);

            for (j = 0; j < 4; j++, k++)
                {
                    if (k < n)
                        {
                            satsys(sats[k], &prn); /* 1-63 */
                            az = ssat[sats[k] - 1].azel[0] * R2D;
                            if (az < 0.0)
                                {
                                    az += 360.0;
                                }
                            el = ssat[sats[k] - 1].azel[1] * R2D;
                            snr = ssat[sats[k] - 1].snr[0] * 0.25;
                            p += std::snprintf(p, MAXSOLBUF - (s - p), ",%02d,%02.0f,%03.0f,%02.0f", prn, el, az, snr);
                        }
                    else
                        {
                            p += std::snprintf(p, MAXSOLBUF - (s - p), ",,,,");
                        }
                }
            p += std::snprintf(p, MAXSOLBUF - (s - p), ",1");
            for (q = s + 1, sum = 0; *q; q++)
                {
                    sum ^= *q; /* check-sum */
                }
            p += std::snprintf(p, MSG_TAIL, "*%02X%c%c", sum, 0x0D, 0x0A);
        }
    return p - reinterpret_cast<char *>(buff);
}


/* output processing options ---------------------------------------------------
 * output processing options to buffer
 * args   : unsigned char *buff IO output buffer
 *          prcopt_t *opt    I   processign options
 * return : number of output bytes
 *-----------------------------------------------------------------------------*/
int outprcopts(unsigned char *buff, const prcopt_t *opt)
{
    const int sys[] = {SYS_GPS, SYS_GLO, SYS_GAL, SYS_QZS, SYS_SBS, 0};
    const char *s1[] = {"single", "dgps", "kinematic", "static", "moving-base", "fixed",
        "ppp-kinematic", "ppp-static", "ppp-fixed", ""};
    const char *s2[] = {"L1", "L1+L2", "L1+L2+L5", "L1+L2+L5+L6", "L1+L2+L5+L6+L7",
        "L1+L2+L5+L6+L7+L8", ""};
    const char *s3[] = {"forward", "backward", "combined"};
    const char *s4[] = {"off", "broadcast", "sbas", "iono-free", "estimation",
        "ionex tec", "qzs", "lex", "vtec_sf", "vtec_ef", "gtec", ""};
    const char *s5[] = {"off", "saastamoinen", "sbas", "est ztd", "est ztd+grad", ""};
    const char *s6[] = {"broadcast", "precise", "broadcast+sbas", "broadcast+ssr apc",
        "broadcast+ssr com", "qzss lex", ""};
    const char *s7[] = {"gps", "glonass", "galileo", "qzss", "sbas", ""};
    const char *s8[] = {"off", "continuous", "instantaneous", "fix and hold", ""};
    const char *s9[] = {"off", "on", "auto calib", "external calib", ""};
    int i;
    char *p = reinterpret_cast<char *>(buff);
    char *s;
    s = p;

    trace(3, "outprcopts:\n");

    p += std::snprintf(p, MAXSOLMSG, "%s pos mode  : %s\n", COMMENTH, s1[opt->mode]);

    if (PMODE_DGPS <= opt->mode && opt->mode <= PMODE_FIXED)
        {
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%s freqs     : %s\n", COMMENTH, s2[opt->nf - 1]);
        }
    if (opt->mode > PMODE_SINGLE)
        {
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%s solution  : %s\n", COMMENTH, s3[opt->soltype]);
        }
    p += std::snprintf(p, MAXSOLMSG - (p - s), "%s elev mask : %.1f deg\n", COMMENTH, opt->elmin * R2D);
    if (opt->mode > PMODE_SINGLE)
        {
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%s dynamics  : %s\n", COMMENTH, opt->dynamics ? "on" : "off");
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%s tidecorr  : %s\n", COMMENTH, opt->tidecorr ? "on" : "off");
        }
    if (opt->mode <= PMODE_FIXED)
        {
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%s ionos opt : %s\n", COMMENTH, s4[opt->ionoopt]);
        }
    p += std::snprintf(p, MAXSOLMSG - (p - s), "%s tropo opt : %s\n", COMMENTH, s5[opt->tropopt]);
    p += std::snprintf(p, MAXSOLMSG - (p - s), "%s ephemeris : %s\n", COMMENTH, s6[opt->sateph]);
    if (opt->navsys != SYS_GPS)
        {
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%s navi sys  :", COMMENTH);
            for (i = 0; sys[i]; i++)
                {
                    if (opt->navsys & sys[i])
                        {
                            p += std::snprintf(p, MAXSOLMSG - (p - s), " %s", s7[i]);
                        }
                }
            p += std::snprintf(p, MAXSOLMSG - (p - s), "\n");
        }
    if (PMODE_KINEMA <= opt->mode && opt->mode <= PMODE_FIXED)
        {
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%s amb res   : %s\n", COMMENTH, s8[opt->modear]);
            if (opt->navsys & SYS_GLO)
                {
                    p += std::snprintf(p, MAXSOLMSG - (p - s), "%s amb glo   : %s\n", COMMENTH, s9[opt->glomodear]);
                }
            if (opt->thresar[0] > 0.0)
                {
                    p += std::snprintf(p, MAXSOLMSG - (p - s), "%s val thres : %.1f\n", COMMENTH, opt->thresar[0]);
                }
        }
    if (opt->mode == PMODE_MOVEB && opt->baseline[0] > 0.0)
        {
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%s baseline  : %.4f %.4f m\n", COMMENTH,
                opt->baseline[0], opt->baseline[1]);
        }
    for (i = 0; i < 2; i++)
        {
            if (opt->mode == PMODE_SINGLE || (i >= 1 && opt->mode > PMODE_FIXED))
                {
                    continue;
                }
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%s antenna%d  : %-21s (%7.4f %7.4f %7.4f)\n", COMMENTH,
                i + 1, opt->anttype[i], opt->antdel[i][0], opt->antdel[i][1],
                opt->antdel[i][2]);
        }
    return p - reinterpret_cast<char *>(buff);
}


/* output solution header ------------------------------------------------------
 * output solution header to buffer
 * args   : unsigned char *buff IO output buffer
 *          solopt_t *opt    I   solution options
 * return : number of output bytes
 *-----------------------------------------------------------------------------*/
int outsolheads(unsigned char *buff, const solopt_t *opt)
{
    const char *s1[] = {"WGS84", "Tokyo"};
    const char *s2[] = {"ellipsoidal", "geodetic"};
    const char *s3[] = {"GPST", "UTC ", "JST "};
    const char *sep = opt2sep(opt);
    char *p = reinterpret_cast<char *>(buff);
    int timeu = opt->timeu < 0 ? 0 : (opt->timeu > 20 ? 20 : opt->timeu);
    char *s;
    s = p;

    trace(3, "outsolheads:\n");

    if (opt->posf == SOLF_NMEA)
        {
            return 0;
        }

    if (opt->outhead)
        {
            p += std::snprintf(p, MAXSOLMSG, "%s (", COMMENTH);
            if (opt->posf == SOLF_XYZ)
                {
                    p += std::snprintf(p, MAXSOLMSG - (p - s), "x/y/z-ecef=WGS84");
                }
            else if (opt->posf == SOLF_ENU)
                {
                    p += std::snprintf(p, MAXSOLMSG - (p - s), "e/n/u-baseline=WGS84");
                }
            else
                {
                    p += std::snprintf(p, MAXSOLMSG - (p - s), "lat/lon/height=%s/%s", s1[opt->datum], s2[opt->height]);
                }
            p += std::snprintf(p, MAXSOLMSG - (p - s), ",Q=1:fix,2:float,3:sbas,4:dgps,5:single,6:ppp,ns=# of satellites)\n");
        }
    p += std::snprintf(p, MAXSOLMSG - (p - s), "%s  %-*s%s", COMMENTH, (opt->timef ? 16 : 8) + timeu + 1, s3[opt->times], sep);

    if (opt->posf == SOLF_LLH)
        { /* lat/lon/hgt */
            if (opt->degf)
                {
                    p += std::snprintf(p, MAXSOLMSG - (p - s), "%16s%s%16s%s%10s%s%3s%s%3s%s%8s%s%8s%s%8s%s%8s%s%8s%s%8s%s%6s%s%6s\n",
                        "latitude(d'\")", sep, "longitude(d'\")", sep, "height(m)", sep,
                        "Q", sep, "ns", sep, "sdn(m)", sep, "sde(m)", sep, "sdu(m)", sep,
                        "sdne(m)", sep, "sdeu(m)", sep, "sdue(m)", sep, "age(s)", sep, "ratio");
                }
            else
                {
                    p += std::snprintf(p, MAXSOLMSG - (p - s), "%14s%s%14s%s%10s%s%3s%s%3s%s%8s%s%8s%s%8s%s%8s%s%8s%s%8s%s%6s%s%6s\n",
                        "latitude(deg)", sep, "longitude(deg)", sep, "height(m)", sep,
                        "Q", sep, "ns", sep, "sdn(m)", sep, "sde(m)", sep, "sdu(m)", sep,
                        "sdne(m)", sep, "sdeu(m)", sep, "sdun(m)", sep, "age(s)", sep, "ratio");
                }
        }
    else if (opt->posf == SOLF_XYZ)
        { /* x/y/z-ecef */
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%14s%s%14s%s%14s%s%3s%s%3s%s%8s%s%8s%s%8s%s%8s%s%8s%s%8s%s%6s%s%6s\n",
                "x-ecef(m)", sep, "y-ecef(m)", sep, "z-ecef(m)", sep, "Q", sep, "ns", sep,
                "sdx(m)", sep, "sdy(m)", sep, "sdz(m)", sep, "sdxy(m)", sep,
                "sdyz(m)", sep, "sdzx(m)", sep, "age(s)", sep, "ratio");
        }
    else if (opt->posf == SOLF_ENU)
        { /* e/n/u-baseline */
            p += std::snprintf(p, MAXSOLMSG - (p - s), "%14s%s%14s%s%14s%s%3s%s%3s%s%8s%s%8s%s%8s%s%8s%s%8s%s%8s%s%6s%s%6s\n",
                "e-baseline(m)", sep, "n-baseline(m)", sep, "u-baseline(m)", sep,
                "Q", sep, "ns", sep, "sde(m)", sep, "sdn(m)", sep, "sdu(m)", sep,
                "sden(m)", sep, "sdnu(m)", sep, "sdue(m)", sep, "age(s)", sep, "ratio");
        }
    return p - reinterpret_cast<char *>(buff);
}


/* output solution body --------------------------------------------------------
 * output solution body to buffer
 * args   : unsigned char *buff IO output buffer
 *          sol_t  *sol      I   solution
 *          double *rb       I   base station position {x,y,z} (ecef) (m)
 *          solopt_t *opt    I   solution options
 * return : number of output bytes
 *-----------------------------------------------------------------------------*/
int outsols(unsigned char *buff, const sol_t *sol, const double *rb,
    const solopt_t *opt)
{
    gtime_t time;
    gtime_t ts = {0, 0.0};
    double gpst;
    int week;
    int timeu;
    const char *sep = opt2sep(opt);
    char s[255];
    unsigned char *p = buff;

    trace(3, "outsols :\n");

    if (opt->posf == SOLF_NMEA)
        {
            if (opt->nmeaintv[0] < 0.0)
                {
                    return 0;
                }
            if (!screent(sol->time, ts, ts, opt->nmeaintv[0]))
                {
                    return 0;
                }
        }
    if (sol->stat <= SOLQ_NONE || (opt->posf == SOLF_ENU && norm_rtk(rb, 3) <= 0.0))
        {
            return 0;
        }
    timeu = opt->timeu < 0 ? 0 : (opt->timeu > 20 ? 20 : opt->timeu);

    time = sol->time;
    if (opt->times >= TIMES_UTC)
        {
            time = gpst2utc(time);
        }
    if (opt->times == TIMES_JST)
        {
            time = timeadd(time, 9 * 3600.0);
        }

    if (opt->timef)
        {
            time2str(time, s, timeu);
        }
    else
        {
            gpst = time2gpst(time, &week);
            if (86400 * 7 - gpst < 0.5 / pow(10.0, timeu))
                {
                    week++;
                    gpst = 0.0;
                }
            std::snprintf(s, sizeof(s), "%4d%s%*.*f", week, sep, 6 + (timeu <= 0 ? 0 : timeu + 1), timeu, gpst);
        }
    switch (opt->posf)
        {
        case SOLF_LLH:
            p += outpos(p, s, sol, opt);
            break;
        case SOLF_XYZ:
            p += outecef(p, s, sol, opt);
            break;
        case SOLF_ENU:
            p += outenu(p, s, sol, rb, opt);
            break;
        case SOLF_NMEA:
            p += outnmea_rmc(p, sol);
            p += outnmea_gga(p, sol);
            break;
        }
    return p - buff;
}


/* output solution extended ----------------------------------------------------
 * output solution extended information
 * args   : unsigned char *buff IO output buffer
 *          sol_t  *sol      I   solution
 *          ssat_t *ssat     I   satellite status
 *          solopt_t *opt    I   solution options
 * return : number of output bytes
 * notes  : only support nmea
 *-----------------------------------------------------------------------------*/
int outsolexs(unsigned char *buff, const sol_t *sol, const ssat_t *ssat,
    const solopt_t *opt)
{
    gtime_t ts = {0, 0.0};
    unsigned char *p = buff;

    trace(3, "outsolexs:\n");

    if (opt->posf == SOLF_NMEA)
        {
            if (opt->nmeaintv[1] < 0.0)
                {
                    return 0;
                }
            if (!screent(sol->time, ts, ts, opt->nmeaintv[1]))
                {
                    return 0;
                }
        }
    if (opt->posf == SOLF_NMEA)
        {
            p += outnmea_gsa(p, sol, ssat);
            p += outnmea_gsv(p, sol, ssat);
        }
    return p - buff;
}


/* output processing option ----------------------------------------------------
 * output processing option to file
 * args   : FILE   *fp       I   output file pointer
 *          prcopt_t *opt    I   processing options
 * return : none
 *-----------------------------------------------------------------------------*/
void outprcopt(FILE *fp, const prcopt_t *opt)
{
    unsigned char buff[MAXSOLMSG + 1];
    int n;

    trace(3, "outprcopt:\n");

    if ((n = outprcopts(buff, opt)) > 0)
        {
            fwrite(buff, n, 1, fp);
        }
}


/* output solution header ------------------------------------------------------
 * output solution header to file
 * args   : FILE   *fp       I   output file pointer
 *          solopt_t *opt    I   solution options
 * return : none
 *-----------------------------------------------------------------------------*/
void outsolhead(FILE *fp, const solopt_t *opt)
{
    unsigned char buff[MAXSOLMSG + 1];
    int n;

    trace(3, "outsolhead:\n");

    if ((n = outsolheads(buff, opt)) > 0)
        {
            fwrite(buff, n, 1, fp);
        }
}


/* output solution body --------------------------------------------------------
 * output solution body to file
 * args   : FILE   *fp       I   output file pointer
 *          sol_t  *sol      I   solution
 *          double *rb       I   base station position {x,y,z} (ecef) (m)
 *          solopt_t *opt    I   solution options
 * return : none
 *-----------------------------------------------------------------------------*/
void outsol(FILE *fp, const sol_t *sol, const double *rb,
    const solopt_t *opt)
{
    unsigned char buff[MAXSOLMSG + 1];
    int n;

    trace(3, "outsol  :\n");

    if ((n = outsols(buff, sol, rb, opt)) > 0)
        {
            fwrite(buff, n, 1, fp);
        }
}


/* output solution extended ----------------------------------------------------
 * output solution extended information to file
 * args   : FILE   *fp       I   output file pointer
 *          sol_t  *sol      I   solution
 *          ssat_t *ssat     I   satellite status
 *          solopt_t *opt    I   solution options
 * return : output size (bytes)
 * notes  : only support nmea
 *-----------------------------------------------------------------------------*/
void outsolex(FILE *fp, const sol_t *sol, const ssat_t *ssat,
    const solopt_t *opt)
{
    unsigned char buff[MAXSOLMSG + 1];
    int n;

    trace(3, "outsolex:\n");

    if ((n = outsolexs(buff, sol, ssat, opt)) > 0)
        {
            fwrite(buff, n, 1, fp);
        }
}

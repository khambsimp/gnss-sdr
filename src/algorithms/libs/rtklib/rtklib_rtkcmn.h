/*!
 * \file rtklib_rtkcmn.h
 * \brief rtklib common functions
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
 * References :
 *     [1] IS-GPS-200M, Navstar GPS Space Segment/Navigation User Interfaces,
 *         May, 2021
 *     [2] RTCA/DO-229C, Minimum operational performance standards for global
 *         positioning system/wide area augmentation system airborne equipment,
 *         RTCA inc, November 28, 2001
 *     [3] M.Rothacher, R.Schmid, ANTEX: The Antenna Exchange Format Version 1.4,
 *         15 September, 2010
 *     [4] A.Gelb ed., Applied Optimal Estimation, The M.I.T Press, 1974
 *     [5] A.E.Niell, Global mapping functions for the atmosphere delay at radio
 *         wavelengths, Journal of geophysical research, 1996
 *     [6] W.Gurtner and L.Estey, RINEX The Receiver Independent Exchange Format
 *         Version 3.00, November 28, 2007
 *     [7] J.Kouba, A Guide to using International GNSS Service (IGS) products,
 *         May 2009
 *     [8] China Satellite Navigation Office, BeiDou navigation satellite system
 *         signal in space interface control document, open service signal B1I
 *         (version 1.0), Dec 2012
 *     [9] J.Boehm, A.Niell, P.Tregoning and H.Shuh, Global Mapping Function
 *         (GMF): A new empirical mapping function base on numerical weather
 *         model data, Geophysical Research Letters, 33, L07304, 2006
 *     [10] GLONASS/GPS/Galileo/Compass/SBAS NV08C receiver series BINR interface
 *         protocol specification ver.1.3, August, 2012
 *
 * -----------------------------------------------------------------------------
 */

#ifndef GNSS_SDR_RTKLIB_RTKCMN_H
#define GNSS_SDR_RTKLIB_RTKCMN_H

#include "rtklib.h"
#include <cstddef>
#include <string>

#if HAS_STD_FILESYSTEM
#include <system_error>
namespace errorlib = std;
#if HAS_STD_FILESYSTEM_EXPERIMENTAL
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif
#else
#include <boost/filesystem/operations.hpp>   // for create_directories, exists
#include <boost/filesystem/path.hpp>         // for path, operator<<
#include <boost/filesystem/path_traits.hpp>  // for filesystem
#include <boost/system/error_code.hpp>       // for error_code
namespace fs = boost::filesystem;
namespace errorlib = boost::system;
#endif

/* coordinate rotation matrix ------------------------------------------------*/
#define Rx(t, X)                                     \
    do                                               \
        {                                            \
            (X)[0] = 1.0;                            \
            (X)[1] = (X)[2] = (X)[3] = (X)[6] = 0.0; \
            (X)[4] = (X)[8] = cos(t);                \
            (X)[7] = sin(t);                         \
            (X)[5] = -(X)[7];                        \
        }                                            \
    while (0)

#define Ry(t, X)                                     \
    do                                               \
        {                                            \
            (X)[4] = 1.0;                            \
            (X)[1] = (X)[3] = (X)[5] = (X)[7] = 0.0; \
            (X)[0] = (X)[8] = cos(t);                \
            (X)[2] = sin(t);                         \
            (X)[6] = -(X)[2];                        \
        }                                            \
    while (0)

#define Rz(t, X)                                     \
    do                                               \
        {                                            \
            (X)[8] = 1.0;                            \
            (X)[2] = (X)[5] = (X)[6] = (X)[7] = 0.0; \
            (X)[0] = (X)[4] = cos(t);                \
            (X)[3] = sin(t);                         \
            (X)[1] = -(X)[3];                        \
        }                                            \
    while (0)

char *strncpy_no_trunc(char *out, size_t outsz, const char *in, size_t insz);
void fatalerr(const char *format, ...);
int satno(int sys, int prn);
int satsys(int sat, int *prn);
int satid2no(const char *id);
std::string satno2id(int sat);
int satexclude(int sat, int svh, const prcopt_t *opt);
int testsnr(int base, int freq, double el, double snr, const snrmask_t *mask);
unsigned char obs2code(const char *obs, int *freq);
char *code2obs(unsigned char code, int *freq);
void setcodepri(int sys, int freq, const char *pri);
int getcodepri(int sys, unsigned char code, const char *opt);
unsigned int getbitu(const unsigned char *buff, int pos, int len);
int getbits(const unsigned char *buff, int pos, int len);
void setbitu(unsigned char *buff, int pos, int len, unsigned int data);
void setbits(unsigned char *buff, int pos, int len, int data);
unsigned int rtk_crc32(const unsigned char *buff, int len);
unsigned int rtk_crc24q(const unsigned char *buff, int len);
unsigned short rtk_crc16(const unsigned char *buff, int len);
int decode_word(unsigned int word, unsigned char *data);
double *mat(int n, int m);
int *imat(int n, int m);
double *zeros(int n, int m);
double *eye(int n);
double dot(const double *a, const double *b, int n);
double norm_rtk(const double *a, int n);
void cross3(const double *a, const double *b, double *c);
int normv3(const double *a, double *b);
void matcpy(double *A, const double *B, int n, int m);
void matmul(const char *tr, int n, int k, int m, double alpha,
    const double *A, const double *B, double beta, double *C);
int matinv(double *A, int n);
int solve(const char *tr, const double *A, const double *Y, int n,
    int m, double *X);
int lsq(const double *A, const double *y, int n, int m, double *x,
    double *Q);
int filter_(const double *x, const double *P, const double *H,
    const double *v, const double *R, int n, int m,
    double *xp, double *Pp);
int filter(double *x, double *P, const double *H, const double *v,
    const double *R, int n, int m);
int smoother(const double *xf, const double *Qf, const double *xb,
    const double *Qb, int n, double *xs, double *Qs);
void matfprint(const double A[], int n, int m, int p, int q, FILE *fp);
void matsprint(const double A[], int n, int m, int p, int q, std::string &buffer);
void matprint(const double A[], int n, int m, int p, int q);
double str2num(const char *s, int i, int n);
int str2time(const char *s, int i, int n, gtime_t *t);
gtime_t epoch2time(const double *ep);
void time2epoch(gtime_t t, double *ep);
gtime_t gpst2time(int week, double sec);
double time2gpst(gtime_t t, int *week);
gtime_t gst2time(int week, double sec);
double time2gst(gtime_t t, int *week);
gtime_t bdt2time(int week, double sec);
double time2bdt(gtime_t t, int *week);
gtime_t timeadd(gtime_t t, double sec);
double timediff(gtime_t t1, gtime_t t2);
double timediffweekcrossover(gtime_t t1, gtime_t t2);
gtime_t timeget();
void timeset(gtime_t t);
int read_leaps_text(FILE *fp);
int read_leaps_usno(FILE *fp);
int read_leaps(const char *file);
gtime_t gpst2utc(gtime_t t);
gtime_t utc2gpst(gtime_t t);
gtime_t gpst2bdt(gtime_t t);
gtime_t bdt2gpst(gtime_t t);
double time2sec(gtime_t time, gtime_t *day);
double utc2gmst(gtime_t t, double ut1_utc);
void time2str(gtime_t t, char *s, int n);
char *time_str(gtime_t t, int n);
double time2doy(gtime_t t);
int adjgpsweek(int week, bool pre_2009_file = false);
unsigned int tickget();
void sleepms(int ms);
void deg2dms(double deg, double *dms, int ndec);
void deg2dms(double deg, double *dms);
double dms2deg(const double *dms);
void ecef2pos(const double *r, double *pos);
void pos2ecef(const double *pos, double *r);
void xyz2enu(const double *pos, double *E);
void ecef2enu(const double *pos, const double *r, double *e);
void enu2ecef(const double *pos, const double *e, double *r);
void covenu(const double *pos, const double *P, double *Q);
void covecef(const double *pos, const double *Q, double *P);
void ast_args(double t, double *f);
void nut_iau1980(double t, const double *f, double *dpsi, double *deps);
void eci2ecef(gtime_t tutc, const double *erpv, double *U, double *gmst);
int decodef(char *p, int n, double *v);
void addpcv(const pcv_t *pcv, pcvs_t *pcvs);
int readngspcv(const char *file, pcvs_t *pcvs);
int readantex(const char *file, pcvs_t *pcvs);
int readpcv(const char *file, pcvs_t *pcvs);
pcv_t *searchpcv(int sat, const char *type, gtime_t time,
    const pcvs_t *pcvs);
void readpos(const char *file, const char *rcv, double *pos);
int readblqrecord(FILE *fp, double *odisp);
int readblq(const char *file, const char *sta, double *odisp);
int readerp(const char *file, erp_t *erp);
int geterp(const erp_t *erp, gtime_t time, double *erpv);
int cmpeph(const void *p1, const void *p2);
void uniqeph(nav_t *nav);
int cmpgeph(const void *p1, const void *p2);
void uniqgeph(nav_t *nav);
int cmpseph(const void *p1, const void *p2);
void uniqseph(nav_t *nav);
void uniqnav(nav_t *nav);
int cmpobs(const void *p1, const void *p2);
int sortobs(obs_t *obs);
int screent(gtime_t time, gtime_t ts, gtime_t te, double tint);
int readnav(const char *file, nav_t *nav);
int savenav(const char *file, const nav_t *nav);
void freeobs(obs_t *obs);
void freenav(nav_t *nav, int opt);

void traceopen(const char *file);
void traceclose();
void tracelevel(int level);
void traceswap();
void trace(int level, const char *format, ...);
void tracet(int level, const char *format, ...);
void tracemat(int level, const double *A, int n, int m, int p, int q);
void traceobs(int level, const obsd_t *obs, int n);
// void tracenav(int level, const nav_t *nav);
// void tracegnav(int level, const nav_t *nav);
// void tracehnav(int level, const nav_t *nav);
// void tracepeph(int level, const nav_t *nav);
// void tracepclk(int level, const nav_t *nav);
// void traceb  (int level, const unsigned char *p, int n);

int execcmd(const char *cmd);
void createdir(fs::path const &path);
int reppath(std::string const &path, std::string &rpath, gtime_t time, const char *rov,
    const char *base);
double satwavelen(int sat, int frq, const nav_t *nav);
double geodist(const double *rs, const double *rr, double *e);
double satazel(const double *pos, const double *e, double *azel);

void dops(int ns, const double *azel, double elmin, double *dop);
double ionmodel(gtime_t t, const double *ion, const double *pos,
    const double *azel);
double ionmapf(const double *pos, const double *azel);
double ionppp(const double *pos, const double *azel, double re,
    double hion, double *posp);
double tropmodel(gtime_t time, const double *pos, const double *azel,
    double humi);
double interpc(const double coef[], double lat);
double mapf(double el, double a, double b, double c);
double nmf(gtime_t time, const double pos[], const double azel[],
    double *mapfw);
double tropmapf(gtime_t time, const double pos[], const double azel[],
    double *mapfw);
double interpvar(double ang, const double *var);

void antmodel(const pcv_t *pcv, const double *del, const double *azel,
    int opt, double *dant);

void antmodel_s(const pcv_t *pcv, double nadir, double *dant);
void sunmoonpos_eci(gtime_t tut, double *rsun, double *rmoon);
void sunmoonpos(gtime_t tutc, const double *erpv, double *rsun,
    double *rmoon, double *gmst);
void csmooth(obs_t *obs, int ns);
int rtk_uncompress(const char *file, char *uncfile);
int expath(const char *path, char *paths[], int nmax);
void windupcorr(gtime_t time, const double *rs, const double *rr, double *phw);

#endif  // GNSS_SDR_RTKLIB_RTKCMN_H

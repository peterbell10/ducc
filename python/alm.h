/*
 *  This code is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This code is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this code; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*! \file alm.h
 *  Class for storing spherical harmonic coefficients.
 *
 *  Copyright (C) 2003-2020 Max-Planck-Society
 *  \author Martin Reinecke
 */

#ifndef DUCC0_ALM_H
#define DUCC0_ALM_H

#if 1
#include <complex>
#include <cmath>
#include "ducc0/infra/threading.h"
#endif

#include "ducc0/infra/mav.h"
#include "ducc0/infra/error_handling.h"

namespace ducc0 {

namespace detail_alm {

using namespace std;

/*! Base class for calculating the storage layout of spherical harmonic
    coefficients. */
class Alm_Base
  {
  protected:
    size_t lmax, arrsize;
    vector<size_t> mval;
    vector<ptrdiff_t> mstart;

  public:
    /*! Returns the total number of coefficients for maximum quantum numbers
        \a l and \a m. */
    static size_t Num_Alms (size_t l, size_t m)
      {
      MR_assert(m<=l,"mmax must not be larger than lmax");
      return ((m+1)*(m+2))/2 + (m+1)*(l-m);
      }

    Alm_Base (size_t lmax_, const vector<size_t> &mval_,
              const vector<ptrdiff_t> &mstart_)
      : lmax(lmax_), mval(mval_)
      {
      MR_assert(mval.size()>0, "no m indices supplied");
      MR_assert(mstart_.size()==mval.size(), "mval and mstart have different sizes");
      for (size_t i=0; i<mval.size(); ++i)
        {
        MR_assert(mval[i]<=lmax, "m >= lmax");
        if (i>0)
          MR_assert(mval[i]>mval[i-1], "m not strictly ascending");
        }
      mstart.resize(mval.back()+1, -2*lmax);
      arrsize=0;
      for (size_t i=0; i<mval.size(); ++i)
        {
        mstart[mval[i]] = mstart_[i];
        arrsize = size_t(max(ptrdiff_t(arrsize), mstart_[i]+ptrdiff_t(lmax+1)));
        }
      }

    Alm_Base (size_t lmax_, const vector<size_t> &mval_)
      : lmax(lmax_), mval(mval_)
      {
      MR_assert(mval.size()>0, "no m indices supplied");
      for (size_t i=0; i<mval.size(); ++i)
        {
        MR_assert(mval[i]<=lmax, "m >= lmax");
        if (i>0)
          MR_assert(mval[i]>mval[i-1], "m not strictly ascending");
        }
      mstart.resize(mval.back()+1, -2*lmax);
      for (size_t i=0, cnt=0; i<mval.size(); ++i, cnt+=lmax-mval[i]+1)
        mstart[mval[i]] = ptrdiff_t(cnt)-ptrdiff_t(mval[i]);
      arrsize = size_t(mstart.back()+ptrdiff_t(lmax+1));
      }

    /*! Constructs an Alm_Base object with given \a lmax and \a mmax. */
    Alm_Base (size_t lmax_, size_t mmax_)
      : lmax(lmax_), mval(mmax_+1), mstart(mmax_+1)
      {
      ptrdiff_t idx = 0;
      for (size_t m=0; m<=mmax_; ++m)
        {
        mval[m] = m;
        mstart[m] = idx-m;
        idx += lmax-m+1;
        }
      arrsize = Num_Alms(lmax_, mmax_);
      }

    /*! Returns the maximum \a l */
    size_t Lmax() const { return lmax; }
    /*! Returns the maximum \a m */
    size_t Mmax() const { return mval.back(); }

    size_t n_entries() const { return arrsize; }

    /*! Returns an array index for a given m, from which the index of a_lm
        can be obtained by adding l. */
    size_t index_l0 (size_t m) const
      { return mstart[m]; }

    /*! Returns the array index of the specified coefficient. */
    size_t index (size_t l, size_t m) const
      { return index_l0(m) + l; }

    bool conformable(const Alm_Base &other) const
      {
      return (lmax==other.lmax) && (mval==other.mval) && (mstart==other.mstart);
      }
    bool complete() const
      { return mval.size() == lmax+1; }
  };

/*! Class for storing spherical harmonic coefficients. */
template<typename T> class Alm: public Alm_Base
  {
  private:
    mav<T,1> alm;

    template<typename Func> void applyLM(Func func)
      {
      for (auto m: mval)
        for (size_t l=m; l<=lmax; ++l)
          func(l,m,alm.v(index(l,m)));
      }

  public:
    /*! Constructs an Alm object with given \a lmax and \a mmax. */
    Alm (mav<T,1> &data, size_t lmax_, size_t mmax_)
      : Alm_Base(lmax_, mmax_), alm(data)
      { MR_assert(alm.size()==Num_Alms(lmax, mmax_), "bad array size"); }
    Alm (const mav<T,1> &data, size_t lmax_, size_t mmax_)
      : Alm_Base(lmax_, mmax_), alm(data)
      { MR_assert(alm.size()==Num_Alms(lmax, mmax_), "bad array size"); }
    Alm (size_t lmax_=0, size_t mmax_=0)
      : Alm_Base(lmax_,mmax_), alm ({Num_Alms(lmax,mmax_)}) {}

    /*! Sets all coefficients to zero. */
    void SetToZero ()
      { alm.fill(0); }

    /*! Multiplies all coefficients by \a factor. */
    template<typename T2> void Scale (const T2 &factor)
      { for (size_t m=0; m<alm.size(); ++m) alm.v(m)*=factor; }
    /*! \a a(l,m) *= \a factor[l] for all \a l,m. */
    template<typename T2> void ScaleL (const mav<T2,1> &factor)
      {
      MR_assert(factor.size()>size_t(lmax),
        "alm.ScaleL: factor array too short");
      applyLM([&factor](size_t l, size_t /*m*/, T &v){v*=factor(l);}); 
      }
    /*! \a a(l,m) *= \a factor[m] for all \a l,m. */
    template<typename T2> void ScaleM (const mav<T2,1> &factor)
      {
      MR_assert(factor.size()>size_t(Mmax()),
        "alm.ScaleM: factor array too short");
      applyLM([&factor](size_t /*l*/, size_t m, T &v){v*=factor(m);}); 
      }
    /*! Adds \a num to a_00. */
    template<typename T2> void Add (const T2 &num)
      {
      MR_assert(mval[0]==0, "cannot add a constant: no m=0 mode present");
      alm.v(index_l0(0))+=num;
      }

    /*! Returns a reference to the specified coefficient. */
    T &operator() (size_t l, size_t m)
      { return alm.v(index(l,m)); }
    /*! Returns a constant reference to the specified coefficient. */
    const T &operator() (size_t l, size_t m) const
      { return alm(index(l,m)); }

    /*! Returns a pointer for a given m, from which the address of a_lm
        can be obtained by adding l. */
    T *mstart (size_t m)
      { return &alm.v(index_l0(m)); }
    /*! Returns a pointer for a given m, from which the address of a_lm
        can be obtained by adding l. */
    const T *mstart (size_t m) const
      { return &alm(index_l0(m)); }

    /*! Returns a constant reference to the a_lm data. */
    const mav<T,1> &Alms() const { return alm; }

    /*! Returns a reference to the a_lm data. */
    mav<T,1> &Alms() { return alm; }

    ptrdiff_t stride() const
      { return alm.stride(0); }

    /*! Adds all coefficients from \a other to the own coefficients. */
    void Add (const Alm &other)
      {
      MR_assert (conformable(other), "A_lm are not conformable");
      for (size_t m=0; m<alm.size(); ++m)
        alm.v(m) += other.alm(m);
      }
  };

#if 1
/*! Class for calculation of the Wigner matrix at arbitrary angles, using Risbo
    recursion in a way that can be OpenMP-parallelised. This approach uses more
    memory and is slightly slower than wigner_d_risbo_scalar. */
class wigner_d_risbo_openmp
  {
  private:
    double p,q;
    vector<double> sqt;
    mav<double,2> d, dd;
    ptrdiff_t n;

  public:
    wigner_d_risbo_openmp(size_t lmax, double ang)
      : p(sin(ang/2)), q(cos(ang/2)), sqt(2*lmax+1),
        d({lmax+1,2*lmax+1}), dd({lmax+1,2*lmax+1}), n(-1)
      { for (size_t m=0; m<sqt.size(); ++m) sqt[m] = std::sqrt(double(m)); }

    const mav<double,2> &recurse()
      {
      ++n;
      if (n==0)
        d.v(0,0) = 1;
      else if (n==1)
        {
        d.v(0,0) = q*q; d.v(0,1) = -p*q*sqt[2]; d.v(0,2) = p*p;
        d.v(1,0) = -d(0,1); d.v(1,1) = q*q-p*p; d.v(1,2) = d(0,1);
        }
      else
        {
        // padding
        int sign = (n&1)? -1 : 1;
        for (int i=0; i<=2*n-2; ++i)
          {
          d.v(n,i) = sign*d(n-2,2*n-2-i);
          sign=-sign;
          }
        for (int j=2*n-1; j<=2*n; ++j)
          {
          auto &xd((j&1) ? d : dd);
          auto &xdd((j&1) ? dd: d);
          double xj = 1./j;
          xdd.v(0,0) = q*xd(0,0);
          for (int i=1;i<j; ++i)
            xdd.v(0,i) = xj*sqt[j]*(q*sqt[j-i]*xd(0,i) - p*sqt[i]*xd(0,i-1));
          xdd.v(0,j) = -p*xd(0,j-1);
// parallelize
          for (int k=1; k<=n; ++k)
            {
            double t1 = xj*sqt[j-k]*q, t2 = xj*sqt[j-k]*p;
            double t3 = xj*sqt[k  ]*p, t4 = xj*sqt[k  ]*q;
            xdd.v(k,0) = xj*sqt[j]*(q*sqt[j-k]*xd(k,0) + p*sqt[k]*xd(k-1,0));
            for (int i=1; i<j; ++i)
              xdd.v(k,i) = t1*sqt[j-i]*xd(k,i) - t2*sqt[i]*xd(k,i-1)
                        + t3*sqt[j-i]*xd(k-1,i) + t4*sqt[i]*xd(k-1,i-1);
            xdd.v(k,j) = -t2*sqt[j]*xd(k,j-1) + t4*sqt[j]*xd(k-1,j-1);
            }
          }
        }
      return d;
      }
  };

template<typename T> void rotate_alm (Alm<complex<T>> &alm,
  double psi, double theta, double phi)
  {
  auto lmax=alm.Lmax();
  MR_assert (alm.complete(), "rotate_alm: need complete A_lm set");

  if (theta!=0)
    {
    vector<complex<double> > exppsi(lmax+1), expphi(lmax+1);
    for (size_t m=0; m<=lmax; ++m)
      {
      exppsi[m] = polar(1.,-psi*m);
      expphi[m] = polar(1.,-phi*m);
      }
    vector<complex<double> > almtmp(lmax+1);
size_t nthreads=1;
    wigner_d_risbo_openmp rec(lmax,theta);
    for (size_t l=0; l<=lmax; ++l)
      {
      const auto &d(rec.recurse());

      for (size_t m=0; m<=l; ++m)
        almtmp[m] = complex<double>(alm(l,0))*d(l,l+m);

      execStatic(l+1, nthreads, 0, [&](Scheduler &sched)
        {
        auto rng=sched.getNext();
        auto lo=rng.lo;
        auto hi=rng.hi;

        bool flip = true;
        for (size_t mm=1; mm<=l; ++mm)
          {
          auto t1 = complex<double>(alm(l,mm))*exppsi[mm];
          bool flip2 = ((mm+lo)&1);
          for (auto m=lo; m<hi; ++m)
            {
            double d1 = flip2 ? -d(l-mm,l-m) : d(l-mm,l-m);
            double d2 = flip  ? -d(l-mm,l+m) : d(l-mm,l+m);
            double f1 = d1+d2, f2 = d1-d2;
            almtmp[m]+=complex<double>(t1.real()*f1,t1.imag()*f2);
            flip2 = !flip2;
            }
          flip = !flip;
          }
        });

      for (size_t m=0; m<=l; ++m)
        alm(l,m) = complex<T>(almtmp[m]*expphi[m]);
      }
    }
  else
    {
    for (size_t m=0; m<=lmax; ++m)
      {
      auto ang = polar(1.,-(psi+phi)*m);
      for (size_t l=m; l<=lmax; ++l)
        alm(l,m) *= ang;
      }
    }
  }
#endif
}

using detail_alm::Alm_Base;
using detail_alm::Alm;
#if 1
using detail_alm::rotate_alm;
#endif
}

#endif

/* Copyright (c) 2008-2017 the MRtrix3 contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * MRtrix is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * For more details, see http://www.mrtrix.org/.
 */


#ifndef __dwi_svr_recon_h__
#define __dwi_svr_recon_h__


#include <vector>
#include <Eigen/Dense>
#include <Eigen/Sparse>

#include "types.h"
#include "header.h"
#include "transform.h"
#include "math/SH.h"
#include "dwi/svr/psf.h"


namespace MR {
  namespace DWI {
    class ReconMatrix;
    class ReconMatrixAdjoint;
  }
}


namespace Eigen {
  namespace internal {
    // ReconMatrix inherits its traits from SparseMatrix
    template<>
    struct traits<MR::DWI::ReconMatrix> : public Eigen::internal::traits<Eigen::SparseMatrix<float,Eigen::RowMajor> >
    {};

    template<>
    struct traits<MR::DWI::ReconMatrixAdjoint> : public Eigen::internal::traits<Eigen::SparseMatrix<float,Eigen::RowMajor> >
    {};
  }
}


namespace MR
{
  namespace DWI
  {

    class ReconMatrix : public Eigen::EigenBase<ReconMatrix>
    {  MEMALIGN(ReconMatrix);
    public:
      // Required typedefs, constants, and method:
      typedef float Scalar;
      typedef float RealScalar;
      typedef int StorageIndex;
      enum {
        ColsAtCompileTime = Eigen::Dynamic,
        MaxColsAtCompileTime = Eigen::Dynamic,
        IsRowMajor = true
      };

      Eigen::Index rows() const { return M.rows(); }
      Eigen::Index cols() const { return M.cols()*Y.cols(); }


      template<typename Rhs>
      Eigen::Product<ReconMatrix,Rhs,Eigen::AliasFreeProduct> operator*(const Eigen::MatrixBase<Rhs>& x) const {
        return Eigen::Product<ReconMatrix,Rhs,Eigen::AliasFreeProduct>(*this, x.derived());
      }


      typedef Eigen::SparseMatrix<float, Eigen::RowMajor> SparseMat;


      // Custom API:
      ReconMatrix(const Header& in, const Eigen::MatrixXf& rigid, const Eigen::MatrixXf& grad, const int lmax)
        : lmax(lmax),
          nxy (in.size(0)*in.size(1)), nz (in.size(2)), nv (in.size(3)),
          M(nxy*nz*nv, nxy*nz),
          Y(nz*nv, Math::SH::NforL(lmax))
      {
        init_M(in, rigid);
        init_Y(in, rigid, grad);
      }

      const SparseMat& getM() const { return M; }
      const Eigen::MatrixXf& getY() const { return Y; }

      inline const size_t get_grad_idx(const size_t idx) const { return idx / nxy; }

      const ReconMatrixAdjoint adjoint() const;


    private:
      const int lmax;
      const size_t nxy, nz, nv;
      SparseMat M;
      Eigen::MatrixXf Y;


      inline void init_M(const Header& in, const Eigen::MatrixXf& rigid)
      {
        DEBUG("initialise M");
        // Note that this step is highly time and memory critical!
        // Special care must be taken when inserting elements and it is advised to reserve appropriate memory in advance.

        int n = 2;
        PSF<float> psf (n);

        // reserve memory for 8 elements along each row (outer strides with row-major order).
        M.reserve(Eigen::VectorXi::Constant(nv*nz*nxy, 8*n*n*n));

        // set up transform
        Transform T0 (in);          // assume output transform = input transform; needs extending for superresolution

        transform_type Ts2r, Tr2s;

        Eigen::Vector3f ps, pr, p;
        Eigen::Vector3i p0;

        // fill weights
        size_t i = 0;
        for (size_t v = 0; v < nv; v++) {
          if (rigid.rows() == nv) {
            Ts2r = T0.scanner2voxel * get_transform(rigid.row(v)) * T0.voxel2scanner;
            Tr2s = Ts2r.inverse();
          }

          for (size_t z = 0; z < nz; z++) {
            if (rigid.rows() == nv*nz) {
              Ts2r = T0.scanner2voxel * get_transform(rigid.row(v*nz+z)) * T0.voxel2scanner;
              Tr2s = Ts2r.inverse();
            }

            for (size_t y = 0; y < in.size(1); y++) {
              for (size_t x = 0; x < in.size(0); x++, i++) {
                ps = Eigen::Vector3f(x, y, z);
                pr = (Ts2r.cast<float>() * ps);

                for (int rx = -n; rx < n; rx++) {
                  for (int ry = -n; ry < n; ry++) {
                    for (int rz = -n; rz < n; rz++) {
                      p0 = Eigen::Vector3i(std::ceil(pr[0]), std::ceil(pr[1]), std::ceil(pr[2])) + Eigen::Vector3i(rx, ry, rz);
                      if (inbounds(in, p0[0], p0[1], p0[2])) {
                        p = Tr2s.cast<float>() * p0.cast<float>();
                        M.insert(i, get_idx(in, p0[0], p0[1], p0[2])) = psf(ps - p);
                      }
                    }
                  }
                }

              }
            }

          }
        }

        M.makeCompressed();
      }


      inline void init_Y(const Header& in, const Eigen::MatrixXf& rigid, const Eigen::MatrixXf& grad)
      {
        DEBUG("initialise Y");
        assert (grad.rows() == nv);     // one gradient per volume

        Eigen::Vector3f vec;
        Eigen::Matrix3f rot;
        rot.setIdentity();
        Eigen::VectorXf delta;

        for (size_t i = 0; i < nv; i++) {
          vec = {grad(i, 0), grad(i, 1), grad(i, 2)};
          if (rigid.rows() == nv)
            rot = get_rotation(rigid(i,3), rigid(i,4), rigid(i,5));

          for (size_t j = 0; j < nz; j++) {
            // rotate vector with motion parameters
            if (rigid.rows() == nv*nz)
              rot = get_rotation(rigid(i*nz+j,3), rigid(i*nz+j,4), rigid(i*nz+j,5));

            // evaluate basis functions
            Y.row(i*nz+j) = Math::SH::delta(delta, rot*vec, lmax);
          }

        }

      }


      inline Eigen::Matrix3f get_rotation(const float a1, const float a2, const float a3) const
      {
        Eigen::Matrix3f m;
        m = Eigen::AngleAxisf(a1, Eigen::Vector3f::UnitX())
          * Eigen::AngleAxisf(a2, Eigen::Vector3f::UnitY())
          * Eigen::AngleAxisf(a3, Eigen::Vector3f::UnitZ());
        return m;
      }


      inline transform_type get_transform(const Eigen::VectorXf& p) const
      {
        transform_type T;
        T.translation() = Eigen::Vector3d( double(p[0]), double(p[1]), double(p[2]) );
        T.linear() = get_rotation(p[3], p[4], p[5]).template cast<double>();
        return T;
      }


      inline size_t get_idx(const Header& h, const int x, const int y, const int z) const
      {
        return size_t(z*h.size(1)*h.size(0) + y*h.size(0) + x);
      }


      inline bool inbounds(const Header& h, const int x, const int y, const int z) const
      {
        return (x >= 0) && (x < h.size(0))
            && (y >= 0) && (y < h.size(1))
            && (z >= 0) && (z < h.size(2));
      }


    };


    class ReconMatrixAdjoint : public Eigen::EigenBase<ReconMatrixAdjoint>
    {  MEMALIGN(ReconMatrixAdjoint);
    public:
      // Required typedefs, constants, and method:
      typedef float Scalar;
      typedef float RealScalar;
      typedef int StorageIndex;
      enum {
        ColsAtCompileTime = Eigen::Dynamic,
        MaxColsAtCompileTime = Eigen::Dynamic,
        IsRowMajor = true
      };
      Eigen::Index rows() const { return R.cols(); }
      Eigen::Index cols() const { return R.rows(); }

      template<typename Rhs>
      Eigen::Product<ReconMatrixAdjoint,Rhs,Eigen::AliasFreeProduct> operator*(const Eigen::MatrixBase<Rhs>& x) const {
        return Eigen::Product<ReconMatrixAdjoint,Rhs,Eigen::AliasFreeProduct>(*this, x.derived());
      }

      ReconMatrixAdjoint(const ReconMatrix& R)
        : R(R)
      {  }

      const ReconMatrix& R;

    };



  }
}


// Implementation of ReconMatrix * Eigen::DenseVector though a specialization of internal::generic_product_impl:
namespace Eigen {
  namespace internal {

    template<typename Rhs>
    struct generic_product_impl<MR::DWI::ReconMatrix, Rhs, SparseShape, DenseShape, GemvProduct>
      : generic_product_impl_base<MR::DWI::ReconMatrix,Rhs,generic_product_impl<MR::DWI::ReconMatrix,Rhs> >
    {
      typedef typename Product<MR::DWI::ReconMatrix,Rhs>::Scalar Scalar;

      template<typename Dest>
      static void scaleAndAddTo(Dest& dst, const MR::DWI::ReconMatrix& lhs, const Rhs& rhs, const Scalar& alpha)
      {
        // This method should implement "dst += alpha * lhs * rhs" inplace
        assert(alpha==Scalar(1) && "scaling is not implemented");

        //TRACE;
        auto Y = lhs.getY();
        size_t nc = Y.cols();
        size_t nxyz = lhs.getM().cols();
        VectorXf r (lhs.rows());

        for (size_t j = 0; j < nc; j++) {
          r = lhs.getM() * rhs.segment(j*nxyz, nxyz);
          for (size_t i = 0; i < lhs.rows(); i++)
            dst[i] += r[i] * Y(lhs.get_grad_idx(i), j);
        }

      }

    };


    template<typename Rhs>
    struct generic_product_impl<MR::DWI::ReconMatrixAdjoint, Rhs, SparseShape, DenseShape, GemvProduct>
      : generic_product_impl_base<MR::DWI::ReconMatrixAdjoint,Rhs,generic_product_impl<MR::DWI::ReconMatrixAdjoint,Rhs> >
    {
      typedef typename Product<MR::DWI::ReconMatrixAdjoint,Rhs>::Scalar Scalar;

      template<typename Dest>
      static void scaleAndAddTo(Dest& dst, const MR::DWI::ReconMatrixAdjoint& lhs, const Rhs& rhs, const Scalar& alpha)
      {
        // This method should implement "dst += alpha * lhs * rhs" inplace
        assert(alpha==Scalar(1) && "scaling is not implemented");

        //TRACE;
        auto Y = lhs.R.getY();
        size_t nc = Y.cols();
        size_t nxyz = lhs.R.getM().cols();
        VectorXf r (lhs.cols());

        for (size_t j = 0; j < nc; j++) {
          r = rhs;
          for (size_t i = 0; i < lhs.cols(); i++)
            r[i] *= Y(lhs.R.get_grad_idx(i), j);
          dst.segment(j*nxyz, nxyz) += lhs.R.getM().adjoint() * r;
        }

      }

    };


  }
}


#endif


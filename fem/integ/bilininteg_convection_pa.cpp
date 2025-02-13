// Copyright (c) 2010-2025, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "../../general/forall.hpp"
#include "../bilininteg.hpp"
#include "../gridfunc.hpp"
#include "../qfunction.hpp"
#include "../ceed/integrators/convection/convection.hpp"

namespace mfem
{

// PA Convection Assemble 2D kernel
static void PAConvectionSetup2D(const int NQ,
                                const int NE,
                                const Array<real_t> &w,
                                const Vector &j,
                                const Vector &vel,
                                const real_t alpha,
                                Vector &op)
{
   constexpr int DIM = 2;

   const bool const_v = vel.Size() == DIM;

   const auto W = w.Read();
   const auto J = Reshape(j.Read(), NQ,DIM,DIM,NE);
   const auto V = const_v ?
                  Reshape(vel.Read(), DIM,1,1) :
                  Reshape(vel.Read(), DIM,NQ,NE);
   auto y = Reshape(op.Write(), NQ,DIM,NE);

   mfem::forall(NE*NQ, [=] MFEM_HOST_DEVICE (int q_global)
   {
      const int e = q_global / NQ;
      const int q = q_global % NQ;
      const real_t J11 = J(q,0,0,e);
      const real_t J21 = J(q,1,0,e);
      const real_t J12 = J(q,0,1,e);
      const real_t J22 = J(q,1,1,e);
      const real_t w = alpha * W[q];
      const real_t v0 = const_v ? V(0,0,0) : V(0,q,e);
      const real_t v1 = const_v ? V(1,0,0) : V(1,q,e);
      const real_t wx = w * v0;
      const real_t wy = w * v1;
      // y = alpha * W * det(J) * J^{-1} . v = adj(J) . { wx, wy }
      y(q,0,e) =  wx * J22 - wy * J12; // 1
      y(q,1,e) = -wx * J21 + wy * J11; // 2
   });
}

// PA Convection Assemble 3D kernel
static void PAConvectionSetup3D(const int NQ,
                                const int NE,
                                const Array<real_t> &w,
                                const Vector &j,
                                const Vector &vel,
                                const real_t alpha,
                                Vector &op)
{
   constexpr int DIM = 3;
   constexpr int SDIM = DIM;
   const auto W = Reshape(w.Read(), NQ);
   const auto J = Reshape(j.Read(), NQ,SDIM,DIM,NE);
   const bool const_v = vel.Size() == DIM;
   const auto V = const_v ?
                  Reshape(vel.Read(), 3,1,1) :
                  Reshape(vel.Read(), 3,NQ,NE);
   auto y = Reshape(op.Write(), NQ,3,NE);
   mfem::forall(NE*NQ, [=] MFEM_HOST_DEVICE (int q_global)
   {
      const int e = q_global / NQ;
      const int q = q_global % NQ;
      const real_t J11 = J(q,0,0,e);
      const real_t J12 = J(q,0,1,e);
      const real_t J13 = J(q,0,2,e);
      const real_t J21 = J(q,1,0,e);
      const real_t J22 = J(q,1,1,e);
      const real_t J23 = J(q,1,2,e);
      const real_t J31 = J(q,2,0,e);
      const real_t J32 = J(q,2,1,e);
      const real_t J33 = J(q,2,2,e);
      const real_t w = alpha * W(q);
      const real_t v0 = const_v ? V(0,0,0) : V(0,q,e);
      const real_t v1 = const_v ? V(1,0,0) : V(1,q,e);
      const real_t v2 = const_v ? V(2,0,0) : V(2,q,e);
      const real_t wx = w * v0;
      const real_t wy = w * v1;
      const real_t wz = w * v2;
      // A = adj(J)
      const real_t A11 = (J22 * J33) - (J23 * J32);
      const real_t A12 = (J32 * J13) - (J12 * J33);
      const real_t A13 = (J12 * J23) - (J22 * J13);
      const real_t A21 = (J31 * J23) - (J21 * J33);
      const real_t A22 = (J11 * J33) - (J13 * J31);
      const real_t A23 = (J21 * J13) - (J11 * J23);
      const real_t A31 = (J21 * J32) - (J31 * J22);
      const real_t A32 = (J31 * J12) - (J11 * J32);
      const real_t A33 = (J11 * J22) - (J12 * J21);
      // y = alpha * W * det(J) * J^{-1} . v = adj(J) . { wx, wy, wz }
      y(q,0,e) = wx * A11 + wy * A12 + wz * A13;
      y(q,1,e) = wx * A21 + wy * A22 + wz * A23;
      y(q,2,e) = wx * A31 + wy * A32 + wz * A33;
   });
}

static void PAConvectionSetup(const int dim,
                              const int NQ,
                              const int NE,
                              const Array<real_t> &W,
                              const Vector &J,
                              const Vector &coeff,
                              const real_t alpha,
                              Vector &op)
{
   if (dim == 1) { MFEM_ABORT("dim==1 not supported in PAConvectionSetup"); }
   if (dim == 2)
   {
      PAConvectionSetup2D(NQ, NE, W, J, coeff, alpha, op);
   }
   if (dim == 3)
   {
      PAConvectionSetup3D(NQ, NE, W, J, coeff, alpha, op);
   }
}

void ConvectionIntegrator::AssemblePA(const FiniteElementSpace &fes)
{
   const MemoryType mt = (pa_mt == MemoryType::DEFAULT) ?
                         Device::GetDeviceMemoryType() : pa_mt;
   // Assumes tensor-product elements
   Mesh *mesh = fes.GetMesh();
   const FiniteElement &el = *fes.GetTypicalFE();
   ElementTransformation &Trans = *mesh->GetTypicalElementTransformation();
   const IntegrationRule *ir = IntRule ? IntRule : &GetRule(el, Trans);
   if (DeviceCanUseCeed())
   {
      delete ceedOp;
      const bool mixed = mesh->GetNumGeometries(mesh->Dimension()) > 1 ||
                         fes.IsVariableOrder();
      if (mixed)
      {
         ceedOp = new ceed::MixedPAConvectionIntegrator(*this, fes, Q, alpha);
      }
      else
      {
         ceedOp = new ceed::PAConvectionIntegrator(fes, *ir, Q, alpha);
      }
      return;
   }
   const int dims = el.GetDim();
   const int symmDims = dims;
   nq = ir->GetNPoints();
   dim = mesh->Dimension();
   ne = fes.GetNE();
   geom = mesh->GetGeometricFactors(*ir, GeometricFactors::JACOBIANS, mt);
   maps = &el.GetDofToQuad(*ir, DofToQuad::TENSOR);
   dofs1D = maps->ndof;
   quad1D = maps->nqpt;
   pa_data.SetSize(symmDims * nq * ne, mt);

   QuadratureSpace qs(*mesh, *ir);
   CoefficientVector vel(*Q, qs, CoefficientStorage::COMPRESSED);

   PAConvectionSetup(dim, nq, ne, ir->GetWeights(), geom->J,
                     vel, alpha, pa_data);
}

void ConvectionIntegrator::AssembleDiagonalPA(Vector &diag)
{
   if (DeviceCanUseCeed())
   {
      ceedOp->GetDiagonal(diag);
   }
   else
   {
      MFEM_ABORT("AssembleDiagonalPA not yet implemented for"
                 " ConvectionIntegrator.");
   }
}

// PA Convection Apply 2D kernel
template<int T_D1D = 0, int T_Q1D = 0> static
void PAConvectionApply2D(const int ne,
                         const Array<real_t> &b,
                         const Array<real_t> &g,
                         const Array<real_t> &bt,
                         const Array<real_t> &gt,
                         const Vector &op_,
                         const Vector &x_,
                         Vector &y_,
                         const int d1d = 0,
                         const int q1d = 0)
{
   const int NE = ne;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= DeviceDofQuadLimits::Get().MAX_D1D, "");
   MFEM_VERIFY(Q1D <= DeviceDofQuadLimits::Get().MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto G = Reshape(g.Read(), Q1D, D1D);
   auto Bt = Reshape(bt.Read(), D1D, Q1D);
   auto op = Reshape(op_.Read(), Q1D, Q1D, 2, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, NE);
   auto y = Reshape(y_.ReadWrite(), D1D, D1D, NE);
   mfem::forall(NE, [=] MFEM_HOST_DEVICE (int e)
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      // the following variables are evaluated at compile time
      constexpr int max_D1D = T_D1D ? T_D1D : DofQuadLimits::MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : DofQuadLimits::MAX_Q1D;

      real_t u[max_D1D][max_D1D];
      for (int dy = 0; dy < D1D; ++dy)
      {
         for (int dx = 0; dx < D1D; ++dx)
         {
            u[dy][dx] = x(dx,dy,e);
         }
      }
      real_t Bu[max_D1D][max_Q1D];
      real_t Gu[max_D1D][max_Q1D];
      for (int dy = 0; dy < D1D; ++dy)
      {
         for (int qx = 0; qx < Q1D; ++qx)
         {
            Bu[dy][qx] = 0.0;
            Gu[dy][qx] = 0.0;
            for (int dx = 0; dx < D1D; ++dx)
            {
               const real_t bx = B(qx,dx);
               const real_t gx = G(qx,dx);
               const real_t x = u[dy][dx];
               Bu[dy][qx] += bx * x;
               Gu[dy][qx] += gx * x;
            }
         }
      }
      real_t GBu[max_Q1D][max_Q1D];
      real_t BGu[max_Q1D][max_Q1D];
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            GBu[qy][qx] = 0.0;
            BGu[qy][qx] = 0.0;
            for (int dy = 0; dy < D1D; ++dy)
            {
               const real_t bx = B(qy,dy);
               const real_t gx = G(qy,dy);
               GBu[qy][qx] += gx * Bu[dy][qx];
               BGu[qy][qx] += bx * Gu[dy][qx];
            }
         }
      }
      // Calculate Dxy, xDy in plane
      real_t DGu[max_Q1D][max_Q1D];
      for (int qy = 0; qy < Q1D; ++qy)
      {
         for (int qx = 0; qx < Q1D; ++qx)
         {
            const real_t O1 = op(qx,qy,0,e);
            const real_t O2 = op(qx,qy,1,e);

            const real_t gradX = BGu[qy][qx];
            const real_t gradY = GBu[qy][qx];

            DGu[qy][qx] = (O1 * gradX) + (O2 * gradY);
         }
      }
      real_t BDGu[max_D1D][max_Q1D];
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            BDGu[dy][qx] = 0.0;
            for (int qy = 0; qy < Q1D; ++qy)
            {
               const real_t w = Bt(dy,qy);
               BDGu[dy][qx] += w * DGu[qy][qx];
            }
         }
      }
      for (int dx = 0; dx < D1D; ++dx)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            real_t BBDGu = 0.0;
            for (int qx = 0; qx < Q1D; ++qx)
            {
               const real_t w = Bt(dx,qx);
               BBDGu += w * BDGu[dy][qx];
            }
            y(dx,dy,e) += BBDGu;
         }
      }
   });
}

// Optimized PA Convection Apply 2D kernel
template<int T_D1D = 0, int T_Q1D = 0, int T_NBZ = 0> static
void SmemPAConvectionApply2D(const int ne,
                             const Array<real_t> &b,
                             const Array<real_t> &g,
                             const Array<real_t> &bt,
                             const Array<real_t> &gt,
                             const Vector &op_,
                             const Vector &x_,
                             Vector &y_,
                             const int d1d = 0,
                             const int q1d = 0)
{
   const int NE = ne;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   constexpr int NBZ = T_NBZ ? T_NBZ : 1;
   MFEM_VERIFY(D1D <= DeviceDofQuadLimits::Get().MAX_D1D, "");
   MFEM_VERIFY(Q1D <= DeviceDofQuadLimits::Get().MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto G = Reshape(g.Read(), Q1D, D1D);
   auto Bt = Reshape(bt.Read(), D1D, Q1D);
   auto op = Reshape(op_.Read(), Q1D, Q1D, 2, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, NE);
   auto y = Reshape(y_.ReadWrite(), D1D, D1D, NE);
   mfem::forall_2D_batch(NE, Q1D, Q1D, NBZ, [=] MFEM_HOST_DEVICE (int e)
   {
      const int tidz = MFEM_THREAD_ID(z);
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      // the following variables are evaluated at compile time
      constexpr int NBZ = T_NBZ ? T_NBZ : 1;
      constexpr int max_D1D = T_D1D ? T_D1D : DofQuadLimits::MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : DofQuadLimits::MAX_Q1D;
      // constexpr int MDQ = (max_Q1D > max_D1D) ? max_Q1D : max_D1D;
      MFEM_SHARED real_t u[NBZ][max_D1D][max_D1D];
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(dx,x,D1D)
         {
            // e is really equal to e+tidz
            u[tidz][dy][dx] = x(dx,dy,e);
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_SHARED real_t Bu[NBZ][max_D1D][max_Q1D];
      MFEM_SHARED real_t Gu[NBZ][max_D1D][max_Q1D];
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            Bu[tidz][dy][qx] = 0.0;
            Gu[tidz][dy][qx] = 0.0;
            for (int dx = 0; dx < D1D; ++dx)
            {
               const real_t bx = B(qx,dx);
               const real_t gx = G(qx,dx);
               const real_t x = u[tidz][dy][dx];
               Bu[tidz][dy][qx] += bx * x;
               Gu[tidz][dy][qx] += gx * x;
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_SHARED real_t GBu[NBZ][max_Q1D][max_Q1D];
      MFEM_SHARED real_t BGu[NBZ][max_Q1D][max_Q1D];
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            GBu[tidz][qy][qx] = 0.0;
            BGu[tidz][qy][qx] = 0.0;
            for (int dy = 0; dy < D1D; ++dy)
            {
               const real_t bx = B(qy,dy);
               const real_t gx = G(qy,dy);
               GBu[tidz][qy][qx] += gx * Bu[tidz][dy][qx];
               BGu[tidz][qy][qx] += bx * Gu[tidz][dy][qx];
            }
         }
      }
      MFEM_SYNC_THREAD;
      // Calculate Dxy, xDy in plane
      MFEM_SHARED real_t DGu[NBZ][max_Q1D][max_Q1D];
      MFEM_FOREACH_THREAD(qy,y,Q1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            const real_t O1 = op(qx,qy,0,e);
            const real_t O2 = op(qx,qy,1,e);

            const real_t gradX = BGu[tidz][qy][qx];
            const real_t gradY = GBu[tidz][qy][qx];

            DGu[tidz][qy][qx] = (O1 * gradX) + (O2 * gradY);
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_SHARED real_t BDGu[NBZ][max_D1D][max_Q1D];
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            BDGu[tidz][dy][qx] = 0.0;
            for (int qy = 0; qy < Q1D; ++qy)
            {
               const real_t w = Bt(dy,qy);
               BDGu[tidz][dy][qx] += w * DGu[tidz][qy][qx];
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dx,x,D1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            real_t BBDGu = 0.0;
            for (int qx = 0; qx < Q1D; ++qx)
            {
               const real_t w = Bt(dx,qx);
               BBDGu += w * BDGu[tidz][dy][qx];
            }
            y(dx,dy,e) += BBDGu;
         }
      }
   });
}

// PA Convection Apply 3D kernel
template<int T_D1D = 0, int T_Q1D = 0> static
void PAConvectionApply3D(const int ne,
                         const Array<real_t> &b,
                         const Array<real_t> &g,
                         const Array<real_t> &bt,
                         const Array<real_t> &gt,
                         const Vector &op_,
                         const Vector &x_,
                         Vector &y_,
                         const int d1d = 0,
                         const int q1d = 0)
{
   const int NE = ne;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= DeviceDofQuadLimits::Get().MAX_D1D, "");
   MFEM_VERIFY(Q1D <= DeviceDofQuadLimits::Get().MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto G = Reshape(g.Read(), Q1D, D1D);
   auto Bt = Reshape(bt.Read(), D1D, Q1D);
   auto op = Reshape(op_.Read(), Q1D, Q1D, Q1D, 3, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, D1D, NE);
   auto y = Reshape(y_.ReadWrite(), D1D, D1D, D1D, NE);
   mfem::forall(NE, [=] MFEM_HOST_DEVICE (int e)
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      // the following variables are evaluated at compile time
      constexpr int max_D1D = T_D1D ? T_D1D : DofQuadLimits::MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : DofQuadLimits::MAX_Q1D;

      real_t u[max_D1D][max_D1D][max_D1D];
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            for (int dx = 0; dx < D1D; ++dx)
            {
               u[dz][dy][dx] = x(dx,dy,dz,e);
            }
         }
      }
      real_t Bu[max_D1D][max_D1D][max_Q1D];
      real_t Gu[max_D1D][max_D1D][max_Q1D];
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            for (int qx = 0; qx < Q1D; ++qx)
            {
               Bu[dz][dy][qx] = 0.0;
               Gu[dz][dy][qx] = 0.0;
               for (int dx = 0; dx < D1D; ++dx)
               {
                  const real_t bx = B(qx,dx);
                  const real_t gx = G(qx,dx);
                  const real_t x = u[dz][dy][dx];
                  Bu[dz][dy][qx] += bx * x;
                  Gu[dz][dy][qx] += gx * x;
               }
            }
         }
      }
      real_t BBu[max_D1D][max_Q1D][max_Q1D];
      real_t GBu[max_D1D][max_Q1D][max_Q1D];
      real_t BGu[max_D1D][max_Q1D][max_Q1D];
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int qx = 0; qx < Q1D; ++qx)
         {
            for (int qy = 0; qy < Q1D; ++qy)
            {
               BBu[dz][qy][qx] = 0.0;
               GBu[dz][qy][qx] = 0.0;
               BGu[dz][qy][qx] = 0.0;
               for (int dy = 0; dy < D1D; ++dy)
               {
                  const real_t bx = B(qy,dy);
                  const real_t gx = G(qy,dy);
                  BBu[dz][qy][qx] += bx * Bu[dz][dy][qx];
                  GBu[dz][qy][qx] += gx * Bu[dz][dy][qx];
                  BGu[dz][qy][qx] += bx * Gu[dz][dy][qx];
               }
            }
         }
      }
      real_t GBBu[max_Q1D][max_Q1D][max_Q1D];
      real_t BGBu[max_Q1D][max_Q1D][max_Q1D];
      real_t BBGu[max_Q1D][max_Q1D][max_Q1D];
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            for (int qz = 0; qz < Q1D; ++qz)
            {
               GBBu[qz][qy][qx] = 0.0;
               BGBu[qz][qy][qx] = 0.0;
               BBGu[qz][qy][qx] = 0.0;
               for (int dz = 0; dz < D1D; ++dz)
               {
                  const real_t bx = B(qz,dz);
                  const real_t gx = G(qz,dz);
                  GBBu[qz][qy][qx] += gx * BBu[dz][qy][qx];
                  BGBu[qz][qy][qx] += bx * GBu[dz][qy][qx];
                  BBGu[qz][qy][qx] += bx * BGu[dz][qy][qx];
               }
            }
         }
      }
      // Calculate Dxy, xDy in plane
      real_t DGu[max_Q1D][max_Q1D][max_Q1D];
      for (int qz = 0; qz < Q1D; ++qz)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            for (int qx = 0; qx < Q1D; ++qx)
            {
               const real_t O1 = op(qx,qy,qz,0,e);
               const real_t O2 = op(qx,qy,qz,1,e);
               const real_t O3 = op(qx,qy,qz,2,e);

               const real_t gradX = BBGu[qz][qy][qx];
               const real_t gradY = BGBu[qz][qy][qx];
               const real_t gradZ = GBBu[qz][qy][qx];

               DGu[qz][qy][qx] = (O1 * gradX) + (O2 * gradY) + (O3 * gradZ);
            }
         }
      }
      real_t BDGu[max_D1D][max_Q1D][max_Q1D];
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            for (int dz = 0; dz < D1D; ++dz)
            {
               BDGu[dz][qy][qx] = 0.0;
               for (int qz = 0; qz < Q1D; ++qz)
               {
                  const real_t w = Bt(dz,qz);
                  BDGu[dz][qy][qx] += w * DGu[qz][qy][qx];
               }
            }
         }
      }
      real_t BBDGu[max_D1D][max_D1D][max_Q1D];
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int qx = 0; qx < Q1D; ++qx)
         {
            for (int dy = 0; dy < D1D; ++dy)
            {
               BBDGu[dz][dy][qx] = 0.0;
               for (int qy = 0; qy < Q1D; ++qy)
               {
                  const real_t w = Bt(dy,qy);
                  BBDGu[dz][dy][qx] += w * BDGu[dz][qy][qx];
               }
            }
         }
      }
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            for (int dx = 0; dx < D1D; ++dx)
            {
               real_t BBBDGu = 0.0;
               for (int qx = 0; qx < Q1D; ++qx)
               {
                  const real_t w = Bt(dx,qx);
                  BBBDGu += w * BBDGu[dz][dy][qx];
               }
               y(dx,dy,dz,e) += BBBDGu;
            }
         }
      }
   });
}

// Optimized PA Convection Apply 3D kernel
template<int T_D1D = 0, int T_Q1D = 0> static
void SmemPAConvectionApply3D(const int ne,
                             const Array<real_t> &b,
                             const Array<real_t> &g,
                             const Array<real_t> &bt,
                             const Array<real_t> &gt,
                             const Vector &op_,
                             const Vector &x_,
                             Vector &y_,
                             const int d1d = 0,
                             const int q1d = 0)
{
   const int NE = ne;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= DeviceDofQuadLimits::Get().MAX_D1D, "");
   MFEM_VERIFY(Q1D <= DeviceDofQuadLimits::Get().MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto G = Reshape(g.Read(), Q1D, D1D);
   auto Bt = Reshape(bt.Read(), D1D, Q1D);
   auto op = Reshape(op_.Read(), Q1D, Q1D, Q1D, 3, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, D1D, NE);
   auto y = Reshape(y_.ReadWrite(), D1D, D1D, D1D, NE);
   mfem::forall_3D(NE, Q1D, Q1D, Q1D, [=] MFEM_HOST_DEVICE (int e)
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      // the following variables are evaluated at compile time
      constexpr int max_D1D = T_D1D ? T_D1D : DofQuadLimits::MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : DofQuadLimits::MAX_Q1D;
      constexpr int max_DQ = (max_Q1D > max_D1D) ? max_Q1D : max_D1D;
      MFEM_SHARED real_t sm0[max_DQ*max_DQ*max_DQ];
      MFEM_SHARED real_t sm1[max_DQ*max_DQ*max_DQ];
      MFEM_SHARED real_t sm2[max_DQ*max_DQ*max_DQ];
      MFEM_SHARED real_t sm3[max_DQ*max_DQ*max_DQ];
      MFEM_SHARED real_t sm4[max_DQ*max_DQ*max_DQ];
      MFEM_SHARED real_t sm5[max_DQ*max_DQ*max_DQ];

      real_t (*u)[max_D1D][max_D1D] = (real_t (*)[max_D1D][max_D1D]) sm0;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            MFEM_FOREACH_THREAD(dx,x,D1D)
            {
               u[dz][dy][dx] = x(dx,dy,dz,e);
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*Bu)[max_D1D][max_Q1D] = (real_t (*)[max_D1D][max_Q1D])sm1;
      real_t (*Gu)[max_D1D][max_Q1D] = (real_t (*)[max_D1D][max_Q1D])sm2;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            MFEM_FOREACH_THREAD(qx,x,Q1D)
            {
               real_t Bu_ = 0.0;
               real_t Gu_ = 0.0;
               for (int dx = 0; dx < D1D; ++dx)
               {
                  const real_t bx = B(qx,dx);
                  const real_t gx = G(qx,dx);
                  const real_t x = u[dz][dy][dx];
                  Bu_ += bx * x;
                  Gu_ += gx * x;
               }
               Bu[dz][dy][qx] = Bu_;
               Gu[dz][dy][qx] = Gu_;
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*BBu)[max_Q1D][max_Q1D] = (real_t (*)[max_Q1D][max_Q1D])sm3;
      real_t (*GBu)[max_Q1D][max_Q1D] = (real_t (*)[max_Q1D][max_Q1D])sm4;
      real_t (*BGu)[max_Q1D][max_Q1D] = (real_t (*)[max_Q1D][max_Q1D])sm5;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            MFEM_FOREACH_THREAD(qy,y,Q1D)
            {
               real_t BBu_ = 0.0;
               real_t GBu_ = 0.0;
               real_t BGu_ = 0.0;
               for (int dy = 0; dy < D1D; ++dy)
               {
                  const real_t bx = B(qy,dy);
                  const real_t gx = G(qy,dy);
                  BBu_ += bx * Bu[dz][dy][qx];
                  GBu_ += gx * Bu[dz][dy][qx];
                  BGu_ += bx * Gu[dz][dy][qx];
               }
               BBu[dz][qy][qx] = BBu_;
               GBu[dz][qy][qx] = GBu_;
               BGu[dz][qy][qx] = BGu_;
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*GBBu)[max_Q1D][max_Q1D] = (real_t (*)[max_Q1D][max_Q1D])sm0;
      real_t (*BGBu)[max_Q1D][max_Q1D] = (real_t (*)[max_Q1D][max_Q1D])sm1;
      real_t (*BBGu)[max_Q1D][max_Q1D] = (real_t (*)[max_Q1D][max_Q1D])sm2;
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            MFEM_FOREACH_THREAD(qz,z,Q1D)
            {
               real_t GBBu_ = 0.0;
               real_t BGBu_ = 0.0;
               real_t BBGu_ = 0.0;
               for (int dz = 0; dz < D1D; ++dz)
               {
                  const real_t bx = B(qz,dz);
                  const real_t gx = G(qz,dz);
                  GBBu_ += gx * BBu[dz][qy][qx];
                  BGBu_ += bx * GBu[dz][qy][qx];
                  BBGu_ += bx * BGu[dz][qy][qx];
               }
               GBBu[qz][qy][qx] = GBBu_;
               BGBu[qz][qy][qx] = BGBu_;
               BBGu[qz][qy][qx] = BBGu_;
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*DGu)[max_Q1D][max_Q1D] = (real_t (*)[max_Q1D][max_Q1D])sm3;
      MFEM_FOREACH_THREAD(qz,z,Q1D)
      {
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            MFEM_FOREACH_THREAD(qx,x,Q1D)
            {
               const real_t O1 = op(qx,qy,qz,0,e);
               const real_t O2 = op(qx,qy,qz,1,e);
               const real_t O3 = op(qx,qy,qz,2,e);

               const real_t gradX = BBGu[qz][qy][qx];
               const real_t gradY = BGBu[qz][qy][qx];
               const real_t gradZ = GBBu[qz][qy][qx];

               DGu[qz][qy][qx] = (O1 * gradX) + (O2 * gradY) + (O3 * gradZ);
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*BDGu)[max_Q1D][max_Q1D] = (real_t (*)[max_Q1D][max_Q1D])sm4;
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            MFEM_FOREACH_THREAD(dz,z,D1D)
            {
               real_t BDGu_ = 0.0;
               for (int qz = 0; qz < Q1D; ++qz)
               {
                  const real_t w = Bt(dz,qz);
                  BDGu_ += w * DGu[qz][qy][qx];
               }
               BDGu[dz][qy][qx] = BDGu_;
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*BBDGu)[max_D1D][max_Q1D] = (real_t (*)[max_D1D][max_Q1D])sm5;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            MFEM_FOREACH_THREAD(dy,y,D1D)
            {
               real_t BBDGu_ = 0.0;
               for (int qy = 0; qy < Q1D; ++qy)
               {
                  const real_t w = Bt(dy,qy);
                  BBDGu_ += w * BDGu[dz][qy][qx];
               }
               BBDGu[dz][dy][qx] = BBDGu_;
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            MFEM_FOREACH_THREAD(dx,x,D1D)
            {
               real_t BBBDGu = 0.0;
               for (int qx = 0; qx < Q1D; ++qx)
               {
                  const real_t w = Bt(dx,qx);
                  BBBDGu += w * BBDGu[dz][dy][qx];
               }
               y(dx,dy,dz,e) += BBBDGu;
            }
         }
      }
   });
}

// PA Convection Apply 2D kernel
template<int T_D1D = 0, int T_Q1D = 0> static
void PAConvectionApplyT2D(const int ne,
                          const Array<real_t> &b,
                          const Array<real_t> &g,
                          const Array<real_t> &bt,
                          const Array<real_t> &gt,
                          const Vector &op_,
                          const Vector &x_,
                          Vector &y_,
                          const int d1d = 0,
                          const int q1d = 0)
{
   const int NE = ne;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= DeviceDofQuadLimits::Get().MAX_D1D, "");
   MFEM_VERIFY(Q1D <= DeviceDofQuadLimits::Get().MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto Bt = Reshape(bt.Read(), D1D, Q1D);
   auto Gt = Reshape(gt.Read(), D1D, Q1D);
   auto op = Reshape(op_.Read(), Q1D, Q1D, 2, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, NE);
   auto y = Reshape(y_.ReadWrite(), D1D, D1D, NE);
   mfem::forall(NE, [=] MFEM_HOST_DEVICE (int e)
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      // the following variables are evaluated at compile time
      constexpr int max_D1D = T_D1D ? T_D1D : DofQuadLimits::MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : DofQuadLimits::MAX_Q1D;

      real_t u[max_D1D][max_D1D];
      for (int dy = 0; dy < D1D; ++dy)
      {
         for (int dx = 0; dx < D1D; ++dx)
         {
            u[dy][dx] = x(dx,dy,e);
         }
      }
      real_t Bu[max_D1D][max_Q1D];
      for (int dy = 0; dy < D1D; ++dy)
      {
         for (int qx = 0; qx < Q1D; ++qx)
         {
            Bu[dy][qx] = 0.0;
            for (int dx = 0; dx < D1D; ++dx)
            {
               const real_t bx = B(qx,dx);
               const real_t x = u[dy][dx];
               Bu[dy][qx] += bx * x;
            }
         }
      }
      real_t BBu[max_Q1D][max_Q1D];
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            BBu[qy][qx] = 0.0;
            for (int dy = 0; dy < D1D; ++dy)
            {
               const real_t bx = B(qy,dy);
               BBu[qy][qx] += bx * Bu[dy][qx];
            }
         }
      }
      // Calculate Dxy, xDy in plane
      real_t DBu[max_Q1D][max_Q1D][2];
      for (int qy = 0; qy < Q1D; ++qy)
      {
         for (int qx = 0; qx < Q1D; ++qx)
         {
            const real_t O1 = op(qx,qy,0,e);
            const real_t O2 = op(qx,qy,1,e);

            const real_t X = BBu[qy][qx];

            DBu[qy][qx][0] = O1 * X;
            DBu[qy][qx][1] = O2 * X;
         }
      }
      real_t GDBu[max_D1D][max_Q1D][2];
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            GDBu[dy][qx][0] = 0.0;
            GDBu[dy][qx][1] = 0.0;
            for (int qy = 0; qy < Q1D; ++qy)
            {
               const real_t by = Bt(dy,qy);
               const real_t gy = Gt(dy,qy);
               GDBu[dy][qx][0] += by * DBu[qy][qx][0];
               GDBu[dy][qx][1] += gy * DBu[qy][qx][1];
            }
         }
      }
      for (int dx = 0; dx < D1D; ++dx)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            real_t res = 0.0;
            for (int qx = 0; qx < Q1D; ++qx)
            {
               const real_t bx = Bt(dx,qx);
               const real_t gx = Gt(dx,qx);
               res += gx * GDBu[dy][qx][0] + bx * GDBu[dy][qx][1];
            }
            y(dx,dy,e) += res;
         }
      }
   });
}

// Optimized PA Convection Apply 2D kernel
template<int T_D1D = 0, int T_Q1D = 0, int T_NBZ = 0> static
void SmemPAConvectionApplyT2D(const int ne,
                              const Array<real_t> &b,
                              const Array<real_t> &g,
                              const Array<real_t> &bt,
                              const Array<real_t> &gt,
                              const Vector &op_,
                              const Vector &x_,
                              Vector &y_,
                              const int d1d = 0,
                              const int q1d = 0)
{
   const int NE = ne;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   constexpr int NBZ = T_NBZ ? T_NBZ : 1;
   MFEM_VERIFY(D1D <= DeviceDofQuadLimits::Get().MAX_D1D, "");
   MFEM_VERIFY(Q1D <= DeviceDofQuadLimits::Get().MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto Bt = Reshape(bt.Read(), D1D, Q1D);
   auto Gt = Reshape(gt.Read(), D1D, Q1D);
   auto op = Reshape(op_.Read(), Q1D, Q1D, 2, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, NE);
   auto y = Reshape(y_.ReadWrite(), D1D, D1D, NE);
   mfem::forall_2D_batch(NE, Q1D, Q1D, NBZ, [=] MFEM_HOST_DEVICE (int e)
   {
      const int tidz = MFEM_THREAD_ID(z);
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      // the following variables are evaluated at compile time
      constexpr int NBZ = T_NBZ ? T_NBZ : 1;
      constexpr int max_D1D = T_D1D ? T_D1D : DofQuadLimits::MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : DofQuadLimits::MAX_Q1D;
      MFEM_SHARED real_t u[NBZ][max_D1D][max_D1D];
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(dx,x,D1D)
         {
            // e is really equal to e+tidz
            u[tidz][dy][dx] = x(dx,dy,e);
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_SHARED real_t Bu[NBZ][max_D1D][max_Q1D];
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            Bu[tidz][dy][qx] = 0.0;
            for (int dx = 0; dx < D1D; ++dx)
            {
               const real_t bx = B(qx,dx);
               const real_t x = u[tidz][dy][dx];
               Bu[tidz][dy][qx] += bx * x;
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_SHARED real_t BBu[NBZ][max_Q1D][max_Q1D];
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            BBu[tidz][qy][qx] = 0.0;
            for (int dy = 0; dy < D1D; ++dy)
            {
               const real_t bx = B(qy,dy);
               BBu[tidz][qy][qx] += bx * Bu[tidz][dy][qx];
            }
         }
      }
      MFEM_SYNC_THREAD;
      // Calculate Dxy, xDy in plane
      MFEM_SHARED real_t DBu[NBZ][max_Q1D][max_Q1D][2];
      MFEM_FOREACH_THREAD(qy,y,Q1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            const real_t O1 = op(qx,qy,0,e);
            const real_t O2 = op(qx,qy,1,e);

            const real_t X = BBu[tidz][qy][qx];

            DBu[tidz][qy][qx][0] = O1 * X;
            DBu[tidz][qy][qx][1] = O2 * X;
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_SHARED real_t GDBu[NBZ][max_D1D][max_Q1D][2];
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            GDBu[tidz][dy][qx][0] = 0.0;
            GDBu[tidz][dy][qx][1] = 0.0;
            for (int qy = 0; qy < Q1D; ++qy)
            {
               const real_t by = Bt(dy,qy);
               const real_t gy = Gt(dy,qy);
               GDBu[tidz][dy][qx][0] += by * DBu[tidz][qy][qx][0];
               GDBu[tidz][dy][qx][1] += gy * DBu[tidz][qy][qx][1];
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dx,x,D1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            real_t res = 0.0;
            for (int qx = 0; qx < Q1D; ++qx)
            {
               const real_t bx = Bt(dx,qx);
               const real_t gx = Gt(dx,qx);
               res += gx * GDBu[tidz][dy][qx][0] + bx * GDBu[tidz][dy][qx][1];
            }
            y(dx,dy,e) += res;
         }
      }
   });
}

// PA Convection Apply 3D kernel
template<int T_D1D = 0, int T_Q1D = 0> static
void PAConvectionApplyT3D(const int ne,
                          const Array<real_t> &b,
                          const Array<real_t> &g,
                          const Array<real_t> &bt,
                          const Array<real_t> &gt,
                          const Vector &op_,
                          const Vector &x_,
                          Vector &y_,
                          const int d1d = 0,
                          const int q1d = 0)
{
   const int NE = ne;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= DeviceDofQuadLimits::Get().MAX_D1D, "");
   MFEM_VERIFY(Q1D <= DeviceDofQuadLimits::Get().MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto Bt = Reshape(bt.Read(), D1D, Q1D);
   auto Gt = Reshape(gt.Read(), D1D, Q1D);
   auto op = Reshape(op_.Read(), Q1D, Q1D, Q1D, 3, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, D1D, NE);
   auto y = Reshape(y_.ReadWrite(), D1D, D1D, D1D, NE);
   mfem::forall(NE, [=] MFEM_HOST_DEVICE (int e)
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      // the following variables are evaluated at compile time
      constexpr int max_D1D = T_D1D ? T_D1D : DofQuadLimits::MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : DofQuadLimits::MAX_Q1D;

      real_t u[max_D1D][max_D1D][max_D1D];
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            for (int dx = 0; dx < D1D; ++dx)
            {
               u[dz][dy][dx] = x(dx,dy,dz,e);
            }
         }
      }
      real_t Bu[max_D1D][max_D1D][max_Q1D];
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            for (int qx = 0; qx < Q1D; ++qx)
            {
               Bu[dz][dy][qx] = 0.0;
               for (int dx = 0; dx < D1D; ++dx)
               {
                  const real_t bx = B(qx,dx);
                  const real_t x = u[dz][dy][dx];
                  Bu[dz][dy][qx] += bx * x;
               }
            }
         }
      }
      real_t BBu[max_D1D][max_Q1D][max_Q1D];
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int qx = 0; qx < Q1D; ++qx)
         {
            for (int qy = 0; qy < Q1D; ++qy)
            {
               BBu[dz][qy][qx] = 0.0;
               for (int dy = 0; dy < D1D; ++dy)
               {
                  const real_t bx = B(qy,dy);
                  BBu[dz][qy][qx] += bx * Bu[dz][dy][qx];
               }
            }
         }
      }
      real_t BBBu[max_Q1D][max_Q1D][max_Q1D];
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            for (int qz = 0; qz < Q1D; ++qz)
            {
               BBBu[qz][qy][qx] = 0.0;
               for (int dz = 0; dz < D1D; ++dz)
               {
                  const real_t bx = B(qz,dz);
                  BBBu[qz][qy][qx] += bx * BBu[dz][qy][qx];
               }
            }
         }
      }
      // Calculate Dxy, xDy in plane
      real_t DBu[max_Q1D][max_Q1D][max_Q1D][3];
      for (int qz = 0; qz < Q1D; ++qz)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            for (int qx = 0; qx < Q1D; ++qx)
            {
               const real_t O1 = op(qx,qy,qz,0,e);
               const real_t O2 = op(qx,qy,qz,1,e);
               const real_t O3 = op(qx,qy,qz,2,e);

               const real_t X = BBBu[qz][qy][qx];

               DBu[qz][qy][qx][0] = O1 * X;
               DBu[qz][qy][qx][1] = O2 * X;
               DBu[qz][qy][qx][2] = O3 * X;
            }
         }
      }
      real_t GDBu[max_D1D][max_Q1D][max_Q1D][3];
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            for (int dz = 0; dz < D1D; ++dz)
            {
               GDBu[dz][qy][qx][0] = 0.0;
               GDBu[dz][qy][qx][1] = 0.0;
               GDBu[dz][qy][qx][2] = 0.0;
               for (int qz = 0; qz < Q1D; ++qz)
               {
                  const real_t bz = Bt(dz,qz);
                  const real_t gz = Gt(dz,qz);
                  GDBu[dz][qy][qx][0] += bz * DBu[qz][qy][qx][0];
                  GDBu[dz][qy][qx][1] += bz * DBu[qz][qy][qx][1];
                  GDBu[dz][qy][qx][2] += gz * DBu[qz][qy][qx][2];
               }
            }
         }
      }
      real_t GGDBu[max_D1D][max_D1D][max_Q1D][3];
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int qx = 0; qx < Q1D; ++qx)
         {
            for (int dy = 0; dy < D1D; ++dy)
            {
               GGDBu[dz][dy][qx][0] = 0.0;
               GGDBu[dz][dy][qx][1] = 0.0;
               GGDBu[dz][dy][qx][2] = 0.0;
               for (int qy = 0; qy < Q1D; ++qy)
               {
                  const real_t by = Bt(dy,qy);
                  const real_t gy = Gt(dy,qy);
                  GGDBu[dz][dy][qx][0] += by * GDBu[dz][qy][qx][0];
                  GGDBu[dz][dy][qx][1] += gy * GDBu[dz][qy][qx][1];
                  GGDBu[dz][dy][qx][2] += by * GDBu[dz][qy][qx][2];
               }
            }
         }
      }
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            for (int dx = 0; dx < D1D; ++dx)
            {
               real_t res = 0.0;
               for (int qx = 0; qx < Q1D; ++qx)
               {
                  const real_t bx = Bt(dx,qx);
                  const real_t gx = Gt(dx,qx);
                  res += gx * GGDBu[dz][dy][qx][0];
                  res += bx * GGDBu[dz][dy][qx][1];
                  res += bx * GGDBu[dz][dy][qx][2];
               }
               y(dx,dy,dz,e) += res;
            }
         }
      }
   });
}

// Optimized PA Convection Apply 3D kernel
template<int T_D1D = 0, int T_Q1D = 0> static
void SmemPAConvectionApplyT3D(const int ne,
                              const Array<real_t> &b,
                              const Array<real_t> &g,
                              const Array<real_t> &bt,
                              const Array<real_t> &gt,
                              const Vector &op_,
                              const Vector &x_,
                              Vector &y_,
                              const int d1d = 0,
                              const int q1d = 0)
{
   const int NE = ne;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= DeviceDofQuadLimits::Get().MAX_D1D, "");
   MFEM_VERIFY(Q1D <= DeviceDofQuadLimits::Get().MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto Bt = Reshape(bt.Read(), D1D, Q1D);
   auto Gt = Reshape(gt.Read(), D1D, Q1D);
   auto op = Reshape(op_.Read(), Q1D, Q1D, Q1D, 3, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, D1D, NE);
   auto y = Reshape(y_.ReadWrite(), D1D, D1D, D1D, NE);
   mfem::forall_3D(NE, Q1D, Q1D, Q1D, [=] MFEM_HOST_DEVICE (int e)
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      // the following variables are evaluated at compile time
      constexpr int max_D1D = T_D1D ? T_D1D : DofQuadLimits::MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : DofQuadLimits::MAX_Q1D;
      constexpr int max_DQ = (max_Q1D > max_D1D) ? max_Q1D : max_D1D;
      MFEM_SHARED real_t sm0[3*max_DQ*max_DQ*max_DQ];
      MFEM_SHARED real_t sm1[3*max_DQ*max_DQ*max_DQ];

      real_t (*u)[max_D1D][max_D1D] = (real_t (*)[max_D1D][max_D1D]) sm0;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            MFEM_FOREACH_THREAD(dx,x,D1D)
            {
               u[dz][dy][dx] = x(dx,dy,dz,e);
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*Bu)[max_D1D][max_Q1D] = (real_t (*)[max_D1D][max_Q1D])sm1;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            MFEM_FOREACH_THREAD(qx,x,Q1D)
            {
               real_t Bu_ = 0.0;
               for (int dx = 0; dx < D1D; ++dx)
               {
                  const real_t bx = B(qx,dx);
                  const real_t x = u[dz][dy][dx];
                  Bu_ += bx * x;
               }
               Bu[dz][dy][qx] = Bu_;
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*BBu)[max_Q1D][max_Q1D] = (real_t (*)[max_Q1D][max_Q1D])sm0;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            MFEM_FOREACH_THREAD(qy,y,Q1D)
            {
               real_t BBu_ = 0.0;
               for (int dy = 0; dy < D1D; ++dy)
               {
                  const real_t bx = B(qy,dy);
                  BBu_ += bx * Bu[dz][dy][qx];
               }
               BBu[dz][qy][qx] = BBu_;
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*BBBu)[max_Q1D][max_Q1D] = (real_t (*)[max_Q1D][max_Q1D])sm1;
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            MFEM_FOREACH_THREAD(qz,z,Q1D)
            {
               real_t BBBu_ = 0.0;
               for (int dz = 0; dz < D1D; ++dz)
               {
                  const real_t bx = B(qz,dz);
                  BBBu_ += bx * BBu[dz][qy][qx];
               }
               BBBu[qz][qy][qx] = BBBu_;
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*DBu)[max_Q1D][max_Q1D][3] = (real_t (*)[max_Q1D][max_Q1D][3])sm0;
      MFEM_FOREACH_THREAD(qz,z,Q1D)
      {
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            MFEM_FOREACH_THREAD(qx,x,Q1D)
            {
               const real_t O1 = op(qx,qy,qz,0,e);
               const real_t O2 = op(qx,qy,qz,1,e);
               const real_t O3 = op(qx,qy,qz,2,e);

               const real_t X = BBBu[qz][qy][qx];

               DBu[qz][qy][qx][0] = O1 * X;
               DBu[qz][qy][qx][1] = O2 * X;
               DBu[qz][qy][qx][2] = O3 * X;
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*GDBu)[max_Q1D][max_Q1D][3] = (real_t (*)[max_Q1D][max_Q1D][3])sm1;
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            MFEM_FOREACH_THREAD(dz,z,D1D)
            {
               real_t GDBu0 = 0.0;
               real_t GDBu1 = 0.0;
               real_t GDBu2 = 0.0;
               for (int qz = 0; qz < Q1D; ++qz)
               {
                  const real_t bz = Bt(dz,qz);
                  const real_t gz = Gt(dz,qz);
                  GDBu0 += bz * DBu[qz][qy][qx][0];
                  GDBu1 += bz * DBu[qz][qy][qx][1];
                  GDBu2 += gz * DBu[qz][qy][qx][2];
               }
               GDBu[dz][qy][qx][0] = GDBu0;
               GDBu[dz][qy][qx][1] = GDBu1;
               GDBu[dz][qy][qx][2] = GDBu2;
            }
         }
      }
      MFEM_SYNC_THREAD;
      real_t (*GGDBu)[max_D1D][max_Q1D][3] = (real_t (*)[max_D1D][max_Q1D][3])sm0;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            MFEM_FOREACH_THREAD(dy,y,D1D)
            {
               real_t GGDBu0 = 0.0;
               real_t GGDBu1 = 0.0;
               real_t GGDBu2 = 0.0;
               for (int qy = 0; qy < Q1D; ++qy)
               {
                  const real_t by = Bt(dy,qy);
                  const real_t gy = Gt(dy,qy);
                  GGDBu0 += by * GDBu[dz][qy][qx][0];
                  GGDBu1 += gy * GDBu[dz][qy][qx][1];
                  GGDBu2 += by * GDBu[dz][qy][qx][2];
               }
               GGDBu[dz][dy][qx][0] = GGDBu0;
               GGDBu[dz][dy][qx][1] = GGDBu1;
               GGDBu[dz][dy][qx][2] = GGDBu2;
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            MFEM_FOREACH_THREAD(dx,x,D1D)
            {
               real_t res = 0.0;
               for (int qx = 0; qx < Q1D; ++qx)
               {
                  const real_t bx = Bt(dx,qx);
                  const real_t gx = Gt(dx,qx);
                  res += gx * GGDBu[dz][dy][qx][0];
                  res += bx * GGDBu[dz][dy][qx][1];
                  res += bx * GGDBu[dz][dy][qx][2];
               }
               y(dx,dy,dz,e) += res;
            }
         }
      }
   });
}

static void PAConvectionApply(const int dim,
                              const int D1D,
                              const int Q1D,
                              const int NE,
                              const Array<real_t> &B,
                              const Array<real_t> &G,
                              const Array<real_t> &Bt,
                              const Array<real_t> &Gt,
                              const Vector &op,
                              const Vector &x,
                              Vector &y)
{
   if (dim == 2)
   {
      switch ((D1D << 4 ) | Q1D)
      {
         case 0x22: return SmemPAConvectionApply2D<2,2,8>(NE,B,G,Bt,Gt,op,x,y);
         case 0x33: return SmemPAConvectionApply2D<3,3,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x34: return SmemPAConvectionApply2D<3,4,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x44: return SmemPAConvectionApply2D<4,4,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x46: return SmemPAConvectionApply2D<4,6,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x55: return SmemPAConvectionApply2D<5,5,2>(NE,B,G,Bt,Gt,op,x,y);
         case 0x58: return SmemPAConvectionApply2D<5,8,2>(NE,B,G,Bt,Gt,op,x,y);
         case 0x66: return SmemPAConvectionApply2D<6,6,1>(NE,B,G,Bt,Gt,op,x,y);
         case 0x77: return SmemPAConvectionApply2D<7,7,1>(NE,B,G,Bt,Gt,op,x,y);
         case 0x88: return SmemPAConvectionApply2D<8,8,1>(NE,B,G,Bt,Gt,op,x,y);
         case 0x99: return SmemPAConvectionApply2D<9,9,1>(NE,B,G,Bt,Gt,op,x,y);
         default:   return PAConvectionApply2D(NE,B,G,Bt,Gt,op,x,y,D1D,Q1D);
      }
   }
   else if (dim == 3)
   {
      switch ((D1D << 4 ) | Q1D)
      {
         case 0x22: return SmemPAConvectionApply3D<2,2>(NE,B,G,Bt,Gt,op,x,y);
         case 0x23: return SmemPAConvectionApply3D<2,3>(NE,B,G,Bt,Gt,op,x,y);
         case 0x24: return SmemPAConvectionApply3D<2,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x26: return SmemPAConvectionApply3D<2,6>(NE,B,G,Bt,Gt,op,x,y);
         case 0x34: return SmemPAConvectionApply3D<3,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x35: return SmemPAConvectionApply3D<3,5>(NE,B,G,Bt,Gt,op,x,y);
         case 0x45: return SmemPAConvectionApply3D<4,5>(NE,B,G,Bt,Gt,op,x,y);
         case 0x48: return SmemPAConvectionApply3D<4,8>(NE,B,G,Bt,Gt,op,x,y);
         case 0x56: return SmemPAConvectionApply3D<5,6>(NE,B,G,Bt,Gt,op,x,y);
         case 0x67: return SmemPAConvectionApply3D<6,7>(NE,B,G,Bt,Gt,op,x,y);
         case 0x78: return SmemPAConvectionApply3D<7,8>(NE,B,G,Bt,Gt,op,x,y);
         case 0x89: return SmemPAConvectionApply3D<8,9>(NE,B,G,Bt,Gt,op,x,y);
         default:   return PAConvectionApply3D(NE,B,G,Bt,Gt,op,x,y,D1D,Q1D);
      }
   }
   MFEM_ABORT("Unknown kernel.");
}

static void PAConvectionApplyT(const int dim,
                               const int D1D,
                               const int Q1D,
                               const int NE,
                               const Array<real_t> &B,
                               const Array<real_t> &G,
                               const Array<real_t> &Bt,
                               const Array<real_t> &Gt,
                               const Vector &op,
                               const Vector &x,
                               Vector &y)
{
   if (dim == 2)
   {
      switch ((D1D << 4 ) | Q1D)
      {
         case 0x22: return SmemPAConvectionApplyT2D<2,2,8>(NE,B,G,Bt,Gt,op,x,y);
         case 0x33: return SmemPAConvectionApplyT2D<3,3,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x34: return SmemPAConvectionApplyT2D<3,4,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x44: return SmemPAConvectionApplyT2D<4,4,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x46: return SmemPAConvectionApplyT2D<4,6,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x55: return SmemPAConvectionApplyT2D<5,5,2>(NE,B,G,Bt,Gt,op,x,y);
         case 0x58: return SmemPAConvectionApplyT2D<5,8,2>(NE,B,G,Bt,Gt,op,x,y);
         case 0x66: return SmemPAConvectionApplyT2D<6,6,1>(NE,B,G,Bt,Gt,op,x,y);
         case 0x77: return SmemPAConvectionApplyT2D<7,7,1>(NE,B,G,Bt,Gt,op,x,y);
         case 0x88: return SmemPAConvectionApplyT2D<8,8,1>(NE,B,G,Bt,Gt,op,x,y);
         case 0x99: return SmemPAConvectionApplyT2D<9,9,1>(NE,B,G,Bt,Gt,op,x,y);
         default:   return PAConvectionApplyT2D(NE,B,G,Bt,Gt,op,x,y,D1D,Q1D);
      }
   }
   else if (dim == 3)
   {
      switch ((D1D << 4 ) | Q1D)
      {
         case 0x22: return SmemPAConvectionApplyT3D<2,2>(NE,B,G,Bt,Gt,op,x,y);
         case 0x23: return SmemPAConvectionApplyT3D<2,3>(NE,B,G,Bt,Gt,op,x,y);
         case 0x24: return SmemPAConvectionApplyT3D<2,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x26: return SmemPAConvectionApplyT3D<2,6>(NE,B,G,Bt,Gt,op,x,y);
         case 0x34: return SmemPAConvectionApplyT3D<3,4>(NE,B,G,Bt,Gt,op,x,y);
         case 0x35: return SmemPAConvectionApplyT3D<3,5>(NE,B,G,Bt,Gt,op,x,y);
         case 0x45: return SmemPAConvectionApplyT3D<4,5>(NE,B,G,Bt,Gt,op,x,y);
         case 0x48: return SmemPAConvectionApplyT3D<4,8>(NE,B,G,Bt,Gt,op,x,y);
         case 0x56: return SmemPAConvectionApplyT3D<5,6>(NE,B,G,Bt,Gt,op,x,y);
         case 0x67: return SmemPAConvectionApplyT3D<6,7>(NE,B,G,Bt,Gt,op,x,y);
         case 0x78: return SmemPAConvectionApplyT3D<7,8>(NE,B,G,Bt,Gt,op,x,y);
         case 0x89: return SmemPAConvectionApplyT3D<8,9>(NE,B,G,Bt,Gt,op,x,y);
         default:   return PAConvectionApplyT3D(NE,B,G,Bt,Gt,op,x,y,D1D,Q1D);
      }
   }
   MFEM_ABORT("Unknown kernel.");
}

void ConvectionIntegrator::AddMultPA(const Vector &x, Vector &y) const
{
   if (DeviceCanUseCeed())
   {
      ceedOp->AddMult(x, y);
   }
   else
   {
      PAConvectionApply(dim, dofs1D, quad1D, ne,
                        maps->B, maps->G, maps->Bt, maps->Gt,
                        pa_data, x, y);
   }
}

void ConvectionIntegrator::AddMultTransposePA(const Vector &x, Vector &y) const
{
   if (DeviceCanUseCeed())
   {
      MFEM_ABORT("AddMultPA not yet implemented with libCEED for"
                 " ConvectionIntegrator.");
   }
   else
   {
      PAConvectionApplyT(dim, dofs1D, quad1D, ne,
                         maps->B, maps->G, maps->Bt, maps->Gt,
                         pa_data, x, y);
   }
}

} // namespace mfem

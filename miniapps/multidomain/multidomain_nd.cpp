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

// This miniapp is a variant of the multidomain miniapp which aims to extend
// the demonstration given therein to PDEs involving H(curl) finite elements.
//
// A 3D domain comprised of an outer box with a cylinder shaped inside is used.
//
// A magnetic diffusion equation is described on the outer box domain
//
//                  dH/dt = -∇×(σ∇×H)   in outer box
//                    n×H = n×H_wall    on outside wall
//               n×(σ∇×H) = 0           on inside (cylinder) wall
//
// with magnetic field H and coefficient σ (non-physical in this example).
//
// A convection-diffusion equation is described inside the cylinder domain
//
//          dH/dt = -∇×(σ∇×H)+α∇×(v×H)  in inner cylinder
//            n×H = n×H_wall            on cylinder wall (obtained from
//                                      diffusion equation)
//       n×(σ∇×H) = 0                   else
//
// with magnetic field H, coefficients σ, α, and prescribed velocity
// profile v.
//
// To couple the solutions of both equations, a segregated solve with one way
// coupling approach is used. The diffusion equation of the outer box is solved
// from the timestep H_box(t) to H_box(t+dt). Then for the convection-diffusion
// equation H_wall is set to H_box(t+dt) and the equation is solved for H(t+dt)
// which results in a first-order one way coupling. It is important to note
// that when using Nedelec basis functions, as in this example, only the
// tangential portion of H is communicated between the two regions.

#include "mfem.hpp"
#include <fstream>
#include <iostream>

using namespace mfem;

// Prescribed velocity profile for the convection-diffusion equation inside the
// cylinder. The profile is constructed s.t. it approximates a no-slip (v=0)
// directly at the cylinder wall boundary.
void velocity_profile(const Vector &c, Vector &q)
{
   real_t A = 1.0;
   real_t x = c(0);
   real_t y = c(1);
   real_t r = sqrt(pow(x, 2.0) + pow(y, 2.0));

   q(0) = 0.0;
   q(1) = 0.0;

   if (std::abs(r) >= 0.25 - 1e-8)
   {
      q(2) = 0.0;
   }
   else
   {
      q(2) = A * exp(-(pow(x, 2.0) / 2.0 + pow(y, 2.0) / 2.0));
   }
}

// A simple vector field which is everywhere parallel to the xy plane and
// wraps around the domain in a counter-clockwise fashion.
void square_xy(const Vector &p, Vector &v)
{
   v.SetSize(3);

   v[0] = -2.0 * p[1];
   v[1] =  2.0 * p[0];
   v[2] = 0.0;
}

/**
 * @brief Convection-diffusion time dependent operator for a vector field
 *
 *              dH/dt = -∇ × σ ∇ × H  + α ∇ ×(v × H)
 *
 * Can also be used to create a diffusion or convection only operator by setting
 * α or σ to zero.
 */
class ConvectionDiffusionTDO : public TimeDependentOperator
{
public:
   /**
    * @brief Construct a new convection-diffusion time dependent operator.
    *
    * @param fes The ParFiniteElementSpace the solution is defined on
    * @param ess_tdofs All essential true dofs in the Nedelec space
    * @param alpha The convection coefficient
    * @param kappa The diffusion coefficient
    */
   ConvectionDiffusionTDO(ParFiniteElementSpace &fes,
                          Array<int> ess_tdofs,
                          real_t alpha = 1.0,
                          real_t sigma = 1.0e-1)
      : TimeDependentOperator(fes.GetTrueVSize()),
        Mform(&fes),
        Kform(&fes),
        bform(&fes),
        ess_tdofs_(ess_tdofs),
        M_solver(fes.GetComm())
   {
      d = new ConstantCoefficient(-sigma);
      q = new VectorFunctionCoefficient(fes.GetParMesh()->Dimension(),
                                        velocity_profile);

      aq = new ScalarVectorProductCoefficient(alpha, *q);

      Mform.AddDomainIntegrator(new VectorFEMassIntegrator);
      Mform.Assemble(0);
      Mform.Finalize();

      Kform.AddDomainIntegrator(new MixedWeakCurlCrossIntegrator(*aq));
      Kform.AddDomainIntegrator(new CurlCurlIntegrator(*d));
      Kform.Assemble(0);

      Array<int> empty;
      Kform.FormSystemMatrix(empty, K);
      Mform.FormSystemMatrix(ess_tdofs_, M);

      bform.Assemble();
      b = bform.ParallelAssemble();

      M_solver.iterative_mode = false;
      M_solver.SetRelTol(1e-8);
      M_solver.SetAbsTol(0.0);
      M_solver.SetMaxIter(100);
      M_solver.SetPrintLevel(0);
      M_prec.SetType(HypreSmoother::Jacobi);
      M_solver.SetPreconditioner(M_prec);
      M_solver.SetOperator(*M);

      t1.SetSize(height);
      t2.SetSize(height);
   }

   void Mult(const Vector &u, Vector &du_dt) const override
   {
      K->Mult(u, t1);
      t1.Add(1.0, *b);
      M_solver.Mult(t1, du_dt);
      du_dt.SetSubVector(ess_tdofs_, 0.0);
   }

   ~ConvectionDiffusionTDO() override
   {
      delete aq;
      delete q;
      delete d;
      delete b;
   }

   /// Mass form
   ParBilinearForm Mform;

   /// Stiffness form. Might include diffusion, convection or both.
   ParBilinearForm Kform;

   /// Mass opeperator
   OperatorHandle M;

   /// Stiffness opeperator. Might include diffusion, convection or both.
   OperatorHandle K;

   /// RHS form
   ParLinearForm bform;

   /// RHS vector
   Vector *b = nullptr;

   /// Velocity coefficient
   VectorCoefficient *q = nullptr;

   /// alpha * Velocity coefficient
   VectorCoefficient *aq = nullptr;

   /// Diffusion coefficient
   Coefficient *d = nullptr;

   /// Essential true dof array. Relevant for eliminating boundary conditions
   /// when using a Nedelec space.
   Array<int> ess_tdofs_;

   real_t current_dt = -1.0;

   /// Mass matrix solver
   CGSolver M_solver;

   /// Mass matrix preconditioner
   HypreSmoother M_prec;

   /// Auxiliary vectors
   mutable Vector t1, t2;
};

int main(int argc, char *argv[])
{
   Mpi::Init();
   Hypre::Init();
   int num_procs = Mpi::WorldSize();
   int myid = Mpi::WorldRank();

   int order = 2;
   real_t t_final = 5.0;
   real_t dt = 1.0e-5;
   bool visualization = true;
   int visport = 19916;
   int vis_steps = 10;

   OptionsParser args(argc, argv);
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree).");
   args.AddOption(&t_final, "-tf", "--t-final",
                  "Final time; start time is 0.");
   args.AddOption(&dt, "-dt", "--time-step",
                  "Time step.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&vis_steps, "-vs", "--visualization-steps",
                  "Visualize every n-th timestep.");
   args.ParseCheck();

   Mesh *serial_mesh = new Mesh("multidomain-hex.mesh");
   ParMesh parent_mesh = ParMesh(MPI_COMM_WORLD, *serial_mesh);
   delete serial_mesh;

   parent_mesh.UniformRefinement();

   ND_FECollection fec(order, parent_mesh.Dimension());

   // Create the sub-domains and accompanying Finite Element spaces from
   // corresponding attributes. This specific mesh has two domain attributes and
   // 9 boundary attributes.
   Array<int> cylinder_domain_attributes(1);
   cylinder_domain_attributes[0] = 1;

   auto cylinder_submesh =
      ParSubMesh::CreateFromDomain(parent_mesh, cylinder_domain_attributes);

   ParFiniteElementSpace fes_cylinder(&cylinder_submesh, &fec);

   Array<int> inflow_attributes(cylinder_submesh.bdr_attributes.Max());
   inflow_attributes = 0;
   inflow_attributes[7] = 1;

   Array<int> inner_cylinder_wall_attributes(
      cylinder_submesh.bdr_attributes.Max());
   inner_cylinder_wall_attributes = 0;
   inner_cylinder_wall_attributes[8] = 1;

   // For the convection-diffusion equation inside the cylinder domain, the
   // inflow surface and outer wall are treated as Dirichlet boundary
   // conditions.
   Array<int> inflow_tdofs, interface_tdofs, ess_tdofs;
   fes_cylinder.GetEssentialTrueDofs(inflow_attributes, inflow_tdofs);
   fes_cylinder.GetEssentialTrueDofs(inner_cylinder_wall_attributes,
                                     interface_tdofs);
   ess_tdofs.Append(inflow_tdofs);
   ess_tdofs.Append(interface_tdofs);
   ess_tdofs.Sort();
   ess_tdofs.Unique();
   ConvectionDiffusionTDO cd_tdo(fes_cylinder, ess_tdofs);

   ParGridFunction magnetic_field_cylinder_gf(&fes_cylinder);
   magnetic_field_cylinder_gf = 0.0;

   Vector magnetic_field_cylinder;
   magnetic_field_cylinder_gf.GetTrueDofs(magnetic_field_cylinder);

   RK3SSPSolver cd_ode_solver;
   cd_ode_solver.Init(cd_tdo);

   Array<int> outer_domain_attributes(1);
   outer_domain_attributes[0] = 2;

   auto block_submesh = ParSubMesh::CreateFromDomain(parent_mesh,
                                                     outer_domain_attributes);

   ParFiniteElementSpace fes_block(&block_submesh, &fec);

   Array<int> block_wall_attributes(block_submesh.bdr_attributes.Max());
   block_wall_attributes = 0;
   block_wall_attributes[0] = 1;
   block_wall_attributes[1] = 1;
   block_wall_attributes[2] = 1;
   block_wall_attributes[3] = 1;

   Array<int> outer_cylinder_wall_attributes(
      block_submesh.bdr_attributes.Max());
   outer_cylinder_wall_attributes = 0;
   outer_cylinder_wall_attributes[8] = 1;

   fes_block.GetEssentialTrueDofs(block_wall_attributes, ess_tdofs);

   ConvectionDiffusionTDO d_tdo(fes_block, ess_tdofs, 0.0, 1.0);

   ParGridFunction magnetic_field_block_gf(&fes_block);
   magnetic_field_block_gf = 0.0;

   VectorFunctionCoefficient one(3, square_xy);
   magnetic_field_block_gf.ProjectBdrCoefficientTangent(one,
                                                        block_wall_attributes);

   Vector magnetic_field_block;
   magnetic_field_block_gf.GetTrueDofs(magnetic_field_block);

   RK3SSPSolver d_ode_solver;
   d_ode_solver.Init(d_tdo);

   Array<int> cylinder_surface_attributes(1);
   cylinder_surface_attributes[0] = 9;

   auto cylinder_surface_submesh =
      ParSubMesh::CreateFromBoundary(parent_mesh, cylinder_surface_attributes);

   char vishost[] = "localhost";
   socketstream cyl_sol_sock;
   if (visualization)
   {
      cyl_sol_sock.open(vishost, visport);
      cyl_sol_sock << "parallel " << num_procs << " " << myid << "\n";
      cyl_sol_sock.precision(8);
      cyl_sol_sock << "solution\n" << cylinder_submesh
                   << magnetic_field_cylinder_gf
                   << "window_title \"Time step: " << 0 << "\""
                   << "keys cvv\n autoscale off\n valuerange 0 1.414\n"
                   << "pause\n" << std::flush;
   }
   socketstream block_sol_sock;
   if (visualization)
   {
      block_sol_sock.open(vishost, visport);
      block_sol_sock << "parallel " << num_procs << " " << myid << "\n";
      block_sol_sock.precision(8);
      block_sol_sock << "solution\n" << block_submesh
                     << magnetic_field_block_gf
                     << "window_title \"Time step: " << 0 << "\""
                     << "window_geometry 400 0 400 350\n"
                     << "keys cvv\n autoscale off\n valuerange 0 1.414\n"
                     << "pause\n" << std::flush;
   }

   // Create the transfer map needed in the time integration loop
   auto magnetic_field_block_to_cylinder_map = ParSubMesh::CreateTransferMap(
                                                  magnetic_field_block_gf,
                                                  magnetic_field_cylinder_gf);

   real_t t = 0.0;
   bool last_step = false;
   for (int ti = 1; !last_step; ti++)
   {
      if (t + dt >= t_final - dt/2)
      {
         last_step = true;
      }

      // Advance the diffusion equation on the outer block to the next time step
      d_ode_solver.Step(magnetic_field_block, t, dt);
      {
         // Transfer the solution from the inner surface of the outer block to
         // the cylinder outer surface to act as a boundary condition.
         magnetic_field_block_gf.SetFromTrueDofs(magnetic_field_block);

         magnetic_field_block_to_cylinder_map.Transfer(magnetic_field_block_gf,
                                                       magnetic_field_cylinder_gf);

         magnetic_field_cylinder_gf.GetTrueDofs(magnetic_field_cylinder);
      }
      // Advance the convection-diffusion equation on the outer block to the
      // next time step
      cd_ode_solver.Step(magnetic_field_cylinder, t, dt);

      if (last_step || (ti % vis_steps) == 0)
      {
         if (myid == 0)
         {
            out << "step " << ti << ", t = " << t << std::endl;
         }

         magnetic_field_cylinder_gf.SetFromTrueDofs(magnetic_field_cylinder);
         magnetic_field_block_gf.SetFromTrueDofs(magnetic_field_block);

         if (visualization)
         {
            cyl_sol_sock << "parallel " << num_procs << " " << myid << "\n";
            cyl_sol_sock << "solution\n" << cylinder_submesh
                         << magnetic_field_cylinder_gf
                         << "window_title \"Time step: " << ti << "\""
                         << std::flush;
            block_sol_sock << "parallel " << num_procs << " " << myid << "\n";
            block_sol_sock << "solution\n" << block_submesh
                           << magnetic_field_block_gf
                           << "window_title \"Time step: " << ti << "\""
                           << std::flush;
         }
      }
   }

   return 0;
}

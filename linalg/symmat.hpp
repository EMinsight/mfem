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

#ifndef MFEM_SYMMETRICMAT
#define MFEM_SYMMETRICMAT

#include "../config/config.hpp"
#include "../general/globals.hpp"
#include "matrix.hpp"

namespace mfem
{

/// Dense symmetric matrix storing the upper triangular part. This class so far
/// has little functionality beyond storage.
class DenseSymmetricMatrix : public Matrix
{
private:
   Memory<real_t> data;

public:

   /** Default constructor for DenseSymmetricMatrix.
       Sets data = NULL and height = width = 0. */
   DenseSymmetricMatrix();

   /// Creates square matrix of size s.
   explicit DenseSymmetricMatrix(int s);

   /// Construct a DenseSymmetricMatrix using an existing data array.
   /** The DenseSymmetricMatrix does not assume ownership of the data array, i.e. it will
       not delete the array. */
   DenseSymmetricMatrix(real_t *d, int s)
      : Matrix(s, s) { UseExternalData(d, s); }

   /// Change the data array and the size of the DenseSymmetricMatrix.
   /** The DenseSymmetricMatrix does not assume ownership of the data array, i.e. it will
       not delete the data array @a d. This method should not be used with
       DenseSymmetricMatrix that owns its current data array. */
   void UseExternalData(real_t *d, int s)
   {
      data.Wrap(d, (s*(s+1))/2, false);
      height = s; width = s;
   }

   /// Change the data array and the size of the DenseSymmetricMatrix.
   /** The DenseSymmetricMatrix does not assume ownership of the data array, i.e. it will
       not delete the new array @a d. This method will delete the current data
       array, if owned. */
   void Reset(real_t *d, int s)
   { if (OwnsData()) { data.Delete(); } UseExternalData(d, s); }

   /** Clear the data array and the dimensions of the DenseSymmetricMatrix. This method
       should not be used with DenseSymmetricMatrix that owns its current data array. */
   void ClearExternalData() { data.Reset(); height = width = 0; }

   /// Delete the matrix data array (if owned) and reset the matrix state.
   void Clear()
   { if (OwnsData()) { data.Delete(); } ClearExternalData(); }

   /// Change the size of the DenseSymmetricMatrix to s x s.
   void SetSize(int s);

   /// Return the number of stored nonzeros in the matrix.
   int GetStoredSize() const { return Height()*(Height()+1)/2; }

   /// Returns the matrix data array.
   inline real_t *Data() const
   { return const_cast<real_t*>((const real_t*)data);}

   /// Returns the matrix data array.
   inline real_t *GetData() const { return Data(); }

   Memory<real_t> &GetMemory() { return data; }
   const Memory<real_t> &GetMemory() const { return data; }

   /// Return the DenseSymmetricMatrix data (host pointer) ownership flag.
   inline bool OwnsData() const { return data.OwnsHostPtr(); }

   /// Returns reference to a_{ij}.
   inline real_t &operator()(int i, int j);

   /// Returns constant reference to a_{ij}.
   inline const real_t &operator()(int i, int j) const;

   /// Returns reference to a_{ij}.
   real_t &Elem(int i, int j) override;

   /// Returns constant reference to a_{ij}.
   const real_t &Elem(int i, int j) const override;

   /// Sets the matrix elements equal to constant c
   DenseSymmetricMatrix &operator=(real_t c);

   DenseSymmetricMatrix &operator*=(real_t c);

   /// Sets the matrix size and elements equal to those of m
   DenseSymmetricMatrix &operator=(const DenseSymmetricMatrix &m);

   std::size_t MemoryUsage() const { return data.Capacity() * sizeof(real_t); }

   /// Shortcut for mfem::Read( GetMemory(), TotalSize(), on_dev).
   const real_t *Read(bool on_dev = true) const
   { return mfem::Read(data, Height()*Width(), on_dev); }

   /// Shortcut for mfem::Read(GetMemory(), TotalSize(), false).
   const real_t *HostRead() const
   { return mfem::Read(data, Height()*Width(), false); }

   /// Shortcut for mfem::Write(GetMemory(), TotalSize(), on_dev).
   real_t *Write(bool on_dev = true)
   { return mfem::Write(data, Height()*Width(), on_dev); }

   /// Shortcut for mfem::Write(GetMemory(), TotalSize(), false).
   real_t *HostWrite()
   { return mfem::Write(data, Height()*Width(), false); }

   /// Shortcut for mfem::ReadWrite(GetMemory(), TotalSize(), on_dev).
   real_t *ReadWrite(bool on_dev = true)
   { return mfem::ReadWrite(data, Height()*Width(), on_dev); }

   /// Shortcut for mfem::ReadWrite(GetMemory(), TotalSize(), false).
   real_t *HostReadWrite()
   { return mfem::ReadWrite(data, Height()*Width(), false); }

   /// Matrix vector multiplication.
   void Mult(const Vector &x, Vector &y) const override;

   /// Returns a pointer to (an approximation) of the matrix inverse.
   MatrixInverse *Inverse() const override;

   /// Destroys the symmetric matrix.
   virtual ~DenseSymmetricMatrix();
};

// Inline methods

// The number of entries stored in rows 1,...,k is
// n + n-1 + n-2 + ... + n-k+1, where there are k terms. This equals
// kn - sum_{i=1}^{k-1} i = kn - (k-1)k/2
// This formula is used for the offset for each row.
inline real_t &DenseSymmetricMatrix::operator()(int i, int j)
{
   MFEM_ASSERT(data && i >= 0 && i < height && j >= 0 && j < width, "");
   if (i > j)  // reverse i and j
   {
      return data[(j*height) - (((j-1)*j)/2) + i - j];
   }
   else
   {
      return data[(i*height) - (((i-1)*i)/2) + j - i];
   }
}

inline const real_t &DenseSymmetricMatrix::operator()(int i, int j) const
{
   MFEM_ASSERT(data && i >= 0 && i < height && j >= 0 && j < width, "");
   if (i > j)  // reverse i and j
   {
      return data[(j*height) - (((j-1)*j)/2) + i - j];
   }
   else
   {
      return data[(i*height) - (((i-1)*i)/2) + j - i];
   }
}

} // namespace mfem

#endif

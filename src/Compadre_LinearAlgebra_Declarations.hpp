#ifndef _COMPADRE_LINEAR_ALGEBRA_DECLARATIONS_HPP_
#define _COMPADRE_LINEAR_ALGEBRA_DECLARATIONS_HPP_

#include "Compadre_Config.h"
#include "Compadre_Typedefs.hpp"

#ifdef COMPADRE_USE_CUDA
  #include <cuda_runtime.h>
  #include <cublas_v2.h>
  #include <cublas_api.h>
  #include <cusolverDn.h>
#endif

#ifdef COMPADRE_USE_LAPACK
  extern "C" int dgels_( char* trans, int *m, int *n, int *k, double *a, 
              int *lda, double *c, int *ldc, double *work, int *lwork, int *info);
  
  extern "C" void dgelsd_( int* m, int* n, int* nrhs, double* a, int* lda,
              double* b, int* ldb, double* s, double* rcond, int* rank,
              double* work, int* lwork, int* iwork, int* info );
#ifdef COMPADRE_USE_OPENBLAS
  void openblas_set_num_threads(int num_threads);
#endif
#endif // COMPADRE_USE_LAPACK

namespace Compadre {

namespace GMLS_LinearAlgebra {

    /*! \brief Creates a matrix M=A^T*A from a matrix A
        \param teamMember    [in] - Kokkos::TeamPolicy member type (created by parallel_for)
        \param M_data       [out] - result of weighted_P^T * weighted_P
        \param weighted_P    [in] - matrix to be multiplied by its transpose
        \param columns       [in] - number of columns in weighted_P
        \param rows          [in] - number of rows in weighted_P
    */
    KOKKOS_INLINE_FUNCTION
    void createM(const member_type& teamMember, scratch_matrix_right_type M_data, scratch_matrix_right_type weighted_P, const int columns, const int rows);

    /*! \brief Calculates two eigenvectors corresponding to two dominant eigenvalues
        \param teamMember    [in] - Kokkos::TeamPolicy member type (created by parallel_for)
        \param V            [out] - dimension * dimension Kokkos View
        \param PtP           [in] - dimension * dimension Kokkos View
        \param dimensions    [in] - dimension of PtP
    */
    KOKKOS_INLINE_FUNCTION
    void largestTwoEigenvectorsThreeByThreeSymmetric(const member_type& teamMember, scratch_matrix_right_type V, scratch_matrix_right_type PtP, const int dimensions, pool_type& random_number_pool);

    /*! \brief Calls LAPACK or CUBLAS to solve a batch of QR problems

         P contains num_matrices * lda * ndb data which is num_matrices different matrices, and
         RHS contains num_matrices * ldn * ndb data which is num_matrices different matrix right hand sides.

        \param P                [in/out] - evaluation of sampling functional on polynomial basis (in), meaningless workspace output (out)
        \param lda                  [in] - row dimension of each matrix in P
        \param nda                  [in] - columns dimension of each matrix in P
        \param RHS              [in/out] - basis to invert P against (in), polynomial coefficients (out)
        \param ldb                  [in] - row dimension of each matrix in RHS
        \param ndb                  [in] - column dimension of each matrix in RHS
        \param M                    [in] - number of rows containing data (maximum rows over everything in batch)
        \param N                    [in] - number of columns containing data
        \param num_matrices         [in] - number of target (GMLS problems to be solved)
        \param max_neighbors        [in] - integer for maximum neighbor over all targets
        \param neighbor_list_sizes  [in] - pointer to all neighbor list sizes for each target
    */
    void batchQRFactorize(double *P, int lda, int nda, double *RHS, int ldb, int ndb, int M, int N, const int num_matrices, const size_t max_neighbors = 0, int * neighbor_list_sizes = NULL);

    /*! \brief Calls LAPACK or CUBLAS to solve a batch of SVD problems

         P contains num_matrices * lda * ndb data which is num_matrices different matrices, and
         RHS contains num_matrices * ldn * ndb data which is num_matrices different matrix right hand sides.

        \param P                [in/out] - evaluation of sampling functional on polynomial basis (in), meaningless workspace output (out)
        \param lda                  [in] - row dimension of each matrix in P
        \param nda                  [in] - columns dimension of each matrix in P
        \param RHS              [in/out] - basis to invert P against (in), polynomial coefficients (out)
        \param ldb                  [in] - row dimension of each matrix in RHS
        \param ndb                  [in] - column dimension of each matrix in RHS
        \param M                    [in] - number of rows containing data (maximum rows over everything in batch)
        \param N                    [in] - number of columns containing data
        \param num_matrices         [in] - number of target (GMLS problems to be solved)
        \param max_neighbors        [in] - integer for maximum neighbor over all targets
        \param neighbor_list_sizes  [in] - pointer to all neighbor list sizes for each target
    */
    void batchSVDFactorize(double *P, int lda, int nda, double *RHS, int ldb, int ndb, int M, int N, const int num_matrices, const size_t max_neighbors = 0, int * neighbor_list_sizes = NULL);

}; // GMLS_LinearAlgebra
}; // Compadre

#endif

#include "Compadre_GMLS.hpp"
#include "Compadre_GMLS_ApplyTargetEvaluations.hpp"
#include "Compadre_GMLS_Basis.hpp"
#include "Compadre_GMLS_Quadrature.hpp"
#include "Compadre_GMLS_Targets.hpp"

#include <float.h> // for DBL_MAX

namespace Compadre {

void GMLS::generatePolynomialCoefficients() {

    /*
     *    Operations to Device
     */

    // copy over operations
    _operations = decltype(_operations)("operations", _lro.size());
    _host_operations = Kokkos::create_mirror_view(_operations);
    
    compadre_assert_release((_max_num_neighbors >= 0) && "Neighbor lists not set in GMLS class before calling generateAlphas");
    
    // loop through list of linear reconstruction operations to be performed and set them on the host
    for (int i=0; i<_lro.size(); ++i) _host_operations(i) = _lro[i];

    // get copy of operations on the device
    Kokkos::deep_copy(_operations, _host_operations);

    /*
     *    Initialize Alphas and Prestencil Weights
     */

    // initialize all alpha values to be used for taking the dot product with data to get a reconstruction 
    const int max_evaluation_sites = (static_cast<int>(_additional_evaluation_indices.extent(1)) > 1) 
                ? static_cast<int>(_additional_evaluation_indices.extent(1)) : 1;
    // would have to store for the max case (potentially number
    _alphas = decltype(_alphas)("coefficients", _neighbor_lists.extent(0), _total_alpha_values*max_evaluation_sites, _max_num_neighbors);

    // initialize the prestencil weights that are applied to sampling data to put it into a form 
    // that the GMLS operator will be able to operate on
    auto sro = _data_sampling_functional;
    _prestencil_weights = decltype(_prestencil_weights)("Prestencil weights",
            std::pow(2,sro.use_target_site_weights), 
            (sro.transform_type==DifferentEachTarget 
                    || sro.transform_type==DifferentEachNeighbor) ?
                _neighbor_lists.extent(0) : 1,
            (sro.transform_type==DifferentEachNeighbor) ?
                _max_num_neighbors : 1,
            (sro.output_rank>0) ?
                _local_dimensions : 1,
            (sro.input_rank>0) ?
                _global_dimensions : 1);

    /*
     *    Determine if Nontrivial Null Space in Solution
     */

    // check whether the sampling function acting on the basis will induce a nontrivial nullspace
    // an example would be reconstructing from gradient information, which would annihilate constants
    _nontrivial_nullspace = _polynomial_sampling_functional.nontrivial_nullspace;

    /*
     *    Determine if Nonstandard Sampling Dimension or Basis Component Dimension
     */

    // calculate sampling dimension 
    // this would normally be SamplingOutputTensorRank[_data_sampling_functional], but we also want to handle the
    // case of reconstructions where a scalar basis is reused as a vector, and this handles everything

    // this handles scalars, vectors, and scalars that are reused as vectors
    _sampling_multiplier = std::pow(_local_dimensions, 
            std::min(ActualReconstructionSpaceRank[(int)_reconstruction_space], 
                _data_sampling_functional.output_rank));

    // effective number of components in the basis
    _data_sampling_multiplier = std::pow(_local_dimensions, 
                _data_sampling_functional.output_rank);

    // calculate the dimension of the basis (a vector space on a manifold requires two components, for example)
    _basis_multiplier = std::pow(_local_dimensions, ActualReconstructionSpaceRank[(int)_reconstruction_space]);

    // special case for using a higher order for sampling from a polynomial space that are gradients of a scalar polynomial
    if (_polynomial_sampling_functional == StaggeredEdgeAnalyticGradientIntegralSample) {
        // if the reconstruction is being made with a gradient of a basis, then we want that basis to be one order higher so that
        // the gradient is consistent with the convergence order expected.
        _poly_order += 1;
        _NP = this->getNP(_poly_order, _dimensions);
    }

    /*
     *    Dimensions
     */

    // for tallying scratch space needed for device kernel calls
    _team_scratch_size_a = 0;

    // TEMPORARY, take to zero after conversion
    _team_scratch_size_b = 0;
    _thread_scratch_size_a = 0;
    _thread_scratch_size_b = 0;

    // dimensions that are relevant for each subproblem
    int max_num_rows = _sampling_multiplier*_max_num_neighbors;
    int this_num_columns = _basis_multiplier*_NP;
    int manifold_NP = 0;

    if (_dense_solver_type == DenseSolverType::MANIFOLD) {
        // these dimensions already calculated differ in the case of manifolds
        manifold_NP = this->getNP(_curvature_poly_order, _dimensions-1);
        _NP = this->getNP(_poly_order, _dimensions-1);
        const int max_manifold_NP = (manifold_NP > _NP) ? manifold_NP : _NP;
        this_num_columns = _basis_multiplier*max_manifold_NP;
        const int max_P_row_size = ((_dimensions-1)*manifold_NP > max_manifold_NP*_total_alpha_values*_basis_multiplier) ? (_dimensions-1)*manifold_NP : max_manifold_NP*_total_alpha_values*_basis_multiplier*max_evaluation_sites;

        /*
         *    Calculate Scratch Space Allocations
         */

        _team_scratch_size_b += scratch_matrix_right_type::shmem_size(_dimensions-1, _dimensions-1); // G
        _team_scratch_size_b += scratch_matrix_right_type::shmem_size(_dimensions, _dimensions); // PTP matrix
        _team_scratch_size_b += scratch_vector_type::shmem_size( (_dimensions-1)*_max_num_neighbors ); // manifold_gradient

        _team_scratch_size_b += scratch_vector_type::shmem_size(_max_num_neighbors*std::max(_sampling_multiplier,_basis_multiplier)); // t1 work vector for qr
        _team_scratch_size_b += scratch_vector_type::shmem_size(_max_num_neighbors*std::max(_sampling_multiplier,_basis_multiplier)); // t2 work vector for qr

        _team_scratch_size_b += scratch_vector_type::shmem_size(max_P_row_size); // row of P matrix, one for each operator
        _thread_scratch_size_b += scratch_vector_type::shmem_size(max_manifold_NP*_basis_multiplier); // delta, used for each thread
        if (_data_sampling_functional == VaryingManifoldVectorPointSample) {
            _thread_scratch_size_b += scratch_vector_type::shmem_size(_dimensions*_dimensions); // temporary tangent calculations, used for each thread
        }


        // allocate data on the device (initialized to zero)
        _T = Kokkos::View<double*>("tangent approximation",_target_coordinates.extent(0)*_dimensions*_dimensions);
        _manifold_metric_tensor_inverse = Kokkos::View<double*>("manifold metric tensor inverse",_target_coordinates.extent(0)*(_dimensions-1)*(_dimensions-1));
        _manifold_curvature_coefficients = Kokkos::View<double*>("manifold curvature coefficients",_target_coordinates.extent(0)*manifold_NP);
        _manifold_curvature_gradient = Kokkos::View<double*>("manifold curvature gradient",_target_coordinates.extent(0)*(_dimensions-1));

    } else  { // Standard GMLS

        /*
         *    Calculate Scratch Space Allocations
         */

        _team_scratch_size_a += scratch_vector_type::shmem_size(max_num_rows); // t1 work vector for qr
        _team_scratch_size_a += scratch_vector_type::shmem_size(max_num_rows); // t2 work vector for qr

        // row of P matrix, one for each operator
        // +1 is for the original target site which always gets evaluated
        _team_scratch_size_b += scratch_vector_type::shmem_size(this_num_columns*_total_alpha_values*max_evaluation_sites); 

        _thread_scratch_size_b += scratch_vector_type::shmem_size(this_num_columns); // delta, used for each thread
    }

    /*
     *    Allocate Global Device Storage of Data Needed Over Multiple Calls
     */

    // allocate data on the device (initialized to zero)
    _P = Kokkos::View<double*>("P", 
            TO_GLOBAL(_target_coordinates.extent(0))*TO_GLOBAL(max_num_rows)*TO_GLOBAL(this_num_columns));
    _RHS = Kokkos::View<double*>("RHS", 
            TO_GLOBAL(_target_coordinates.extent(0))*TO_GLOBAL(max_num_rows)*TO_GLOBAL(max_num_rows));
    _w = Kokkos::View<double*>("w", 
            TO_GLOBAL(_target_coordinates.extent(0))*TO_GLOBAL(max_num_rows));
    Kokkos::fence();
    
    /*
     *    Calculate Optimal Threads Based On Levels of Parallelism
     */


#ifdef COMPADRE_USE_CUDA
    _threads_per_team = 128;
    if (_basis_multiplier*_NP > 96) _threads_per_team += 128;
#else
    _threads_per_team = 1;
#endif


    if (_dense_solver_type == DenseSolverType::MANIFOLD) {

        /*
         *    MANIFOLD Problems
         */

        // generate quadrature for staggered approach
        this->generate1DQuadrature();

        if (!_orthonormal_tangent_space_provided) { // user did not specify orthonormal tangent directions, so we approximate them first
            // coarse tangent plane approximation construction of P^T*P
            this->CallFunctorWithTeamThreads<ComputeCoarseTangentPlane>(_threads_per_team, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);

            // if the user provided the reference outward normal direction, then orient the computed or user provided
            // outward normal directions in the tangent bundle
            if (_reference_outward_normal_direction_provided && _use_reference_outward_normal_direction_provided_to_orient_surface) {
                // use the reference outward normal direction provided by the user to orient
                // the tangent bundle
                this->CallFunctorWithTeamThreads<FixTangentDirectionOrdering>(_threads_per_team, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);
            }

            // assembles the P*sqrt(weights) matrix and constructs sqrt(weights)*Identity for curvature
            this->CallFunctorWithTeamThreads<AssembleCurvaturePsqrtW>(_threads_per_team, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);

            // solves P*sqrt(weights) against sqrt(weights)*Identity, stored in RHS
            Kokkos::Profiling::pushRegion("Curvature QR Factorization");
            GMLS_LinearAlgebra::batchQRFactorize(_P.data(), max_num_rows, this_num_columns, _RHS.data(), max_num_rows, max_num_rows, _max_num_neighbors, manifold_NP, _target_coordinates.extent(0), _max_num_neighbors, _number_of_neighbors_list.data());
            Kokkos::Profiling::popRegion();

            // evaluates targets, applies target evaluation to polynomial coefficients for curvature
            this->CallFunctorWithTeamThreads<GetAccurateTangentDirections>(_threads_per_team, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);

            // copy tangent bundle from device back to host
            _host_T = Kokkos::create_mirror_view(_T);
            Kokkos::deep_copy(_host_T, _T);
        }

        // this time assembling curvature PsqrtW matrix is using a highly accurate approximation of the tangent, previously calculated
        // assembles the P*sqrt(weights) matrix and constructs sqrt(weights)*Identity for curvature
        this->CallFunctorWithTeamThreads<AssembleCurvaturePsqrtW>(_threads_per_team, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);

        // solves P*sqrt(weights) against sqrt(weights)*Identity, stored in RHS
        Kokkos::Profiling::pushRegion("Curvature QR Factorization");
        GMLS_LinearAlgebra::batchQRFactorize(_P.data(), max_num_rows, this_num_columns, _RHS.data(), max_num_rows, max_num_rows, _max_num_neighbors, manifold_NP, _target_coordinates.extent(0), _max_num_neighbors, _number_of_neighbors_list.data());
        Kokkos::Profiling::popRegion();

        // evaluates targets, applies target evaluation to polynomial coefficients for curvature
        this->CallFunctorWithTeamThreads<ApplyCurvatureTargets>(_threads_per_team, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);
        Kokkos::fence();

        // prestencil weights calculated here. appropriate because:
        // precedes polynomial reconstruction from data (replaces contents of _RHS) 
        // follows reconstruction of geometry
        // calculate prestencil weights
#ifdef COMPADRE_USE_CUDA
        int nv=8;
#else
        int nv=1;
#endif
        int nt=_threads_per_team/nv;
        this->CallFunctorWithTeamThreadsAndVectors<ComputePrestencilWeights>(nt, nv, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);

        // assembles the P*sqrt(weights) matrix and constructs sqrt(weights)*Identity
        this->CallFunctorWithTeamThreads<AssembleManifoldPsqrtW>(_threads_per_team, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);

        // solves P*sqrt(weights) against sqrt(weights)*Identity, stored in RHS
        // uses SVD if necessary or if explicitly asked to do so (much slower than QR)
        if (_nontrivial_nullspace || _dense_solver_type == DenseSolverType::SVD) {
            Kokkos::Profiling::pushRegion("Manifold SVD Factorization");
            GMLS_LinearAlgebra::batchSVDFactorize(_P.data(), max_num_rows, this_num_columns, _RHS.data(), max_num_rows, max_num_rows, max_num_rows, this_num_columns, _target_coordinates.extent(0), _max_num_neighbors, _number_of_neighbors_list.data());
            Kokkos::Profiling::popRegion();
        } else {
            Kokkos::Profiling::pushRegion("Manifold QR Factorization");
            GMLS_LinearAlgebra::batchQRFactorize(_P.data(), max_num_rows, this_num_columns, _RHS.data(), max_num_rows, max_num_rows, max_num_rows, this_num_columns, _target_coordinates.extent(0), _max_num_neighbors, _number_of_neighbors_list.data());
            Kokkos::Profiling::popRegion();
        }
        Kokkos::fence();

    } else {

        /*
         *    STANDARD GMLS Problems
         */

        // generate quadrature for face normal approach
        this->generate1DQuadrature();

        // assembles the P*sqrt(weights) matrix and constructs sqrt(weights)*Identity
        this->CallFunctorWithTeamThreads<AssembleStandardPsqrtW>(_threads_per_team, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);
        Kokkos::fence();

        // solves P*sqrt(weights) against sqrt(weights)*Identity, stored in RHS
        // uses SVD if necessary or if explicitly asked to do so (much slower than QR)
        if (_nontrivial_nullspace || _dense_solver_type == DenseSolverType::SVD) {
            Kokkos::Profiling::pushRegion("SVD Factorization");
            GMLS_LinearAlgebra::batchSVDFactorize(_P.data(), max_num_rows, this_num_columns, _RHS.data(), max_num_rows, max_num_rows, max_num_rows, this_num_columns, _target_coordinates.extent(0), _max_num_neighbors, _number_of_neighbors_list.data());
            Kokkos::Profiling::popRegion();
        } else {
            Kokkos::Profiling::pushRegion("QR Factorization");
            GMLS_LinearAlgebra::batchQRFactorize(_P.data(), max_num_rows, this_num_columns, _RHS.data(), max_num_rows, max_num_rows, max_num_rows, this_num_columns, _target_coordinates.extent(0), _max_num_neighbors, _number_of_neighbors_list.data());
            Kokkos::Profiling::popRegion();
        }

#ifdef COMPADRE_USE_CUDA
        int nv=8;
#else
        int nv=1;
#endif
        int nt=_threads_per_team/nv;
        this->CallFunctorWithTeamThreadsAndVectors<ComputePrestencilWeights>(nt, nv, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);
        Kokkos::fence();
    }

    // deallocate _P and _w
    _P = Kokkos::View<double*>("P",0);
    _w = Kokkos::View<double*>("w",0);

}

void GMLS::generateAlphas() {

    // check if polynomial coefficients for reconstruction are already generated
    if (_RHS.extent(0) <= 0) this->generatePolynomialCoefficients();

    /*
     *    Calculate Optimal Threads Based On Levels of Parallelism
     */


    if (_dense_solver_type == DenseSolverType::MANIFOLD) {

        /*
         *    MANIFOLD Problems
         */

        // evaluates targets, applies target evaluation to polynomial coefficients to store in _alphas
        this->CallFunctorWithTeamThreads<ApplyManifoldTargets>(_threads_per_team, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);

    } else {

        /*
         *    STANDARD GMLS Problems
         */

        // evaluates targets, applies target evaluation to polynomial coefficients to store in _alphas
#ifdef COMPADRE_USE_CUDA
        int nv=8;
#else
        int nv=1;
#endif
        int nt=_threads_per_team/nv;
        this->CallFunctorWithTeamThreadsAndVectors<ApplyStandardTargets>(nt, nv, _team_scratch_size_a, _team_scratch_size_b, _thread_scratch_size_a, _thread_scratch_size_b);

    }
    Kokkos::fence();

    /*
     *    Device to Host Copy Of Solution
     */

    // copy computed alphas back to the host
    _host_alphas = Kokkos::create_mirror_view(_alphas);
    if (_data_sampling_functional != PointSample) {
        _host_prestencil_weights = Kokkos::create_mirror_view(_prestencil_weights);
        Kokkos::deep_copy(_host_prestencil_weights, _prestencil_weights);
    }
    Kokkos::deep_copy(_host_alphas, _alphas);
    Kokkos::fence();
}


KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const AssembleStandardPsqrtW&, const member_type& teamMember) const {

    /*
     *    Dimensions
     */

    const int target_index = teamMember.league_rank();

    const int max_num_rows = _sampling_multiplier*_max_num_neighbors;
    const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
    const int this_num_columns = _basis_multiplier*_NP;

    /*
     *    Data
     */

    // team_scratch all have a copy each per team
    // the threads in a team work on these local copies that only the team sees
    // thread_scratch has a copy per thread

    scratch_matrix_right_type PsqrtW(_P.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(this_num_columns), max_num_rows, this_num_columns);
    scratch_matrix_right_type RHS(_RHS.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(max_num_rows), max_num_rows, max_num_rows);
    scratch_vector_type w(_w.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows), max_num_rows);

    // delta, used for each thread
    scratch_vector_type delta(teamMember.thread_scratch(_scratch_thread_level_b), this_num_columns);

    /*
     *    Assemble P*sqrt(W) and sqrt(w)*Identity
     */

    // creates the matrix sqrt(W)*P
    this->createWeightsAndP(teamMember, delta, PsqrtW, w, _dimensions, _poly_order, true /*weight_p*/, NULL /*&V*/, _reconstruction_space, _polynomial_sampling_functional);

    // fill in RHS with Identity * sqrt(weights)
    Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,this_num_rows), [=] (const int i) {
        for(int j = 0; j < this_num_rows; ++j) {
            RHS(j,i) = (i==j) ? std::sqrt(w(i)) : 0;
        }
    });

    teamMember.team_barrier();
}


KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const ApplyStandardTargets&, const member_type& teamMember) const {

    /*
     *    Dimensions
     */

    const int target_index = teamMember.league_rank();

    const int max_num_rows = _sampling_multiplier*_max_num_neighbors;
    const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
    const int this_num_columns = _basis_multiplier*_NP;
    const int max_evaluation_sites = (static_cast<int>(_additional_evaluation_indices.extent(1)) > 1) 
                ? static_cast<int>(_additional_evaluation_indices.extent(1)) : 1;

    /*
     *    Data
     */

    scratch_matrix_right_type PsqrtW(_P.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(this_num_columns), max_num_rows, this_num_columns);
    // Coefficients for polynomial basis have overwritten _RHS
    scratch_matrix_right_type Coeffs(_RHS.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(max_num_rows), max_num_rows, max_num_rows);
    scratch_vector_type w(_w.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows), max_num_rows);

    scratch_vector_type t1(teamMember.team_scratch(_scratch_team_level_a), max_num_rows);
    scratch_vector_type t2(teamMember.team_scratch(_scratch_team_level_a), max_num_rows);
    scratch_matrix_right_type P_target_row(teamMember.team_scratch(_scratch_team_level_b), _total_alpha_values*max_evaluation_sites, this_num_columns);

    /*
     *    Apply Standard Target Evaluations to Polynomial Coefficients
     */

    // get evaluation of target functionals
    this->computeTargetFunctionals(teamMember, t1, t2, P_target_row);
    teamMember.team_barrier();

    this->applyTargetsToCoefficients(teamMember, t1, t2, Coeffs, PsqrtW, w, P_target_row, _NP); 
    teamMember.team_barrier();
}


KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const ComputeCoarseTangentPlane&, const member_type& teamMember) const {

    /*
     *    Dimensions
     */

    const int target_index = teamMember.league_rank();

    const int max_num_rows = _sampling_multiplier*_max_num_neighbors;
    const int manifold_NP = this->getNP(_curvature_poly_order, _dimensions-1);
    const int max_manifold_NP = (manifold_NP > _NP) ? manifold_NP : _NP;
    const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
    const int this_num_columns = _basis_multiplier*max_manifold_NP;

    /*
     *    Data
     */

    scratch_matrix_right_type PsqrtW(_P.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(this_num_columns), max_num_rows, this_num_columns);
    scratch_vector_type w(_w.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows), max_num_rows);
    scratch_matrix_right_type T(_T.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(_dimensions)*TO_GLOBAL(_dimensions), _dimensions, _dimensions);

    scratch_matrix_right_type PTP(teamMember.team_scratch(_scratch_team_level_b), _dimensions, _dimensions);

    // delta, used for each thread
    scratch_vector_type delta(teamMember.thread_scratch(_scratch_thread_level_b), max_manifold_NP*_basis_multiplier);

    /*
     *    Determine Coarse Approximation of Manifold Tangent Plane
     */

    // getting x y and z from which to derive a manifold
    this->createWeightsAndPForCurvature(teamMember, delta, PsqrtW, w, _dimensions, true /* only specific order */);

    // create PsqrtW^T*PsqrtW
    GMLS_LinearAlgebra::createM(teamMember, PTP, PsqrtW, _dimensions /* # of columns */, this->getNNeighbors(target_index));

    // create coarse approximation of tangent plane in first two rows of T, with normal direction in third column
    GMLS_LinearAlgebra::largestTwoEigenvectorsThreeByThreeSymmetric(teamMember, T, PTP, _dimensions, 
            const_cast<pool_type&>(_random_number_pool));

    teamMember.team_barrier();

}

KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const AssembleCurvaturePsqrtW&, const member_type& teamMember) const {

    /*
     *    Dimensions
     */

    const int target_index = teamMember.league_rank();

    const int max_num_rows = _sampling_multiplier*_max_num_neighbors;
    const int manifold_NP = this->getNP(_curvature_poly_order, _dimensions-1);
    const int max_manifold_NP = (manifold_NP > _NP) ? manifold_NP : _NP;
    const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
    const int this_num_neighbors = this->getNNeighbors(target_index);
    const int this_num_columns = _basis_multiplier*max_manifold_NP;

    /*
     *    Data
     */

    scratch_matrix_right_type CurvaturePsqrtW(_P.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(this_num_columns), max_num_rows, this_num_columns);
    scratch_matrix_right_type RHS(_RHS.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(max_num_rows), max_num_rows, max_num_rows);
    scratch_vector_type w(_w.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows), max_num_rows);
    scratch_matrix_right_type T(_T.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(_dimensions)*TO_GLOBAL(_dimensions), _dimensions, _dimensions);

    // delta, used for each thread
    scratch_vector_type delta(teamMember.thread_scratch(_scratch_thread_level_b), max_manifold_NP*_basis_multiplier);


    //
    //  RECONSTRUCT ON THE TANGENT PLANE USING LOCAL COORDINATES
    //

    // creates the matrix sqrt(W)*P
    this->createWeightsAndPForCurvature(teamMember, delta, CurvaturePsqrtW, w, _dimensions-1, false /* only specific order */, &T);
    teamMember.team_barrier();

    // CurvaturePsqrtW is sized according to max_num_rows x this_num_columns of which in this case
    // we are only using this_num_neighbors x manifold_NP
    
    // fill in RHS with Identity * sqrt(weights)
    Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,this_num_neighbors), [=] (const int i) {
        for(int j = 0; j < this_num_neighbors; ++j) {
            RHS(j,i) = (i==j) ? std::sqrt(w(i)) : 0;
        }
    });
    teamMember.team_barrier();

}

KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const GetAccurateTangentDirections&, const member_type& teamMember) const {

    /*
     *    Dimensions
     */

    const int target_index = teamMember.league_rank();

    const int max_num_rows = _sampling_multiplier*_max_num_neighbors;
    const int manifold_NP = this->getNP(_curvature_poly_order, _dimensions-1);
    const int max_manifold_NP = (manifold_NP > _NP) ? manifold_NP : _NP;
    const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
    const int this_num_neighbors = this->getNNeighbors(target_index);
    const int max_evaluation_sites = (static_cast<int>(_additional_evaluation_indices.extent(1)) > 1) 
                ? static_cast<int>(_additional_evaluation_indices.extent(1)) : 1;

    /*
     *    Data
     */

    scratch_matrix_right_type Q(_RHS.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(max_num_rows), max_num_rows, max_num_rows);
    scratch_matrix_right_type T(_T.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(_dimensions)*TO_GLOBAL(_dimensions), _dimensions, _dimensions);

    scratch_vector_type t1(teamMember.team_scratch(_scratch_team_level_b), _max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
    scratch_vector_type t2(teamMember.team_scratch(_scratch_team_level_b), _max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
    scratch_vector_type manifold_gradient(teamMember.team_scratch(_scratch_team_level_b), (_dimensions-1)*_max_num_neighbors);
    scratch_matrix_right_type P_target_row(teamMember.team_scratch(_scratch_team_level_b), _total_alpha_values*max_evaluation_sites, max_manifold_NP*_basis_multiplier);


    /*
     *    Manifold
     */


    //
    //  GET TARGET COEFFICIENTS RELATED TO GRADIENT TERMS
    //
    // reconstruct grad_xi1 and grad_xi2, not used for manifold_coeffs
    this->computeCurvatureFunctionals(teamMember, t1, t2, P_target_row, &T);
    teamMember.team_barrier();

    double grad_xi1 = 0, grad_xi2 = 0;
    for (int i=0; i<this->getNNeighbors(target_index); ++i) {
        for (int k=0; k<_dimensions-1; ++k) {
            double alpha_ij = 0;
            int offset = getTargetOffsetIndexDevice(0, 0, k, 0);
            Kokkos::parallel_reduce(Kokkos::TeamThreadRange(teamMember,
                    manifold_NP), [=] (const int l, double &talpha_ij) {
                Kokkos::single(Kokkos::PerThread(teamMember), [&] () {
                    talpha_ij += P_target_row(offset,l)*Q(i,l);
                });
            }, alpha_ij);
            Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
                manifold_gradient(i*(_dimensions-1) + k) = alpha_ij; // stored staggered, grad_xi1, grad_xi2, grad_xi1, grad_xi2, ....
            });
        }
        teamMember.team_barrier();

        XYZ rel_coord = getRelativeCoord(target_index, i, _dimensions, &T);
        double normal_coordinate = rel_coord[_dimensions-1];

        // apply coefficients to sample data
        grad_xi1 += manifold_gradient(i*(_dimensions-1)) * normal_coordinate;
        if (_dimensions>2) grad_xi2 += manifold_gradient(i*(_dimensions-1)+1) * normal_coordinate;
        teamMember.team_barrier();
    }

    // Constructs high order orthonormal tangent space T and inverse of metric tensor
    Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {

        double grad_xi[2] = {grad_xi1, grad_xi2};
        double T_row[3];

        // Construct T (high order approximation of orthonormal tangent vectors)
        for (int i=0; i<_dimensions-1; ++i) {
            for (int j=0; j<_dimensions; ++j) {
                T_row[j] = T(i,j);
            }
            // build
            for (int j=0; j<_dimensions; ++j) {
                T(i,j) = grad_xi[i]*T(_dimensions-1,j);
                T(i,j) += T_row[j];
            }
        }

        // calculate norm
        double norm = 0;
        for (int j=0; j<_dimensions; ++j) {
            norm += T(0,j)*T(0,j);
        }

        // normalize first vector
        norm = std::sqrt(norm);
        for (int j=0; j<_dimensions; ++j) {
            T(0,j) /= norm;
        }

        // orthonormalize next vector
        if (_dimensions-1 == 2) { // 2d manifold
            double dot_product = T(0,0)*T(1,0) + T(0,1)*T(1,1) + T(0,2)*T(1,2);
            for (int j=0; j<_dimensions; ++j) {
                T(1,j) -= dot_product*T(0,j);
            }
            // normalize second vector
            norm = 0;
            for (int j=0; j<_dimensions; ++j) {
                norm += T(1,j)*T(1,j);
            }
            norm = std::sqrt(norm);
            for (int j=0; j<_dimensions; ++j) {
                T(1,j) /= norm;
            }
        }

        // get normal vector to first two rows of T
        double norm_t_normal = 0;
        if (_dimensions>2) {
            T(_dimensions-1,0) = T(0,1)*T(1,2) - T(1,1)*T(0,2);
            norm_t_normal += T(_dimensions-1,0)*T(_dimensions-1,0);
            T(_dimensions-1,1) = -(T(0,0)*T(1,2) - T(1,0)*T(0,2));
            norm_t_normal += T(_dimensions-1,1)*T(_dimensions-1,1);
            T(_dimensions-1,2) = T(0,0)*T(1,1) - T(1,0)*T(0,1);
            norm_t_normal += T(_dimensions-1,2)*T(_dimensions-1,2);
        } else {
            T(_dimensions-1,0) = T(1,1) - T(0,1);
            norm_t_normal += T(_dimensions-1,0)*T(_dimensions-1,0);
            T(_dimensions-1,1) = T(0,0) - T(1,0);
            norm_t_normal += T(_dimensions-1,1)*T(_dimensions-1,1);
        }
        norm_t_normal = std::sqrt(norm_t_normal);
        for (int i=0; i<_dimensions-1; ++i) {
            T(_dimensions-1,i) /= norm_t_normal;
        }
    });
    teamMember.team_barrier();
}

KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const FixTangentDirectionOrdering&, const member_type& teamMember) const {

    /*
     *    Dimensions
     */

    const int target_index = teamMember.league_rank();

    /*
     *    Data
     */

    scratch_matrix_right_type T(_T.data() + target_index*_dimensions*_dimensions, _dimensions, _dimensions);
    scratch_vector_type N(_ref_N.data() + target_index*_dimensions, _dimensions);

    // take the dot product of the calculated normal in the tangent bundle with a given reference outward normal 
    // direction provided by the user. if the dot product is negative, flip the tangent vector ordering 
    // and flip the sign on the normal direction.
    Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
        compadre_kernel_assert_debug(_dimensions > 1 && "FixTangentDirectionOrder called on manifold with a dimension of 0.");
        double dot_product = 0;
        for (int i=0; i<_dimensions; ++i) {
            dot_product += T(_dimensions-1,i) * N(i);

        }
        if (dot_product < 0) {
            if (_dimensions==3) {
                for (int i=0; i<_dimensions; ++i) {
                    // swap tangent directions
                    double tmp = T(0,i);
                    T(0,i) = T(1,i);
                    T(1,i) = tmp;
                }
            }
            for (int i=0; i<_dimensions; ++i) {
                // flip the sign of the normal vector
                T(_dimensions-1,i) *= -1;

            }
        }
    });
    teamMember.team_barrier();
}

KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const ApplyCurvatureTargets&, const member_type& teamMember) const {

    /*
     *    Dimensions
     */

    const int target_index = teamMember.league_rank();

    const int max_num_rows = _sampling_multiplier*_max_num_neighbors;
    const int manifold_NP = this->getNP(_curvature_poly_order, _dimensions-1);
    const int max_manifold_NP = (manifold_NP > _NP) ? manifold_NP : _NP;
    const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
    const int this_num_neighbors = this->getNNeighbors(target_index);
    const int max_evaluation_sites = (static_cast<int>(_additional_evaluation_indices.extent(1)) > 1) 
                ? static_cast<int>(_additional_evaluation_indices.extent(1)) : 1;

    /*
     *    Data
     */

    scratch_matrix_right_type Q(_RHS.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(max_num_rows), max_num_rows, max_num_rows);
    scratch_matrix_right_type T(_T.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(_dimensions)*TO_GLOBAL(_dimensions), _dimensions, _dimensions);

    scratch_matrix_right_type G_inv(_manifold_metric_tensor_inverse.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL((_dimensions-1))*TO_GLOBAL((_dimensions-1)), _dimensions-1, _dimensions-1);
    scratch_vector_type manifold_coeffs(_manifold_curvature_coefficients.data() + target_index*manifold_NP, manifold_NP);
    scratch_vector_type manifold_gradient_coeffs(_manifold_curvature_gradient.data() + target_index*(_dimensions-1), (_dimensions-1));

    scratch_matrix_right_type G(teamMember.team_scratch(_scratch_team_level_b), _dimensions-1, _dimensions-1);
    scratch_vector_type t1(teamMember.team_scratch(_scratch_team_level_b), _max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
    scratch_vector_type t2(teamMember.team_scratch(_scratch_team_level_b), _max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
    scratch_vector_type manifold_gradient(teamMember.team_scratch(_scratch_team_level_b), (_dimensions-1)*_max_num_neighbors);
    scratch_matrix_right_type P_target_row(teamMember.team_scratch(_scratch_team_level_b), _total_alpha_values*max_evaluation_sites, max_manifold_NP*_basis_multiplier);

    /*
     *    Manifold
     */


    //
    //  GET TARGET COEFFICIENTS RELATED TO GRADIENT TERMS
    //
    // reconstruct grad_xi1 and grad_xi2, not used for manifold_coeffs
    this->computeCurvatureFunctionals(teamMember, t1, t2, P_target_row, &T);
    teamMember.team_barrier();

    Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
        for (int j=0; j<manifold_NP; ++j) { // set to zero
            manifold_coeffs(j) = 0;
        }
    });
    teamMember.team_barrier();

    double grad_xi1 = 0, grad_xi2 = 0;
    for (int i=0; i<this->getNNeighbors(target_index); ++i) {
        for (int k=0; k<_dimensions-1; ++k) {
            double alpha_ij = 0;
            int offset = getTargetOffsetIndexDevice(0, 0, k, 0);
            Kokkos::parallel_reduce(Kokkos::TeamThreadRange(teamMember,
                    manifold_NP), [=] (const int l, double &talpha_ij) {
                Kokkos::single(Kokkos::PerThread(teamMember), [&] () {
                    talpha_ij += P_target_row(offset,l)*Q(i,l);
                });
            }, alpha_ij);
            Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
                manifold_gradient(i*(_dimensions-1) + k) = alpha_ij; // stored staggered, grad_xi1, grad_xi2, grad_xi1, grad_xi2, ....
            });
        }
        teamMember.team_barrier();

        XYZ rel_coord = getRelativeCoord(target_index, i, _dimensions, &T);
        double normal_coordinate = rel_coord[_dimensions-1];

        // apply coefficients to sample data
        grad_xi1 += manifold_gradient(i*(_dimensions-1)) * normal_coordinate;
        if (_dimensions>2) grad_xi2 += manifold_gradient(i*(_dimensions-1)+1) * normal_coordinate;

        Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
            Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
                // coefficients without a target premultiplied
                for (int j=0; j<manifold_NP; ++j) {
                    manifold_coeffs(j) += Q(i,j) * normal_coordinate;
                }
            });
        });
        teamMember.team_barrier();
    }

    // Constructs high order orthonormal tangent space T and inverse of metric tensor
    Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {

        manifold_gradient_coeffs(0) = grad_xi1;
        if (_dimensions>2) manifold_gradient_coeffs(1) = grad_xi2;

        // need to get 2x2 matrix of metric tensor
        G(0,0) = 1 + grad_xi1*grad_xi1;

        if (_dimensions>2) {
            G(0,1) = grad_xi1*grad_xi2;
            G(1,0) = grad_xi2*grad_xi1;
            G(1,1) = 1 + grad_xi2*grad_xi2;
        }

        double G_determinant;
        if (_dimensions==2) {
            G_determinant = G(0,0);
            compadre_kernel_assert_debug((G_determinant!=0) && "Determinant is zero.");
            G_inv(0,0) = 1/G_determinant;
        } else {
            G_determinant = G(0,0)*G(1,1) - G(0,1)*G(1,0); //std::sqrt(G_inv(0,0)*G_inv(1,1) - G_inv(0,1)*G_inv(1,0));
            compadre_kernel_assert_debug((G_determinant!=0) && "Determinant is zero.");
            {
                // inverse of 2x2
                G_inv(0,0) = G(1,1)/G_determinant;
                G_inv(1,1) = G(0,0)/G_determinant;
                G_inv(0,1) = -G(0,1)/G_determinant;
                G_inv(1,0) = -G(1,0)/G_determinant;
            }
        }

    });
    teamMember.team_barrier();
    //
    //  END OF MANIFOLD METRIC CALCULATIONS
    //
}

KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const AssembleManifoldPsqrtW&, const member_type& teamMember) const {

    /*
     *    Dimensions
     */

    const int target_index = teamMember.league_rank();

    const int max_num_rows = _sampling_multiplier*_max_num_neighbors;
    const int manifold_NP = this->getNP(_curvature_poly_order, _dimensions-1);
    const int max_manifold_NP = (manifold_NP > _NP) ? manifold_NP : _NP;
    const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
    const int this_num_columns = _basis_multiplier*max_manifold_NP;
    const int this_num_neighbors = this->getNNeighbors(target_index);

    /*
     *    Data
     */

    scratch_matrix_right_type PsqrtW(_P.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(this_num_columns), max_num_rows, this_num_columns);
    scratch_matrix_right_type Q(_RHS.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(max_num_rows), max_num_rows, max_num_rows);
    scratch_vector_type w(_w.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows), max_num_rows);
    scratch_matrix_right_type T(_T.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(_dimensions)*TO_GLOBAL(_dimensions), _dimensions, _dimensions);

    // delta, used for each thread
    scratch_vector_type delta(teamMember.thread_scratch(_scratch_thread_level_b), max_manifold_NP*_basis_multiplier);


    /*
     *    Manifold
     */


    this->createWeightsAndP(teamMember, delta, PsqrtW, w, _dimensions-1, _poly_order, true /* weight with W*/, &T, _reconstruction_space, _polynomial_sampling_functional);
    teamMember.team_barrier();
    
    // fill in RHS with Identity * sqrt(weights)
    Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,this_num_rows), [=] (const int i) {
        for(int j = 0; j < this_num_rows; ++j) {
            Q(j,i) = (i==j) ? std::sqrt(w(i)) : 0;
        }
    });

    teamMember.team_barrier();
}

KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const ApplyManifoldTargets&, const member_type& teamMember) const {

    /*
     *    Dimensions
     */

    const int target_index = teamMember.league_rank();

    const int max_num_rows = _sampling_multiplier*_max_num_neighbors;
    const int manifold_NP = this->getNP(_curvature_poly_order, _dimensions-1);
    const int max_manifold_NP = (manifold_NP > _NP) ? manifold_NP : _NP;
    const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
    const int this_num_columns = _basis_multiplier*max_manifold_NP;
    const int max_evaluation_sites = (static_cast<int>(_additional_evaluation_indices.extent(1)) > 1) 
                ? static_cast<int>(_additional_evaluation_indices.extent(1)) : 1;

    /*
     *    Data
     */

    scratch_matrix_right_type PsqrtW(_P.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(this_num_columns), max_num_rows, this_num_columns);
    scratch_matrix_right_type Coeffs(_RHS.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(max_num_rows), max_num_rows, max_num_rows);
    scratch_vector_type w(_w.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows), max_num_rows);
    scratch_matrix_right_type T(_T.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(_dimensions)*TO_GLOBAL(_dimensions), _dimensions, _dimensions);

    scratch_matrix_right_type G_inv(_manifold_metric_tensor_inverse.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL((_dimensions-1))*TO_GLOBAL((_dimensions-1)), _dimensions-1, _dimensions-1);
    scratch_vector_type manifold_coeffs(_manifold_curvature_coefficients.data() + target_index*manifold_NP, manifold_NP);
    scratch_vector_type manifold_gradient_coeffs(_manifold_curvature_gradient.data() + target_index*(_dimensions-1), (_dimensions-1));

    scratch_vector_type t1(teamMember.team_scratch(_scratch_team_level_b), _max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
    scratch_vector_type t2(teamMember.team_scratch(_scratch_team_level_b), _max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
    scratch_matrix_right_type P_target_row(teamMember.team_scratch(_scratch_team_level_b), _total_alpha_values*max_evaluation_sites, max_manifold_NP*_basis_multiplier);

    /*
     *    Apply Standard Target Evaluations to Polynomial Coefficients
     */

    this->computeTargetFunctionalsOnManifold(teamMember, t1, t2, P_target_row, T, G_inv, manifold_coeffs, manifold_gradient_coeffs);
    teamMember.team_barrier();

    this->applyTargetsToCoefficients(teamMember, t1, t2, Coeffs, PsqrtW, w, P_target_row, _NP); 

    teamMember.team_barrier();
}


KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const ComputePrestencilWeights&, const member_type& teamMember) const {

    /*
     *    Dimensions
     */

    const int target_index = teamMember.league_rank();

    const int max_num_rows = _sampling_multiplier*_max_num_neighbors;
    const int manifold_NP = this->getNP(_curvature_poly_order, _dimensions-1);
    const int max_manifold_NP = (manifold_NP > _NP) ? manifold_NP : _NP;
    const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
    const int max_evaluation_sites = (static_cast<int>(_additional_evaluation_indices.extent(1)) > 1) 
                ? static_cast<int>(_additional_evaluation_indices.extent(1)) : 1;

    /*
     *    Data
     */


    scratch_vector_type t1(teamMember.team_scratch(_scratch_team_level_b), _max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
    scratch_vector_type t2(teamMember.team_scratch(_scratch_team_level_b), _max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));


    // holds polynomial coefficients for curvature reconstruction
    scratch_matrix_right_type Q(_RHS.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(max_num_rows)*TO_GLOBAL(max_num_rows), max_num_rows, max_num_rows);

    scratch_matrix_right_type T(_T.data() 
            + TO_GLOBAL(target_index)*TO_GLOBAL(_dimensions)*TO_GLOBAL(_dimensions), _dimensions, _dimensions);

    scratch_vector_type manifold_gradient(teamMember.team_scratch(_scratch_team_level_b), (_dimensions-1)*_max_num_neighbors);
    scratch_matrix_right_type P_target_row(teamMember.team_scratch(_scratch_team_level_b), _total_alpha_values*max_evaluation_sites, max_manifold_NP*_basis_multiplier);

    /*
     *    Prestencil Weight Calculations
     */

    if (_data_sampling_functional == StaggeredEdgeAnalyticGradientIntegralSample) {
        Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
            Kokkos::single(Kokkos::PerThread(teamMember), [&] () {
                _prestencil_weights(0,0,0,0,0) = -1;
                _prestencil_weights(1,0,0,0,0) = 1;
            });
        });
    } else if (_data_sampling_functional == ManifoldVectorPointSample) {
        Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
            Kokkos::single(Kokkos::PerThread(teamMember), [&] () {
                for (int j=0; j<_dimensions; ++j) {
                    for (int k=0; k<_dimensions-1; ++k) {
                        _prestencil_weights(0,target_index,0,k,j) =  T(k,j);
                    }
                }
            });
        });
    } else if (_data_sampling_functional == StaggeredEdgeIntegralSample) {
        compadre_kernel_assert_debug(_dense_solver_type==DenseSolverType::MANIFOLD && "StaggeredEdgeIntegralSample prestencil weight only written for manifolds.");
        const int neighbor_offset = _neighbor_lists.extent(1)-1;
        Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,this->getNNeighbors(target_index)), [=] (const int m) {
            Kokkos::single(Kokkos::PerThread(teamMember), [&] () {
                for (int quadrature = 0; quadrature<_number_of_quadrature_points; ++quadrature) {
                    XYZ tangent_quadrature_coord_2d;
                    for (int j=0; j<_dimensions-1; ++j) {
                        tangent_quadrature_coord_2d[j] = getTargetCoordinate(target_index, j, &T);
                        tangent_quadrature_coord_2d[j] -= getNeighborCoordinate(target_index, m, j, &T);
                    }
                    double tangent_vector[3];
                    tangent_vector[0] = tangent_quadrature_coord_2d[0]*T(0,0) + tangent_quadrature_coord_2d[1]*T(1,0);
                    tangent_vector[1] = tangent_quadrature_coord_2d[0]*T(0,1) + tangent_quadrature_coord_2d[1]*T(1,1);
                    tangent_vector[2] = tangent_quadrature_coord_2d[0]*T(0,2) + tangent_quadrature_coord_2d[1]*T(1,2);

                    for (int j=0; j<_dimensions; ++j) {
                        _prestencil_weights(0,target_index,m,0,j) +=  (1-_parameterized_quadrature_sites[quadrature])*tangent_vector[j]*_quadrature_weights[quadrature];
                        _prestencil_weights(1,target_index,m,0,j) +=  _parameterized_quadrature_sites[quadrature]*tangent_vector[j]*_quadrature_weights[quadrature];
                    }
                }
            });
        });
    } else if (_data_sampling_functional == VaryingManifoldVectorPointSample) {

        scratch_vector_type delta(teamMember.thread_scratch(_scratch_thread_level_b), manifold_NP);

        Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,this->getNNeighbors(target_index)), [&] (const int m) {
            
            Kokkos::single(Kokkos::PerThread(teamMember), [&] () {
                this->calcGradientPij(delta.data(), target_index, m, 0 /*alpha*/, 0 /*partial_direction*/, _dimensions-1, _curvature_poly_order, false /*specific order only*/, &T, ReconstructionSpace::ScalarTaylorPolynomial, PointSample);
            });
            // reconstructs gradient at local neighbor index m
            double grad_xi1 = 0, grad_xi2 = 0;
            Kokkos::parallel_reduce(Kokkos::ThreadVectorRange(teamMember,this->getNNeighbors(target_index)), [=] (const int i, double &t_grad_xi1) {
                double alpha_ij = 0;
                for (int l=0; l<manifold_NP; ++l) {
                    alpha_ij += delta(l)*Q(i,l);
                }
                XYZ rel_coord = getRelativeCoord(target_index, i, _dimensions, &T);
                double normal_coordinate = rel_coord[_dimensions-1];

                // apply coefficients to sample data
                t_grad_xi1 += alpha_ij * normal_coordinate;
            }, grad_xi1);
            t1(m) = grad_xi1;

            Kokkos::single(Kokkos::PerThread(teamMember), [&] () {
                this->calcGradientPij(delta.data(), target_index, m, 0 /*alpha*/, 1 /*partial_direction*/, _dimensions-1, _curvature_poly_order, false /*specific order only*/, &T, ReconstructionSpace::ScalarTaylorPolynomial, PointSample);
            });
            Kokkos::parallel_reduce(Kokkos::ThreadVectorRange(teamMember,this->getNNeighbors(target_index)), [=] (const int i, double &t_grad_xi2) {
                double alpha_ij = 0;
                for (int l=0; l<manifold_NP; ++l) {
                    alpha_ij += delta(l)*Q(i,l);
                }
                XYZ rel_coord = getRelativeCoord(target_index, i, _dimensions, &T);
                double normal_coordinate = rel_coord[_dimensions-1];

                // apply coefficients to sample data
                if (_dimensions>2) t_grad_xi2 += alpha_ij * normal_coordinate;
            }, grad_xi2);
            t2(m) = grad_xi2;
        });

        teamMember.team_barrier();

        scratch_matrix_right_type tangent(teamMember.thread_scratch(_scratch_thread_level_b), _dimensions-1, _dimensions);

        Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,this->getNNeighbors(target_index)), [=] (const int m) {
            // constructs tangent vector at neighbor site
            Kokkos::single(Kokkos::PerThread(teamMember), [&] () {
                for (int j=0; j<_dimensions; ++j) {
                    tangent(0,j) = t1(m)*T(_dimensions-1,j) + T(0,j);
                    tangent(1,j) = t2(m)*T(_dimensions-1,j) + T(1,j);
                }

                // calculate norm
                double norm = 0;
                for (int j=0; j<_dimensions; ++j) {
                    norm += tangent(0,j)*tangent(0,j);
                }

                // normalize first vector
                norm = std::sqrt(norm);
                for (int j=0; j<_dimensions; ++j) {
                    tangent(0,j) /= norm;
                }

                // orthonormalize next vector
                if (_dimensions-1 == 2) { // 2d manifold
                    double dot_product = tangent(0,0)*tangent(1,0) + tangent(0,1)*tangent(1,1) + tangent(0,2)*tangent(1,2);
                    for (int j=0; j<_dimensions; ++j) {
                        tangent(1,j) -= dot_product*tangent(0,j);
                    }
                    // normalize second vector
                    norm = 0;
                    for (int j=0; j<_dimensions; ++j) {
                        norm += tangent(1,j)*tangent(1,j);
                    }
                    norm = std::sqrt(norm);
                    for (int j=0; j<_dimensions; ++j) {
                        tangent(1,j) /= norm;
                    }
                }

                // stores matrix of tangent and normal directions as a prestencil weight
                for (int j=0; j<_dimensions; ++j) {
                    for (int k=0; k<_dimensions-1; ++k) {
                        _prestencil_weights(0,target_index,m,k,j) =  tangent(k,j);
                    }
                }
            });
        });
    }
    teamMember.team_barrier();
}

}; // Compadre

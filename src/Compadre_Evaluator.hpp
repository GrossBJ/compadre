#ifndef _COMPADRE_EVALUATOR_HPP_
#define _COMPADRE_EVALUATOR_HPP_

#include "Compadre_Typedefs.hpp"
#include "Compadre_GMLS.hpp"

namespace Compadre {

//! Creates 1D subviews of data from a 2D view, generally constructed with CreateNDSliceOnDeviceView
template<typename T, typename T2, typename T3=void>
struct SubviewND { 
    
    T _data_in;
    T2 _data_original_view;
    bool _scalar_as_vector_if_needed;

    SubviewND(T data_in, T2 data_original_view, bool scalar_as_vector_if_needed) {
        _data_in = data_in;
        _data_original_view = data_original_view;
        _scalar_as_vector_if_needed = scalar_as_vector_if_needed; 
    }

    auto get1DView(const int column_num) -> decltype(Kokkos::subview(_data_in, Kokkos::ALL, column_num)) {
        if (!_scalar_as_vector_if_needed) {
            compadre_assert_debug((column_num<_data_in.extent(1)) && "Subview asked for column > second dimension of input data.");
        }
        if (column_num<_data_in.extent(1))
            return Kokkos::subview(_data_in, Kokkos::ALL, column_num);
        else // scalar treated as a vector (being reused for each component of the vector input that was expected)
            return Kokkos::subview(_data_in, Kokkos::ALL, 0);
    }

    auto get2DView(const int column_num, const int block_size) -> decltype(Kokkos::subview(_data_in, Kokkos::ALL, 
                Kokkos::make_pair(column_num*block_size, (column_num+1)*block_size))) {
        if (!_scalar_as_vector_if_needed) {
            compadre_assert_debug((((column_num+1)*block_size-1)<_data_in.extent(1)) && "Subview asked for column > second dimension of input data.");
        }
        if (((column_num+1)*block_size-1)<_data_in.extent(1))
            return Kokkos::subview(_data_in, Kokkos::ALL, Kokkos::make_pair(column_num*block_size, (column_num+1)*block_size));
        else
            compadre_assert_debug(((block_size-1)<_data_in.extent(1)) && "Subview asked for column > second dimension of input data.");
            return Kokkos::subview(_data_in, Kokkos::ALL, Kokkos::make_pair(0,block_size));
    }

    T2 copyToAndReturnOriginalView() {
        Kokkos::deep_copy(_data_original_view, _data_in);
        Kokkos::fence();
        return _data_original_view;
    }

};

//! Creates 1D subviews of data from a 1D view, generally constructed with CreateNDSliceOnDeviceView
template<typename T, typename T2>
struct SubviewND<T, T2, enable_if_t<(T::rank<2)> >
{ 

    T _data_in;
    T2 _data_original_view;
    bool _scalar_as_vector_if_needed;

    SubviewND(T data_in, T2 data_original_view, bool scalar_as_vector_if_needed) {
        _data_in = data_in;
        _data_original_view = data_original_view;
        _scalar_as_vector_if_needed = scalar_as_vector_if_needed; 
    }

    auto get1DView(const int column_num) -> decltype(Kokkos::subview(_data_in, Kokkos::ALL)) {
        // TODO: There is a valid use case for violating this assert, so in the future we may want
        // to add other logic to the evaluator function calling this so that it knows to do nothing with
        // this data.
        if (!_scalar_as_vector_if_needed) {
            compadre_assert_debug((column_num==0) && "Subview asked for column column_num!=0, but _data_in is rank 1.");
        }
        return Kokkos::subview(_data_in, Kokkos::ALL);
    }

    auto get2DView(const int column_num, const int block_size) -> decltype(Kokkos::subview(_data_in, Kokkos::ALL)) {
        compadre_assert_release((block_size==1) && "2D subview requested not compatible with one column.");
        return Kokkos::subview(_data_in, Kokkos::ALL);
    }

    T2 copyToAndReturnOriginalView() {
        Kokkos::deep_copy(_data_original_view, _data_in);
        Kokkos::fence();
        return _data_original_view;
    }

};

//! Copies data_in to the device, and then allows for access to 1D columns of data on device.
//! Handles either 2D or 1D views as input, and they can be on the host or the device.
template <typename T>
auto CreateNDSliceOnDeviceView(T sampling_input_data_host_or_device, bool scalar_as_vector_if_needed) -> SubviewND<decltype(Kokkos::create_mirror_view(
                    device_execution_space::memory_space(), sampling_input_data_host_or_device)), T> {

    // makes view on the device (does nothing if already on the device)
    auto sampling_input_data_device = Kokkos::create_mirror_view(
        device_execution_space::memory_space(), sampling_input_data_host_or_device);
    Kokkos::deep_copy(sampling_input_data_device, sampling_input_data_host_or_device);
    Kokkos::fence();

    return SubviewND<decltype(sampling_input_data_device),T>(sampling_input_data_device, 
            sampling_input_data_host_or_device, scalar_as_vector_if_needed);
}

//! \brief Lightweight Evaluator Helper
//! This class is a lightweight wrapper for extracting and applying all relevant data from a GMLS class
//! in order to transform data into a form that can be acted on by the GMLS operator, apply the action of
//! the GMLS operator, and then transform data again (only if on a manifold)
class Evaluator {

private:

    GMLS *_gmls;


public:

    Evaluator(GMLS *gmls) : _gmls(gmls) {
        Kokkos::fence();
    };

    ~Evaluator() {};

    //! Dot product of alphas with sampling data, FOR A SINGLE target_index,  where sampling data is in a 1D/2D Kokkos View
    //! 
    //! This function is to be used when the alpha values have already been calculated and stored for use 
    //!
    //! Only supports one output component / input component at a time. The user will need to loop over the output 
    //! components in order to fill a vector target or matrix target.
    //! 
    //! Assumptions on input data:
    //! \param sampling_input_data      [in] - 1D/2D Kokkos View (no restriction on memory space)
    //! \param column_of_input          [in] - Column of sampling_input_data to use for this input component
    //! \param lro                      [in] - Target operation from the TargetOperation enum
    //! \param target_index             [in] - Target # user wants to reconstruct target functional at, corresponds to row number of neighbor_lists
    //! \param output_component_axis_1  [in] - Row for a rank 2 tensor or rank 1 tensor, 0 for a scalar output
    //! \param output_component_axis_2  [in] - Columns for a rank 2 tensor, 0 for rank less than 2 output tensor
    //! \param input_component_axis_1   [in] - Row for a rank 2 tensor or rank 1 tensor, 0 for a scalar input
    //! \param input_component_axis_2   [in] - Columns for a rank 2 tensor, 0 for rank less than 2 input tensor
    //! \param scalar_as_vector_if_needed [in] - If a 1D view is given, where a 2D view is expected (scalar values given where a vector was expected), then the scalar will be repeated for as many components as the vector has
    template <typename view_type_data>
    double applyAlphasToDataSingleComponentSingleTargetSite(view_type_data sampling_input_data, const int column_of_input, TargetOperation lro, const int target_index, const int evaluation_site_local_index, const int output_component_axis_1, const int output_component_axis_2, const int input_component_axis_1, const int input_component_axis_2, bool scalar_as_vector_if_needed = true) const {

        double value = 0;

        const int alpha_input_output_component_index = _gmls->getAlphaColumnOffset(lro, output_component_axis_1, 
                output_component_axis_2, input_component_axis_1, input_component_axis_2, evaluation_site_local_index);

        auto sampling_subview_maker = CreateNDSliceOnDeviceView(sampling_input_data, scalar_as_vector_if_needed);

        
        // gather needed information for evaluation
        auto neighbor_lists = _gmls->getNeighborLists();
        auto alphas         = _gmls->getAlphas();
        auto neighbor_lists_lengths = _gmls->getNeighborListsLengths();
        auto sampling_data_device = sampling_subview_maker.get1DView(column_of_input);
        
        // loop through neighbor list for this target_index
        // grabbing data from that entry of data
        Kokkos::parallel_reduce("applyAlphasToData::Device", 
                Kokkos::RangePolicy<device_execution_space>(0,neighbor_lists_lengths(target_index)), 
                KOKKOS_LAMBDA(const int i, double& t_value) {

            t_value += sampling_data_device(neighbor_lists(target_index, i+1))
                *alphas(target_index, alpha_input_output_component_index, i);

        }, value );
        Kokkos::fence();

        return value;
    }

    //! Dot product of alphas with sampling data where sampling data is in a 1D/2D Kokkos View and output view is also 
    //! a 1D/2D Kokkos View, however THE SAMPLING DATA and OUTPUT VIEW MUST BE ON THE DEVICE!
    //! 
    //! This function is to be used when the alpha values have already been calculated and stored for use.
    //!
    //! Only supports one output component / input component at a time. The user will need to loop over the output 
    //! components in order to fill a vector target or matrix target.
    //! 
    //! Assumptions on input data:
    //! \param output_data_single_column       [out] - 1D Kokkos View (memory space must be device_execution_space::memory_space())
    //! \param sampling_data_single_column      [in] - 1D Kokkos View (memory space must match output_data_single_column)
    //! \param lro                              [in] - Target operation from the TargetOperation enum
    //! \param sro                              [in] - Sampling functional from the SamplingFunctional enum
    //! \param evaluation_site_location         [in] - local column index of site from additional evaluation sites list or 0 for the target site
    //! \param output_component_axis_1          [in] - Row for a rank 2 tensor or rank 1 tensor, 0 for a scalar output
    //! \param output_component_axis_2          [in] - Columns for a rank 2 tensor, 0 for rank less than 2 output tensor
    //! \param input_component_axis_1           [in] - Row for a rank 2 tensor or rank 1 tensor, 0 for a scalar input
    //! \param input_component_axis_2           [in] - Columns for a rank 2 tensor, 0 for rank less than 2 input tensor
    //! \param pre_transform_local_index        [in] - For manifold problems, this is the local coordinate direction that sampling data may need to be transformed to before the application of GMLS
    //! \param pre_transform_global_index       [in] - For manifold problems, this is the global coordinate direction that sampling data can be represented in
    //! \param post_transform_local_index       [in] - For manifold problems, this is the local coordinate direction that vector output target functionals from GMLS will output into
    //! \param post_transform_global_index      [in] - For manifold problems, this is the global coordinate direction that the target functional output from GMLS will be transformed into
    //! \param transform_output_ambient         [in] - Whether or not a 1D output from GMLS is on the manifold and needs to be mapped to ambient space
    //! \param vary_on_target                   [in] - Whether the sampling functional has a tensor to act on sampling data that varies with each target site
    //! \param vary_on_neighbor                 [in] - Whether the sampling functional has a tensor to act on sampling data that varies with each neighbor site in addition to varying wit each target site
    template <typename view_type_data_out, typename view_type_data_in>
    void applyAlphasToDataSingleComponentAllTargetSitesWithPreAndPostTransform(view_type_data_out output_data_single_column, view_type_data_in sampling_data_single_column, TargetOperation lro, const SamplingFunctional sro, const int evaluation_site_local_index, const int output_component_axis_1, const int output_component_axis_2, const int input_component_axis_1, const int input_component_axis_2, const int pre_transform_local_index = -1, const int pre_transform_global_index = -1, const int post_transform_local_index = -1, const int post_transform_global_index = -1, bool vary_on_target = false, bool vary_on_neighbor = false) const {

        const int alpha_input_output_component_index = _gmls->getAlphaColumnOffset(lro, output_component_axis_1, 
                output_component_axis_2, input_component_axis_1, input_component_axis_2, evaluation_site_local_index);
        const int alpha_input_output_component_index2 = alpha_input_output_component_index;

        auto global_dimensions = _gmls->getGlobalDimensions();

        // gather needed information for evaluation
        auto neighbor_lists = _gmls->getNeighborLists();
        auto alphas         = _gmls->getAlphas();
        auto prestencil_weights = _gmls->getPrestencilWeights();

        const int num_targets = neighbor_lists.extent(0); // one row for each target

        // make sure input and output views have same memory space
        compadre_assert_debug((std::is_same<typename view_type_data_out::memory_space, typename view_type_data_in::memory_space>::value) && 
                "output_data_single_column view and input_data_single_column view have difference memory spaces.");

        bool weight_with_pre_T = (pre_transform_local_index>=0 && pre_transform_global_index>=0) ? true : false;
        bool target_plus_neighbor_staggered_schema = sro.use_target_site_weights;

        // loops over target indices
        Kokkos::parallel_for(team_policy(num_targets, Kokkos::AUTO),
                KOKKOS_LAMBDA(const member_type& teamMember) {

            const int target_index = teamMember.league_rank();
            teamMember.team_barrier();


            const double previous_value = output_data_single_column(target_index);

            // loops over neighbors of target_index
            double gmls_value = 0;
            Kokkos::parallel_reduce(Kokkos::TeamThreadRange(teamMember, neighbor_lists(target_index,0)), [=](const int i, double& t_value) {
                const double neighbor_varying_pre_T =  (weight_with_pre_T && vary_on_neighbor) ?
                    prestencil_weights(0, target_index, i, pre_transform_local_index, pre_transform_global_index)
                    : 1.0;

                t_value += neighbor_varying_pre_T * sampling_data_single_column(neighbor_lists(target_index, i+1))
                    *alphas(target_index, alpha_input_output_component_index, i);

            }, gmls_value );

            // data contract for sampling functional
            double pre_T = 1.0;
            if (weight_with_pre_T) {
                if (!vary_on_neighbor && vary_on_target) {
                    pre_T = prestencil_weights(0, target_index, 0, pre_transform_local_index, 
                            pre_transform_global_index); 
                } else if (!vary_on_target) { // doesn't vary on target or neighbor
                    pre_T = prestencil_weights(0, 0, 0, pre_transform_local_index, 
                            pre_transform_global_index); 
                }
            }

            double staggered_value_from_targets = 0;
            double pre_T_staggered = 1.0;
            // loops over target_index for each neighbor for staggered approaches
            if (target_plus_neighbor_staggered_schema) {
                Kokkos::parallel_reduce(Kokkos::TeamThreadRange(teamMember, neighbor_lists(target_index,0)), [=](const int i, double& t_value) {
                    const double neighbor_varying_pre_T_staggered =  (weight_with_pre_T && vary_on_neighbor) ?
                        prestencil_weights(1, target_index, i, pre_transform_local_index, pre_transform_global_index)
                        : 1.0;

                    t_value += neighbor_varying_pre_T_staggered * sampling_data_single_column(neighbor_lists(target_index, 1))
                        *alphas(target_index, alpha_input_output_component_index2, i);

                }, staggered_value_from_targets );

                // for staggered approaches that transform source data for the target and neighbors
                if (weight_with_pre_T) {
                    if (!vary_on_neighbor && vary_on_target) {
                        pre_T_staggered = prestencil_weights(1, target_index, 0, pre_transform_local_index, 
                                pre_transform_global_index); 
                    } else if (!vary_on_target) { // doesn't vary on target or neighbor
                        pre_T_staggered = prestencil_weights(1, 0, 0, pre_transform_local_index, 
                                pre_transform_global_index); 
                    }
                }
            }

            double added_value = pre_T*gmls_value + pre_T_staggered*staggered_value_from_targets;
            Kokkos::single(Kokkos::PerTeam(teamMember), [=] () {
                output_data_single_column(target_index) = previous_value + added_value;
            });
        });
        Kokkos::fence();
    }

    //! Postprocessing for manifolds. Maps local chart vector solutions to ambient space.
    //! THE SAMPLING DATA and OUTPUT VIEW MUST BE ON THE DEVICE!
    //! 
    //! Only supports one output component / input component at a time. The user will need to loop over the output 
    //! components in order to transform a vector target.
    //! 
    //! Assumptions on input data:
    //! \param output_data_single_column       [out] - 1D Kokkos View (memory space must be device_execution_space::memory_space())
    //! \param sampling_data_single_column      [in] - 1D Kokkos View (memory space must match output_data_single_column)
    //! \param local_dim_index                  [in] - For manifold problems, this is the local coordinate direction that sampling data may need to be transformed to before the application of GMLS
    //! \param global_dim_index                 [in] - For manifold problems, this is the global coordinate direction that sampling data can be represented in
    template <typename view_type_data_out, typename view_type_data_in>
    void applyLocalChartToAmbientSpaceTransform(view_type_data_out output_data_single_column, view_type_data_in sampling_data_single_column, const int local_dim_index, const int global_dim_index) const {

        // Does T transpose times a vector
        auto global_dimensions = _gmls->getGlobalDimensions();

        // gather needed information for evaluation
        auto neighbor_lists = _gmls->getNeighborLists();
        const int num_targets = neighbor_lists.extent(0); // one row for each target

        auto tangent_directions = _gmls->getTangentDirections();

        // make sure input and output views have same memory space
        compadre_assert_debug((std::is_same<typename view_type_data_out::memory_space, typename view_type_data_in::memory_space>::value) && 
                "output_data_single_column view and input_data_single_column view have difference memory spaces.");

        // loops over target indices
        Kokkos::parallel_for(team_policy(num_targets, Kokkos::AUTO),
                KOKKOS_LAMBDA(const member_type& teamMember) {

            const int target_index = teamMember.league_rank();

            scratch_matrix_right_type T
                    (tangent_directions.data() + TO_GLOBAL(target_index)*TO_GLOBAL(global_dimensions)*TO_GLOBAL(global_dimensions), 
                     global_dimensions, global_dimensions);
            teamMember.team_barrier();


            const double previous_value = output_data_single_column(target_index);

            double added_value = T(local_dim_index, global_dim_index)*sampling_data_single_column(target_index);
            Kokkos::single(Kokkos::PerTeam(teamMember), [=] () {
                output_data_single_column(target_index) = previous_value + added_value;
            });
        });
        Kokkos::fence();
    }

    //! Transformation of data under GMLS
    //! 
    //! This function is the go to function to be used when the alpha values have already been calculated and stored for use. The sampling functional provided instructs how a data transformation tensor is to be used on source data before it is provided to the GMLS operator. Once the sampling functional (if applicable) and the GMLS operator have been applied, this function also handles mapping the local vector back to the ambient space if working on a manifold problem and a target functional who has rank 1 output.
    //!
    //! Produces a Kokkos View as output with a Kokkos memory_space provided as a template tag by the caller. 
    //! The data type (double* or double**) must also be specified as a template type if one wish to get a 1D 
    //! Kokkos View back that can be indexed into with only one ordinal.
    //! 
    //! Assumptions on input data:
    //! \param sampling_data              [in] - 1D or 2D Kokkos View that has the layout #targets * columns of data. Memory space for data can be host or device. 
    //! \param lro                        [in] - Target operation from the TargetOperation enum
    //! \param sro_in                     [in] - Sampling functional from the SamplingFunctional enum
    //! \param scalar_as_vector_if_needed [in] - If a 1D view is given, where a 2D view is expected (scalar values given where a vector was expected), then the scalar will be repeated for as many components as the vector has
    //! \param evaluation_site_local_index [in] - 0 corresponds to evaluating at the target site itself, while a number larger than 0 indicates evaluation at a site other than the target, and specified by calling setAdditionalEvaluationSitesData on the GMLS class
    template <typename output_data_type = double**, typename output_memory_space, typename view_type_input_data, typename output_array_layout = typename view_type_input_data::array_layout>
    Kokkos::View<output_data_type, output_array_layout, output_memory_space>  // shares layout of input by default
            applyAlphasToDataAllComponentsAllTargetSites(view_type_input_data sampling_data, TargetOperation lro, const SamplingFunctional sro_in = PointSample, bool scalar_as_vector_if_needed = true, const int evaluation_site_local_index = 0) const {


        // output can be device or host
        // input can be device or host
        // move everything to device and calculate there, then move back to host if necessary

        typedef Kokkos::View<output_data_type, output_array_layout, output_memory_space> output_view_type;

        auto problem_type = _gmls->getProblemType();
        auto global_dimensions = _gmls->getGlobalDimensions();
        auto output_dimension_of_operator = _gmls->getOutputDimensionOfOperation(lro);
        auto input_dimension_of_operator = _gmls->getInputDimensionOfOperation(lro);

        // gather needed information for evaluation
        auto neighbor_lists = _gmls->getNeighborLists();

        // determines the number of columns needed for output after action of the target functional
        int output_dimensions = output_dimension_of_operator;

        // special case for VectorPointSample, because if it is on a manifold it includes data transform to local charts
        auto sro = (problem_type==MANIFOLD && sro_in==VectorPointSample) ? ManifoldVectorPointSample : sro_in;

        // create view on whatever memory space the user specified with their template argument when calling this function
        output_view_type target_output = createView<output_view_type>("output of target operation", 
                neighbor_lists.extent(0) /* number of targets */, output_dimensions);

        // make sure input and output columns make sense under the target operation
        compadre_assert_debug(((output_dimension_of_operator==1 && output_view_type::rank==1) || output_view_type::rank!=1) && 
                "Output view is requested as rank 1, but the target requires a rank larger than 1. Try double** as template argument.");

        // we need to specialize a template on the rank of the output view type and the input view type
        auto sampling_subview_maker = CreateNDSliceOnDeviceView(sampling_data, scalar_as_vector_if_needed);
        auto output_subview_maker = CreateNDSliceOnDeviceView(target_output, false); // output will always be the correct dimension

        // figure out preprocessing and postprocessing
        auto prestencil_weights = _gmls->getPrestencilWeights();

        // all loop logic based on transforming data under a sampling functional
        // into something that is valid input for GMLS
        bool vary_on_target, vary_on_neighbor;
        auto sro_style = sro.transform_type;
        bool loop_global_dimensions = sro.input_rank>0 && sro_style!=Identity; 

        compadre_assert_release(_gmls->getDataSamplingFunctional()==sro || sro_style==Identity 
                && "SamplingFunctional requested for Evaluator does not match GMLS data sampling functional or is not of type 'Identity'.");

        if (sro.transform_type == Identity || sro.transform_type == SameForAll) {
            vary_on_target = false;
            vary_on_neighbor = false;
        } else if (sro.transform_type == DifferentEachTarget) {
            vary_on_target = true;
            vary_on_neighbor = false;
        } else if (sro.transform_type == DifferentEachNeighbor) {
            vary_on_target = true;
            vary_on_neighbor = true;
        }


        // only written for up to rank 1 to rank 1 (in / out)
        // loop over components of output of the target operation
        for (int i=0; i<output_dimension_of_operator; ++i) {
            const int output_component_axis_1 = i;
            const int output_component_axis_2 = 0;
            // loop over components of input of the target operation
            for (int j=0; j<input_dimension_of_operator; ++j) {
                const int input_component_axis_1 = j;
                const int input_component_axis_2 = 0;

                if (loop_global_dimensions) {
                    for (int k=0; k<global_dimensions; ++k) { // loop for handling sampling functional
                        this->applyAlphasToDataSingleComponentAllTargetSitesWithPreAndPostTransform(
                                output_subview_maker.get1DView(i), sampling_subview_maker.get1DView(k), lro, sro, 
                                evaluation_site_local_index, output_component_axis_1, output_component_axis_2, input_component_axis_1, 
                                input_component_axis_2, j, k, -1, -1,
                                vary_on_target, vary_on_neighbor);
                    }
                } else if (sro_style != Identity) {
                    this->applyAlphasToDataSingleComponentAllTargetSitesWithPreAndPostTransform(
                            output_subview_maker.get1DView(i), sampling_subview_maker.get1DView(j), lro, sro, 
                            evaluation_site_local_index, output_component_axis_1, output_component_axis_2, input_component_axis_1, 
                            input_component_axis_2, 0, 0, -1, -1,
                            vary_on_target, vary_on_neighbor);
                } else { // standard
                    this->applyAlphasToDataSingleComponentAllTargetSitesWithPreAndPostTransform(
                            output_subview_maker.get1DView(i), sampling_subview_maker.get1DView(j), lro, sro, 
                            evaluation_site_local_index, output_component_axis_1, output_component_axis_2, input_component_axis_1, 
                            input_component_axis_2);
                }
            }
        }

        bool transform_gmls_output_to_ambient = (problem_type==MANIFOLD && TargetOutputTensorRank[(int)lro]==1);
        if (transform_gmls_output_to_ambient) {
            Kokkos::fence();

            // create view on whatever memory space the user specified with their template argument when calling this function
            output_view_type ambient_target_output = createView<output_view_type>(
                    "output of transform to ambient space", neighbor_lists.extent(0) /* number of targets */,
                    global_dimensions);
            auto transformed_output_subview_maker = CreateNDSliceOnDeviceView(ambient_target_output, false); 
            // output will always be the correct dimension
            for (int i=0; i<global_dimensions; ++i) {
                for (int j=0; j<output_dimension_of_operator; ++j) {
                    this->applyLocalChartToAmbientSpaceTransform(
                            transformed_output_subview_maker.get1DView(i), output_subview_maker.get1DView(j), j, i);
                }
            }
            // copy back to whatever memory space the user requester through templating from the device
            Kokkos::deep_copy(ambient_target_output, transformed_output_subview_maker.copyToAndReturnOriginalView());
            return ambient_target_output;
        }

        // copy back to whatever memory space the user requester through templating from the device
        Kokkos::deep_copy(target_output, output_subview_maker.copyToAndReturnOriginalView());
        return target_output;
    }


    //! Dot product of data with full polynomial coefficient basis where sampling data is in a 1D/2D Kokkos View and output view is also 
    //! a 1D/2D Kokkos View, however THE SAMPLING DATA and OUTPUT VIEW MUST BE ON THE DEVICE!
    //! 
    //! This function is to be used when the polynomial coefficient basis has already been calculated and stored for use.
    //!
    //! Only supports one output component / input component at a time. The user will need to loop over the output 
    //! components in order to fill a vector target or matrix target.
    //! 
    //! Assumptions on input data:
    //! \param output_data_block_column       [out] - 2D Kokkos View (memory space must be device_execution_space::memory_space())
    //! \param sampling_data_single_column      [in] - 1D Kokkos View (memory space must match output_data_single_column)
    //! \param lro                              [in] - Target operation from the TargetOperation enum
    //! \param sro                              [in] - Sampling functional from the SamplingFunctional enum
    //! \param target_index                     [in] - Target # user wants to reconstruct target functional at, corresponds to row number of neighbor_lists
    //! \param output_component_axis_1          [in] - Row for a rank 2 tensor or rank 1 tensor, 0 for a scalar output
    //! \param output_component_axis_2          [in] - Columns for a rank 2 tensor, 0 for rank less than 2 output tensor
    //! \param input_component_axis_1           [in] - Row for a rank 2 tensor or rank 1 tensor, 0 for a scalar input
    //! \param input_component_axis_2           [in] - Columns for a rank 2 tensor, 0 for rank less than 2 input tensor
    //! \param pre_transform_local_index        [in] - For manifold problems, this is the local coordinate direction that sampling data may need to be transformed to before the application of GMLS
    //! \param pre_transform_global_index       [in] - For manifold problems, this is the global coordinate direction that sampling data can be represented in
    //! \param post_transform_local_index       [in] - For manifold problems, this is the local coordinate direction that vector output target functionals from GMLS will output into
    //! \param post_transform_global_index      [in] - For manifold problems, this is the global coordinate direction that the target functional output from GMLS will be transformed into
    //! \param transform_output_ambient         [in] - Whether or not a 1D output from GMLS is on the manifold and needs to be mapped to ambient space
    //! \param vary_on_target                   [in] - Whether the sampling functional has a tensor to act on sampling data that varies with each target site
    //! \param vary_on_neighbor                 [in] - Whether the sampling functional has a tensor to act on sampling data that varies with each neighbor site in addition to varying wit each target site
    template <typename view_type_data_out, typename view_type_data_in>
    void applyFullPolynomialCoefficientsBasisToDataSingleComponent(view_type_data_out output_data_block_column, view_type_data_in sampling_data_single_column, TargetOperation lro, const SamplingFunctional sro, const int output_component_axis_1, const int output_component_axis_2, const int input_component_axis_1, const int input_component_axis_2, const int pre_transform_local_index = -1, const int pre_transform_global_index = -1, const int post_transform_local_index = -1, const int post_transform_global_index = -1, bool transform_output_ambient = false, bool vary_on_target = false, bool vary_on_neighbor = false) const {

        auto neighbor_lists = _gmls->getNeighborLists();
        const int coefficient_matrix_tile_size = _gmls->getPolynomialCoefficientsMatrixTileSize();

        // alphas gracefully handle a scalar basis used repeatedly (VectorOfScalarClones) because the alphas
        // hold a 0 when the input index is greater than 0
        // this can be detected for the polynomial coefficients by noticing that the input_component_axis_1 is greater than 0,
        // but the offset this would produce into the coefficients matrix is larger than its dimensions
        auto max_num_neighbors = (neighbor_lists.extent(1) - 1);
        if (input_component_axis_1*max_num_neighbors >= coefficient_matrix_tile_size) return;

        auto global_dimensions = _gmls->getGlobalDimensions();

        // gather needed information for evaluation
        auto coeffs         = _gmls->getFullPolynomialCoefficientsBasis();
        auto tangent_directions = _gmls->getTangentDirections();
        auto prestencil_weights = _gmls->getPrestencilWeights();

        const int num_targets = neighbor_lists.extent(0); // one row for each target

        // make sure input and output views have same memory space
        compadre_assert_debug((std::is_same<typename view_type_data_out::memory_space, typename view_type_data_in::memory_space>::value) && 
                "output_data_block_column view and input_data_single_column view have difference memory spaces.");

        bool weight_with_pre_T = (pre_transform_local_index>=0 && pre_transform_global_index>=0) ? true : false;
        bool target_plus_neighbor_staggered_schema = sro.use_target_site_weights;

        // loops over target indices
        for (int j=0; j<_gmls->getPolynomialCoefficientsSize(); ++j) {
            Kokkos::parallel_for(team_policy(num_targets, Kokkos::AUTO),
                    KOKKOS_LAMBDA(const member_type& teamMember) {

                const int target_index = teamMember.league_rank();

                scratch_matrix_right_type T (tangent_directions.data() 
                            + TO_GLOBAL(target_index)*TO_GLOBAL(global_dimensions)*TO_GLOBAL(global_dimensions), 
                         global_dimensions, global_dimensions);

                scratch_matrix_right_type Coeffs(coeffs.data() 
                            + TO_GLOBAL(target_index)*TO_GLOBAL(coefficient_matrix_tile_size)
                                *TO_GLOBAL(coefficient_matrix_tile_size), 
                            coefficient_matrix_tile_size, coefficient_matrix_tile_size);
                teamMember.team_barrier();


                const double previous_value = output_data_block_column(target_index, j);

                // loops over neighbors of target_index
                double gmls_value = 0;
                Kokkos::parallel_reduce(Kokkos::TeamThreadRange(teamMember, neighbor_lists(target_index,0)), [=](const int i, double& t_value) {
                    const double neighbor_varying_pre_T =  (weight_with_pre_T && vary_on_neighbor) ?
                        prestencil_weights(0, target_index, i, pre_transform_local_index, pre_transform_global_index)
                        : 1.0;

                    t_value += neighbor_varying_pre_T * sampling_data_single_column(neighbor_lists(target_index, i+1))
                        *Coeffs(i, j);
                        //*Coeffs(ORDER_INDICES(i+input_component_axis_1*neighbor_lists(target_index,0), j));

                }, gmls_value );

                // data contract for sampling functional
                double pre_T = 1.0;
                if (weight_with_pre_T) {
                    if (!vary_on_neighbor && vary_on_target) {
                        pre_T = prestencil_weights(0, target_index, 0, pre_transform_local_index, 
                                pre_transform_global_index); 
                    } else if (!vary_on_target) { // doesn't vary on target or neighbor
                        pre_T = prestencil_weights(0, 0, 0, pre_transform_local_index, 
                                pre_transform_global_index); 
                    }
                }

                double staggered_value_from_targets = 0;
                double pre_T_staggered = 1.0;
                // loops over target_index for each neighbor for staggered approaches
                if (target_plus_neighbor_staggered_schema) {
                    Kokkos::parallel_reduce(Kokkos::TeamThreadRange(teamMember, neighbor_lists(target_index,0)), [=](const int i, double& t_value) {
                        const double neighbor_varying_pre_T_staggered =  (weight_with_pre_T && vary_on_neighbor) ?
                            prestencil_weights(1, target_index, i, pre_transform_local_index, pre_transform_global_index)
                            : 1.0;

                        t_value += neighbor_varying_pre_T_staggered * sampling_data_single_column(neighbor_lists(target_index, 1))
                            *Coeffs(i, j);
                            //*Coeffs(ORDER_INDICES(i+input_component_axis_1*neighbor_lists(target_index,0), j));

                    }, staggered_value_from_targets );

                    // for staggered approaches that transform source data for the target and neighbors
                    if (weight_with_pre_T) {
                        if (!vary_on_neighbor && vary_on_target) {
                            pre_T_staggered = prestencil_weights(1, target_index, 0, pre_transform_local_index, 
                                    pre_transform_global_index); 
                        } else if (!vary_on_target) { // doesn't vary on target or neighbor
                            pre_T_staggered = prestencil_weights(1, 0, 0, pre_transform_local_index, 
                                    pre_transform_global_index); 
                        }
                    }
                }

                double post_T = (transform_output_ambient) ? T(post_transform_local_index, post_transform_global_index) : 1.0;
                double added_value = post_T*(pre_T*gmls_value + pre_T_staggered*staggered_value_from_targets);
                Kokkos::single(Kokkos::PerTeam(teamMember), [=] () {
                    output_data_block_column(target_index, j) = previous_value + added_value;
                });
            });
            Kokkos::fence();
        }
    }

    //! Generation of polynomial reconstruction coefficients by applying to data in GMLS
    //! 
    //! Polynomial reconstruction coefficients exist for each target, but there are coefficients for each neighbor (a basis for all potentional input data). This function uses a particular choice of data to contract over this basis and return the polynomial reconstructions coefficients specific to this data.
    //!
    //! Produces a Kokkos View as output with a Kokkos memory_space provided as a template tag by the caller. 
    //! The data type (double* or double**) must also be specified as a template type if one wish to get a 1D 
    //! Kokkos View back that can be indexed into with only one ordinal.
    //! 
    //! Assumptions on input data:
    //! \param sampling_data              [in] - 1D or 2D Kokkos View that has the layout #targets * columns of data. Memory space for data can be host or device. 
    //! \param lro                        [in] - Target operation from the TargetOperation enum
    //! \param sro                        [in] - Sampling functional from the SamplingFunctional enum
    //! \param scalar_as_vector_if_needed [in] - If a 1D view is given, where a 2D view is expected (scalar values given where a vector was expected), then the scalar will be repeated for as many components as the vector has
    template <typename output_data_type = double**, typename output_memory_space, typename view_type_input_data, typename output_array_layout = typename view_type_input_data::array_layout>
    Kokkos::View<output_data_type, output_array_layout, output_memory_space>  // shares layout of input by default
            applyFullPolynomialCoefficientsBasisToDataAllComponents(view_type_input_data sampling_data, bool scalar_as_vector_if_needed = true) const {

        // this function returns the polynomial coefficients, not the evaluation of a target operation applied to the polynomial coefficients
        // because of this, it doesn't make sense to specify a TargetOperation, so we use the ScalarPointEvaluation because we need an lro
        // that will index input tile sizes correctly in the GMLS object.

        TargetOperation lro = TargetOperation::ScalarPointEvaluation;

        // we do a quick check that the user specified ScalarPointEvaluation as one of the target operations they wanted to have
        // performed when they created the GMLS class. If they didn't, then we have no valid index into the input and output
        // sizes needed to determine bounds for loops

        compadre_assert_release((_gmls->getTargetOperationLocalIndex(lro) >= 0) && "applyFullPolynomialCoefficientsBasisToDataAllComponents called"
                " on a GMLS class where ScalarPointEvaluation was not among the registered TargetOperations");

        // output can be device or host
        // input can be device or host
        // move everything to device and calculate there, then move back to host if necessary

        typedef Kokkos::View<output_data_type, output_array_layout, output_memory_space> output_view_type;

        auto problem_type = _gmls->getProblemType();
        auto global_dimensions = _gmls->getGlobalDimensions();
        auto output_dimension_of_operator = _gmls->getOutputDimensionOfOperation(lro);
        auto input_dimension_of_operator = _gmls->getInputDimensionOfOperation(lro);

        // gather needed information for evaluation
        auto neighbor_lists = _gmls->getNeighborLists();

        // determines the number of columns needed for output after action of the target functional
        int output_dimensions;
        if (problem_type==MANIFOLD && TargetOutputTensorRank[(int)lro]==1) {
            output_dimensions = global_dimensions;
        } else {
            output_dimensions = output_dimension_of_operator;
        }

        // don't need to check for VectorPointSample and problem_type==MANIFOLD because _gmls already handled this
        const SamplingFunctional sro = _gmls->getDataSamplingFunctional();

        // create view on whatever memory space the user specified with their template argument when calling this function
        output_view_type coefficient_output("output coefficients", neighbor_lists.extent(0) /* number of targets */, 
                output_dimensions*_gmls->getPolynomialCoefficientsSize() /* number of coefficients */);

        // make sure input and output columns make sense under the target operation
        compadre_assert_debug(((output_dimension_of_operator==1 && output_view_type::rank==1) || output_view_type::rank!=1) && 
                "Output view is requested as rank 1, but the target requires a rank larger than 1. Try double** as template argument.");

        // we need to specialize a template on the rank of the output view type and the input view type
        auto sampling_subview_maker = CreateNDSliceOnDeviceView(sampling_data, scalar_as_vector_if_needed);
        auto output_subview_maker = CreateNDSliceOnDeviceView(coefficient_output, false); // output will always be the correct dimension

        // figure out preprocessing and postprocessing
        auto prestencil_weights = _gmls->getPrestencilWeights();

        // all loop logic based on transforming data under a sampling functional
        // into something that is valid input for GMLS
        bool vary_on_target, vary_on_neighbor;
        auto sro_style = sro.transform_type;
        bool loop_global_dimensions = sro.input_rank>0 && sro_style!=Identity; 


        if (sro.transform_type == Identity || sro.transform_type == SameForAll) {
            vary_on_target = false;
            vary_on_neighbor = false;
        } else if (sro.transform_type == DifferentEachTarget) {
            vary_on_target = true;
            vary_on_neighbor = false;
        } else if (sro.transform_type == DifferentEachNeighbor) {
            vary_on_target = true;
            vary_on_neighbor = true;
        }

        bool transform_gmls_output_to_ambient = (problem_type==MANIFOLD && TargetOutputTensorRank[(int)lro]==1);

        // only written for up to rank 0 to rank 0 (in / out)
        // loop over components of output of the target operation
        for (int i=0; i<output_dimension_of_operator; ++i) {
            const int output_component_axis_1 = i;
            const int output_component_axis_2 = 0;
            // loop over components of input of the target operation
            for (int j=0; j<input_dimension_of_operator; ++j) {
                const int input_component_axis_1 = j;
                const int input_component_axis_2 = 0;

                if (loop_global_dimensions && transform_gmls_output_to_ambient) {
                    for (int k=0; k<global_dimensions; ++k) { // loop for handling sampling functional
                        for (int l=0; l<global_dimensions; ++l) { // loop for transforming output of GMLS to ambient
                            this->applyFullPolynomialCoefficientsBasisToDataSingleComponent(
                                    output_subview_maker.get2DView(k,_gmls->getPolynomialCoefficientsSize()), 
                                    sampling_subview_maker.get1DView(l), 
                                    lro, sro, output_component_axis_1, output_component_axis_2, 
                                    input_component_axis_1, input_component_axis_2, j, k, i, l,
                                    transform_gmls_output_to_ambient, vary_on_target, vary_on_neighbor);
                        }
                    }
                } else if (transform_gmls_output_to_ambient) {
                    for (int k=0; k<global_dimensions; ++k) { // loop for transforming output of GMLS to ambient
                        this->applyFullPolynomialCoefficientsBasisToDataSingleComponent(
                                output_subview_maker.get2DView(k,_gmls->getPolynomialCoefficientsSize()), 
                                sampling_subview_maker.get1DView(j), lro, sro, 
                                output_component_axis_1, output_component_axis_2, input_component_axis_1, 
                                input_component_axis_2, -1, -1, i, k,
                                transform_gmls_output_to_ambient, vary_on_target, vary_on_neighbor);
                    }
                } else if (loop_global_dimensions) {
                    for (int k=0; k<global_dimensions; ++k) { // loop for handling sampling functional
                        this->applyFullPolynomialCoefficientsBasisToDataSingleComponent(
                                output_subview_maker.get2DView(i,_gmls->getPolynomialCoefficientsSize()), 
                                sampling_subview_maker.get1DView(k), lro, sro, 
                                output_component_axis_1, output_component_axis_2, input_component_axis_1, 
                                input_component_axis_2, j, k, -1, -1, transform_gmls_output_to_ambient,
                                vary_on_target, vary_on_neighbor);
                    }
                } else if (sro_style != Identity) {
                    this->applyFullPolynomialCoefficientsBasisToDataSingleComponent(
                            output_subview_maker.get2DView(i,_gmls->getPolynomialCoefficientsSize()), 
                            sampling_subview_maker.get1DView(j), lro, sro, 
                            output_component_axis_1, output_component_axis_2, input_component_axis_1, 
                            input_component_axis_2, 0, 0, -1, -1,
                            transform_gmls_output_to_ambient, vary_on_target, vary_on_neighbor);
                } else { // standard
                    this->applyFullPolynomialCoefficientsBasisToDataSingleComponent(
                            output_subview_maker.get2DView(i,_gmls->getPolynomialCoefficientsSize()), 
                            sampling_subview_maker.get1DView(j), lro, sro, 
                            output_component_axis_1, output_component_axis_2, input_component_axis_1, 
                            input_component_axis_2);
                }
            }
        }

        // copy back to whatever memory space the user requester through templating from the device
        Kokkos::deep_copy(coefficient_output, output_subview_maker.copyToAndReturnOriginalView());
        return coefficient_output;
    }

}; // Evaluator

}; // Compadre

#endif

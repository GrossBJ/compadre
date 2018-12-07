#ifndef _COMPADRE_SOLVER_HPP_
#define _COMPADRE_SOLVER_HPP_

#include "Compadre_Config.h"
#include "Compadre_Typedefs.hpp"

#ifdef TRILINOS_LINEAR_SOLVES

namespace Tpetra {
template <typename scalar_type, typename  local_index_type, typename global_index_type, typename node_type>
class Operator;
}

namespace Ifpack2 {
template <typename scalar_type, typename  local_index_type, typename global_index_type, typename node_type>
class Preconditioner;
}

namespace Compadre {

class SolverT {

	protected:

		Teuchos::RCP<Teuchos::ParameterList> _parameters;
		std::string _parameter_filename;
		const Teuchos::RCP<const Teuchos::Comm<int> > _comm;
		Teuchos::RCP<thyra_block_op_type> _A;
		std::vector<std::vector<Teuchos::RCP<crs_matrix_type> > > _A_tpetra;
		std::vector<Teuchos::RCP<thyra_mvec_type> > _b;
		std::vector<Teuchos::RCP<mvec_type> > _b_tpetra;
		std::vector<Teuchos::RCP<mvec_type> > _x;

	public:

		SolverT( const Teuchos::RCP<const Teuchos::Comm<int> > comm, std::vector<std::vector<Teuchos::RCP<crs_matrix_type> > > A,
				std::vector<Teuchos::RCP<mvec_type> > b);

		virtual ~SolverT() {};

		void setParameters(Teuchos::ParameterList& parameters) {
			_parameters = Teuchos::rcp(&parameters, false);
			_parameter_filename = _parameters->get<std::string>("file");
		}

		Teuchos::RCP<mvec_type> getSolution(local_index_type idx = -1) const;

		void solve();

};

}

#endif
#endif
/*********************************************************************************
* Copyright (C) 2019 Alexey V. Akimov
*
* This file is distributed under the terms of the GNU General Public License
* as published by the Free Software Foundation, either version 2 of
* the License, or (at your option) any later version.
* See the file LICENSE in the root directory of this distribution
* or <http://www.gnu.org/licenses/>.
*
*********************************************************************************/
/**
  \file dynamics.cpp
  \brief The file implements the general framework to run:
   * adiabatic dynamics (Verlet)
   * nonadiabatic Ehrenfest dynamics
   * nonadiabatic TSH dynamics 
   * include thermostat, if needed
   * include decoherence
   * include quantum nuclear effect (ETHD) 
   * include phase corrections/state tracking in NA-MD
   * the same framework for multiple trajectories
   * include coupled-trajectories methods (planned)
   * enable the NBRA-like calculations as well as non-NBRA
*/

#include "Surface_Hopping.h"
#include "Energy_and_Forces.h"
#include "electronic/libelectronic.h"
#include "Dynamics.h"
#include "dyn_control_params.h"


/// liblibra namespace
namespace liblibra{

/// libdyn namespace
namespace libdyn{


namespace bp = boost::python;

void aux_get_transforms(CMATRIX** Uprev, nHamiltonian& ham){

  // For adiabatic representation only:
  // Save the previous orbitals info - in case we need to
  // do either phase correction of state tracking

  int ntraj = ham.children.size();

  for(int traj=0; traj<ntraj; traj++){
    *Uprev[traj] = ham.children[traj]->get_basis_transform();  
  }

}


void update_Hamiltonian_q(dyn_control_params& prms, MATRIX& q, vector<CMATRIX>& projectors,
                          nHamiltonian& ham, 
                          bp::object py_funct, bp::object model_params){

  /**
    Update of the vibronic Hamiltonian in response to changed q
  */

  //------ Update the internals of the Hamiltonian object --------
  // We call the external function that would do the calculations
  if(prms.rep_tdse==0){      
    if(prms.rep_ham==0){
      ham.compute_diabatic(py_funct, bp::object(q), model_params, 1);
    }
  }
  if(prms.rep_tdse==1){      
    if(prms.rep_ham==0){
      ham.compute_diabatic(py_funct, bp::object(q), model_params, 1);
      ham.compute_adiabatic(1, 1);
    }
    else if(prms.rep_ham==1){
      ham.compute_adiabatic(py_funct, bp::object(q), model_params, 1);
    }
  }


}


void update_Hamiltonian_q(bp::dict prms, MATRIX& q, vector<CMATRIX>& projectors,
                          nHamiltonian& ham, 
                          bp::object py_funct, bp::object model_params){

  dyn_control_params _prms;
  _prms.set_parameters(prms);

  update_Hamiltonian_q(_prms, q, projectors, ham, py_funct, model_params);

}


void update_Hamiltonian_q_ethd(dyn_control_params& prms, MATRIX& q, MATRIX& p, vector<CMATRIX>& projectors,
                          nHamiltonian& ham, 
                          bp::object py_funct, bp::object model_params, MATRIX& invM){

  if(prms.entanglement_opt==0){    /* Nothing to do */   }
  else if(prms.entanglement_opt==1){   ham.add_ethd_adi(q, invM, 1);  }
  else if(prms.entanglement_opt==2){   ham.add_ethd3_adi(q, invM, prms.ETHD3_alpha, 1);  }
  else if(prms.entanglement_opt==22){  ham.add_ethd3_adi(q, p, invM, prms.ETHD3_alpha, prms.ETHD3_beta, 1);  }
  else{
    cout<<"ERROR in update_Hamiltonian_q_ethd: The entanglement option = "<<prms.entanglement_opt<<" is not avaialable\n";
    exit(0);
  }

}

void update_Hamiltonian_q_ethd(bp::dict prms, MATRIX& q, MATRIX& p, vector<CMATRIX>& projectors,
                          nHamiltonian& ham, 
                          bp::object py_funct, bp::object model_params, MATRIX& invM){

  dyn_control_params _prms;
  _prms.set_parameters(prms);

  update_Hamiltonian_q_ethd(_prms, q, p, projectors, ham, py_funct, model_params, invM);

}


void update_Hamiltonian_p(dyn_control_params& prms, nHamiltonian& ham, 
                          MATRIX& p, MATRIX& invM){

  /**
    Update of the vibronic Hamiltonian in response to changed p
  */

  // For the purpose of updating the NACs and Hvibs for just the quantum DOFs,
  // we'll reset the momenta for all other DOFs to zero, to effectively turn of
  // the effect of classical momenta on the NAC calculations (in case those derivative
  // couplings have been computed)
  int ndof, ntraj;
  vector<int>& which_dofs = prms.quantum_dofs;
  int n_active_dof = which_dofs.size();
  ndof = p.n_rows;
  ntraj = p.n_cols;

  MATRIX p_quantum_dof(ndof, ntraj);

  for(int idof = 0; idof < n_active_dof; idof++){
    int dof = which_dofs[idof];

    for(int itraj = 0; itraj < ntraj; itraj++){
      p_quantum_dof.set(dof, itraj,  p.get(dof, itraj) );
    }
  }
  


  // Update NACs and Hvib for all trajectories
  if(prms.rep_tdse==0){  

    if(prms.nac_update_method==0){ ;;  }
    else if(prms.nac_update_method==1){
      ham.compute_nac_dia(p_quantum_dof, invM, 0, 1);
      ham.compute_hvib_dia(1);
    }
  }
  else if(prms.rep_tdse==1){  

    if(prms.nac_update_method==0){ ;;  }
    else if(prms.nac_update_method==1){
      ham.compute_nac_adi(p_quantum_dof, invM, 0, 1); 
      ham.compute_hvib_adi(1);
    }
  }
}


void update_Hamiltonian_p(bp::dict prms, nHamiltonian& ham, MATRIX& p, MATRIX& invM){

  dyn_control_params _prms;
  _prms.set_parameters(prms);

  update_Hamiltonian_p(_prms, ham, p, invM);

}



CMATRIX transform_amplitudes(int rep_in, int rep_out, CMATRIX& C, nHamiltonian& ham){
/**
  This function converts the amplitudes from one representation to another

  The reason: we may be solving TD-SE (computing forces) in one representation
  but compute the hopping probabilities in another one.

  This function assumes we already have the basis transformation matrix in ham object 
  computed/updated

*/

  int nst = C.n_rows;    
  int ntraj = C.n_cols;

  CMATRIX Coeff(nst,ntraj); 

  /// Depending on the basis, select which   
  /// C - the basis in which the electron-nuclear propagation is done
  /// Coeff - the basis in which SH is done

  // Input in the diabatic basis
  if(rep_in==0){                  
 
    // Output in the diabatic basis too
    if(rep_out==0){ 
      Coeff = C; 
    }

    // Output in the adiabatic basis
    else if(rep_out==1){  

      ham.ampl_dia2adi(C, Coeff, 0, 1);  

    }

  }

  // Input in the adiabatic basis 
  else if(rep_in==1){   

    // Output in the diabatic basis 
    if(rep_out==0){  

      ham.ampl_adi2dia(Coeff, C, 0, 1); 

    } 

    // Output in the diabatic basis too
    else if(rep_out==1){ 

      Coeff = C;  

    } 
  }

  return Coeff;
}





//vector<CMATRIX> compute_St(nHamiltonian& ham, CMATRIX** Uprev){
vector<CMATRIX> compute_St(nHamiltonian& ham, vector<CMATRIX>& Uprev){
/**
  This function computes the time-overlap matrices for all trajectories

*/

  int nst = ham.nadi;
  int ntraj = ham.children.size();

  vector<CMATRIX> St(ntraj, CMATRIX(nst, nst));

  for(int traj=0; traj<ntraj; traj++){
    St[traj] = Uprev[traj].H() * ham.children[traj]->get_basis_transform();
  }

  return St;

}

vector<CMATRIX> compute_St(nHamiltonian& ham){
/**
  This function computes the time-overlap matrices for all trajectories

*/

  int nst = ham.nadi;
  int ntraj = ham.children.size();

  vector<CMATRIX> St(ntraj, CMATRIX(nst, nst));

  for(int traj=0; traj<ntraj; traj++){
    St[traj] = ham.children[traj]->get_time_overlap_adi();
  }

  return St;

}





void compute_dynamics(MATRIX& q, MATRIX& p, MATRIX& invM, CMATRIX& C, vector<CMATRIX>& projectors,
              vector<int>& act_states,              
              nHamiltonian& ham, bp::object py_funct, bp::dict params, bp::dict dyn_params, Random& rnd){
/**
  This is a version to maintain the backward-compatibility
*/ 

  dyn_control_params prms;
  prms.set_parameters(dyn_params);

  int ntraj = q.n_cols;
  vector<Thermostat> therm(ntraj, Thermostat(prms.thermostat_params));

  compute_dynamics(q, p, invM, C, projectors, act_states, ham, py_funct, params, dyn_params, rnd, therm);

}



void compute_dynamics(MATRIX& q, MATRIX& p, MATRIX& invM, CMATRIX& C, vector<CMATRIX>& projectors,
              vector<int>& act_states,              
              nHamiltonian& ham, bp::object py_funct, bp::dict params, bp::dict dyn_params, Random& rnd,
              vector<Thermostat>& therm){

/**
  \brief One step of the TSH algorithm for electron-nuclear DOFs for one trajectory

  \param[in] Integration time step
  \param[in,out] q [Ndof x Ntraj] nuclear coordinates. Change during the integration.
  \param[in,out] p [Ndof x Ntraj] nuclear momenta. Change during the integration.
  \param[in] invM [Ndof  x 1] inverse nuclear DOF masses. 
  \param[in,out] C [nadi x ntraj]  or [ndia x ntraj] matrix containing the electronic coordinates. The amplitudes
   are assumed to be dynamically-consistent
  \param[in,out] projectors [ntraj CMATRIX(nadi, nadi)] - the projector matrices that account for the state tracking and 
  phase correction. These matrices should be considered as the dynamical varibles, similar to quantum amplitudes. Except
  their evolution does not necessarily follow from some equations of motion, but rather from various ad hoc schemes.
  \param[in,out] act_states - vector of ntraj indices of the physical states in which each of the trajectories
  initially is (active states). 
  \param[in] ham Is the Hamiltonian object that works as a functor (takes care of all calculations of given type) 
  - its internal variables (well, actually the variables it points to) are changed during the compuations
  \param[in] py_funct Python function object that is called when this algorithm is executed. The called Python function does the necessary 
  computations to update the diabatic Hamiltonian matrix (and derivatives), stored externally.
  \param[in] params The Python object containing any necessary parameters passed to the "py_funct" function when it is executed.
  \param[in] params1 The Python dictionary containing the control parameters passed to this function
  \param[in] rnd The Random number generator object

  Return: propagates C, q, p and updates state variables

*/


  dyn_control_params prms;
  prms.set_parameters(dyn_params);

  int cdof;
  int ndof = q.n_rows;
  int ntraj = q.n_cols;
  int nst = C.n_rows;    
  int traj, dof, idof;
  int n_therm_dofs;

  MATRIX coherence_time(nst, ntraj); // for DISH
  MATRIX coherence_interval(nst, ntraj); // for DISH
  vector<int> project_out_states(ntraj); // for DISH

  vector<CMATRIX> Uprev;
  vector<CMATRIX> St(ntraj, CMATRIX(nst, nst));
  vector<CMATRIX> Eadi(ntraj, CMATRIX(nst, nst));  
  vector<MATRIX> decoherence_rates(ntraj, MATRIX(nst, nst)); 
  vector<double> Ekin(ntraj, 0.0);  
  vector<MATRIX> prev_ham_dia(ntraj, MATRIX(nst, nst));
  MATRIX gamma(ndof, ntraj);
  MATRIX p_traj(ndof, 1);
  vector<int> t1(ndof, 0); for(dof=0;dof<ndof;dof++){  t1[dof] = dof; }
  vector<int> t2(1,0);

  //============ Sanity checks ==================
  if(prms.ensemble==1){  
    n_therm_dofs = therm[0].Nf_t + therm[0].Nf_r;
    if(n_therm_dofs != prms.thermostat_dofs.size()){
      cout<<"Error in compute_dynamics: The number of thermostat DOFs ( currently "<<n_therm_dofs<<") must be \
      equal to the number of thermostat dofs set up by the `thermostat_dofs` parameter ( currently "
      <<prms.thermostat_dofs.size()<<")\nExiting...\n";
      exit(0);
    }
  }



  if(prms.tsh_method == 3){ // DISH
    for(traj=0; traj<ntraj; traj++){
      prev_ham_dia[traj] = ham.children[traj]->get_ham_dia().real();  
    }
  }

  //============ Update the Hamiltonian object =============
  // In case, we may need phase correction & state reordering
  // prepare the temporary files
  if(prms.rep_tdse==1){      
    if(prms.do_phase_correction || prms.state_tracking_algo > 0){

      // On-the-fly calculations, from the wavefunctions
      if(prms.time_overlap_method==0){
        Uprev = vector<CMATRIX>(ntraj, CMATRIX(nst, nst));

        for(traj=0; traj<ntraj; traj++){
          Uprev[traj] = ham.children[traj]->get_basis_transform();  
        }
      }      
    }// do_phase_correction || state_tracking_algo > 0
  }// rep == 1


  //============== Electronic propagation ===================
  // Evolve electronic DOFs for all trajectories
  propagate_electronic(0.5*prms.dt, C, projectors, ham.children, prms.rep_tdse);   

  //============== Nuclear propagation ===================

  // NVT dynamics
  if(prms.ensemble==1){  
    for(idof=0; idof<n_therm_dofs; idof++){
      dof = prms.thermostat_dofs[idof];
      for(traj=0; traj<ntraj; traj++){
        p.scale(dof, traj, therm[traj].vel_scale(0.5*prms.dt));
      }// traj
    }// idof 
  }
 
  p = p + aux_get_forces(prms, C, projectors, act_states, ham) * 0.5 * prms.dt;

  // Kinetic constraint
  for(cdof = 0; cdof < prms.constrained_dofs.size(); cdof++){   
    p.scale(prms.constrained_dofs[cdof], -1, 0.0); 
  }



  if(prms.entanglement_opt==22){
    gamma = ETHD3_friction(q, p, invM, prms.ETHD3_alpha, prms.ETHD3_beta);
  }

  // Update coordinates of nuclei for all trajectories
  for(traj=0; traj<ntraj; traj++){
    for(dof=0; dof<ndof; dof++){  
      q.add(dof, traj,  invM.get(dof,0) * p.get(dof,traj) * prms.dt ); 

      if(prms.entanglement_opt==22){
        q.add(dof, traj,  invM.get(dof,0) * gamma.get(dof,traj) * prms.dt ); 
      }

    }
  }


  // Recompute the matrices at the new geometry and apply any necessary fixes 
  update_Hamiltonian_q(prms, q, projectors, ham, py_funct, params);
  update_Hamiltonian_q_ethd(prms, q, p, projectors, ham, py_funct, params, invM);


//  std::string key;
//  int timestep;
//  for(int i=0;i<len(params.values());i++){
//      key = extract<std::string>(params.keys()[i]);
//      if(key=="timestep"){  timestep = extract< int >(params.values()[i]); }
//  }
//  cout<<" timestep = "<<timestep<<endl;
//  cout<<"tracking_algo = "<<prms.state_tracking_algo<<endl;
//  cout<<"phase_corr = "<<prms.do_phase_correction<<endl;
//  cout<<"prms.rep_tdse = "<<prms.rep_tdse<<endl;
//  cout<<"Debug: before the update_projectors\n";
//  cout<<projectors[0].get(0,0)<<endl;
   

  // Apply phase correction and state reordering as needed
  if(prms.rep_tdse==1){

    if(prms.state_tracking_algo > 0 || prms.do_phase_correction){

      if(prms.time_overlap_method==0){    St = compute_St(ham, Uprev);    }
      else if(prms.time_overlap_method==1){    St = compute_St(ham);      }

      Eadi = get_Eadi(ham);           // these are raw properties
      update_projectors(prms, projectors, Eadi, St, rnd);

    }
  }// rep_tdse == 1


//  cout<<"Debug: after the update_projectors\n";
//  cout<<projectors[0].get(0,0)<<endl;



  // NVT dynamics
  if(prms.ensemble==1){  
    for(traj=0; traj<ntraj; traj++){
      t2[0] = traj; 
      pop_submatrix(p, p_traj, t1, t2);
      double ekin = compute_kinetic_energy(p_traj, invM, prms.thermostat_dofs);
      therm[traj].propagate_nhc(prms.dt, ekin, 0.0, 0.0);
    }

  }


  p = p + aux_get_forces(prms, C, projectors, act_states, ham) * 0.5 * prms.dt;

  // Kinetic constraint
  for(cdof=0; cdof<prms.constrained_dofs.size(); cdof++){   
    p.scale(prms.constrained_dofs[cdof], -1, 0.0); 
  }


  // NVT dynamics
  if(prms.ensemble==1){  
    for(idof=0; idof<n_therm_dofs; idof++){
      dof = prms.thermostat_dofs[idof];
      for(traj=0; traj<ntraj; traj++){
        p.scale(dof, traj, therm[traj].vel_scale(0.5*prms.dt));
      }// traj
    }// idof 
  }


  //============== Electronic propagation ===================
  // Evolve electronic DOFs for all trajectories
  update_Hamiltonian_p(prms, ham, p, invM);
  propagate_electronic(0.5*prms.dt, C, projectors, ham.children, prms.rep_tdse);   



  //============== Begin the TSH part ===================

  // To be able to compute transition probabilities, compute the corresponding amplitudes
  // This transformation is between diabatic and raw adiabatic representations
  CMATRIX Coeff(nst,ntraj); 
  Coeff = transform_amplitudes(prms.rep_tdse, prms.rep_sh, C, ham);

  if(prms.rep_tdse==0){
    // If we solve TD-SE in the diabatic rep, C (when transformed to adiabatic basis by the code above)
    // returned is in the raw representation, so we need to make it dynamically-consistent

    Coeff = raw_to_dynconsyst(Coeff, projectors);
  }
  else if(prms.rep_tdse==1){ 
    // If we solve TD-SE in the adiabatic rep, C is already dynamically-consistent
    ;;
  }



  //================= Update decoherence rates & times ================
  //MATRIX decoh_rates(*prms.decoh_rates);

  if(prms.decoherence_algo==0 || prms.decoherence_algo==2 || prms.tsh_method==3){

    // Just use the plain times given from the input, usually the
    // mSDM formalism
    if(prms.decoherence_times_type==0){
      for(traj=0; traj<ntraj; traj++){   decoherence_rates[traj] = *prms.decoherence_rates;   }
    }

    // Compute the dephasing rates according the original energy-based formalism
    else if(prms.decoherence_times_type==1){
      Eadi = get_Eadi(ham); 
      Ekin = compute_kinetic_energies(p, invM);
      decoherence_rates = edc_rates(Eadi, Ekin, prms.decoherence_C_param, prms.decoherence_eps_param);       
    }

    //== Optionally, apply the dephasing-informed correction ==
    if(prms.dephasing_informed==1){
      Eadi = get_Eadi(ham); 
      MATRIX ave_gaps(*prms.ave_gaps);
      dephasing_informed_correction(decoherence_rates, Eadi, ave_gaps);
    }

  }
  


  //============ Apply decoherence corrections ==================
  // SDM and alike methods
  if(prms.decoherence_algo==0){
    Coeff = sdm(Coeff, prms.dt, act_states, decoherence_rates, prms.sdm_norm_tolerance);
  }
 
 
  //========= Use the resulting amplitudes to do the hopping =======
  // FSSH, GFSH or MSSH
  if(prms.tsh_method == 0 || prms.tsh_method == 1 || prms.tsh_method == 2){

    // Compute hopping probabilities
    vector<MATRIX> g( hop_proposal_probabilities(prms, q, p, invM, Coeff, projectors, ham, prev_ham_dia) );

    // Propose new discrete states    
    vector<int> prop_states( propose_hops(g, act_states, rnd) );

    // Decide if to accept the transitions (and then which)
    vector<int> old_states(act_states);
    act_states = accept_hops(prms, q, p, invM, Coeff, projectors, ham, prop_states, act_states, rnd);

    // Velocity rescaling
    handle_hops_nuclear(prms, q, p, invM, Coeff, projectors, ham, act_states, old_states);

    if(prms.decoherence_algo==1){
      // Instantaneous decoherence 
      instantaneous_decoherence(Coeff, act_states, prop_states, old_states, 
          prms.instantaneous_decoherence_variant, prms.collapse_option);
    }
        
  }// tsh_method = 0, 1, 2


  // DISH
  else if(prms.tsh_method == 3 ){

    /// Advance coherence times
    coherence_time.add(-1, -1, prms.dt);

    /// Older version:
    /**
    vector<int> prop_states( dish_hop_proposal(act_states, Coeff, coherence_time, decoherence_rates, rnd) );

    /// Decide if to accept the transitions (and then which)
    vector<int> old_states(act_states);
    act_states = accept_hops(prms, q, p, invM, Coeff, projectors, ham, prop_states, act_states, rnd);

    /// Velocity rescaling
    handle_hops_nuclear(prms, q, p, invM, Coeff, projectors, ham, act_states, old_states);

    dish_project_out_collapse(old_states, prop_states, act_states, Coeff, coherence_time, prms.collapse_option);
    */

    /// New version, as of 8/3/2020
    vector<int> old_states(act_states);
    act_states = dish(prms, q, p, invM, Coeff, projectors, ham, act_states, coherence_time, decoherence_rates, rnd);

    /// Velocity rescaling
    handle_hops_nuclear(prms, q, p, invM, Coeff, projectors, ham, act_states, old_states);


  }// tsh_method = 3


  /// Convert the temporary amplitudes Coeff to the actual variables C
  if(prms.rep_tdse==0){
    // If we solve TD-SE in the diabatic rep, C (when transformed to adiabatic basis by the code above)
    // returned is in the raw representation, so we need to make it dynamically-consistent

    Coeff = dynconsyst_to_raw(Coeff, projectors);
  }
  else if(prms.rep_tdse==1){ 
    // If we solve TD-SE in the adiabatic rep, C is already dynamically-consistent
    ;;
  }

  // Need the inverse
  //Coeff = transform_amplitudes(prms.rep_tdse, prms.rep_sh, C, ham);

  C = Coeff;



}


}// namespace libdyn
}// liblibra


/*
 * =====================================================================================
 *
 *    Description:  Corblivar floorplanner (SA operations and related handler)
 *
 *    Copyright (C) 2013 Johann Knechtel, johann.knechtel@ifte.de, www.ifte.de
 *
 *    This file is part of Corblivar.
 *    
 *    Corblivar is free software: you can redistribute it and/or modify it under the terms
 *    of the GNU General Public License as published by the Free Software Foundation,
 *    either version 3 of the License, or (at your option) any later version.
 *    
 *    Corblivar is distributed in the hope that it will be useful, but WITHOUT ANY
 *    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 *    PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *    
 *    You should have received a copy of the GNU General Public License along with
 *    Corblivar.  If not, see <http://www.gnu.org/licenses/>.
 *
 * =====================================================================================
 */

// own Corblivar header
#include "FloorPlanner.hpp"
// required Corblivar headers
#include "Point.hpp"
#include "Math.hpp"
#include "CorblivarCore.hpp"
#include "CorblivarAlignmentReq.hpp"
#include "Net.hpp"
#include "IO.hpp"
#include "Clustering.hpp"

// main handler
bool FloorPlanner::performSA(CorblivarCore& corb) {
	int i, ii;
	int innerLoopMax;
	int accepted_ops;
	double accepted_ops_ratio;
	bool op_success;
	double cur_cost, best_cost, prev_cost, cost_diff, avg_cost, fitting_cost;
	Cost cost, cost_sanity_check;
	std::vector<double> cost_samples;
	double cur_temp, init_temp;
	double r;
	int layout_fit_counter;
	double fitting_layouts_ratio;
	bool valid_layout_found;
	int i_valid_layout_found;
	bool best_sol_found;
	bool accept;
	bool SA_phase_two, SA_phase_two_init;
	bool valid_layout;
	TempPhase cooling_phase;

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "-> FloorPlanner::performSA(" << &corb << ")" << std::endl;
	}

	// for handling floorplacement benchmarks, i.e., floorplanning w/ very large
	// blocks, we handle this naively by preferring these large blocks in the lower
	// left corner, i.e., perform a sorting of the sequences by block size
	//
	// also, for random layout operations in SA phase one, these blocks are not
	// allowed to be swapped or moved, see performOpMoveOrSwapBlocks
	if (this->layoutOp.parameters.floorplacement) {
		corb.sortCBLs(this->logMed(), CorblivarCore::SORT_CBLS_BY_BLOCKS_SIZE);
	}

	// init SA: initial sampling; setup parameters, setup temperature schedule
	this->initSA(corb, cost_samples, innerLoopMax, init_temp);

	/// main SA loop
	//
	// init loop parameters
	i = 1;
	cur_temp = init_temp;
	cooling_phase = TempPhase::PHASE_1;
	SA_phase_two = SA_phase_two_init = false;
	valid_layout_found = false;
	i_valid_layout_found = Point::UNDEF;
	fitting_layouts_ratio = 0.0;
	// dummy large value to accept first fitting solution
	best_cost = 100.0 * Math::stdDev(cost_samples);

	/// outer loop: annealing -- temperature steps
	while (i <= this->schedule.loop_limit) {

		if (this->logMax()) {
			std::cout << "SA> Optimization step: " << i << "/" << this->schedule.loop_limit << std::endl;
		}

		// init loop parameters
		ii = 1;
		avg_cost = 0.0;
		accepted_ops = 0;
		layout_fit_counter = 0;
		SA_phase_two_init = false;
		best_sol_found = false;

		// init cost for current layout and fitting ratio
		this->generateLayout(corb, this->opt_flags.alignment && SA_phase_two);
		cur_cost = this->evaluateLayout(corb.getAlignments(), fitting_layouts_ratio, SA_phase_two).total_cost;

		// inner loop: layout operations
		while (ii <= innerLoopMax) {

			// perform layout op
			op_success = layoutOp.performLayoutOp(corb, layout_fit_counter, SA_phase_two, false, (cooling_phase == TempPhase::PHASE_3));

			if (op_success) {

				prev_cost = cur_cost;

				// generate layout; also memorize whether layout is valid;
				// note that this return value is only effective if
				// FloorPlanner::DBG_LAYOUT is set
				valid_layout = this->generateLayout(corb, this->opt_flags.alignment && SA_phase_two);

				// dbg invalid layouts
				if (FloorPlanner::DBG_LAYOUT && !valid_layout) {

					// generate invalid floorplan for dbg
					IO::writeFloorplanGP(*this, corb.getAlignments(), "invalid_layout");
					// generate related Corblivar solution
					if (this->IO_conf.solution_out.is_open()) {
						this->IO_conf.solution_out << corb.CBLsString() << std::endl;
						this->IO_conf.solution_out.close();
					}
					// abort further run
					exit(1);
				}

				// evaluate layout, new cost
				cost = this->evaluateLayout(corb.getAlignments(), fitting_layouts_ratio, SA_phase_two);
				cur_cost = cost.total_cost;
				// cost difference
				cost_diff = cur_cost - prev_cost;

				if (FloorPlanner::DBG_SA) {
					std::cout << "DBG_SA> Inner step: " << ii << "/" << innerLoopMax << std::endl;
					std::cout << "DBG_SA> Cost diff: " << cost_diff << std::endl;
				}

				// revert solution w/ worse or same cost, depending on temperature
				accept = true;
				if (cost_diff >= 0.0) {
					r = Math::randF(0, 1);
					if (r > exp(- cost_diff / cur_temp)) {

						if (FloorPlanner::DBG_SA) {
							std::cout << "DBG_SA> Revert op" << std::endl;
						}
						accept = false;

						// revert last op
						layoutOp.performLayoutOp(corb, layout_fit_counter, SA_phase_two, true);
						// reset cost according to reverted CBL
						cur_cost = prev_cost;
					}
				}

				// solution to be accepted, i.e., previously not reverted
				if (accept) {
					// update ops count
					accepted_ops++;
					// sum up cost for subsequent avg determination
					avg_cost += cur_cost;

					// consider solution to be accepted only if it
					// actually fits the fixed outline
					if (cost.fits_fixed_outline) {

						// consider to switch to SA phase two when
						// first fitting solution is found
						if (!SA_phase_two) {

							// however, cases w/ alignment
							// require another check
							if (this->opt_flags.alignment) {

								// first, we need to re-determine
								// the layout w/ enforced
								// alignment which has not
								// happened previously
								this->generateLayout(corb, true);

								// re-determine if layout still
								// fits, after applying alignment
								// during layout generation; note
								// that the fitting_layouts_ratio
								// doesn't matter here, so it's
								// arbitrarily set to 1.0
								this->evaluateAreaOutline(cost_sanity_check, 1.0);
							}

							// for cases w/ alignment, only
							// proceed when the layout fits;
							// for cases w/o alignment,
							// proceed anyway
							if (
								(this->opt_flags.alignment && cost_sanity_check.fits_fixed_outline) ||
								!this->opt_flags.alignment
							   ) {

								// switch phase
								SA_phase_two = SA_phase_two_init = true;

								// re-calculate cost for new phase; assume
								// fitting ratio 1.0 for initialization
								// and for effective comparison of further
								// fitting solutions; also initialize all
								// max cost terms
								fitting_cost =
									this->evaluateLayout(corb.getAlignments(), 1.0, true, true).total_cost;

								// also memorize in which iteration we
								// found the first valid layout
								i_valid_layout_found = i;

								// logging
								if (this->logMax()) {
									std::cout << "SA> " << std::endl;
								}
								if (this->logMed()) {
									std::cout << "SA> Phase II: optimizing within outline; switch cost function ..." << std::endl;
								}
								if (this->logMax()) {
									std::cout << "SA> " << std::endl;
								}

								// update count of solutions fitting into outline
								layout_fit_counter++;
							}
						}
						// not first but any fitting solution; in
						// order to compare different fitting
						// solutions equally, consider cost terms
						// w/ fitting ratio 1.0
						else {
							fitting_cost = cost.total_cost_fitting;

							// update count of solutions
							// fitting into outline
							layout_fit_counter++;
						}

						// memorize best solution which fits into outline
						if (fitting_cost < best_cost) {

							best_cost = fitting_cost;
							corb.storeBestCBLs();
							valid_layout_found = best_sol_found = true;
						}
					}
				}

				// after phase transition, skip current global iteration
				// in order to consider updated cost function
				if (SA_phase_two_init) {
					break;
				}
				// consider next loop iteration
				else {
					ii++;
				}
			}
		}

		// determine ratio of solutions fitting into outline in current temp step;
		// note that during the next temp step this ratio is fixed in order to
		// avoid sudden changes of related cost terms during few iterations
		if (accepted_ops > 0) {
			fitting_layouts_ratio = static_cast<double>(layout_fit_counter) / accepted_ops;
		}
		else {
			fitting_layouts_ratio = 0.0;
		}

		// determine avg cost for temp step
		if (accepted_ops > 0) {
			avg_cost /= accepted_ops;
		}

		// determine accepted-ops ratio
		accepted_ops_ratio = static_cast<double>(accepted_ops) / ii;

		if (this->logMax()) {
			std::cout << "SA> Step done:" << std::endl;
			std::cout << "SA>  new best solution found: " << best_sol_found << std::endl;
			std::cout << "SA>  accept-ops ratio: " << accepted_ops_ratio << std::endl;
			std::cout << "SA>  valid-layouts ratio: " << fitting_layouts_ratio << std::endl;
			std::cout << "SA>  avg cost: " << avg_cost << std::endl;
			std::cout << "SA>  temp: " << cur_temp << std::endl;
		}

		// log temperature step
		TempStep cur_step;
		cur_step.step = i;
		cur_step.temp = cur_temp;
		cur_step.avg_cost = avg_cost;
		cur_step.new_best_sol_found = best_sol_found;
		cur_step.cost_best_sol = best_cost;
		this->tempSchedule.push_back(std::move(cur_step));

		// update SA temperature
		cooling_phase = this->updateTemp(cur_temp, i, i_valid_layout_found);

		// consider next outer step
		i++;
	}

	if (this->logMed()) {
		std::cout << "SA> Done" << std::endl;
		std::cout << std::endl;
	}

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "<- FloorPlanner::performSA : " << valid_layout_found << std::endl;
	}

	return valid_layout_found;
}

FloorPlanner::TempPhase FloorPlanner::updateTemp(double& cur_temp, int const& iteration, int const& iteration_first_valid_layout) const {
	float loop_factor;
	double prev_temp;
	TempPhase phase;
	std::vector<double> prev_avg_cost;
	double std_dev_avg_cost;
	unsigned i, temp_schedule_size;

	prev_temp = cur_temp;

	// consider reheating in case the SA search has converged in some (possibly local) minima
	//
	// determine std dev of avg cost if some previous temperature steps exist
	temp_schedule_size = this->tempSchedule.size();
	if (temp_schedule_size >= FloorPlanner::SA_REHEAT_COST_SAMPLES) {

		for (i = 1; i <= FloorPlanner::SA_REHEAT_COST_SAMPLES; i++) {
			prev_avg_cost.push_back(this->tempSchedule[temp_schedule_size - i].avg_cost);
		}

		std_dev_avg_cost = Math::stdDev(prev_avg_cost);
	}
	// else ignore std dev by setting it above limit
	else {
		std_dev_avg_cost = FloorPlanner::SA_REHEAT_STD_DEV_COST_LIMIT + 1;
	}

	// phase 3; brief reheating due to cost convergence
	if (std_dev_avg_cost <= FloorPlanner::SA_REHEAT_STD_DEV_COST_LIMIT) {

		cur_temp *= this->schedule.temp_factor_phase3;

		phase = TempPhase::PHASE_3;
	}
	// phase 1; adaptive cooling (slows down from schedule.temp_factor_phase1 to
	// schedule.temp_factor_phase1_limit)
	else if (iteration_first_valid_layout == Point::UNDEF) {

		loop_factor = (this->schedule.temp_factor_phase1_limit - this->schedule.temp_factor_phase1) *
			static_cast<float>(iteration - 1) / (this->schedule.loop_limit - 1.0);

		// note that loop_factor is additive in this case; the cooling factor is
		// increased w/ increasing iterations
		cur_temp *= this->schedule.temp_factor_phase1 + loop_factor;

		phase = TempPhase::PHASE_1;
	}
	// phase 2; reheating and converging (initially reheats and then increases cooling
	// rate faster, i.e., heating factor is decreased w/ increasing iterations to
	// enable convergence)
	else {
		// note that loop_factor must only consider the remaining iteration range
		loop_factor = 1.0 - static_cast<float>(iteration - iteration_first_valid_layout) /
			static_cast<float>(this->schedule.loop_limit - iteration_first_valid_layout);

		cur_temp *= this->schedule.temp_factor_phase2 * loop_factor;

		phase = TempPhase::PHASE_2;
	}

	if (this->logMax()) {
		std::cout << "SA>  (new) temp-update factor: " << cur_temp / prev_temp << " (phase " << phase << ")" << std::endl;
	}

	return phase;
}

void FloorPlanner::initSA(CorblivarCore& corb, std::vector<double>& cost_samples, int& innerLoopMax, double& init_temp) {
	int i;
	int accepted_ops;
	bool op_success;
	double cur_cost, prev_cost, cost_diff;

	// reset max cost
	this->max_cost_WL = 0.0;
	this->max_cost_routing_util = 0.0;
	this->max_cost_TSVs = 0;
	this->max_cost_thermal = 0.0;
	this->max_cost_alignments = 0.0;

	// reset temperature-schedule log
	this->tempSchedule.clear();

	// backup initial CBLs
	corb.backupCBLs();

	// init SA parameter: inner loop ops
	innerLoopMax = std::pow(static_cast<double>(this->blocks.size()), this->schedule.loop_factor);

	/// initial sampling
	//
	if (this->logMed()) {
		std::cout << "SA> Perform initial solution-space sampling..." << std::endl;
	}

	// init cost; ignore alignment here
	this->generateLayout(corb);
	cur_cost = this->evaluateLayout(corb.getAlignments()).total_cost;

	// perform some random operations, for SA temperature = 0.0
	// i.e., consider only solutions w/ improved cost
	// track acceptance ratio and cost (phase one, area and AR mismatch)
	// also trigger cost function to assume no fitting layouts
	i = 1;
	accepted_ops = 0;
	cost_samples.reserve(SA_SAMPLING_LOOP_FACTOR * this->blocks.size());

	while (i <= SA_SAMPLING_LOOP_FACTOR * static_cast<int>(this->blocks.size())) {

		// trigger random op; assume some fitting layout was found previously such
		// that not only blocks exceeding the outline are adapted but rather
		// random operations are performed
		op_success = layoutOp.performLayoutOp(corb, 1);

		if (op_success) {

			prev_cost = cur_cost;

			// generate layout
			this->generateLayout(corb);
			// evaluate layout, new cost
			cur_cost = this->evaluateLayout(corb.getAlignments()).total_cost;
			// cost difference
			cost_diff = cur_cost - prev_cost;

			// solution w/ worse cost, revert
			if (cost_diff > 0.0) {
				// revert last op
				layoutOp.performLayoutOp(corb, 1, false, true);
				// reset cost according to reverted CBL
				cur_cost = prev_cost;
			}
			// accept solution w/ improved cost
			else {
				// update ops count
				accepted_ops++;
			}
			// store cost
			cost_samples.push_back(cur_cost);

			i++;
		}
	}

	// init SA parameter: start temp, depends on std dev of costs [Huan86, see
	// Shahookar91]
	init_temp = Math::stdDev(cost_samples) * this->schedule.temp_init_factor;

	if (this->logMed()) {
		std::cout << "SA> Done; std dev of cost: " << Math::stdDev(cost_samples) << ", initial temperature: " << init_temp << std::endl;
		std::cout << "SA> " << std::endl;
		std::cout << "SA> Perform simulated annealing process..." << std::endl;
		std::cout << "SA> Phase I: packing blocks into outline..." << std::endl;
		std::cout << "SA> " << std::endl;
	}

	// restore initial CBLs
	corb.restoreCBLs();
}

void FloorPlanner::finalize(CorblivarCore& corb, bool const& determ_overall_cost, bool const& handle_corblivar) {
	struct timeb end;
	std::stringstream runtime;
	bool valid_solution;
	double x, y;
	Cost cost;
	unsigned i;
	int clustered_TSVs;
	std::map<double, Clustering::Hotspot, std::greater<double>>::iterator it_hotspots;
	double avg_peak_temp, avg_base_temp, avg_temp_gradient, avg_score, avg_bins_count;

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "-> FloorPlanner::finalize(" << &corb << ", " << determ_overall_cost << ", " << handle_corblivar << ")" << std::endl;
	}

	// consider as regular Corblivar run
	if (handle_corblivar) {
		// apply best solution, if available, as final solution
		valid_solution = corb.applyBestCBLs(this->logMin());
		// generate final layout
		this->generateLayout(corb, this->opt_flags.alignment);
	}

	// determine final cost, also for non-Corblivar calls
	if (!handle_corblivar || valid_solution) {

		// determine overall blocks outline; reasonable die outline for whole
		// 3D-IC stack
		x = y = 0.0;
		for (Block const& b : this->blocks) {
			x = std::max(x, b.bb.ur.x);
			y = std::max(y, b.bb.ur.y);
		}

		// shrink fixed outline considering the final layout
		if (this->IC.outline_shrink) {

			this->resetDieProperties(x, y);
		}

		// determine cost terms and overall cost
		cost = this->evaluateLayout(corb.getAlignments(), 1.0, true, false, true);

		// logging IO_conf.results; consider non-normalized, actual values
		if (this->logMin()) {

			std::cout << "Corblivar> Characteristica of final solution:" << std::endl;

			// overall cost only encode a useful number in case the whole
			// optimization run is done, i.e., not for reading in given
			// solution files
			if (determ_overall_cost) {
				std::cout << "Corblivar> Final (adapted) cost: " << cost.total_cost << std::endl;
				this->IO_conf.results << "Final (adapted) cost: " << cost.total_cost << std::endl;
			}

			std::cout << "Corblivar> Max blocks-outline / die-outline ratio: " << cost.area_actual_value << std::endl;
			this->IO_conf.results << "Max blocks-outline / die-outline ratio: " << cost.area_actual_value << std::endl;
			this->IO_conf.results << std::endl;

			std::cout << "Corblivar> Overall deadspace [%]: " << 100.0 * (this->IC.stack_deadspace / this->IC.stack_area) << std::endl;
			this->IO_conf.results << "Overall deadspace [%]: " << 100.0 * (this->IC.stack_deadspace / this->IC.stack_area) << std::endl;
			this->IO_conf.results << std::endl;

			std::cout << "Corblivar> Overall blocks outline (reasonable stack outline):" << std::endl;
			std::cout << "Corblivar>  x = " << x << std::endl;
			std::cout << "Corblivar>  y = " << y << std::endl;
			this->IO_conf.results << "Overall blocks outline (reasonable stack outline):" << std::endl;
			this->IO_conf.results << " x = " << x << std::endl;
			this->IO_conf.results << " y = " << y << std::endl;
			this->IO_conf.results << std::endl;

			std::cout << "Corblivar> Alignment mismatches [um]: " << cost.alignments_actual_value << std::endl;
			this->IO_conf.results << "Alignment mismatches [um]: " << cost.alignments_actual_value << std::endl;
			this->IO_conf.results << std::endl;

			std::cout << "Corblivar> HPWL: " << cost.HPWL_actual_value << std::endl;
			this->IO_conf.results << "HPWL: " << cost.HPWL_actual_value << std::endl;
			this->IO_conf.results << std::endl;

			std::cout << "Corblivar> Max routing utilization: " << cost.routing_util_actual_value << std::endl;
			this->IO_conf.results << "Max routing utilization: " << cost.routing_util_actual_value << std::endl;
			this->IO_conf.results << std::endl;

			std::cout << "Corblivar> TSVs: " << cost.TSVs_actual_value << std::endl;
			this->IO_conf.results << "TSVs: " << cost.TSVs_actual_value << std::endl;

			std::cout << "Corblivar>  TSV islands: " << this->TSVs.size() << std::endl;
			this->IO_conf.results << " TSV islands: " << this->TSVs.size() << std::endl;

			clustered_TSVs = 0;
			for (i = 0; i < this->TSVs.size(); i++) {
				clustered_TSVs += this->TSVs[i].TSVs_count;
			}

			if (!this->TSVs.empty()) {
				std::cout << "Corblivar>  Avg TSV count per island: " << clustered_TSVs / this->TSVs.size() << std::endl;
				this->IO_conf.results << " Avg TSV count per island: " << clustered_TSVs / this->TSVs.size() << std::endl;
			}

			std::cout << "Corblivar>  Deadspace utilization by TSVs [%]: " << 100.0 * cost.TSVs_area_deadspace_ratio << std::endl;
			this->IO_conf.results << " Deadspace utilization by TSVs [%]: " << 100.0 * cost.TSVs_area_deadspace_ratio << std::endl;
			this->IO_conf.results << std::endl;

			std::cout << "Corblivar> Hotspot regions (on lowest layer 0): " << this->clustering.hotspots.size() << std::endl;
			this->IO_conf.results << "Hotspot regions (on lowest layer 0): " << this->clustering.hotspots.size() << std::endl;

			if (!this->clustering.hotspots.empty()) {

				avg_peak_temp = avg_base_temp = avg_temp_gradient = avg_score = avg_bins_count = 0.0;
				for (it_hotspots = this->clustering.hotspots.begin(); it_hotspots != this->clustering.hotspots.end(); ++it_hotspots) {
					avg_peak_temp += (*it_hotspots).second.peak_temp;
					avg_base_temp += (*it_hotspots).second.base_temp;
					avg_temp_gradient += (*it_hotspots).second.temp_gradient;
					avg_score += (*it_hotspots).second.score;
					avg_bins_count += (*it_hotspots).second.bins.size();
				}

				avg_peak_temp /= this->clustering.hotspots.size();
				avg_base_temp /= this->clustering.hotspots.size();
				avg_temp_gradient /= this->clustering.hotspots.size();
				avg_score /= this->clustering.hotspots.size();
				avg_bins_count /= this->clustering.hotspots.size();

				std::cout << "Corblivar>  Avg peak temp: " << avg_peak_temp << std::endl;
				this->IO_conf.results << " Avg peak temp: " << avg_peak_temp << std::endl;
				std::cout << "Corblivar>  Avg base temp: " << avg_base_temp << std::endl;
				this->IO_conf.results << " Avg base temp: " << avg_base_temp << std::endl;
				std::cout << "Corblivar>  Avg temp gradient: " << avg_temp_gradient << std::endl;
				this->IO_conf.results << " Avg temp gradient: " << avg_temp_gradient << std::endl;
				std::cout << "Corblivar>  Avg score: " << avg_score << std::endl;
				this->IO_conf.results << " Avg score: " << avg_score << std::endl;
				std::cout << "Corblivar>  Avg bin count: " << avg_bins_count << std::endl;
				this->IO_conf.results << " Avg bin count: " << avg_bins_count << std::endl;
			}
			this->IO_conf.results << std::endl;

			std::cout << "Corblivar> Temp cost (estimated max temp for lowest layer [K]): " << cost.thermal_actual_value << std::endl;
			this->IO_conf.results << "Temp cost (estimated max temp for lowest layer [K]): " << cost.thermal_actual_value << std::endl;
			this->IO_conf.results << std::endl;

			std::cout << std::endl;
		}
	}

	// generate temperature-schedule data
	IO::writeTempSchedule(*this);

	// generate floorplan plots
	IO::writeFloorplanGP(*this, corb.getAlignments());

	// generate Corblivar data if solution file is used as output
	if (handle_corblivar && this->IO_conf.solution_out.is_open()) {
		this->IO_conf.solution_out << corb.CBLsString() << std::endl;
		this->IO_conf.solution_out.close();

		// delete file in case no valid solution was generated
		if (!valid_solution) {
			remove(this->IO_conf.solution_file.c_str());
		}
	}

	// thermal-analysis files
	if ((!handle_corblivar || valid_solution) && this->IO_conf.power_density_file_avail) {
		// generate power, thermal, routing-utilization and TSV-density maps
		IO::writeMaps(*this);
		// generate HotSpot files
		IO::writeHotSpotFiles(*this);
	}

	// determine overall runtime
	ftime(&end);
	if (this->logMin()) {
		runtime << "Runtime: " << (1000.0 * (end.time - this->time_start.time) + (end.millitm - this->time_start.millitm)) / 1000.0 << " s";
		std::cout << "Corblivar> " << runtime.str() << std::endl;
		this->IO_conf.results << runtime.str() << std::endl;
	}

	// close IO_conf.results file
	this->IO_conf.results.close();

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "<- FloorPlanner::finalize" << std::endl;
	}
}

bool FloorPlanner::generateLayout(CorblivarCore& corb, bool const& perform_alignment) {
	bool ret;

	// generate layout
	ret = corb.generateLayout(perform_alignment);

	// annotate alignment success/failure in blocks; required for maintaining
	// succeeded alignments during subsequent packing
	if (this->opt_flags.alignment && this->layoutOp.parameters.packing_iterations > 0) {
		// ignore related cost; use dummy variable
		Cost dummy;
		// also don't derive TSVs; not required here
		this->evaluateAlignments(dummy, corb.getAlignments(), false);
	}

	// perform packing if desired; perform on each die for each
	// dimension separately and subsequently; multiple iterations may
	// provide denser packing configurations
	for (int d = 0; d < this->IC.layers; d++) {

		CorblivarDie& die = corb.editDie(d);

		// sanity check for empty dies
		if (!die.getCBL().empty()) {

			for (int i = 1; i <= this->layoutOp.parameters.packing_iterations; i++) {
				die.performPacking(Direction::HORIZONTAL);
				die.performPacking(Direction::VERTICAL);
			}
		}

		// dbg: sanity check for valid layout
		if (FloorPlanner::DBG_LAYOUT) {

			// if true, the layout is buggy, i.e., invalid
			if (die.debugLayout()) {
				return false;
			}
		}
	}

	return ret;
}

// adaptive cost model w/ two phases: first phase considers only cost for packing into
// outline, second phase considers further factors like WL, thermal distr, etc.
FloorPlanner::Cost FloorPlanner::evaluateLayout(std::vector<CorblivarAlignmentReq> const& alignments, double const& fitting_layouts_ratio, bool const& SA_phase_two, bool const& set_max_cost, bool const& finalize) {
	Cost cost;

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "-> FloorPlanner::evaluateLayout(" << &alignments << ", " << fitting_layouts_ratio << ", " << SA_phase_two << ", " << set_max_cost << ", " << finalize << ")" << std::endl;
	}

	// phase one: consider only cost for packing into outline
	if (!SA_phase_two) {

		// area and outline cost, already weighted w/ global weight factor
		this->evaluateAreaOutline(cost, fitting_layouts_ratio);

		// determine total cost
		//
		// invert weight of area and outline cost since it's the only cost term
		cost.total_cost = (1.0 / FloorPlanner::SA_COST_WEIGHT_AREA_OUTLINE) * cost.area_outline;
	}
	// phase two: consider further cost factors
	else {
		// area and outline cost, already weighted w/ global weight factor
		this->evaluateAreaOutline(cost, fitting_layouts_ratio);

		// determine interconnects cost; also determines hotspot regions and
		// clusters signal TSVs accordingly
		//
		// for finalize calls, we need to initialize the max_cost
		if (finalize) {
			this->evaluateInterconnects(cost, alignments, true);
		}
		else if (this->opt_flags.interconnects) {
			this->evaluateInterconnects(cost, alignments, set_max_cost);
		}
		// no optimization considered, reset cost to zero
		else {
			cost.HPWL = cost.HPWL_actual_value = 0.0;
			cost.routing_util = cost.routing_util_actual_value = 0.0;
			cost.TSVs = cost.TSVs_actual_value = 0;
			cost.TSVs_area_deadspace_ratio = 0.0;
		}

		// cost for failed alignments (i.e., alignment mismatches)
		//
		// also annotates failed request, this provides feedback for further
		// alignment optimization; also derives and stores TSV islands
		//
		// for finalize calls, we need to initialize the max_cost
		if (finalize) {
			this->evaluateAlignments(cost, alignments, true, true, true);
		}
		else if (this->opt_flags.alignment) {
			this->evaluateAlignments(cost, alignments, true, set_max_cost);
		}
		// no optimization considered, reset cost to zero
		else {
			cost.alignments = cost.alignments_actual_value = 0.0;
		}

		// temperature-distribution cost and profile
		//
		// note that vertical buses and TSV islands impact heat conduction, thus
		// the block alignment / bus structures and interconnects are analysed
		// before thermal distribution
		//
		// for finalize calls, we need to initialize the max_cost
		if (finalize) {
			this->evaluateThermalDistr(cost, true);
		}
		else if (this->opt_flags.thermal) {
			this->evaluateThermalDistr(cost, set_max_cost);
		}
		// no optimization considered, reset cost to zero
		else {
			cost.thermal = cost.thermal_actual_value = 0.0;
		}

		// for finalize calls, re-determine interconnects and the resulting
		// thermal profile in order to properly model hotspot cluster and TSV
		// islands; the final / best solution's thermal distribution---which was
		// determined above and which is the input data for hotspot determination
		// and TSV clustering---is thus properly addressed / improved by according
		// TSV clustering
		if (finalize) {

			this->evaluateInterconnects(cost, alignments);
			this->evaluateAlignments(cost, alignments, true, false, true);

			this->evaluateThermalDistr(cost);
		}

		// determine total cost; weight and sum up cost terms
		cost.total_cost =
			FloorPlanner::SA_COST_WEIGHT_OTHERS * (
					this->weights.WL * cost.HPWL
					+ this->weights.routing_util * cost.routing_util
					+ this->weights.TSVs * cost.TSVs
					+ this->weights.alignment * cost.alignments
					+ this->weights.thermal * cost.thermal
				)
			// area, outline cost is already weighted
			+ cost.area_outline;

		// determine total cost assuming a fitting ratio of 1.0
		cost.total_cost_fitting =
			FloorPlanner::SA_COST_WEIGHT_OTHERS * (
					this->weights.WL * cost.HPWL
					+ this->weights.routing_util * cost.routing_util
					+ this->weights.TSVs * cost.TSVs
					+ this->weights.alignment * cost.alignments
					+ this->weights.thermal * cost.thermal
				)
			// consider only area term for ratio 1.0, see evaluateAreaOutline
			+ cost.area_actual_value * FloorPlanner::SA_COST_WEIGHT_AREA_OUTLINE;
	}

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "DBG_LAYOUT> Total cost: " << cost.total_cost << std::endl;
		std::cout << "DBG_LAYOUT>  HPWL cost: " << cost.HPWL << std::endl;
		std::cout << "DBG_LAYOUT>  Routing-utilization cost: " << cost.routing_util << std::endl;
		std::cout << "DBG_LAYOUT>  TSVs cost: " << cost.TSVs << std::endl;
		std::cout << "DBG_LAYOUT>  Alignments cost: " << cost.alignments << std::endl;
		std::cout << "DBG_LAYOUT>  Thermal cost: " << cost.thermal << std::endl;
	}

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "<- FloorPlanner::evaluateLayout : " << cost << std::endl;
	}

	return cost;
}

void FloorPlanner::evaluateThermalDistr(Cost& cost, bool const& set_max_cost) {

	// generate power maps based on layout and blocks' power densities
	this->thermalAnalyzer.generatePowerMaps(this->IC.layers, this->blocks,
			this->getOutline(), this->power_blurring_parameters);

	// adapt power maps to account for TSVs' impact
	this->thermalAnalyzer.adaptPowerMaps(this->IC.layers, this->TSVs, this->nets, this->power_blurring_parameters);

	// perform actual thermal analysis
	this->thermalAnalyzer.performPowerBlurring(this->thermal_analysis, this->IC.layers,
			this->power_blurring_parameters);

	// memorize max cost; initial sampling
	if (set_max_cost) {
		this->max_cost_thermal = this->thermal_analysis.cost_temp;
	}

	// store normalized temp cost
	cost.thermal = this->thermal_analysis.cost_temp / this->max_cost_thermal;
	// store actual temp value
	cost.thermal_actual_value = this->thermal_analysis.max_temp;
};

// adaptive cost model: terms for area and AR mismatch are _mutually_ depending on ratio
// of feasible solutions (solutions fitting into outline), leveraged from Chen et al 2006
// ``Modern floorplanning based on B*-Tree and fast simulated annealing''
void FloorPlanner::evaluateAreaOutline(FloorPlanner::Cost& cost, double const& fitting_layouts_ratio) const {
	double cost_area;
	double cost_outline;
	double max_outline_x;
	double max_outline_y;
	int i;
	std::vector<double> dies_AR;
	std::vector<double> dies_area;
	bool layout_fits_in_fixed_outline;

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "-> FloorPlanner::evaluateAreaOutline(" << fitting_layouts_ratio << ")" << std::endl;
	}

	dies_AR.reserve(this->IC.layers);
	dies_area.reserve(this->IC.layers);

	layout_fits_in_fixed_outline = true;
	// determine outline and area
	for (i = 0; i < this->IC.layers; i++) {

		// determine outline for blocks on all dies separately
		max_outline_x = max_outline_y = 0.0;
		for (Block const& block : this->blocks) {

			if (block.layer == i) {
				// update max outline coords
				max_outline_x = std::max(max_outline_x, block.bb.ur.x);
				max_outline_y = std::max(max_outline_y, block.bb.ur.y);
			}
		}

		// area, represented by blocks' outline; normalized to die area
		dies_area.push_back((max_outline_x * max_outline_y) / (this->IC.die_area));

		// aspect ratio; used to guide optimization towards fixed outline
		if (max_outline_y > 0.0) {
			dies_AR.push_back(max_outline_x / max_outline_y);
		}
		// dummy value for empty dies; implies cost of 0.0 for this die, i.e. does
		// not impact cost function
		else {
			dies_AR.push_back(this->IC.die_AR);
		}

		// memorize whether layout fits into outline
		max_outline_x /= this->IC.outline_x;
		max_outline_y /= this->IC.outline_y;
		layout_fits_in_fixed_outline = layout_fits_in_fixed_outline && (max_outline_x <= 1.0 && max_outline_y <= 1.0);
	}

	// cost for AR mismatch, considering max violation guides towards fixed outline
	cost_outline = 0.0;
	for (i = 0; i < this->IC.layers; i++) {
		cost_outline = std::max(cost_outline, std::pow(dies_AR[i] - this->IC.die_AR, 2.0));
	}
	// store actual value
	cost.outline_actual_value = cost_outline;
	// determine cost function value
	cost_outline *= 0.5 * FloorPlanner::SA_COST_WEIGHT_AREA_OUTLINE * (1.0 - fitting_layouts_ratio);

	// cost for area, considering max value of (blocks-outline area) / (die-outline
	// area) guides towards balanced die occupation and area minimization
	cost_area = 0.0;
	for (i = 0; i < this->IC.layers; i++) {
		cost_area = std::max(cost_area, dies_area[i]);
	}
	// store actual value
	cost.area_actual_value = cost_area;
	// determine cost function value
	cost_area *= 0.5 * FloorPlanner::SA_COST_WEIGHT_AREA_OUTLINE * (1.0 + fitting_layouts_ratio);

	cost.area_outline = cost_outline + cost_area;
	cost.fits_fixed_outline = layout_fits_in_fixed_outline;

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "<- FloorPlanner::evaluateAreaOutline" << std::endl;
	}
}

void FloorPlanner::evaluateInterconnects(FloorPlanner::Cost& cost, std::vector<CorblivarAlignmentReq> const& alignments, bool const& set_max_cost) {
	int i;
	std::vector<Rect const*> blocks_to_consider;
	std::vector< std::vector<Clustering::Segments> > nets_segments;
	Rect bb, prev_bb;
	double prev_TSVs;
	double net_weight;
	RoutingUtilization::UtilResult util;
	TSV_Island* island;

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "-> FloorPlanner::evaluateInterconnects(" << &cost << ", " << &alignments << ", " << set_max_cost << ")" << std::endl;
	}

	// reset cost terms
	cost.HPWL = cost.HPWL_actual_value = 0.0;
	cost.routing_util = cost.routing_util_actual_value = 0.0;
	cost.TSVs = cost.TSVs_actual_value = 0;
	cost.TSVs_area_deadspace_ratio = 0.0;

	// reset TSVs
	this->TSVs.clear();

	// reset TSVs also from nets
	for (Net& cur_net : this->nets) {
		cur_net.TSVs.clear();
	}

	// reset routing-utilization estimation
	this->routingUtil.resetUtilMaps(this->IC.layers);

	// allocate vector for blocks to be considered
	blocks_to_consider.reserve(this->blocks.size());
	// allocate vector for nets' segments
	for (i = 0; i < this->IC.layers; i++) {
		nets_segments.emplace_back(std::vector<Clustering::Segments>());
	}

	// determine HPWL and TSVs for each net
	for (Net& cur_net : this->nets) {

		// set layer boundaries, i.e., determine lowest and uppermost layer of
		// net's blocks
		cur_net.setLayerBoundaries();

		// determine net weight, for routing-utilization estimation across
		// multiple layers
		net_weight = 1.0 / (cur_net.layer_top + 1 - cur_net.layer_bottom);

		if (Net::DBG) {
			std::cout << "DBG_NET> Determine interconnects for net " << cur_net.id << std::endl;
		}

		// trivial HPWL estimation, considering one global bounding box; required
		// to compare w/ other 3D floorplanning tools
		if (FloorPlanner::SA_COST_INTERCONNECTS_TRIVIAL_HPWL) {

			// resets blocks to be considered for each cur_net
			blocks_to_consider.clear();

			// blocks for cur_net on all layer
			for (Block const* b : cur_net.blocks) {
				blocks_to_consider.push_back(&b->bb);
			}

			// also consider routes to terminal pins
			for (Pin const* pin :  cur_net.terminals) {
				blocks_to_consider.push_back(&pin->bb);
			}

			// determine HPWL of related blocks using their bounding box;
			// consider center points of blocks instead their whole outline
			bb = Rect::determBoundingBox(blocks_to_consider, true);
			cost.HPWL += bb.w;
			cost.HPWL += bb.h;

			// consider impact on routing-utilization; only in case clustering
			// is not applied (otherwise this is only done after clustering)
			//
			// before TSVs are placed, the net shall impact the routing
			// utilization on all affected layers but with accordingly
			// down-scaled weight
			if (!this->layoutOp.parameters.signal_TSV_clustering) {

				for (i = cur_net.layer_bottom; i <= cur_net.layer_top; i++) {

					this->routingUtil.adaptUtilMap(i, bb, net_weight);
				}
			}

			if (Net::DBG) {
				std::cout << "DBG_NET> 		HPWL of bounding box of blocks to consider: " << (bb.w + bb. h) << std::endl;
			}
		}
		// more detailed estimate; consider HPWL on each layer separately using
		// layer-related bounding boxes
		else {
			// reset of prev_bb not required: for the next net, since we
			// previously determine the respective lower layer, we guarantee
			// that at least one block is in that lowermost layer, i.e., that
			// a non-empty bb can be constructed

			// determine HPWL on each related layer separately
			for (i = cur_net.layer_bottom; i <= cur_net.layer_top; i++) {

				// determine HPWL using the net's bounding box on the
				// current layer
				bb = cur_net.determBoundingBox(i);
				cost.HPWL += bb.w;
				cost.HPWL += bb.h;

				if (Net::DBG) {
					std::cout << "DBG_NET> 		HPWL of bounding box of blocks (in current and possibly upper layers) to consider: " << (bb.w + bb. h) << std::endl;
				}

				// determBoundingBox may also return empty bounding boxes,
				// namely for nets w/o blocks on the currently considered
				// layer. Then, we need to consider the non-empty box from
				// one of the layers below in order to provide a
				// reasonable net's bb
				if (bb.area == 0.0) {
					bb = prev_bb;
				}
				// memorize current non-empty bb as previous bb for next
				// iteration
				else {
					prev_bb = bb;
				}

				// for clustering; memorize bounding boxes for nets
				// connecting further up (i.e., requiring a TSV)
				if (this->layoutOp.parameters.signal_TSV_clustering) {
				
					if (i < cur_net.layer_top) {

						// store bb as net segment; store in layer-wise vector,
						// which is easier to handle during clustering
						nets_segments[i].push_back({&cur_net, bb});

						if (Net::DBG) {
							std::cout << "DBG_NET> 		Consider bounding box for clustering; HPWL: " << (bb.w + bb.h) << std::endl;
						}
					}
				}
				// consider impact of bounding boxes on
				// routing-utilization and place dummy TSVs (not optimized
				// but rather placed into the center of each bounding box,
				// only required for proper thermal simulation)
				//
				// only in case clustering is not applied, otherwise
				// proper routing utilization and TSV placement is done by
				// clustering itself
				else {
					// for ignored clustering, the net shall impact
					// the routing utilization on all affected layers
					// but with accordingly down-scaled weight
					this->routingUtil.adaptUtilMap(i, bb, net_weight);

					// place dummy TSVs in all but uppermost affected
					// layer; required for proper thermal simulation
					//
					if (i < cur_net.layer_top) {

						// define new trivial island, with one TSV
						island = new TSV_Island(
								// net id and layer
								"net_" + std::to_string(cur_net.id) + "_" + std::to_string(i),
								// one TSV count
								1,
								// TSV pitch; required for proper scaling
								// of TSV island
								this->IC.TSV_pitch,
								// reference point for placement
								// of dummy TSV is net bb
								bb,
								// layer assignment
								i
							);

						// here, greedy shifting is ignored for simplicity and runtime
						//

						// memorize TSV in global container
						this->TSVs.push_back(*island);
					}
				}
			}
		}

		if (Net::DBG) {
			prev_TSVs = cost.TSVs;
		}

		// determine TSV count
		cost.TSVs += cur_net.layer_top - cur_net.layer_bottom;

		if (Net::DBG) {
			std::cout << "DBG_NET>  TSVs required: " << cost.TSVs - prev_TSVs << std::endl;
		}
	}

	// perform clustering of regular signal TSVs into TSV islands
	if (!FloorPlanner::SA_COST_INTERCONNECTS_TRIVIAL_HPWL && this->layoutOp.parameters.signal_TSV_clustering) {

		// actual clustering
		this->clustering.clusterSignalTSVs(this->nets, nets_segments, this->TSVs, this->IC.TSV_pitch, this->thermal_analysis);

		// after clustering, we can obtain a more accurate wirelength and
		// routing-utilization estimation by considering TSVs' positions as well
		//
		// reset previous HPWL
		cost.HPWL = 0.0;

		// determine HPWL for each net
		for (Net& cur_net : this->nets) {

			if (Net::DBG) {
				std::cout << "DBG_NET> Determine HPWL (w/ consideration of TSV positions) for net " << cur_net.id << std::endl;
			}

			// determine HPWL on each related layer separately
			for (i = cur_net.layer_bottom; i <= cur_net.layer_top; i++) {

				// determine the net's bounding box on the current layer
				bb = cur_net.determBoundingBox(i);

				// add HPWL of bb to cost
				cost.HPWL += bb.w;
				cost.HPWL += bb.h;

				// update related routing-utilization map; each single net
				// has default weight of 1.0
				this->routingUtil.adaptUtilMap(i, bb, 1.0);

				if (Net::DBG) {
					std::cout << "DBG_NET> 		HPWL to consider: " << (bb.w + bb. h) << std::endl;
				}
			}
		}
	}
	
	// consider alignments' HWPL components and related routing utilization; note that
	// this function does not account for the alignments' TSVs, this is done in
	// evaluateAlignments(); also note that this rough estimate is only required if
	// alignments are not directly optimized, otherwise the HPWL and utilization
	// estimation in evaluateAlignments() is more precise
	if (!FloorPlanner::SA_COST_INTERCONNECTS_TRIVIAL_HPWL && !this->opt_flags.alignment) {
		cost.HPWL += this->evaluateAlignmentsHPWL(alignments);
	}

	// also consider lengths of (regular signal) TSVs in HPWL; each TSV has to pass
	// the whole Si layer and the bonding layer
	if (!FloorPlanner::SA_COST_INTERCONNECTS_TRIVIAL_HPWL) {
		cost.HPWL += cost.TSVs * (this->IC.die_thickness + this->IC.bond_thickness);
	}

	// determine by TSVs occupied deadspace amount
	cost.TSVs_area_deadspace_ratio = (cost.TSVs * std::pow(this->IC.TSV_pitch, 2)) / this->IC.stack_deadspace;

	// determine routing utilization
	util = this->routingUtil.determCost();
	cost.routing_util = util.cost;
	cost.routing_util_actual_value = util.max_util;

	// memorize max cost; initial sampling
	if (set_max_cost) {
		this->max_cost_WL = cost.HPWL;
		this->max_cost_TSVs = cost.TSVs;
		this->max_cost_routing_util = cost.routing_util;
	}

	// store actual values
	cost.HPWL_actual_value = cost.HPWL;
	cost.TSVs_actual_value = cost.TSVs;

	// normalized values; max value refers to initial sampling, thus sanity check for
	// zero cost is not required
	cost.HPWL /= this->max_cost_WL;
	cost.routing_util /= this->max_cost_routing_util;

	// sanity check for normalizing TSVs cost; zero max cost applies to 2D
	// floorplanning
	if (this->max_cost_TSVs != 0) {
		cost.TSVs /= this->max_cost_TSVs;
	}

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "<- FloorPlanner::evaluateInterconnects" << std::endl;
	}
}

// costs are derived from spatial mismatch b/w blocks' alignment and intended alignment;
// note that this function also marks requests as failed or successful
void FloorPlanner::evaluateAlignments(Cost& cost, std::vector<CorblivarAlignmentReq> const& alignments, bool const& derive_TSVs, bool const& set_max_cost, bool const& finalize) {
	Rect intersect, bb, routing_bb;
	int prev_TSVs;
	int layer, min_layer, max_layer;
	CorblivarAlignmentReq::Evaluate eval;
	TSV_Island* island;
	bool shift;
	RoutingUtilization::UtilResult util;

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "-> FloorPlanner::evaluateAlignments(" << &cost << ", " << &alignments << ", " << derive_TSVs << ", " << set_max_cost << ", " << finalize << ")" << std::endl;
	}

	cost.alignments = cost.alignments_actual_value = 0.0;
	prev_TSVs = cost.TSVs_actual_value;

	// evaluate all alignment requests
	for (CorblivarAlignmentReq const& req : alignments) {

		// actual evaluation handler
		eval = req.evaluate();
		cost.alignments += eval.cost;
		cost.alignments_actual_value += eval.actual_mismatch;

		// blocks are on the same layer, i.e., neither vertical bus nor require
		// TSVs; however, this alignment impacts WL and routing utilization
		if (req.s_i->layer == req.s_j->layer) {

			routing_bb = Rect::determBoundingBox(req.s_i->bb, req.s_j->bb);

			// add HPWL of bb to cost
			cost.HPWL_actual_value += routing_bb.w * req.signals;
			cost.HPWL_actual_value += routing_bb.h * req.signals;

			// update related routing-utilization map; weight according to
			// signals' count
			this->routingUtil.adaptUtilMap(req.s_i->layer, routing_bb, req.signals);
		}

		// derive TSVs for vertical buses if desired or for finalize runs
		if (derive_TSVs || finalize) {

			// consider block intersections, independent of defined alignment;
			// this way, all _embedded_ vertical buses arising from different
			// (not necessarily as vertical buses defined) alignment requests
			// will be considered
			intersect = Rect::determineIntersection(req.s_i->bb, req.s_j->bb);

			// however, for cases w/o block intersections, we still need to
			// check whether blocks are placed on separate dies; then, they
			// require TSV islands as well
			if (intersect.area == 0.0 && (req.s_i->layer != req.s_j->layer)) {

				// however, block alignments with respect to RBOD (since
				// RBOD layer is -1 and RBOD.bb is zero, they will be
				// triggered here) should be ignored as well
				if (req.s_i->id != RBOD::ID && req.s_j->id != RBOD::ID) {

					// here, we consider the blocks' bounding box for
					// guiding placement of the required TSV island
					intersect = Rect::determBoundingBox(req.s_i->bb, req.s_j->bb);
				}
			}

			// either an embedded vertical bus or blocks on separate dies but
			// not overlapping; both cases require TSVs
			if (intersect.area != 0.0) {

				// derive TSVs in all affected layers
				min_layer = std::min(req.s_i->layer, req.s_j->layer);
				max_layer = std::max(req.s_i->layer, req.s_j->layer);

				for (layer = min_layer; layer < max_layer; layer++) {

					// define new island
					island = new TSV_Island(
							// bus id
							"bus_" + req.s_i->id + "_" + req.s_j->id,
							// signal / TSV count
							req.signals,
							// TSV pitch; required for proper scaling
							// of TSV island
							this->IC.TSV_pitch,
							// blocks intersection; reference
							// point for placement of vertical
							// bus / TSV island
							intersect,
							// layer assignment
							layer,
							// for vertical buses, provide
							// specific width according to
							// alignment requirement
							req.vertical_bus() ? req.alignment_x : -1.0
						);

					// perform greedy shifting in case new island
					// overlaps with any previous one
					//
					shift = true;
					while (shift) {

						shift = false;

						for (TSV_Island const& prev_island : this->TSVs) {

							if (prev_island.layer != island->layer) {
								continue;
							}

							if (Rect::rectsIntersect(prev_island.bb, island->bb)) {

								Rect::greedyShiftingRemoveIntersection(prev_island.bb, island->bb);
								shift = true;
							}
						}
					}

					// memorize TSV in global container
					this->TSVs.push_back(*island);

					// determine the HPWL components and routing
					// utilization; net segments are to be considered
					// for routing b/w the block and TSV island on the
					// lowermost and on the topmost layer
					if (layer == min_layer) {

						// determine the routing bb, connecting
						// the TSV island and the block on the
						// layer of interest
						if (layer == req.s_i->layer) {
							routing_bb = Rect::determBoundingBox(island->bb, req.s_i->bb);
						}
						else {
							routing_bb = Rect::determBoundingBox(island->bb, req.s_j->bb);
						}

						// add HPWL of bb to cost
						cost.HPWL_actual_value += routing_bb.w * req.signals;
						cost.HPWL_actual_value += routing_bb.h * req.signals;

						// update related routing-utilization map;
						// weight according to signals' count
						this->routingUtil.adaptUtilMap(layer, routing_bb, req.signals);
					}
					// the TSV itself is not placed on the topmost
					// layer, but rather the one below; the routing
					// utilization, however, is impacting the top
					// layer where routing is conducted from the block
					// to the TSV landing pads
					if (layer == max_layer - 1) {

						// determine the routing bb, connecting
						// the TSV island and the block on the
						// layer of interest
						if (layer + 1 == req.s_i->layer) {
							routing_bb = Rect::determBoundingBox(island->bb, req.s_i->bb);
						}
						else {
							routing_bb = Rect::determBoundingBox(island->bb, req.s_j->bb);
						}

						// add HPWL of bb to cost
						cost.HPWL_actual_value += routing_bb.w * req.signals;
						cost.HPWL_actual_value += routing_bb.h * req.signals;

						// update related routing-utilization map;
						// weight according to signals' count
						this->routingUtil.adaptUtilMap(layer + 1, routing_bb, req.signals);
					}

					// also update global TSV counter accordingly
					if (
						// only for buses _not_ defined as vertical buses;
						// this way, the cost function's minimization of
						// TSVs will not counteract the cost function's
						// block alignment of dedicated vertical buses
						!req.vertical_bus() ||
						// for finalize runs, however, consider all TSVs
						// for final layout evaluation
						finalize) {

							cost.TSVs_actual_value += req.signals;
					}
				}
			}
		}
	}

	// update routing utilization
	util = this->routingUtil.determCost();
	cost.routing_util = util.cost;
	cost.routing_util_actual_value = util.max_util;

	// consider lengths of additional TSVs in HPWL; each TSV has to pass the
	// whole Si layer and the bonding layer
	cost.HPWL_actual_value += (cost.TSVs_actual_value - prev_TSVs) * (this->IC.die_thickness + this->IC.bond_thickness);

	// memorize max cost; initial sampling; also consider HPWL and other adapted cost
	// terms
	if (set_max_cost) {
		this->max_cost_alignments = cost.alignments;
		this->max_cost_WL = cost.HPWL_actual_value;
		this->max_cost_TSVs = cost.TSVs_actual_value;
		this->max_cost_routing_util = cost.routing_util;
	}

	// update normalized cost; refers to max value from initial sampling
	if (this->max_cost_alignments != 0) {
		cost.alignments /= this->max_cost_alignments;
	}

	// update normalized routing utilization
	cost.routing_util /= this->max_cost_routing_util;

	// update normalized TSV cost; sanity check for zero TSVs cost; applies to
	// 2D floorplanning
	if (this->max_cost_TSVs != 0) {
		cost.TSVs = cost.TSVs_actual_value / this->max_cost_TSVs;
	}

	// update by TSVs occupied deadspace amount
	cost.TSVs_area_deadspace_ratio = (cost.TSVs_actual_value * std::pow(this->IC.TSV_pitch, 2)) / this->IC.stack_deadspace;

	// update normalized HPWL cost; sanity check for zero HPWL not required
	// since max cost are initialized during first phase
	cost.HPWL = cost.HPWL_actual_value / this->max_cost_WL;

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "<- FloorPlanner::evaluateAlignments : " << cost.alignments << std::endl;
	}
}

// separate determination of alignments' HPWL which is called from evaluateInterconnects;
// this way, the HPWL components of alignments are always (for active WL optimization)
// considered
double FloorPlanner::evaluateAlignmentsHPWL(std::vector<CorblivarAlignmentReq> const& alignments) {
	Rect bb;
	double HPWL = 0.0;
	int min_layer, max_layer;
	int net_weight;

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "-> FloorPlanner::evaluateAlignmentsHPWL(" << &alignments << ")" << std::endl;
	}

	// evaluate all alignment requests, i.e., consider WL encapsulated w/in (both
	// failed and successfully aligned) massive interconnects
	for (CorblivarAlignmentReq const& req : alignments) {

		// ignore alignments comprising _embedded_ vertical buses for two reasons:
		// first, they are handled (TSV-wise) in evaluateAlignments(); second,
		// they don't contribute to HPWL considering that only block-level
		// interconnects are considered, not the gate-level wires to connect to
		// TSV landing pads w/in blocks
		if (Rect::rectsIntersect(req.s_i->bb, req.s_j->bb)) {
			continue;
		}

		// derive blocks' bb; only consider center points since massive
		// interconnects (on average) will connect to pins within the block
		// outline, not the worst-case outer block boundaries
		//
		// deactivated for now, consider outer block boundaries, as it is done in
		// evaluateAlignments
		//bb = Rect::determBoundingBox(req.s_i->bb, req.s_j->bb, true);
		bb = Rect::determBoundingBox(req.s_i->bb, req.s_j->bb);

		// consider rough estimate for routing utilization: spread across all
		// affected layers
		min_layer = std::min(req.s_i->layer, req.s_j->layer);
		max_layer = std::max(req.s_i->layer, req.s_j->layer);
		// determine net weight, for routing-utilization estimation across
		// multiple layers
		net_weight = 1.0 / (max_layer + 1 - min_layer);

		for (int layer = min_layer; layer <= max_layer; layer++) {
			// update routing-utilization map; consider both (by layer count
			// down-scaled) net weight and signal count
			this->routingUtil.adaptUtilMap(layer, bb, net_weight * req.signals);
		}

		// determine by signal count weighted HPWL
		HPWL += (bb.w) * req.signals;
		HPWL += (bb.h) * req.signals;
	}

	if (FloorPlanner::DBG_CALLS_SA) {
		std::cout << "<- FloorPlanner::evaluateAlignmentsHPWL : " << HPWL << std::endl;
	}

	return HPWL;
}

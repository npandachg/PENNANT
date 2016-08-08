/*
 * Parallel.cc
 *
 *  Created on: May 31, 2013
 *      Author: cferenba
 *
 * Copyright (c) 2013, Los Alamos National Security, LLC.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style open-source
 * license; see top-level LICENSE file for full license text.
 */

#include "Parallel.hh"

#include <vector>
#include <algorithm>
#include <numeric>
#include <iostream>

#include "AddReductionOp.hh"
#include "Driver.hh"
#include "MinReductionOp.hh"
#include "Vec2.hh"

Parallel::Parallel(InputParameters input_params,
		Context ctx, HighLevelRuntime *runtime) :
		global_mesh_(input_params, ctx, runtime),
		ctx_(ctx),
		runtime_(runtime)
//num_subregions = input_params.ntasks_;
{
	  // we're going to use a must epoch launcher, so we need at least as many
	  //  processors in our system as we have subregions - check that now
	  std::set<Processor> all_procs;
	  Realm::Machine::get_machine().get_all_processors(all_procs);
	  int num_loc_procs = 0;
	  for(std::set<Processor>::const_iterator it = all_procs.begin();
	      it != all_procs.end();
	      it++)
	    if((*it).kind() == Processor::LOC_PROC)
	      num_loc_procs++;

	  if(num_loc_procs < num_subregions()) {
	    printf("FATAL ERROR: This test uses a must epoch launcher, which requires\n");
	    printf("  a separate Realm processor for each subregion.  %d of the necessary\n",
		   num_loc_procs);
	    printf("  %d are available.  Please rerun with '-ll:cpu %d'.\n",
		   num_subregions(), num_subregions());
	    exit(1);
	  }

	  Rect<1> launch_bounds(Point<1>(0),Point<1>(num_subregions()-1));

	  double zero = 0.0;
	  DynamicCollective add_reduction =
		runtime_->create_dynamic_collective(ctx_, num_subregions(), AddReductionOp::redop_id,
						   &zero, sizeof(zero));

	  TimeStep max;
	  DynamicCollective min_reduction =
		runtime_->create_dynamic_collective(ctx_, num_subregions(), MinReductionOp::redop_id,
						   &max, sizeof(max));

	  std::vector<SPMDArgs> args(num_subregions());
	  serializer.resize(num_subregions());

	  for (int color = 0; color < num_subregions(); color++) {
		  args[color].add_reduction_ = add_reduction;
		  args[color].min_reduction_ = min_reduction;
		  args[color].shard_id_ = color;
		  args[color].direct_input_params_ = input_params.directs_;
		  // Legion cannot handle data structures with indirections in them
		  args[color].n_meshtype_ = input_params.meshtype_.length();
		  args[color].n_probname_ = input_params.probname_.length();
		  args[color].n_bcx_ = input_params.bcx_.size();
		  args[color].n_bcy_ = input_params.bcy_.size();

		  // Legion cannot handle data structures with indirections in them
		  size_t size = sizeof(SPMDArgs) + (args[color].n_meshtype_ + args[color].n_probname_)
				  * sizeof(char) + (args[color].n_bcx_+ args[color].n_bcy_) * sizeof(double);
		  serializer[color] = malloc(size);
		  unsigned char *next = (unsigned char*)(serializer[color]) ;
		  memcpy((void*)next, (void*)(&(args[color])), sizeof(SPMDArgs));
		  next += sizeof(SPMDArgs);

		  size_t next_size = args[color].n_meshtype_ * sizeof(char);
		  memcpy((void*)next, (void*)input_params.meshtype_.c_str(), next_size);
		  next += next_size;

		  next_size = args[color].n_probname_ * sizeof(char);
		  memcpy((void*)next, (void*)input_params.probname_.c_str(), next_size);
		  next += next_size;

		  next_size = args[color].n_bcx_ * sizeof(double);
		  memcpy((void*)next, (void*)&(input_params.bcx_[0]), next_size);
		  next += next_size;

		  next_size = args[color].n_bcy_ * sizeof(double);
		  memcpy((void*)next, (void*)&(input_params.bcy_[0]), next_size);
		  //LogicalRegion my_zones = runtime_->get_logical_region_by_color(ctx_,
		  // global_mesh.logical_part_zones_, color);
		  DriverTask driver_launcher(global_mesh_.logical_region_global_zones_, serializer[color], size);
		  DomainPoint point(color);
		  must_epoch_launcher.add_single_task(point, driver_launcher);
	  }

}  // init

Parallel::~Parallel() {
	for (int i=0; i < serializer.size(); i++)
		free(serializer[i]);
}

void Parallel::run() {
	  FutureMap fm = runtime_->execute_must_epoch(ctx_, must_epoch_launcher);
	  fm.wait_all_results();
}

void Parallel::globalSum(int& x) {
    if (num_subregions() == 1) return;
#ifdef USE_MPI
    int y;
    MPI_Allreduce(&x, &y, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    x = y;
#endif
}


void Parallel::globalSum(int64_t& x) {
    if (num_subregions() == 1) return;
#ifdef USE_MPI
    int64_t y;
    MPI_Allreduce(&x, &y, 1, MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD);
    x = y;
#endif
}


void Parallel::globalSum(double& x) {
    if (num_subregions() == 1) return;
#ifdef USE_MPI
    double y;
    MPI_Allreduce(&x, &y, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    x = y;
#endif
}


void Parallel::gather(int x, int* y) {
    if (num_subregions() == 1) {
        y[0] = x;
        return;
    }
#ifdef USE_MPI
    MPI_Gather(&x, 1, MPI_INT, y, 1, MPI_INT, 0, MPI_COMM_WORLD);
#endif
}


void Parallel::scatter(const int* x, int& y) {
    if (num_subregions() == 1) {
        y = x[0];
        return;
    }
#ifdef USE_MPI
    MPI_Scatter((void*) x, 1, MPI_INT, &y, 1, MPI_INT, 0, MPI_COMM_WORLD);
#endif
}


template<typename T>
void Parallel::gathervImpl(
        const T *x, const int numx,
        T* y, const int* numy) {

    if (num_subregions() == 1) {
        std::copy(x, x + numx, y);
        return;
    }
#ifdef USE_MPI
    const int type_size = sizeof(T);
    int sendcount = type_size * numx;
    std::vector<int> recvcount, disp;
    if (mype == 0) {
        recvcount.resize(num_subregions);
        for (int pe = 0; pe < num_subregions; ++pe) {
            recvcount[pe] = type_size * numy[pe];
        }
        // exclusive scan isn't available in the standard library,
        // so we use an inclusive scan and displace it by one place
        disp.resize(num_subregions + 1);
        std::partial_sum(recvcount.begin(), recvcount.end(), &disp[1]);
    } // if mype

    MPI_Gatherv((void*) x, sendcount, MPI_BYTE,
            y, &recvcount[0], &disp[0], MPI_BYTE,
            0, MPI_COMM_WORLD);
#endif

}


template<>
void Parallel::gatherv(
        const double2 *x, const int numx,
        double2* y, const int* numy) {
    gathervImpl(x, numx, y, numy);
}


template<>
void Parallel::gatherv(
        const double *x, const int numx,
        double* y, const int* numy) {
    gathervImpl(x, numx, y, numy);
}


template<>
void Parallel::gatherv(
        const int *x, const int numx,
        int* y, const int* numy) {
    gathervImpl(x, numx, y, numy);
}

// Legion Stuff

Future Parallel::globalSum(double local_value,
		DynamicCollective& dc_reduction,
		Runtime *runtime, Context ctx,
		Predicate pred)
{
  TaskLauncher launcher(sumTaskID, TaskArgument(&local_value, sizeof(local_value)), pred, 0 /*default mapper*/);
  double zero = 0.0;
  launcher.set_predicate_false_result(TaskArgument(&zero, sizeof(zero)));
  Future f = runtime->execute_task(ctx, launcher);
  runtime->defer_dynamic_collective_arrival(ctx, dc_reduction, f);
  f.get_result<double>();
  dc_reduction = runtime->advance_dynamic_collective(ctx, dc_reduction);
  Future ff2 = runtime->get_dynamic_collective_result(ctx, dc_reduction);
  return ff2;
}

double Parallel::globalSumTask (const Task *task,
                  const std::vector<PhysicalRegion> &regions,
                  Context ctx, HighLevelRuntime *runtime)
{
	double value = *(const double *)(task->args);
	return value;
}

Future Parallel::globalMin(TimeStep local_value,
		DynamicCollective& dc_reduction,
		Runtime *runtime, Context ctx,
		Predicate pred)
{
  TaskLauncher launcher(minTaskID, TaskArgument(&local_value, sizeof(local_value)), pred, 0 /*default mapper*/);
  TimeStep max;
  launcher.set_predicate_false_result(TaskArgument(&max, sizeof(max)));
  Future f = runtime->execute_task(ctx, launcher);
  runtime->defer_dynamic_collective_arrival(ctx, dc_reduction, f);
  f.get_result<TimeStep>();
  dc_reduction = runtime->advance_dynamic_collective(ctx, dc_reduction);
  Future ff2 = runtime->get_dynamic_collective_result(ctx, dc_reduction);
  return ff2;
}

TimeStep Parallel::globalMinTask (const Task *task,
                  const std::vector<PhysicalRegion> &regions,
                  Context ctx, HighLevelRuntime *runtime)
{
	TimeStep value = *(const TimeStep *)(task->args);
	return value;
}


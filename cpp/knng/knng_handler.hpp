#pragma once
#include "../net/process_3D_grid.hpp"
#include "global_tree_handler.hpp"
#include <algorithm>
#include <map>
#include <mpi.h>
#include <omp.h>
#include <set>
#include <string>
#include <vector>
#include "../common/common.h"
#include "math_operations.hpp"
#include <memory>
#include "../lib/Mrpt.h"
#include "../io/file_writer.hpp"

using namespace hipgraph::distviz::common;
using namespace hipgraph::distviz::io;
namespace hipgraph::distviz::knng {

template <typename INDEX_TYPE, typename VALUE_TYPE>
class KNNGHandler {
private:
  int ntrees;
  int tree_depth;
  double tree_depth_ratio;
  int data_dimension;
  int starting_data_index;
  Process3DGrid *grid;
  int global_data_set_size;
  int local_data_set_size;
  vector<vector<vector<DataNode<INDEX_TYPE, VALUE_TYPE>>>> trees_leaf_all;
  shared_ptr<vector<set<INDEX_TYPE>>> process_to_index_set_ptr;
  shared_ptr<vector<set<INDEX_TYPE>>> remote_index_distribution;
  int local_tree_offset;
  int total_leaf_size;
  int leafs_per_node;
  int my_leaf_start_index;
  int my_leaf_end_index;
  std::shared_ptr<std::map<INDEX_TYPE, INDEX_TYPE>> datamap_ptr = std::make_shared<std::map<INDEX_TYPE, INDEX_TYPE>>();
  std::shared_ptr<map<INDEX_TYPE,vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>> local_nn_map_ptr= std::make_shared<map<INDEX_TYPE,vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>>();


  int *receive_random_seeds(int seed) {
    int* receive = new int[grid->col_world_size]();
    if (grid->rank_in_col== 0) {
      for (int i = 0; i < grid->col_world_size; i++)
      {
        std::random_device rd;
        int seed = rd();
        receive[i] = seed;
      }
      MPI_Bcast(receive, grid->col_world_size, MPI_INT, grid->rank_in_col, grid->col_world);
    } else {
      MPI_Bcast(receive, grid->col_world_size, MPI_INT, NULL, grid->col_world);
    }
    return receive;
  }


public:
  KNNGHandler(int ntrees, int tree_depth, double tree_depth_ratio,
        int local_tree_offset, int total_data_set_size, int local_data_set_size,
        int dimension, Process3DGrid* grid) {

    this->data_dimension = dimension;
    this->tree_depth = tree_depth;
    this->global_data_set_size = total_data_set_size;
    this->local_data_set_size = local_data_set_size;
    this->grid = grid;
    this->ntrees = ntrees;
    this->tree_depth_ratio = tree_depth_ratio;

    this->trees_leaf_all = vector<vector<vector<DataNode<INDEX_TYPE,VALUE_TYPE>>>>(ntrees);

    this->process_to_index_set_ptr = make_shared<vector<set<INDEX_TYPE>>>(grid->col_world_size);

    this->remote_index_distribution = make_shared<vector<set<INDEX_TYPE>>>(grid->col_world_size);

    this->local_tree_offset = local_tree_offset;

    this->starting_data_index = (global_data_set_size / grid->col_world_size) * grid->rank_in_col;
  }

  void build_distributed_KNNG(ValueType2DVector<VALUE_TYPE>* input_data, vector<Tuple<VALUE_TYPE>> *output_knng,
                              float density,bool use_locality_optimization, int nn, float target_recall,
                              bool print_output =false, string output_path="knng.txt",
                              bool skip_self_loops=true) {

    unique_ptr<MathOp<INDEX_TYPE,VALUE_TYPE>> mathOp_ptr; //class uses for math operations
    VALUE_TYPE* row_data_array = mathOp_ptr.get()->convert_to_row_major_format(input_data); // this algorithm assumes row major format for operations
    int global_tree_depth = this->tree_depth * this->tree_depth_ratio;

    // generate random seed at process 0 and broadcast it to multiple processes.
    int* receive = this->receive_random_seeds(1);

    // build global sparse random project matrix for all trees
    VALUE_TYPE* B = mathOp_ptr.get()->build_sparse_projection_matrix(this->data_dimension,global_tree_depth * this->ntrees, density, receive[0]);

    // get the matrix projection
    // P= X.R
    VALUE_TYPE* P = mathOp_ptr.get()->multiply_mat(row_data_array, B, this->data_dimension,
                                        global_tree_depth * this->ntrees,
                                        this->local_data_set_size,1.0);

    // creating DRPTGlobal class
    GlobalTreeHandler<INDEX_TYPE,VALUE_TYPE> drpt_global = GlobalTreeHandler<INDEX_TYPE,VALUE_TYPE>(P,
                                                    B,this->grid,
                                                    this->local_data_set_size,
                                                    this->data_dimension,
                                                    global_tree_depth,
                                                    this->ntrees,
                                                    this->global_data_set_size);


    cout << " rank " << grid->rank_in_col << " starting growing trees" << endl;
    // start growing global tree
    drpt_global.grow_global_tree(input_data);
    cout << " rank " <<  grid->rank_in_col << " completing growing trees" << endl;

    //
    //	//calculate locality optimization to improve data locality
    if (use_locality_optimization)
    {
      cout << " rank " << grid->rank_in_col << " starting tree leaf correlation " << endl;
      drpt_global.calculate_tree_leaf_correlation();
      cout << " rank " << grid->rank_in_col << "  tree leaf correlation completed " << endl;
    }

    shared_ptr<DataNode3DVector<INDEX_TYPE,VALUE_TYPE>> leaf_nodes_of_trees_ptr =  make_shared<DataNode3DVector<INDEX_TYPE, VALUE_TYPE>>(ntrees);

    cout << " rank " << grid->rank_in_col << " running  datapoint collection  "<< endl;

    Eigen::MatrixXf data_matrix =  drpt_global.collect_similar_data_points_of_all_trees(use_locality_optimization,
                                                                                       process_to_index_set_ptr.get(),
                                                                                       datamap_ptr.get(),
                                                                                       local_nn_map_ptr.get(),
                                                                                       nn);

    cout<<"rank "<<grid->rank_in_col<<" rows "<<data_matrix.rows()<<" cols "<<data_matrix.cols()<<endl;
//    int effective_nn = 2 * nn;
    int effective_nn =  nn;
    Mrpt mrpt(data_matrix);
    mrpt.grow_autotune(target_recall, effective_nn);

    Eigen::MatrixXi neighbours(data_matrix.cols(),effective_nn);
    Eigen::MatrixXf distances(data_matrix.cols(),effective_nn);

    #pragma omp parallel for schedule (static)
    for(int i=0;i<data_matrix.cols();i++){
      Eigen::VectorXi tempRow(effective_nn);
      Eigen::VectorXf tempDis(effective_nn);
      mrpt.query(data_matrix.col(i), tempRow.data(),tempDis.data());
      neighbours.row(i)=tempRow;
      distances.row(i)=tempDis;
      INDEX_TYPE  global_index =  (*datamap_ptr)[i];
      EdgeNode<INDEX_TYPE,VALUE_TYPE> edge;
      edge.src_index=global_index;
      for(int k=0;k<nn;k++){
       edge.dst_index = (*datamap_ptr)[tempRow[k]];
       edge.distance = tempDis[k];
       (*local_nn_map_ptr)[global_index][k]=edge;
      }
    }
    MPI_Barrier(grid->col_world);
    cout<<"rank "<<grid->rank_in_col<<" local nn slection completed :"<<endl;

    shared_ptr<map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>> final_nn_map = make_shared<map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>>();

    communicate_nns((local_nn_map_ptr).get(),nn,final_nn_map.get());

    cout<<"rank "<<grid->rank_in_col<<" size :"<<(*final_nn_map).size()<<endl;

    if (print_output) {
      auto t = start_clock();
      FileWriter<INDEX_TYPE,VALUE_TYPE> fileWriter;
      fileWriter.mpi_write_edge_list(final_nn_map.get(),output_path,nn-1,grid->rank_in_col,grid->col_world_size,true);
      stop_clock_and_add(t, "IO Time");
    }


    for(auto it=(*final_nn_map).begin(); it!=(*final_nn_map).end();++it){
      vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>> edge_node_list = (*it).second;
      for(int j=0;j<nn;j++){
        EdgeNode<INDEX_TYPE,VALUE_TYPE> edge_node = edge_node_list[j];
        Tuple<VALUE_TYPE> tuple;
        if (edge_node.src_index != edge_node.dst_index) {
          tuple.row = edge_node.src_index;
          tuple.col = edge_node.dst_index;
          tuple.value = edge_node.distance;
          (*output_knng).push_back(tuple);
        }
      }
    }
  }


  void build_local_KNNG(ValueType2DVector<VALUE_TYPE>* input_data, vector<Tuple<VALUE_TYPE>> *output_knng,int nn, float target_recall,
                        bool print_output =false, string output_path="knng.txt", bool skip_self_loops=true) {

    Eigen::MatrixXf data_matrix((*input_data)[0].size(), (*input_data).size());

   #pragma omp parallel for schedule (static)
    for (int i = 0; i < (*input_data).size(); ++i) {
      for (int j = 0; j < (*input_data)[0].size(); ++j) {
        data_matrix(j, i) = (*input_data)[i][j];
      }
    }

    int effective_nn = 2 * nn;
    Mrpt mrpt(data_matrix);
    mrpt.grow_autotune(target_recall, effective_nn);

    Eigen::MatrixXi neighbours(data_matrix.cols(),effective_nn);
    Eigen::MatrixXf distances(data_matrix.cols(),effective_nn);

    int neighhour_size = (skip_self_loops)?nn-1:nn;
    output_knng->resize(data_matrix.cols()*neighhour_size);

    int starting_index = skip_self_loops?1:0;
    int offset = skip_self_loops?-1:0;

    #pragma omp parallel for schedule (static)
    for(int i=0;i<data_matrix.cols();i++){
      Eigen::VectorXi tempRow(effective_nn);
      Eigen::VectorXf tempDis(effective_nn);
      mrpt.query(data_matrix.col(i), tempRow.data(),tempDis.data());
      neighbours.row(i)=tempRow;
      distances.row(i)=tempDis;
      EdgeNode<INDEX_TYPE,VALUE_TYPE> edge;
      edge.src_index=i;
      for(int k=starting_index;k<nn;k++){
        int index = i*(neighhour_size)+k+offset;
        edge.dst_index = tempRow[k];
        edge.distance = tempDis[k];
        Tuple<VALUE_TYPE> tuple;
        tuple.row = edge.src_index;
        tuple.col = edge.dst_index;
        if (tuple.row<0 or tuple.col<0){
          cout<<" woring index found  "<<tuple.row<<"col "<<edge.dst_index<<" distance "<<edge.distance<<" k "<<k<<endl;
        }
        tuple.value = edge.distance;
        (*output_knng)[index]= tuple;
        }
      }

      if (print_output) {
        FileWriter<INDEX_TYPE,VALUE_TYPE> fileWriter;
        fileWriter.write_list(output_knng,output_path);
      }

  }

  void communicate_nns(map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>* local_nns,int nn,
                       map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>* final_nn_map) {

    shared_ptr<map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>> final_nn_sending_map = make_shared<map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>>();



    shared_ptr<vector<INDEX_TYPE>> receiving_indices_count = make_shared<vector<INDEX_TYPE>>(grid->col_world_size);
    shared_ptr<vector<INDEX_TYPE>> disps_receiving_indices = make_shared<vector<INDEX_TYPE>>(grid->col_world_size);

    int send_count = 0;
    int total_receving = 0;

    //send distance threshold to original data owner
    shared_ptr<vector<index_distance_pair<INDEX_TYPE>>> out_index_dis =  make_shared<vector<index_distance_pair<INDEX_TYPE>>>();

    send_min_max_distance_to_data_owner(local_nns,out_index_dis.get(),
                                        receiving_indices_count.get(),
                                        disps_receiving_indices.get(),
                                        send_count,total_receving,nn);

    cout<<" rank "<<grid->rank_in_col<<" after receiving  method  "<<(*out_index_dis).size()<<endl;

    shared_ptr<vector<index_distance_pair<INDEX_TYPE>>> final_sent_indices_to_rank_map = make_shared<vector<index_distance_pair<INDEX_TYPE>>>(local_data_set_size);
    //
    //	//finalize data owners based on data owner having minimum distance threshold.

    finalize_final_dataowner(receiving_indices_count.get(),
                             disps_receiving_indices.get(),
                             out_index_dis.get(),
                             final_sent_indices_to_rank_map.get());

    shared_ptr<vector<vector<index_distance_pair<INDEX_TYPE>>>> final_indices_allocation = make_shared<vector<vector<index_distance_pair<INDEX_TYPE>>>>(grid->col_world_size);
    //	//announce the selected dataowner to all interesting data holders
      announce_final_dataowner(total_receving,
                             receiving_indices_count.get(),
                             disps_receiving_indices.get(),
                             out_index_dis.get(),
                             final_sent_indices_to_rank_map.get(),
                             final_indices_allocation.get());

    shared_ptr<vector<INDEX_TYPE>> sending_selected_indices_count = make_shared<vector<INDEX_TYPE>>(grid->col_world_size);
    shared_ptr<vector<INDEX_TYPE>> sending_selected_indices_nn_count = make_shared<vector<INDEX_TYPE>>(grid->col_world_size);
    shared_ptr<vector<INDEX_TYPE>> receiving_selected_indices_count = make_shared<vector<INDEX_TYPE>>(grid->col_world_size);
    shared_ptr<vector<INDEX_TYPE>> receiving_selected_indices_nn_count = make_shared<vector<INDEX_TYPE>>(grid->col_world_size);

    //	//select final nns to be forwared to dataowners
    select_final_forwarding_nns(final_indices_allocation.get(),local_nns,
                                      final_nn_sending_map.get(),final_nn_map,
                                      sending_selected_indices_count.get(),
                                      sending_selected_indices_nn_count.get());

    send_nns(sending_selected_indices_count.get(),sending_selected_indices_nn_count.get(),
                   receiving_selected_indices_count.get(),final_nn_map,
             final_nn_sending_map.get(),final_indices_allocation.get());

  }

  void send_min_max_distance_to_data_owner(map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>* local_nns,
                                      vector<index_distance_pair<INDEX_TYPE>>* out_index_dis,
                                                                                      vector<INDEX_TYPE>* receiving_indices_count,
                                                                                      vector<INDEX_TYPE>* disps_receiving_indices,
                                                                                     int &send_count,int &total_receiving, int nn) {
    unique_ptr<vector<INDEX_TYPE>> sending_indices_count_ptr = make_unique<vector<INDEX_TYPE>>(grid->col_world_size);
    unique_ptr<vector<INDEX_TYPE>> disps_sending_indices_ptr = make_unique<vector<INDEX_TYPE>>(grid->col_world_size);

    unique_ptr<vector<set<INDEX_TYPE>>> process_se_indexes_ptr = make_unique<vector<set<INDEX_TYPE>>>(grid->col_world_size);
    for (auto it = (*local_nns).begin(); it != (*local_nns).end(); ++it) {
      INDEX_TYPE key = it->first;
      int target_rank = key/(global_data_set_size/grid->col_world_size);
      (*sending_indices_count_ptr)[target_rank]++;
      (*process_se_indexes_ptr)[target_rank].insert(it->first);
    }

    for (int i = 0;i < grid->col_world_size;i++)
    {
      send_count += (*sending_indices_count_ptr)[i];
      (*disps_sending_indices_ptr)[i] = (i > 0) ? (*disps_sending_indices_ptr)[i - 1] + (*sending_indices_count_ptr)[i - 1] : 0;
    }

    auto t = start_clock();
    //sending back received data during collect similar data points to original process
    MPI_Alltoall((*sending_indices_count_ptr).data(),1, MPI_INDEX_TYPE, (*receiving_indices_count).data(), 1, MPI_INDEX_TYPE, grid->col_world);
    stop_clock_and_add(t, "KNNG Communication Time");
////
    for (int i = 0;i < grid->col_world_size;i++)
    {
      total_receiving += (*receiving_indices_count)[i];
      (*disps_receiving_indices)[i] = (i > 0) ? (*disps_receiving_indices)[i - 1] + (*receiving_indices_count)[i - 1] : 0;
    }
//
    unique_ptr<vector<index_distance_pair<INDEX_TYPE>>> in_index_dis = make_unique<vector<index_distance_pair<INDEX_TYPE>>>(send_count);
    out_index_dis->resize(total_receiving);
    cout<<" rank "<<grid->rank_in_col<<" before  total_receiving  "<<total_receiving<<endl;
    int co_process = 0;
    for (int i = 0;i < grid->col_world_size;i++)
    {
      set<INDEX_TYPE> process_se_indexes = (*process_se_indexes_ptr)[i];
      for (auto it = process_se_indexes.begin();it != process_se_indexes.end();it++)
      {
        (*in_index_dis)[co_process].index = (*it);
        (*in_index_dis)[co_process].distance = (*local_nns)[(*it)][nn - 1].distance;
        co_process++;
      }
    }

    t = start_clock();
    //distribute minimum maximum distance threshold (for k=nn)
    MPI_Alltoallv((*in_index_dis).data(), (*sending_indices_count_ptr).data(), (*disps_sending_indices_ptr).data(), MPI_FLOAT_INT,(*out_index_dis).data(),
                  (*receiving_indices_count).data(), (*disps_receiving_indices).data(), MPI_FLOAT_INT, MPI_COMM_WORLD);
    stop_clock_and_add(t, "KNNG Communication Time");
    cout<<" rank "<<grid->rank_in_col<<" after receiving  total_receiving  "<<(*out_index_dis).size()<<endl;

  }
//
//
  void finalize_final_dataowner(vector<INDEX_TYPE>* receiving_indices_count,vector<INDEX_TYPE> *disps_receiving_indices,
                                             vector<index_distance_pair<INDEX_TYPE>> *out_index_dis,
                                vector<index_distance_pair<INDEX_TYPE>> *final_sent_indices_to_rank_map) {

    int my_end_index = starting_data_index + local_data_set_size;

//    cout<<"rank "<<grid->rank_in_col<<" size: "<<out_index_dis->size()<<endl;
    if (out_index_dis->size()>0) {
      #pragma omp parallel for
      for (int i = starting_data_index; i < my_end_index; i++) {
        int selected_rank = -1;
        int search_index = i;
        float minium_distance = std::numeric_limits<float>::max(); // tight bound

        for (int j = 0; j < grid->col_world_size; j++) {
          int amount = (*receiving_indices_count)[j];
          int offset = (*disps_receiving_indices)[j];

          for (int k = offset; k < (offset + amount); k++) {
            if (search_index == (*out_index_dis)[k].index) {
              if (minium_distance > (*out_index_dis)[k].distance) {
                minium_distance = (*out_index_dis)[k].distance;
                selected_rank = j;
              }
              break;
            }
          }
        }
        if (selected_rank > -1) {
          index_distance_pair<INDEX_TYPE> rank_distance;
          rank_distance.index = selected_rank; // TODO: replace with rank
          rank_distance.distance = minium_distance;
          (*final_sent_indices_to_rank_map)[search_index - starting_data_index] =rank_distance;
        }
      }
    }
  }
//
  vector<vector<index_distance_pair<INDEX_TYPE>>>* announce_final_dataowner(int total_receving,
                                                               vector<INDEX_TYPE> *receiving_indices_count,
                                                               vector<INDEX_TYPE> *disps_receiving_indices,
                                                               vector<index_distance_pair<INDEX_TYPE>> *out_index_dis,
                                                               vector<index_distance_pair<INDEX_TYPE>> *final_sent_indices_to_rank_map,
                                                                            vector<vector<index_distance_pair<INDEX_TYPE>>>* final_indices_allocation) {

    unique_ptr<vector<INDEX_TYPE>> minimal_selected_rank_sending = make_unique<vector<INDEX_TYPE>>(total_receving);
    unique_ptr<vector<index_distance_pair<INDEX_TYPE>>> minimal_index_distance = make_unique<vector<index_distance_pair<INDEX_TYPE>>>(total_receving);

#pragma omp parallel for
    for (int i = 0;i < total_receving;i++)
    {
      (*minimal_index_distance)[i].index = (*out_index_dis)[i].index;
      (*minimal_index_distance)[i].distance = (*final_sent_indices_to_rank_map)[(*out_index_dis)[i].index - starting_data_index].distance;
      (*minimal_selected_rank_sending)[i] = (*final_sent_indices_to_rank_map)[(*out_index_dis)[i].index - starting_data_index].index; //TODO: replace
    }

    unique_ptr<vector<INDEX_TYPE>> receiving_indices_count_back = make_unique<vector<INDEX_TYPE>>(grid->col_world_size);
    unique_ptr<vector<INDEX_TYPE>> disps_receiving_indices_count_back = make_unique<vector<INDEX_TYPE>>(grid->col_world_size);

    auto t = start_clock();
    // we recalculate how much we are receiving for minimal dst distribution
    MPI_Alltoall((*receiving_indices_count).data(),1, MPI_INDEX_TYPE, (*receiving_indices_count_back).data(), 1, MPI_INDEX_TYPE, grid->col_world);
    stop_clock_and_add(t, "KNNG Communication Time");


    int total_receivce_back = 0;
    for (int i = 0;i < grid->col_world_size;i++)
    {
      total_receivce_back += (*receiving_indices_count_back)[i];
      (*disps_receiving_indices_count_back)[i] = (i > 0) ?
                                                      (*disps_receiving_indices_count_back)[i - 1] + (*receiving_indices_count_back)[i - 1] : 0;
    }

    unique_ptr<vector<INDEX_TYPE>> minimal_selected_rank_reciving =  make_unique<vector<INDEX_TYPE>>(total_receivce_back);
    unique_ptr<vector<index_distance_pair<INDEX_TYPE>>> minimal_index_distance_receiv = make_unique<vector<index_distance_pair<INDEX_TYPE>>>(total_receivce_back);


    t = start_clock();
    MPI_Alltoallv((*minimal_index_distance).data(), (*receiving_indices_count).data(), (*disps_receiving_indices).data(), MPI_FLOAT_INT,
                  (*minimal_index_distance_receiv).data(),
                  (*receiving_indices_count_back).data(), (*disps_receiving_indices_count_back).data(), MPI_FLOAT_INT,  grid->col_world);


    MPI_Alltoallv((*minimal_selected_rank_sending).data(), (*receiving_indices_count).data(), (*disps_receiving_indices).data(), MPI_INT,
                  (*minimal_selected_rank_reciving).data(),
                  (*receiving_indices_count_back).data(), (*disps_receiving_indices_count_back).data(), MPI_INT, MPI_COMM_WORLD);

    stop_clock_and_add(t, "KNNG Communication Time");


//
#pragma omp parallel
    {
      vector<vector<index_distance_pair<INDEX_TYPE>>> final_indices_allocation_local(grid->col_world_size);

#pragma omp  for nowait
      for (int i = 0;i < total_receivce_back;i++)
      {
        index_distance_pair<INDEX_TYPE> distance_pair;
        distance_pair.index = (*minimal_index_distance_receiv)[i].index;
        distance_pair.distance = (*minimal_index_distance_receiv)[i].distance;
        final_indices_allocation_local[(*minimal_selected_rank_reciving)[i]].
            push_back(distance_pair);

      }

#pragma omp critical
      {
        for (int i = 0;i < grid->col_world_size;i++)
        {
          (*final_indices_allocation)[i].insert((*final_indices_allocation)[i].end(),
                                             final_indices_allocation_local[i].begin(),
                                             final_indices_allocation_local[i].end());
        }
      }
    }
  }
//
  void select_final_forwarding_nns(vector<vector<index_distance_pair<INDEX_TYPE>>>* final_indices_allocation,
                                                map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>* local_nns,
                                                map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>* final_nn_sending_map,
                                                map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>*  final_nn_map,
                                                vector<INDEX_TYPE>* sending_selected_indices_count,
                                                 vector<INDEX_TYPE>* sending_selected_indices_nn_count){

    for (int i = 0;i < grid->col_world_size;i++)
    {

#pragma omp parallel for
      for (int j = 0;j < (*final_indices_allocation)[i].size();j++)
      {
        index_distance_pair<INDEX_TYPE> in_dis = (*final_indices_allocation)[i][j];
        int selected_index = in_dis.index;
        float dst_th = in_dis.distance;
        if (i != grid->rank_in_col)
        {
          if ((*local_nns).find(selected_index)!= (*local_nns).end())
          {
            vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>> target;
            std::copy_if((*local_nns)[selected_index].begin(),
                         (*local_nns)[selected_index].end(),
                         std::back_inserter(target),
                         [dst_th](
                             EdgeNode<INDEX_TYPE,VALUE_TYPE> dataPoint
                         )
                         {
                           return dataPoint.distance < dst_th;
                         });
            if (target.size()> 0)
            {
#pragma omp critical
              {
                if ((*final_nn_sending_map).find(selected_index)== (*final_nn_sending_map).end())
                {
                  (*final_nn_sending_map).insert(pair < INDEX_TYPE, vector <EdgeNode<INDEX_TYPE,VALUE_TYPE>  >>
                                              (selected_index, target));
                  (*sending_selected_indices_nn_count)[i] += target.size();
                  (*sending_selected_indices_count)[i] += 1;
                }
              }
            }
          }
        }
        else
        {
#pragma omp critical
          (*final_nn_map).insert(pair < INDEX_TYPE, vector < EdgeNode<INDEX_TYPE,VALUE_TYPE> >>(selected_index,
                                                           (*local_nns)[selected_index]));
        }
      }
    }

  }
//
  void send_nns(vector<INDEX_TYPE> *sending_selected_indices_count,vector<INDEX_TYPE> *sending_selected_indices_nn_count,vector<INDEX_TYPE> *receiving_selected_indices_count,
                             std::map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>* final_nn_map,
                             std::map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>>* final_nn_sending_map,
                             vector<vector<index_distance_pair<INDEX_TYPE>>>* final_indices_allocation) {

    int total_receiving_count = 0;

    int total_receiving_nn_count = 0;

    auto t = start_clock();
    MPI_Alltoall((*sending_selected_indices_count).data(),1, MPI_INT, (*receiving_selected_indices_count).data(), 1, MPI_INT, MPI_COMM_WORLD);

    stop_clock_and_add(t, "KNNG Communication Time");

    unique_ptr<vector<INDEX_TYPE>> disps_receiving_selected_indices = make_unique<vector<INDEX_TYPE>>(grid->col_world_size);
    unique_ptr<vector<INDEX_TYPE>> disps_sending_selected_indices =  make_unique<vector<INDEX_TYPE>>(grid->col_world_size);
    unique_ptr<vector<INDEX_TYPE>> disps_sending_selected_nn_indices =  make_unique<vector<INDEX_TYPE>>(grid->col_world_size);
    unique_ptr<vector<INDEX_TYPE>> disps_receiving_selected_nn_indices =  make_unique<vector<INDEX_TYPE>>(grid->col_world_size);


    int total_selected_indices_count=0;

    int total_selected_indices_nn_count=0;

    for (int i = 0;i < grid->col_world_size;i++)
    {
      (*disps_receiving_selected_indices)[i] = (i > 0) ? (*disps_receiving_selected_indices)[i - 1] +
                                                             (*receiving_selected_indices_count)[i - 1] : 0;
      (*disps_sending_selected_indices)[i] = (i > 0) ? (*disps_sending_selected_indices)[i - 1] +
                                                           (*sending_selected_indices_count)[i - 1] : 0;
      (*disps_sending_selected_nn_indices)[i] = (i > 0) ? (*disps_sending_selected_nn_indices)[i - 1] +
                                                              (*sending_selected_indices_nn_count)[i - 1] : 0;

      total_selected_indices_count += (*sending_selected_indices_count)[i];
      total_selected_indices_nn_count += (*sending_selected_indices_nn_count)[i];
    }

    unique_ptr<vector<INDEX_TYPE>> sending_selected_indices = make_unique<vector<INDEX_TYPE>>(total_selected_indices_count);

    unique_ptr<vector<INDEX_TYPE>> sending_selected_nn_count_for_each_index = make_unique<vector<INDEX_TYPE>>(total_selected_indices_count);

    unique_ptr<vector<index_distance_pair<INDEX_TYPE>>> sending_selected_nn = make_unique<vector<index_distance_pair<INDEX_TYPE>>>(total_selected_indices_nn_count);

    int inc = 0;
    int selected_nn = 0;
    for (int i = 0;i < grid->col_world_size;i++)
    {
      total_receiving_count += (*receiving_selected_indices_count)[i];
      if (i != grid->rank_in_col)
      {
        vector<index_distance_pair<INDEX_TYPE>> final_indices = (*final_indices_allocation)[i];
        for (int j = 0;j < final_indices.size();j++)
        {
          if ((*final_nn_sending_map).find(final_indices[j].index) != (*final_nn_sending_map).end())
          {
            vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>> nn_sending = (*final_nn_sending_map)[final_indices[j].index];
            if (nn_sending.size()> 0)
            {
              (*sending_selected_indices)[inc] = final_indices[j].index;
              for (int k = 0;k < nn_sending.size();k++)
              {
                (*sending_selected_nn)[selected_nn].index = nn_sending[k].dst_index;
                (*sending_selected_nn)[selected_nn].distance = nn_sending[k].distance;
                selected_nn++;
              }
              (*sending_selected_nn_count_for_each_index)[inc] = nn_sending.size();
              inc++;
            }
          }
        }
      }
    }

    unique_ptr<vector<INDEX_TYPE>> receiving_selected_nn_indices_count = make_unique<vector<INDEX_TYPE>>(total_receiving_count);
    unique_ptr<vector<INDEX_TYPE>> receiving_selected_indices = make_unique<vector<INDEX_TYPE>>(total_receiving_count);


    t = start_clock();
    MPI_Alltoallv((*sending_selected_nn_count_for_each_index).data(), (*sending_selected_indices_count).data(),
                  (*disps_sending_selected_indices).data(), MPI_INDEX_TYPE, (*receiving_selected_nn_indices_count).data(),
                  (*receiving_selected_indices_count).data(), (*disps_receiving_selected_indices).data(), MPI_INDEX_TYPE, grid->col_world);

    MPI_Alltoallv((*sending_selected_indices).data(), (*sending_selected_indices_count).data(), (*disps_sending_selected_indices).data(), MPI_INDEX_TYPE,
                  (*receiving_selected_indices).data(),
                  (*receiving_selected_indices_count).data(), (*disps_receiving_selected_indices).data(), MPI_INDEX_TYPE, grid->col_world);

    stop_clock_and_add(t, "KNNG Communication Time");

    unique_ptr<vector<INDEX_TYPE>> receiving_selected_nn_indices_count_process = make_unique<vector<INDEX_TYPE>>(grid->col_world_size);

    for (int i = 0;i < grid->col_world_size;i++)
    {
      int co = (*receiving_selected_indices_count)[i];
      int offset = (*disps_receiving_selected_indices)[i];

      for (int k = offset;k < (co + offset); k++)
      {
        (*receiving_selected_nn_indices_count_process)[i] += (*receiving_selected_nn_indices_count)[k];
      }
      total_receiving_nn_count += (*receiving_selected_nn_indices_count_process)[i];

      (*disps_receiving_selected_nn_indices)[i] = (i > 0) ? (*disps_receiving_selected_nn_indices)[i - 1] +
                                                                (*receiving_selected_nn_indices_count_process)[i - 1] : 0;
    }

    unique_ptr<vector<index_distance_pair<INDEX_TYPE>>> receving_selected_nn = make_unique<vector<index_distance_pair<INDEX_TYPE>>>(total_receiving_nn_count);



    t = start_clock();
    MPI_Alltoallv((*sending_selected_nn).data(), (*sending_selected_indices_nn_count).data(), (*disps_sending_selected_nn_indices).data(),
                  MPI_FLOAT_INT,
                  (*receving_selected_nn).data(),
                  (*receiving_selected_nn_indices_count_process).data(), (*disps_receiving_selected_nn_indices).data(), MPI_FLOAT_INT,
                  grid->col_world);

    stop_clock_and_add(t, "KNNG Communication Time");

    int nn_index = 0;
    for (int i = 0;i < total_receiving_count;i++)
    {
      auto src_index = (*receiving_selected_indices)[i];
      auto nn_count = (*receiving_selected_nn_indices_count)[i];
      vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>> vec;
      for (int j = 0;j < nn_count;j++)
      {
        auto nn_indi = (*receving_selected_nn)[nn_index].index;
        VALUE_TYPE distance = (*receving_selected_nn)[nn_index].distance;
        EdgeNode<INDEX_TYPE,VALUE_TYPE> dataPoint;
        dataPoint.src_index = src_index;
        dataPoint.dst_index = nn_indi;
        dataPoint.distance = distance;
        vec.push_back(dataPoint);
        nn_index++;
      }

      auto its = (*final_nn_map).find(src_index);
      if (its == (*final_nn_map).end())
      {
        (*final_nn_map).insert(pair < int, vector < EdgeNode<INDEX_TYPE,VALUE_TYPE> >>(src_index, vec));
      }
      else
      {
        vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>> dst;
        vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>> ex_vec = its->second;
        sort(vec.begin(), vec.end(),
             [](const EdgeNode<INDEX_TYPE,VALUE_TYPE>& lhs,const EdgeNode<INDEX_TYPE,VALUE_TYPE>& rhs)
             {
               return lhs.distance < rhs.distance;
             });
        std::merge(ex_vec.begin(), ex_vec.end(), vec.begin(),
                   vec.end(), std::back_inserter(dst),
                   [](const EdgeNode<INDEX_TYPE,VALUE_TYPE>& lhs,const EdgeNode<INDEX_TYPE,VALUE_TYPE>& rhs
                   )
                   {
                     return lhs.distance < rhs.distance;
                   });
        dst.erase(unique(dst.begin(), dst.end(), [](const EdgeNode<INDEX_TYPE,VALUE_TYPE>& lhs,
                                                    const EdgeNode<INDEX_TYPE,VALUE_TYPE>& rhs)
                         {
                           return lhs.dst_index == rhs.dst_index;
                         }),
                  dst.end()
            );
        (its->second) =dst;
      }
    }
  }

};
} // namespace hipgraph::distviz::knng



#pragma once#include "mpi.h"#include <cmath>#include <cstring>#include <fstream>#include <iostream>#include <map>#include <sstream>#include <string>#include <vector>#include "../common/common.h"using namespace std;using namespace hipgraph::distviz::common;namespace hipgraph::distviz::io {template <typename INDEX_TYPE,typename  VALUE_TYPE> class FileWriter {private:  //		vector<vector<IT>> alloc2d(int rows, int cols);public:  int get_number_of_digits(int number) {    int count = 0;    while(number != 0) {      number = number / 10;      count++;    }    return count;  }  int mpi_write_edge_list(std::map<INDEX_TYPE, vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>>> *data_points,                          string output_path, int nn, int rank, int world_size,                          bool filter_self_edges) {    MPI_Offset offset;    MPI_File file;    MPI_Status status;    MPI_Datatype num_as_string;    MPI_Datatype localarray;    char *const fmt = "%d ";    char *const endfmt = "%d\n";    int totalchars = 0;    int local_rows = 0;    for (auto i = (*data_points).begin(); i != (*data_points).end(); i++) {      vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>> vec = i->second;      int l_rows = 0;      if (filter_self_edges) {        vec.erase(remove_if(vec.begin(), vec.end(),                            [&](EdgeNode<INDEX_TYPE,VALUE_TYPE> dataPoint) {                              return dataPoint.src_index == dataPoint.dst_index;                            }),                  vec.end());      }      l_rows = vec.size() >= nn ? nn : vec.size();      local_rows += l_rows;    }    int cols = 2;    int *send_counts = new int[world_size]();    int *recv_counts = new int[world_size]();    for (int i = 0; i < world_size; i++) {      send_counts[i] = local_rows;    }    MPI_Alltoall(send_counts, 1, MPI_INT, recv_counts, 1, MPI_INT,                 MPI_COMM_WORLD);    int startrow = 0;    int global_rows = 0;    for (int j = 0; j < world_size; j++) {      if (j < rank) {        startrow += recv_counts[j];      }      global_rows += recv_counts[j];    }    vector<vector<INDEX_TYPE>> data = vector<vector<INDEX_TYPE>>(local_rows);    for (int i = 0; i < local_rows; i++) {      data[i] = vector<INDEX_TYPE>(cols, -1);    }    int ind = 0;    for (auto i = (*data_points).begin(); i != (*data_points).end(); i++) {      vector<EdgeNode<INDEX_TYPE,VALUE_TYPE>> vec = i->second;      if (filter_self_edges) {        vec.erase(remove_if(vec.begin(), vec.end(),                            [&](EdgeNode<INDEX_TYPE,VALUE_TYPE> dataPoint) {                              return dataPoint.src_index == dataPoint.dst_index;                            }),                  vec.end());      }      for (int j = 0; j < (vec.size() >= nn ? nn : vec.size()); j++) {        if (ind < local_rows) {          data[ind][0] = vec[j].src_index + 1;          data[ind][1] = vec[j].dst_index + 1;          totalchars = totalchars + get_number_of_digits(vec[j].src_index + 1) +                       get_number_of_digits(vec[j].dst_index + 1) + 2;          ind++;        }      }    }    //    //    //    ////	MPI_Type_contiguous(charspernum, MPI_CHAR, &num_as_string);    ////	MPI_Type_commit(&num_as_string);    //    int *send_counts_bytes = new int[world_size]();    int *recv_counts_bytes = new int[world_size]();    for (int i = 0; i < world_size; i++) {      send_counts_bytes[i] = totalchars;    }    MPI_Alltoall(send_counts_bytes, 1, MPI_INDEX_TYPE, recv_counts_bytes, 1, MPI_INDEX_TYPE,                 MPI_COMM_WORLD);    int bytes_disps = 0;    for (int j = 0; j < world_size; j++) {      if (j < rank) {        bytes_disps += recv_counts_bytes[j];      }    }    //    //    //	cout<<"rank "<<rank <<" before sprintf total chars "<<totalchars<<"    //bytes recvied "<<bytes_disps<<endl;    //	/* convert our data into txt */    char *data_as_txt = new char[totalchars + 1];    int count = 0;    int current_total_chars = 0;    for (int i = 0; i < local_rows; i++) {      for (int j = 0; j < cols - 1; j++) {        int local_chars = get_number_of_digits(data[i][j]);        sprintf(&data_as_txt[current_total_chars], fmt, data[i][j]);        current_total_chars = current_total_chars + local_chars + 1;      }      int local_chars = get_number_of_digits(data[i][cols - 1]);      sprintf(&data_as_txt[current_total_chars], endfmt, data[i][cols - 1]);      current_total_chars = current_total_chars + local_chars + 1;    }    //	/* open the file, and set the view */    MPI_File_open(MPI_COMM_WORLD, output_path.c_str(),                  MPI_MODE_CREATE | MPI_MODE_WRONLY, MPI_INFO_NULL, &file);    MPI_File_set_view(file, bytes_disps, MPI_CHAR, MPI_CHAR, (char *)"native",                      MPI_INFO_NULL);    MPI_File_write_all(file, data_as_txt, totalchars, MPI_CHAR, &status);    MPI_File_close(&file);    delete[] send_counts;    delete[] recv_counts;    delete[] send_counts_bytes;    delete[] recv_counts_bytes;    return 0;  }  int write_list(vector<Tuple<VALUE_TYPE>> *output_knng,string output_path) {    MPI_File fh;    MPI_File_open(MPI_COMM_WORLD, output_path.c_str(), MPI_MODE_CREATE | MPI_MODE_WRONLY, MPI_INFO_NULL, &fh);    for (uint64_t i = 0; i < output_knng->size(); ++i) {      char buffer[100];      int offset = snprintf(buffer, sizeof(buffer), "%d", i);      cout<<" row "<<(*output_knng)[i].row<<" col "<<(*output_knng)[i].col<<endl;      offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%d", (*output_knng)[i].row);      offset += snprintf(buffer + offset, sizeof(buffer) - offset, " %d", (*output_knng)[i].col);      offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n");      MPI_File_write_ordered(fh, buffer, offset, MPI_CHAR, MPI_STATUS_IGNORE);    }    MPI_File_close(&fh);  }  template <typename T>  void parallel_write(string file_path, T *nCoordinates, uint64_t rows, uint64_t cols) {    int proc_rank;    MPI_Comm_rank(MPI_COMM_WORLD, &proc_rank);    MPI_File fh;    MPI_File_open(MPI_COMM_WORLD, file_path.c_str(), MPI_MODE_CREATE | MPI_MODE_WRONLY, MPI_INFO_NULL, &fh);    for (uint64_t i = 0; i < rows; ++i) {      uint64_t   node_id = i + 1+ proc_rank*rows;      char buffer[100];      int offset = snprintf(buffer, sizeof(buffer), "%d", node_id);      for (int j = 0; j < cols; ++j) {        offset += snprintf(buffer + offset, sizeof(buffer) - offset, " %.5f", nCoordinates[i * cols + j]);      }      offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n");      MPI_File_write_ordered(fh, buffer, offset, MPI_CHAR, MPI_STATUS_IGNORE);    }    MPI_File_close(&fh);  }};} // namespace drpt
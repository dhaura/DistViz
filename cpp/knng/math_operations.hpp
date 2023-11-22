#pragma once
#include <mkl.h>
#include <mpi.h>
#include <omp.h>
#include <string>
#include <vector>
#include "../common/common.h"

using namespace std;

namespace hipgraph::distviz::knng {

template <typename VALUE_TYPE>
class MathOp {
 public:
  VALUE_TYPE *multiply_mat(VALUE_TYPE *A, VALUE_TYPE *B, int A_rows, int B_cols,
						   int A_cols, int alpha) {
    int result_size = A_cols * B_cols;
    VALUE_TYPE *result = (VALUE_TYPE *) malloc (sizeof (VALUE_TYPE) * result_size);

#pragma omp parallel for
    for (int k = 0; k < result_size; k++)
    {
      result[k] = 1;
    }
    cblas_sgemm (CblasRowMajor, CblasTrans, CblasNoTrans, A_cols, B_cols, A_rows, alpha, A, A_cols,
                B, B_cols, 0.0,
                result, B_cols);
    return result;
  }

  VALUE_TYPE *build_sparse_local_random_matrix(int rows, int cols,float density, int seed){

    VALUE_TYPE *A;
    int size = rows * cols;
    A = (VALUE_TYPE *) malloc (sizeof (VALUE_TYPE) * size);

    std::mt19937 gen (seed);
    std::uniform_real_distribution<float> uni_dist (0, 1);
    std::normal_distribution<float> norm_dist (0, 1);

    //follow row major order
    for (int j = 0; j < rows; ++j)
    {
      for (int i = 0; i < cols; ++i)
      {
        // take value at uniformly at random and check value is greater than density.If so make that entry empty.
        if (uni_dist (gen) > density)
        {
          A[i + j * cols] = 0.0;
        }
        else
        {
          // normal distribution for generate projection matrix.
          A[i + j * cols] = (VALUE_TYPE) norm_dist (gen);
        }
      }
    }
    return A;
  }

  VALUE_TYPE *build_sparse_projection_matrix(int total_dimension, int levels,float density, int seed){

    VALUE_TYPE *local_sparse_matrix = this->build_sparse_local_random_matrix (total_dimension, levels, density, seed);

    return local_sparse_matrix;

  }

  VALUE_TYPE *convert_to_row_major_format(vector<vector<VALUE_TYPE>>* data) {
    cout<<": calling data "<<endl;
    if (data->empty()) {
      return (VALUE_TYPE *)malloc(0);
    }

    int cols = data->size();
    cout<<": size "<<cols<<endl;

    int rows = (*data)[0].size();

    cout<<": rows "<<rows<<endl;

    int total_size = cols * rows;

    VALUE_TYPE *arr = (VALUE_TYPE *)malloc(sizeof(VALUE_TYPE) * total_size);

#pragma omp parallel for
    for (int i = 0; i < rows; i++) {
      for (int j = 0; j < cols; j++) {
        arr[j + i * cols] = 0.0;
      }
    }

#pragma omp parallel for
    for (int i = 0; i < rows; i++) {
      for (int j = 0; j < cols; j++) {
        arr[j + i * cols] = (*data)[j][i];
      }
    }
    return arr;
  }

  VALUE_TYPE *distributed_mean(vector<VALUE_TYPE> &data, vector<int> local_rows,
							   int local_cols,
							   vector<int> total_elements_per_col,
							   StorageFormat format, int rank) {

      VALUE_TYPE *sums = (VALUE_TYPE *) malloc (sizeof (VALUE_TYPE) * local_cols);
      VALUE_TYPE *gsums = (VALUE_TYPE *) malloc (sizeof (VALUE_TYPE) * local_cols);
      for (int i = 0; i < local_cols; i++)
      {
        sums[i] = 0.0;
      }
      if (format == StorageFormat::RAW)
      {
        int data_count_prev = 0;
        for (int i = 0; i < local_cols; i++)
        {
          VALUE_TYPE sum = 0.0;
          //#pragma omp parallel for reduction(+:sum)
          for (int j = 0; j < local_rows[i]; j++)
          {
            sum += data[j + data_count_prev];

          }
          data_count_prev += local_rows[i];

          sums[i] = sum;
        }
      }

      MPI_Allreduce (sums, gsums, local_cols, MPI_VALUE_TYPE, MPI_SUM, MPI_COMM_WORLD);


      for (int i = 0; i < local_cols; i++)
      {

        gsums[i] = gsums[i] / total_elements_per_col[i];
      }
      free (sums);
      return gsums;

  }

  VALUE_TYPE *distributed_variance(vector<VALUE_TYPE> &data,vector<int> local_rows,
                                   int local_cols,vector<int> total_elements_per_col,
                                   StorageFormat format, int rank){
    VALUE_TYPE *means = this->distributed_mean (data, local_rows, local_cols, total_elements_per_col, format, rank);
    VALUE_TYPE *var = (VALUE_TYPE *) malloc (sizeof (VALUE_TYPE) * local_cols);
    VALUE_TYPE *gvariance = (VALUE_TYPE *) malloc (sizeof (VALUE_TYPE) * local_cols);
    for (int i = 0; i < local_cols; i++)
    {
      var[i] = 0.0;
    }
    if (format == StorageFormat::RAW)
    {
      int data_count_prev = 0;
      for (int i = 0; i < local_cols; i++)
      {
        VALUE_TYPE sum = 0.0;
        //#pragma omp parallel for reduction(+:sum)
        for (int j = 0; j < local_rows[i]; j++)
        {
          VALUE_TYPE diff = (data[j + data_count_prev] - means[i]);
          sum += (diff * diff);
        }
        data_count_prev += local_rows[i];
        var[i] = sum;
      }
    }
    MPI_Allreduce (var, gvariance, local_cols, MPI_VALUE_TYPE, MPI_SUM, MPI_COMM_WORLD);

    for (int i = 0; i < local_cols; i++)
    {
      gvariance[i] = gvariance[i] / (total_elements_per_col[i] - 1);
      //        cout<<"Rank "<<rank<<"Variance "<< gvariance[i]<<" "<<endl;
    }
    free (means);
    return gvariance;


  }

  VALUE_TYPE *distributed_median(vector<VALUE_TYPE> &data,vector<int> local_rows, int local_cols,vector<int> total_elements_per_col,
                                 int no_of_bins, StorageFormat format,int rank) {


    VALUE_TYPE *means = this->distributed_mean(data, local_rows, local_cols, total_elements_per_col, format, rank);

    VALUE_TYPE *variance = this->distributed_variance(data, local_rows, local_cols, total_elements_per_col, format,rank);


    VALUE_TYPE *medians = (VALUE_TYPE *) malloc (sizeof (VALUE_TYPE) * local_cols);

    int std1 = 4, std2 = 2, std3 = 1;

    for (int k = 0; k < local_cols; k++)
    {
      medians[k] = INFINITY;
    }

    long factor_long = no_of_bins * 1.0 / (std1 + std2 + std3);
    int factor = (int) ceil (no_of_bins * 1.0 / (std1 + std2 + std3));

    int dist_length = 2 * factor * (std1 + std2 + std3) + 2;

    vector<VALUE_TYPE> distribution (dist_length * local_cols, 0);

    double *gfrequency = (double *) malloc (sizeof (double) * distribution.size ());
    double *freqarray = (double *) malloc (sizeof (double) * distribution.size ());

    double *local_total = new double[1] ();
    double *global_total = new double[1] ();

    int data_count_prev = 0;
    for (int i = 0; i < local_cols; i++)
    {
      VALUE_TYPE mu = means[i];
      VALUE_TYPE sigma = variance[i];

      sigma = sqrt (sigma);

      VALUE_TYPE val = 0.0;

      vector<double> frequency (dist_length, 0);

      int start1 = factor * (std1 + std2 + std3);
      VALUE_TYPE step1 = sigma / (2 * pow (2, std1 * factor) - 2);

      for (int k = start1, j = 1; k < start1 + std1 * factor; k++, j++)
      {
        VALUE_TYPE rate = j * step1;
        distribution[k + dist_length * i] = mu + rate;
        distribution[start1 - j + dist_length * i] = mu - rate;
      }

      int start2 = start1 + std1 * factor;
      int rstart2 = start1 - std1 * factor;
      VALUE_TYPE step2 = sigma / (2 * pow (2, std2 * factor) - 2);

      for (int k = start2, j = 1; k < start2 + std2 * factor; k++, j++)
      {
        VALUE_TYPE rate = sigma + j * step2;
        distribution[k + dist_length * i] = mu + rate;
        distribution[rstart2 - j + dist_length * i] = mu - rate;
      }

      int start3 = start2 + std2 * factor;
      int rstart3 = rstart2 - std2 * factor;
      VALUE_TYPE step3 = sigma / (2 * pow (2, std3 * factor) - 2);

      for (int k = start3, j = 1; k < start3 + std3 * factor; k++, j++)
      {
        VALUE_TYPE rate = 2 * sigma + j * step3;
        distribution[k + dist_length * i] = mu + rate;
        distribution[rstart3 - j + dist_length * i] = mu - rate;
      }

      for (int k = 0; k < local_rows[i]; k++)
      {
        int flag = 1;
        for (int j = 1; j < 2 * no_of_bins + 2; j++)
        {
          VALUE_TYPE dval = data[data_count_prev + k];
          if (distribution[j - 1 + dist_length * i] < dval && distribution[j + dist_length * i] >= dval)
          {
            flag = 0;
            frequency[j] += 1;
          }
        }
        if (flag)
        {
          frequency[0] += 1;
        }
      }

      data_count_prev += local_rows[i];

      for (int k = i * dist_length; k < dist_length + i * dist_length; k++)
      {
        freqarray[k] = frequency[k - i * dist_length];
        local_total[0] += freqarray[k];
        gfrequency[k] = 0;
      }

    }
    MPI_Allreduce (local_total, global_total, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce (freqarray, gfrequency, distribution.size (), MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    for (int i = 0; i < local_cols; i++)
    {
      VALUE_TYPE cfreq = 0;
      VALUE_TYPE cper = 0;
      int selected_index = -1;

      for (int k = i * dist_length; k < dist_length + i * dist_length; k++)
      {
        cfreq += gfrequency[k];
        VALUE_TYPE val = 50 * total_elements_per_col[i] / 100;
        if (cfreq >= val)
        {
          selected_index = k;
          break;
        }

      }

      double count = gfrequency[selected_index];
      VALUE_TYPE median = distribution[selected_index - 1] +
                          ((total_elements_per_col[i] / 2 - (cfreq - count)) / count) *
                              (distribution[selected_index] - distribution[selected_index - 1]);
      medians[i] = median;
    }

    free (gfrequency);
    free (freqarray);

    return medians;

  }

};
}

#include "lib/Mrpt.h"
#include <iostream>
#include "Eigen/Dense"
#include "io/file_reader.hpp"
using namespace std;

int main(int argc, char* argv[]) {

  string input_path = "";

  for (int p = 0; p < argc; p++)
  {
    if (strcmp(argv[p], "-input") == 0)
    {
      input_path = argv[p + 1];
    }
  }

  int k = 10;
  double target_recall = 0.9;
//  int n = 10000, d = 200, k = 10;
//  double target_recall = 0.9;
//  Eigen::MatrixXf X = Eigen::MatrixXf::Random(d, n);
//  Eigen::MatrixXf q = Eigen::VectorXf::Random(d);


  hipgraph::distviz::FileReader file_reader;
  Eigen::VectorXi indices(k);

  Eigen::MatrixXf X = file_reader.load_data(input_path);

//
//  Mrpt::exact_knn(q, X, k, indices_exact.data());
//  std::cout << indices_exact.transpose() << std::endl;

  Mrpt mrpt(X);
  mrpt.grow_autotune(target_recall, k);


  mrpt.query(X.row(0), indices.data());
  std::cout << indices << std::endl;

}

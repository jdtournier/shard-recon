/* Copyright (c) 2017-2019 Daan Christiaens
 *
 * MRtrix and this add-on module are distributed in the hope
 * that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 */

#include <algorithm>
#include <sstream>

#include "command.h"
#include "image.h"
#include "math/SH.h"
#include "dwi/gradient.h"
#include "phase_encoding.h"
#include "dwi/shells.h"
#include "adapter/extract.h"

#include "dwi/svr/qspacebasis.h"
#include "dwi/svr/recon.h"

#define DEFAULT_LMAX 4
#define DEFAULT_SSPW 1.0f
#define DEFAULT_REG 0.001
#define DEFAULT_ZREG 0.001
#define DEFAULT_TOL 1e-4
#define DEFAULT_MAXITER 10


using namespace MR;
using namespace App;


void usage ()
{
  AUTHOR = "Daan Christiaens (daan.christiaens@kcl.ac.uk)";

  SYNOPSIS = "Reconstruct DWI signal from a series of scattered slices with associated motion parameters.";

  DESCRIPTION
  + "";

  ARGUMENTS
  + Argument ("DWI", "the input DWI image.").type_image_in()
  + Argument ("SH", "the output spherical harmonics coefficients image.").type_image_out();


  OPTIONS
  + Option ("motion", "The motion parameters associated with input slices or volumes. "
                      "These are supplied as a matrix of 6 columns encoding the rigid "
                      "transformations w.r.t. scanner space in se(3) Lie algebra." )
    + Argument ("file").type_file_in()

  + Option ("rf", "Basis functions for the radial (multi-shell) domain, provided as matrices in which "
                  "rows correspond with shells and columns with SH harmonic bands.").allow_multiple()
    + Argument ("b").type_file_in()

  + Option ("lmax", "The maximum harmonic order for the output series. (default = " + str(DEFAULT_LMAX) + ")")
    + Argument ("order").type_integer(0, 30)

  + Option ("weights", "Slice weights, provided as a matrix of dimensions Nslices x Nvols.")
    + Argument ("W").type_file_in()

  + Option ("voxweights", "Voxel weights, provided as an image of same dimensions as dMRI data.")
    + Argument ("W").type_image_in()

  + Option ("ssp", "Slice sensitivity profile, either as text file or as a scalar slice thickness for a "
                   "Gaussian SSP, relative to the voxel size. (default = " + str(DEFAULT_SSPW)  + ")")
    + Argument ("w").type_text()

  + Option ("reg", "Isotropic Laplacian regularization. (default = " + str(DEFAULT_REG) + ")")
    + Argument ("l").type_float()

  + Option ("zreg", "Regularization in the slice direction. (default = " + str(DEFAULT_ZREG) + ")")
    + Argument ("l").type_float()

  + Option ("field", "Static susceptibility field, aligned in recon space.")
    + Argument ("map").type_image_in()
    + Argument ("idx").type_integer()

  + Option ("template", "Template header to determine the reconstruction grid.")
    + Argument ("header").type_image_in()

  + DWI::GradImportOptions()

  + PhaseEncoding::ImportOptions

  + DWI::ShellsOption

  + OptionGroup ("Output options")

  + Option ("spred",
            "output source prediction of all scattered slices. (useful for diagnostics)")
    + Argument ("out").type_image_out()

  + Option ("padding", "zero-padding output coefficients to given dimension.")
    + Argument ("rank").type_integer(0)

  + Option ("complete", "complete (zero-filled) source prediction.")

  + OptionGroup ("CG Optimization options")

  + Option ("tolerance", "the tolerance on the conjugate gradient solver. (default = " + str(DEFAULT_TOL) + ")")
    + Argument ("t").type_float(0.0, 1.0)

  + Option ("maxiter",
            "the maximum number of iterations of the conjugate gradient solver. (default = " + str(DEFAULT_MAXITER) + ")")
    + Argument ("n").type_integer(1)

  + Option ("init",
            "initial guess of the reconstruction parameters.")
    + Argument ("img").type_image_in();

}


typedef float value_type;



void run ()
{
  // Load input data
  auto dwi = Image<value_type>::open(argument[0]).with_direct_io({1, 2, 3, 4});

  // Read motion parameters
  auto opt = get_options("motion");
  Eigen::MatrixXf motion;
  if (opt.size())
    motion = load_matrix<float>(opt[0][0]);
  else
    motion = Eigen::MatrixXf::Zero(dwi.size(3), 6);

  // Check dimensions
  if (motion.size() && motion.cols() != 6)
    throw Exception("No. columns in motion parameters must equal 6.");
  if (motion.size() && ((dwi.size(3) * dwi.size(2)) % motion.rows()))
    throw Exception("No. rows in motion parameters does not match image dimensions.");


  // Select shells
  auto grad = DWI::get_DW_scheme (dwi);
  DWI::Shells shells (grad);
  shells.select_shells (false, false, false);

  // Read multi-shell basis
  int lmax = 0;
  vector<Eigen::MatrixXf> rf;
  opt = get_options("rf");
  for (size_t k = 0; k < opt.size(); k++) {
    Eigen::MatrixXf t = load_matrix<float>(opt[k][0]);
    if (t.rows() != shells.count())
      throw Exception("No. shells does not match no. rows in basis function " + opt[k][0] + ".");
    lmax = std::max(2*(int(t.cols())-1), lmax);
    rf.push_back(t);
  }

  // Read slice weights
  Eigen::MatrixXf W = Eigen::MatrixXf::Ones(dwi.size(2), dwi.size(3));
  opt = get_options("weights");
  if (opt.size()) {
    W = load_matrix<float>(opt[0][0]);
    if (W.rows() != dwi.size(2) || W.cols() != dwi.size(3))
      throw Exception("Weights matrix dimensions don't match image dimensions.");
  }

  // Read field map and PE scheme
  opt = get_options("field");
  bool hasfield = opt.size();
  auto fieldmap = Image<value_type>();
  size_t fieldidx = 0;
  Eigen::MatrixXf PE;
  if (hasfield) {
    auto petable = PhaseEncoding::get_scheme(dwi);
    PE = petable.leftCols<3>().cast<float>();
    PE.array().colwise() *= petable.col(3).array().cast<float>();
    fieldmap = Image<value_type>::open(opt[0][0]);
    fieldidx = opt[0][1];
  }

  // Get volume indices
  vector<size_t> idx;
  if (rf.empty()) {
    idx = shells.largest().get_volumes();
  } else {
    for (size_t k = 0; k < shells.count(); k++)
      idx.insert(idx.end(), shells[k].get_volumes().begin(), shells[k].get_volumes().end());
    std::sort(idx.begin(), idx.end());
  }

  // Select subset
  auto dwisub = Adapter::make <Adapter::Extract1D> (dwi, 3, container_cast<vector<uint32_t>> (idx));

  Eigen::MatrixXf gradsub (idx.size(), grad.cols());
  for (size_t i = 0; i < idx.size(); i++)
    gradsub.row(i) = grad.row(idx[i]).template cast<float>();

  size_t ne = motion.rows()/dwi.size(3);
  Eigen::MatrixXf motionsub (ne * idx.size(), 6);
  for (size_t i = 0; i < idx.size(); i++)
    for (size_t j = 0; j < ne; j++)
      motionsub.row(i * ne + j) = motion.row(idx[i] * ne + j);

  Eigen::MatrixXf Wsub (W.rows(), idx.size());
  for (size_t i = 0; i < idx.size(); i++)
    Wsub.col(i) = W.col(idx[i]);

  Eigen::MatrixXf PEsub;
  if (hasfield) {
    PEsub.resize(idx.size(), PE.cols());
    for (size_t i = 0; i < idx.size(); i++)
      PEsub.row(i) = PE.row(idx[i]);
  }

  // SSP
  DWI::SVR::SSP<float> ssp (DEFAULT_SSPW);
  opt = get_options("ssp");
  if (opt.size()) {
    std::string t = opt[0][0];
    try {
      ssp = DWI::SVR::SSP<float>(std::stof(t));
    } catch (std::invalid_argument& e) {
      try {
        Eigen::VectorXf v = load_vector<float>(t);
        ssp = DWI::SVR::SSP<float>(v);
      } catch (...) {
        throw Exception ("Invalid argument for SSP.");
      }
    }
  }

  // Read voxel weights
  Eigen::VectorXf Wvox = Eigen::VectorXf::Ones(dwisub.size(0)*dwisub.size(1)*dwisub.size(2)*dwisub.size(3));
  opt = get_options("voxweights");
  if (opt.size()) {
    auto voxweights = Image<value_type>::open(opt[0][0]);
    check_dimensions(dwisub, voxweights, 0, 4);
    size_t j = 0;
    for (auto l = Loop("loading voxel weights data", {0, 1, 2, 3})(voxweights); l; l++, j++) {
      Wvox[j] = voxweights.value();
    }
  }

  // Other parameters
  if (rf.empty())
    lmax = get_option_value("lmax", DEFAULT_LMAX);
  else
    lmax = std::min(lmax, (int) get_option_value("lmax", lmax));

  float reg = get_option_value("reg", DEFAULT_REG);
  float zreg = get_option_value("zreg", DEFAULT_ZREG);

  value_type tol = get_option_value("tolerance", DEFAULT_TOL);
  size_t maxiter = get_option_value("maxiter", DEFAULT_MAXITER);

  DWI::SVR::QSpaceBasis qbasis {gradsub, lmax, rf, motionsub};

  size_t ncoefs = qbasis.get_ncoefs();
  size_t padding = get_option_value("padding", Math::SH::NforL(lmax));
  if (padding < Math::SH::NforL(lmax))
    throw Exception("user-provided padding too small.");


  // Create source header - needed due to stride handling
  Header srchdr (dwisub);
  Stride::set (srchdr, {1, 2, 3, 4});
  DWI::set_DW_scheme (srchdr, gradsub);
  srchdr.datatype() = DataType::Float32;
  srchdr.sanitise();

  // Create recon header
  Header rechdr (dwisub);
  opt = get_options("template");
  if (opt.size()) {
    rechdr = Header::open(opt[0][0]);
  }
  rechdr.ndim() = 4;
  rechdr.size(3) = ncoefs;
  Stride::set (rechdr, {2, 3, 4, 1});
  rechdr.datatype() = DataType::Float32;
  rechdr.sanitise();


  // Create mapping
  DWI::SVR::ReconMapping map (rechdr, srchdr, qbasis, motionsub, ssp);

  // Set up scattered data matrix
  INFO("initialise reconstruction matrix");
  DWI::SVR::ReconMatrix R (map, reg, zreg);
  R.setWeights(Wsub);

  R.setVoxelWeights(Wvox);

//  if (hasfield)
//    R.setField(fieldmap, fieldidx, PEsub);




  // Read input data to vector (this enforces positive strides!)
  Eigen::VectorXf y (R.rows()); y.setZero();
  size_t j = 0;
  for (auto lv = Loop("loading image data", {0, 1, 2, 3})(dwisub); lv; lv++, j++) {
      float w = Wsub(size_t(dwisub.index(2)), size_t(dwisub.index(3))) * Wvox[j];
      y[j] = std::sqrt(w) * dwisub.value();
  }

  // Fit scattered data in basis...
  INFO("initialise conjugate gradient solver");

  Eigen::LeastSquaresConjugateGradient<DWI::SVR::ReconMatrix, Eigen::IdentityPreconditioner> cg;
  cg.compute(R);

  cg.setTolerance(tol);
  cg.setMaxIterations(maxiter);

  // Solve y = M x
  Eigen::VectorXf x (R.cols());
  opt = get_options("init");
  if (opt.size()) {
    // load initialisation
    auto init = Image<value_type>::open(opt[0][0]).with_direct_io({3, 4, 5, 2, 1});
    check_dimensions(rechdr, init, 0, 3);
    if ((init.size(3) != shells.count()) || (init.size(4) < Math::SH::NforL(lmax)))
      throw Exception("dimensions of init image don't match.");
    // init vector
    Eigen::VectorXf x0 (R.cols());
    // convert from mssh
    Eigen::VectorXf c (shells.count() * Math::SH::NforL(lmax));
    Eigen::MatrixXf x2mssh (c.size(), ncoefs); x2mssh.setZero();
    for (int k = 0; k < shells.count(); k++)
      x2mssh.middleRows(k*Math::SH::NforL(lmax), Math::SH::NforL(lmax)) = qbasis.getShellBasis(k).transpose();
    auto mssh2x = x2mssh.fullPivHouseholderQr();
    size_t j = 0, k = 0;
    for (auto l = Loop("loading initialisation", {0, 1, 2})(init); l; l++, j+=ncoefs) {
      k = 0;
      for (auto l2 = Loop(3)(init); l2; l2++) {
        for (init.index(4) = 0; init.index(4) < Math::SH::NforL(lmax); init.index(4)++)
          c[k++] = std::isfinite((float) init.value()) ? init.value() : 0.0f;
      }
      x0.segment(j, ncoefs) = mssh2x.solve(c);
    }
    INFO("solve from given starting point");
    x = cg.solveWithGuess(y, x0);
  }
  else {
    INFO("solve from zero starting point");
    x = cg.solve(y);
  }

  CONSOLE("CG: #iterations: " + str(cg.iterations()));
  CONSOLE("CG: estimated error: " + str(cg.error()));


  // Write result to output file
  Header msshhdr (rechdr);
  msshhdr.ndim() = 5;
  msshhdr.size(3) = shells.count();
  msshhdr.size(4) = padding;
  Stride::set_from_command_line (msshhdr, {3, 4, 5, 2, 1});
  msshhdr.datatype() = DataType::from_command_line (DataType::Float32);
  PhaseEncoding::set_scheme (msshhdr, Eigen::MatrixXf());
  // store b-values and counts
  {
  std::stringstream ss;
  for (auto b : shells.get_bvalues())
    ss << b << ",";
  std::string key = "shells";
  std::string val = ss.str(); val.erase(val.length()-1);
  msshhdr.keyval()[key] = val;
  } {
  std::stringstream ss;
  for (auto c : shells.get_counts())
    ss << c << ",";
  std::string key = "shellcounts";
  std::string val = ss.str(); val.erase(val.length()-1);
  msshhdr.keyval()[key] = val;
  }

  auto out = Image<value_type>::create (argument[1], msshhdr);

  j = 0;
  Eigen::VectorXf c (ncoefs);
  Eigen::VectorXf sh (padding); sh.setZero();
  for (auto l = Loop("writing result to image", {0, 1, 2})(out); l; l++, j+=ncoefs) {
    c = x.segment(j, ncoefs);
    for (int k = 0; k < shells.count(); k++) {
      out.index(3) = k;
      sh.head(Math::SH::NforL(lmax)) = qbasis.getShellBasis(k).transpose() * c;
      out.row(4) = sh;
    }
  }


  // Output source prediction
  bool complete = get_options("complete").size();
  opt = get_options("spred");
  if (opt.size()) {
    srchdr.size(3) = (complete) ? dwi.size(3) : dwisub.size(3);
    auto spred = Image<value_type>::create(opt[0][0], srchdr);
    auto recon = ImageView<value_type>(rechdr, x.data());
    map.x2y(recon, spred);
  }


}



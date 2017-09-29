// This file is part of the AliceVision project and is made available under
// the terms of the MPL2 license (see the COPYING.md file).

#include <aliceVision/config.hpp>
#include <aliceVision/localization/VoctreeLocalizer.hpp>
#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_CCTAG)
#include <aliceVision/localization/CCTagLocalizer.hpp>
#endif
#include <aliceVision/rig/Rig.hpp>
#include <aliceVision/image/io.hpp>
#include <aliceVision/dataio/FeedProvider.hpp>
#include <aliceVision/feature/ImageDescriber.hpp>
#include <aliceVision/robustEstimation/estimators.hpp>
#include <aliceVision/system/Logger.hpp>

#include <boost/filesystem.hpp>
#include <boost/progress.hpp>
#include <boost/program_options.hpp> 
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/sum.hpp>
#include <boost/ptr_container/ptr_vector.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <memory>

#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_ALEMBIC)
#include <aliceVision/sfm/AlembicExporter.hpp>
#endif


namespace bfs = boost::filesystem;
namespace bacc = boost::accumulators;
namespace po = boost::program_options;

using namespace aliceVision;

std::string myToString(std::size_t i, std::size_t zeroPadding)
{
  std::stringstream ss;
  ss << std::setw(zeroPadding) << std::setfill('0') << i;
  return ss.str();
}

/**
 * @brief It checks if the value for the reprojection error or the matching error
 * is compatible with the given robust estimator. The value cannot be 0 for 
 * LORansac, for ACRansac a value of 0 means to use infinity (ie estimate the 
 * threshold during ransac process)
 * @param e The estimator to be checked.
 * @param value The value for the reprojection or matching error.
 * @return true if the value is compatible
 */
bool checkRobustEstimator(robustEstimation::EROBUST_ESTIMATOR e, double &value)
{
  if(e != robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_LORANSAC &&
     e != robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_ACRANSAC)
  {
    ALICEVISION_CERR("Only " << robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_ACRANSAC 
            << " and " << robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_LORANSAC 
            << " are supported.");
    return false;
  }
  if(value == 0 && 
     e == robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_ACRANSAC)
  {
    // for acransac set it to infinity
    value = std::numeric_limits<double>::infinity();
  }
  // for loransac we need thresholds > 0
  if(e == robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_LORANSAC)
  {
    const double minThreshold = 1e-6;
    if(value <= minThreshold)
    {
      ALICEVISION_CERR("Error: errorMax and matchingError cannot be 0 with " 
              << robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_LORANSAC 
              << " estimator.");
      return false;     
    }
  }

  return true;
}

int main(int argc, char** argv)
{
  // common parameters
  /// the AliceVision .json/abc data file
  std::string sfmFilePath;
  /// the the folder containing the descriptors
  std::string descriptorsFolder;
  /// the media file to localize
  std::vector<std::string> mediaPath;
  /// the calibration file for each camera
  std::vector<std::string> cameraIntrinsics;
  /// the file containing the calibration data for the file (subposes)
  std::string rigCalibPath;
  
  /// the describer types name to use for the matching
  std::string matchDescTypeNames = feature::EImageDescriberType_enumToString(feature::EImageDescriberType::SIFT);
  /// the preset for the feature extractor
  feature::EImageDescriberPreset featurePreset = feature::EImageDescriberPreset::NORMAL;
  /// the describer types to use for the matching
  std::vector<feature::EImageDescriberType> matchDescTypes;
  /// the estimator to use for resection
  robustEstimation::EROBUST_ESTIMATOR resectionEstimator = robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_ACRANSAC;        
  /// the estimator to use for matching
  robustEstimation::EROBUST_ESTIMATOR matchingEstimator = robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_ACRANSAC;        
  /// the possible choices for the estimators as strings
  const std::string str_estimatorChoices = ""+robustEstimation::EROBUST_ESTIMATOR_enumToString(robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_ACRANSAC)
                                          +","+robustEstimation::EROBUST_ESTIMATOR_enumToString(robustEstimation::EROBUST_ESTIMATOR::ROBUST_ESTIMATOR_LORANSAC);
  bool refineIntrinsics = false;
  bool useLocalizeRigNaive = false;
  /// the maximum error allowed for resection
  double resectionErrorMax = 4.0;
  /// the maximum error allowed for image matching with geometric validation
  double matchingErrorMax = 4.0;
  /// the maximum angular error allowed for rig resectioning (in degrees)
  double angularThreshold = 0.1;


  // parameters for voctree localizer
  /// whether to use the voctreeLocalizer or cctagLocalizer
  bool useVoctreeLocalizer = true;
  /// the vocabulary tree file
  std::string vocTreeFilepath;
  /// the vocabulary tree weights file
  std::string weightsFilepath;
  /// the localization algorithm to use for the voctree localizer
  std::string algostring = "AllResults";
  /// number of documents to search when querying the voctree
  std::size_t numResults = 4;
  /// maximum number of matching documents to retain
  std::size_t maxResults = 10;
  
  // parameters for cctag localizer
  std::size_t nNearestKeyFrames = 5;

  /// the Alembic export file
  std::string exportAlembicFile = "trackedcameras.abc";

  std::size_t numCameras = 0;
  po::options_description allParams("This program is used to localize a camera rig composed of internally calibrated cameras");
  
  po::options_description inputParams("Required input parameters");  
  inputParams.add_options()
      ("sfmdata", po::value<std::string>(&sfmFilePath)->required(),
          "The sfm_data.json kind of file generated by AliceVision.")
      ("mediapath", po::value<std::vector<std::string> >(&mediaPath)->multitoken()->required(),
          "The path to the video file, the folder of the image sequence or a text "
          "file (one image path per line) for each camera of the rig "
          "(eg. --mediapath /path/to/cam1.mov /path/to/cam2.mov).")
      ("calibration", po::value<std::string>(&rigCalibPath)->required(), 
          "The file containing the calibration data for the rig (subposes)")
      ("cameraIntrinsics", po::value<std::vector<std::string> >(&cameraIntrinsics)->multitoken()->required(),
          "The intrinsics calibration file for each camera of the rig. "
          "(eg. --cameraIntrinsics /path/to/calib1.txt /path/to/calib2.txt).");
  
  po::options_description commonParams("Common optional parameters for the localizer");
  commonParams.add_options()
      ("descriptorPath", po::value<std::string>(&descriptorsFolder),
          "Folder containing the .desc.")
      ("matchDescTypes", po::value<std::string>(&matchDescTypeNames)->default_value(matchDescTypeNames),
          "The describer types to use for the matching")
      ("preset", po::value<feature::EImageDescriberPreset>(&featurePreset)->default_value(featurePreset), 
          "Preset for the feature extractor when localizing a new image "
          "{LOW,MEDIUM,NORMAL,HIGH,ULTRA}")
      ("resectionEstimator", po::value<robustEstimation::EROBUST_ESTIMATOR>(&resectionEstimator)->default_value(resectionEstimator), 
          std::string("The type of *sac framework to use for resection "
          "{"+str_estimatorChoices+"}").c_str())
      ("matchingEstimator", po::value<robustEstimation::EROBUST_ESTIMATOR>(&matchingEstimator)->default_value(matchingEstimator), 
          std::string("The type of *sac framework to use for matching "
          "{"+str_estimatorChoices+"}").c_str())
      ("refineIntrinsics", po::bool_switch(&refineIntrinsics),
          "Enable/Disable camera intrinsics refinement for each localized image")
      ("reprojectionError", po::value<double>(&resectionErrorMax)->default_value(resectionErrorMax), 
          "Maximum reprojection error (in pixels) allowed for resectioning. If set "
          "to 0 it lets the ACRansac select an optimal value.")
      ("useLocalizeRigNaive", po::bool_switch(&useLocalizeRigNaive),
          "Enable/Disable the naive method for rig localization: naive method tries "
          "to localize each camera separately. This is enabled by default if the "
          "library has not been built with openGV.")
      ("angularThreshold", po::value<double>(&angularThreshold)->default_value(angularThreshold), 
          "The maximum angular threshold in degrees between feature bearing vector and 3D "
          "point direction. Used only with the opengv method.");
  
  // parameters for voctree localizer
    po::options_description voctreeParams("Parameters specific for the vocabulary tree-based localizer");
    voctreeParams.add_options()
      ("voctree", po::value<std::string>(&vocTreeFilepath),
          "[voctree] Filename for the vocabulary tree")
      ("voctreeWeights", po::value<std::string>(&weightsFilepath),
          "[voctree] Filename for the vocabulary tree weights")
      ("algorithm", po::value<std::string>(&algostring)->default_value(algostring),
          "[voctree] Algorithm type: {FirstBest,AllResults}" )
      ("nbImageMatch", po::value<std::size_t>(&numResults)->default_value(numResults),
          "[voctree] Number of images to retrieve in the database")
      ("maxResults", po::value<std::size_t>(&maxResults)->default_value(maxResults), 
          "[voctree] For algorithm AllResults, it stops the image matching when "
          "this number of matched images is reached. If 0 it is ignored.")
      ("matchingError", po::value<double>(&matchingErrorMax)->default_value(matchingErrorMax), 
          "[voctree] Maximum matching error (in pixels) allowed for image matching with "
          "geometric verification. If set to 0 it lets the ACRansac select "
          "an optimal value.")
#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_CCTAG)
  // parameters for cctag localizer
      ("nNearestKeyFrames", po::value<std::size_t>(&nNearestKeyFrames)->default_value(nNearestKeyFrames),
          "[cctag] Number of images to retrieve in database")
#endif
    ;
    
  // output options
  po::options_description outputParams("Options for the output of the localizer");
  outputParams.add_options()  
      ("help,h", "Print this message")
#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_ALEMBIC)
      ("outputAlembic", po::value<std::string>(&exportAlembicFile)->default_value(exportAlembicFile),
          "Filename for the SfMData export file (where camera poses will be stored). "
          "Default : trackedcameras.abc.")
#endif
          ;

  allParams.add(inputParams).add(outputParams).add(commonParams).add(voctreeParams);

  po::variables_map vm;

  try
  {
    po::store(po::parse_command_line(argc, argv, allParams), vm);

    if(vm.count("help") || (argc == 1))
    {
      ALICEVISION_COUT(allParams);
      return EXIT_SUCCESS;
    }

    po::notify(vm);
  }
  catch(boost::program_options::required_option& e)
  {
    ALICEVISION_CERR("ERROR: " << e.what() << std::endl);
    ALICEVISION_COUT("Usage:\n\n" << allParams);
    return EXIT_FAILURE;
  }
  catch(boost::program_options::error& e)
  {
    ALICEVISION_CERR("ERROR: " << e.what() << std::endl);
    ALICEVISION_COUT("Usage:\n\n" << allParams);
    return EXIT_FAILURE;
  }

  if(!checkRobustEstimator(matchingEstimator, matchingErrorMax) || 
     !checkRobustEstimator(resectionEstimator, resectionErrorMax))
  {
    return EXIT_FAILURE;
  }
  
  // check that we have the same number of feeds as the intrinsics
  if((mediaPath.size() != cameraIntrinsics.size()))
  {
    ALICEVISION_CERR("The number of intrinsics and the number of cameras are not the same." << std::endl);
    return EXIT_FAILURE;
  }
  numCameras = mediaPath.size();

  // Init descTypes from command-line string
  matchDescTypes = feature::EImageDescriberType_stringToEnums(matchDescTypeNames);

#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_CCTAG)
  useVoctreeLocalizer = !(matchDescTypes.size() == 1 &&
                        ((matchDescTypes.front() == feature::EImageDescriberType::CCTAG3) ||
                        (matchDescTypes.front() == feature::EImageDescriberType::CCTAG4)));
#endif

  // just debugging prints, print out all the parameters
  {
    ALICEVISION_COUT("Program called with the following parameters:");
    ALICEVISION_COUT("\tsfmdata: " << sfmFilePath);
    ALICEVISION_COUT("\tpreset: " << featurePreset);
    ALICEVISION_COUT("\tmediapath: " << mediaPath);
    ALICEVISION_COUT("\tcameraIntrinsics: " << cameraIntrinsics);
    ALICEVISION_COUT("\tcalibration: " << rigCalibPath);
    ALICEVISION_COUT("\tresectionEstimator: " << resectionEstimator);
    ALICEVISION_COUT("\tmatchingEstimator: " << matchingEstimator);
    ALICEVISION_COUT("\tdescriptorPath: " << descriptorsFolder);
    ALICEVISION_COUT("\trefineIntrinsics: " << refineIntrinsics);
    ALICEVISION_COUT("\tuseLocalizeRigNaive: " << useLocalizeRigNaive);
    ALICEVISION_COUT("\treprojectionError: " << resectionErrorMax);
    ALICEVISION_COUT("\tangularThreshold: " << angularThreshold);
    ALICEVISION_COUT("\tnCameras: " << numCameras);
    ALICEVISION_COUT("\tmatching descriptor types: " << matchDescTypeNames);
    if(useVoctreeLocalizer)
    {
      // parameters for voctree localizer
      ALICEVISION_COUT("\tvoctree: " << vocTreeFilepath);
      ALICEVISION_COUT("\tweights: " << weightsFilepath);
      ALICEVISION_COUT("\tnbImageMatch: " << numResults);
      ALICEVISION_COUT("\tmaxResults: " << maxResults);
      ALICEVISION_COUT("\talgorithm: " << algostring);
      ALICEVISION_COUT("\tmatchingError: " << matchingErrorMax);
    }
#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_CCTAG)
    else
    {
      ALICEVISION_COUT("\tnNearestKeyFrames: " << nNearestKeyFrames);
    }
#endif
#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_ALEMBIC)
    ALICEVISION_COUT("\toutputAlembic: " << exportAlembicFile);
#endif
  }

  std::unique_ptr<localization::LocalizerParameters> param;
  
  std::unique_ptr<localization::ILocalizer> localizer;
  
  // initialize the localizer according to the chosen type of describer
  if(useVoctreeLocalizer)
  {
    ALICEVISION_COUT("Localizing sequence using the voctree localizer");
    localization::VoctreeLocalizer* tmpLoc = new localization::VoctreeLocalizer(sfmFilePath,
                                                            descriptorsFolder,
                                                            vocTreeFilepath,
                                                            weightsFilepath,
                                                            matchDescTypes
                                                            );
    localizer.reset(tmpLoc);
    
    localization::VoctreeLocalizer::Parameters *tmpParam = new localization::VoctreeLocalizer::Parameters();
    param.reset(tmpParam);
    tmpParam->_algorithm = localization::VoctreeLocalizer::initFromString(algostring);;
    tmpParam->_numResults = numResults;
    tmpParam->_maxResults = maxResults;
    tmpParam->_ccTagUseCuda = false;
    tmpParam->_matchingError = matchingErrorMax;
    
  }
#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_CCTAG)
  else
  {
    localization::CCTagLocalizer* tmpLoc = new localization::CCTagLocalizer(sfmFilePath, descriptorsFolder);
    localizer.reset(tmpLoc);
    
    localization::CCTagLocalizer::Parameters *tmpParam = new localization::CCTagLocalizer::Parameters();
    param.reset(tmpParam);
    tmpParam->_nNearestKeyFrames = nNearestKeyFrames;
  }
#endif 

  assert(localizer);
  assert(param);
  
  // set other common parameters
  param->_featurePreset = featurePreset;
  param->_refineIntrinsics = refineIntrinsics;
  param->_errorMax = resectionErrorMax;
  param->_resectionEstimator = resectionEstimator;
  param->_matchingEstimator = matchingEstimator;
  param->_useLocalizeRigNaive = useLocalizeRigNaive;
  param->_angularThreshold = D2R(angularThreshold);

  if(!localizer->isInit())
  {
    ALICEVISION_CERR("ERROR while initializing the localizer!");
    return EXIT_FAILURE;
  }

#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_ALEMBIC)
  sfm::AlembicExporter exporter(exportAlembicFile);
  exporter.initAnimatedCamera("rig");
  exporter.addPoints(localizer->getSfMData().GetLandmarks());
  
  boost::ptr_vector<sfm::AlembicExporter> cameraExporters;
  cameraExporters.reserve(numCameras);

  // this contains the full path and the root name of the file without the extension
  const std::string basename = (bfs::path(exportAlembicFile).parent_path() / bfs::path(exportAlembicFile).stem()).string();

  for(std::size_t i = 0; i < numCameras; ++i)
  {
    cameraExporters.push_back( new sfm::AlembicExporter(basename+".cam"+myToString(i, 2)+".abc"));
    cameraExporters.back().initAnimatedCamera("cam"+myToString(i, 2));
  }
#endif

  std::vector<dataio::FeedProvider*> feeders(numCameras);
  std::vector<std::string> subMediaFilepath(numCameras);
  
  // Init the feeder for each camera
  for(std::size_t idCamera = 0; idCamera < numCameras; ++idCamera)
  {
    const std::string &calibFile = cameraIntrinsics[idCamera];
    const std::string &feedPath = mediaPath[idCamera];
    // contains the directory where the video, the images or the filelist is
    subMediaFilepath[idCamera] = 
        bfs::is_directory(bfs::path(mediaPath[idCamera])) ? 
          (mediaPath[idCamera]) : 
          (bfs::path(mediaPath[idCamera]).parent_path().string());

    // create the feedProvider
    feeders[idCamera] = new dataio::FeedProvider(feedPath, calibFile);
    if(!feeders[idCamera]->isInit())
    {
      ALICEVISION_CERR("ERROR while initializing the FeedProvider for the camera " 
              << idCamera << " " << feedPath);
      return EXIT_FAILURE;
    }
  }

  
  bool haveImage = true;
  std::size_t frameCounter = 0;
  std::size_t numLocalizedFrames = 0;
  
  // load the subposes
  std::vector<geometry::Pose3> vec_subPoses;
  if(numCameras > 1)
    rig::loadRigCalibration(rigCalibPath, vec_subPoses);
  assert(vec_subPoses.size() == numCameras-1);
  geometry::Pose3 rigPose;
  
  // Define an accumulator set for computing the mean and the
  // standard deviation of the time taken for localization
  bacc::accumulator_set<double, bacc::stats<bacc::tag::mean, bacc::tag::min, bacc::tag::max, bacc::tag::sum > > stats;

  // store the result
  std::vector< std::vector<localization::LocalizationResult> > rigResultPerFrame;
  
  while(haveImage)
  {
    // @fixme It's better to have arrays of pointers...
    std::vector<image::Image<unsigned char> > vec_imageGrey;
    std::vector<camera::PinholeRadialK3 > vec_queryIntrinsics;
    vec_imageGrey.reserve(numCameras);
    vec_queryIntrinsics.reserve(numCameras);
           
    // for each camera get the image and the associated internal parameters
    for(std::size_t idCamera = 0; idCamera < numCameras; ++idCamera)
    {
      image::Image<unsigned char> imageGrey;
      camera::PinholeRadialK3 queryIntrinsics;
      bool hasIntrinsics = false;
      std::string currentImgName;
      haveImage = feeders[idCamera]->readImage(imageGrey, queryIntrinsics, currentImgName, hasIntrinsics);
      feeders[idCamera]->goToNextFrame();

      if(!haveImage)
      {
        if(idCamera > 0)
        {
          // this is quite odd, it means that eg the fist camera has an image but
          // one of the others has not image
          ALICEVISION_CERR("This is weird... Camera " << idCamera << " seems not to have any available images while some other cameras do...");
          return EXIT_FAILURE;  // a bit harsh but if we are here it's cheesy to say the less
        }
        break;
      }
      
      // for now let's suppose that the cameras are calibrated internally too
      if(!hasIntrinsics)
      {
        ALICEVISION_CERR("For now only internally calibrated cameras are supported!"
                << "\nCamera " << idCamera << " does not have calibration for image " << currentImgName);
        return EXIT_FAILURE;  // a bit harsh but if we are here it's cheesy to say the less
      }
      
      vec_imageGrey.push_back(imageGrey);
      vec_queryIntrinsics.push_back(queryIntrinsics);
    }
    
    if(!haveImage)
    {
      // no more images are available
      break;
    }
    
    ALICEVISION_COUT("******************************");
    ALICEVISION_COUT("FRAME " << myToString(frameCounter, 4));
    ALICEVISION_COUT("******************************");
    auto detect_start = std::chrono::steady_clock::now();
    std::vector<localization::LocalizationResult> localizationResults;
    const bool isLocalized = localizer->localizeRig(vec_imageGrey,
                                                    param.get(),
                                                    vec_queryIntrinsics,
                                                    vec_subPoses,
                                                    rigPose,
                                                    localizationResults);
    auto detect_end = std::chrono::steady_clock::now();
    auto detect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(detect_end - detect_start);
    ALICEVISION_COUT("Localization took  " << detect_elapsed.count() << " [ms]");
    stats(detect_elapsed.count());
    
    rigResultPerFrame.push_back(localizationResults);
    
    if(isLocalized)
    {
      ++numLocalizedFrames;
#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_ALEMBIC)
      // save the position of the main camera
      exporter.addCameraKeyframe(rigPose, &vec_queryIntrinsics[0], subMediaFilepath[0], frameCounter, frameCounter);
      assert(cameraExporters.size()==numCameras);
      assert(localizationResults.size()==numCameras);
      assert(vec_queryIntrinsics.size()==numCameras);
      // save the position of all cameras of the rig
      for(std::size_t camIDX = 0; camIDX < numCameras; ++camIDX)
      {
        ALICEVISION_COUT("cam pose" << camIDX << "\n" <<  localizationResults[camIDX].getPose().rotation() << "\n" << localizationResults[camIDX].getPose().center());
        if(camIDX > 0)
          ALICEVISION_COUT("cam subpose" << camIDX-1 << "\n" <<  vec_subPoses[camIDX-1].rotation() << "\n" << vec_subPoses[camIDX-1].center());
        cameraExporters[camIDX].addCameraKeyframe(localizationResults[camIDX].getPose(), &vec_queryIntrinsics[camIDX], subMediaFilepath[camIDX], frameCounter, frameCounter);
      }
#endif
    }
    else
    {
     ALICEVISION_CERR("Unable to localize frame " << frameCounter);
#if ALICEVISION_IS_DEFINED(ALICEVISION_HAVE_ALEMBIC)
      exporter.jumpKeyframe();
      assert(cameraExporters.size()==numCameras);
      for(std::size_t camIDX = 0; camIDX < numCameras; ++camIDX)
      {
        cameraExporters[camIDX].jumpKeyframe();
      }
#endif
    }

    ++frameCounter;
  }
  
  // print out some time stats
  ALICEVISION_COUT("\n\n******************************");
  ALICEVISION_COUT("Localized " << numLocalizedFrames << " / " << frameCounter << " images");
  ALICEVISION_COUT("Processing took " << bacc::sum(stats) / 1000 << " [s] overall");
  ALICEVISION_COUT("Mean time for localization:   " << bacc::mean(stats) << " [ms]");
  ALICEVISION_COUT("Max time for localization:   " << bacc::max(stats) << " [ms]");
  ALICEVISION_COUT("Min time for localization:   " << bacc::min(stats) << " [ms]");
}

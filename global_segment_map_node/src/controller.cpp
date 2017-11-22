// Copyright 2017 Margarita Grinvald, ASL, ETH Zurich, Switzerland

#include <cmath>
#include <string>
#include <unordered_set>

#include <glog/logging.h>
#include <minkindr_conversions/kindr_tf.h>
#include <modelify/object_toolbox/object_toolbox.h>
#include <pcl/io/vtk_lib_io.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl_conversions/pcl_conversions.h>
#include <visualization_msgs/MarkerArray.h>
#include <voxblox/integrator/merge_integration.h>
#include <voxblox_ros/conversions.h>
#include <voxblox_ros/mesh_vis.h>

#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

#include "voxblox_gsm/controller.h"

namespace voxblox_gsm {

enum Method { kSimple = 0, kMerged, kMergedDiscard };
bool updatedMesh;
boost::mutex updateMeshMutex;

// TODO(grinvalm): make it more efficient by only updating the
// necessary polygons and not all of them each time.
void visualizeMesh(const voxblox::MeshLayer& mesh_layer) {
  boost::shared_ptr<pcl::visualization::PCLVisualizer> viewer(
      new pcl::visualization::PCLVisualizer("GSM viewer"));
  viewer->setBackgroundColor(0, 0, 0);
  viewer->addCoordinateSystem(0.2);
  viewer->initCameraParameters();
  // TODO(grinvalm): find some general default parameters.
  viewer->setCameraPosition(6.29004, 2.67376, -0.678248, -0.423442, 0.895521,
                            -0.136893);

  pcl::PointCloud<pcl::PointXYZRGB> cloud;
  pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr cloud_ptr(&cloud);
  pcl::PointCloud<pcl::Normal> normals;
  pcl::PointCloud<pcl::Normal>::ConstPtr normals_ptr(&normals);

  while (!viewer->wasStopped()) {
    constexpr int updateIntervalms = 100;
    viewer->spinOnce(updateIntervalms);

    boost::mutex::scoped_lock updatedMeshLock(updateMeshMutex);

    if (updatedMesh) {
      normals.points.clear();
      cloud.points.clear();

      voxblox::Mesh combined_mesh(mesh_layer.block_size(),
                                  voxblox::Point::Zero());

      // Combine everything in the layer into one giant combined mesh.
      size_t v = 0u;
      voxblox::BlockIndexList mesh_indices;
      mesh_layer.getAllAllocatedMeshes(&mesh_indices);
      for (const voxblox::BlockIndex& block_index : mesh_indices) {
        voxblox::Mesh::ConstPtr mesh =
            mesh_layer.getMeshPtrByIndex(block_index);

        for (const voxblox::Point& vert : mesh->vertices) {
          combined_mesh.vertices.push_back(vert);
          combined_mesh.indices.push_back(v++);
        }

        for (const voxblox::Color& color : mesh->colors) {
          combined_mesh.colors.push_back(color);
        }

        for (const voxblox::Point& normal : mesh->normals) {
          normals.points.push_back(
              pcl::Normal(normal(0), normal(1), normal(2)));
        }
      }

      size_t vert_idx = 0;
      for (const voxblox::Point& vert : combined_mesh.vertices) {
        const voxblox::Color& color = combined_mesh.colors[vert_idx];
        pcl::PointXYZRGB point = pcl::PointXYZRGB(color.r, color.g, color.b);
        point.x = vert(0);
        point.y = vert(1);
        point.z = vert(2);
        cloud.points.push_back(point);

        vert_idx++;
      }

      pcl::PointCloud<pcl::PointXYZRGBNormal> cloud_with_normals;
      pcl::concatenateFields(cloud, normals, cloud_with_normals);

      pcl::PCLPointCloud2 pcl_pc;
      pcl::toPCLPointCloud2(cloud_with_normals, pcl_pc);

      std::vector<pcl::Vertices> polygons;

      for (size_t i = 0u; i < combined_mesh.indices.size(); i += 3u) {
        pcl::Vertices face;
        for (int j = 0; j < 3; j++) {
          face.vertices.push_back(combined_mesh.indices.at(i + j));
        }
        polygons.push_back(face);
      }

      pcl::PolygonMesh mesh;
      mesh.cloud = pcl_pc;
      mesh.polygons = polygons;

      viewer->removePolygonMesh("meshes");
      if (!viewer->updatePolygonMesh(mesh, "meshes")) {
        viewer->addPolygonMesh(mesh, "meshes", 0);
      }

      updatedMesh = false;
    }
    updatedMeshLock.unlock();
  }
}

Controller::Controller(ros::NodeHandle* node_handle_private)
    : node_handle_private_(node_handle_private),
      // Increased time limit for lookup in the past of tf messages
      // to give some slack to the pipeline and not lose any messages.
      tf_listener_(ros::Duration(100)) {
  CHECK_NOTNULL(node_handle_private_);

  map_config_.voxel_size = 0.01f;
  map_config_.voxels_per_side = 8u;
  map_.reset(new voxblox::LabelTsdfMap(map_config_));

  voxblox::LabelTsdfIntegrator::Config integrator_config;
  integrator_config.voxel_carving_enabled = false;
  integrator_config.default_truncation_distance = map_config_.voxel_size * 2.0;

  std::string method("merged");
  node_handle_private_->param("method", method, method);
  if (method.compare("merged") == 0) {
    integrator_config.enable_anti_grazing = false;
  } else if (method.compare("merged_discard") == 0) {
    integrator_config.enable_anti_grazing = true;
  } else {
    integrator_config.enable_anti_grazing = false;
  }

  voxblox::LabelTsdfIntegrator::LabelTsdfConfig label_tsdf_integrator_config;

  integrator_.reset(new voxblox::LabelTsdfIntegrator(
      integrator_config, label_tsdf_integrator_config, map_->getTsdfLayerPtr(),
      map_->getLabelLayerPtr(), map_->getHighestLabelPtr()));

  mesh_layer_.reset(new voxblox::MeshLayer(map_->block_size()));
  mesh_integrator_.reset(new voxblox::MeshLabelIntegrator(
      mesh_config_, map_->getTsdfLayerPtr(), map_->getLabelLayerPtr(),
      mesh_layer_.get()));

  // If set, use a timer to progressively integrate the mesh.
  double update_mesh_every_n_sec = 0.0;

  if (update_mesh_every_n_sec > 0.0) {
    update_mesh_timer_ = node_handle_private_->createTimer(
        ros::Duration(update_mesh_every_n_sec), &Controller::updateMeshEvent,
        this);
  }

  callback_count_ = 0u;

  bool interactiveViewer = false;
  if (interactiveViewer) {
    boost::thread visualizerThread(visualizeMesh, boost::ref(*mesh_layer_));
  }
  ros::spinOnce();
}

Controller::~Controller() {}

void Controller::subscribeSegmentPointCloudTopic(
    ros::Subscriber* segment_point_cloud_sub) {
  CHECK_NOTNULL(segment_point_cloud_sub);
  // TODO(grinvalm): parametrize this.
  std::string segment_point_cloud_topic = "/scenenet_node/object_segment";
  node_handle_private_->param<std::string>("segment_point_cloud_topic",
                                           segment_point_cloud_topic,
                                           segment_point_cloud_topic);

  // Large queue size to give slack to the algorithm
  // pipeline and not lose any messages.
  *segment_point_cloud_sub = node_handle_private_->subscribe(
      segment_point_cloud_topic, 2000, &Controller::segmentPointCloudCallback,
      this);
}

void Controller::advertiseMeshTopic(ros::Publisher* mesh_pub) {
  CHECK_NOTNULL(mesh_pub);
  *mesh_pub = node_handle_private_->advertise<visualization_msgs::MarkerArray>(
      "mesh", 1, true);

  mesh_pub_ = mesh_pub;
}

void Controller::serviceValidateMergedObjectTopic(
    ros::ServiceServer* validate_merged_object_srv) {
  CHECK_NOTNULL(validate_merged_object_srv);
  static const std::string kValidateMergedObjectTopicRosParam =
      "validate_merged_object";
  std::string validate_merged_object_topic =
      "validate_merged_object";
  node_handle_private_->param<std::string>(kValidateMergedObjectTopicRosParam,
                                           validate_merged_object_topic,
                                           validate_merged_object_topic);

  *validate_merged_object_srv = node_handle_private_->advertiseService(
      validate_merged_object_topic, &Controller::validateMergedObjectCallback,
      this);
}

void Controller::advertiseGenerateMeshService(
    ros::ServiceServer* generate_mesh_srv) {
  CHECK_NOTNULL(generate_mesh_srv);
  *generate_mesh_srv = node_handle_private_->advertiseService(
      "generate_mesh", &Controller::generateMeshCallback, this);
}

void Controller::advertiseExtractSegmentsService(
    ros::ServiceServer* extract_segments_srv) {
  CHECK_NOTNULL(extract_segments_srv);
  *extract_segments_srv = node_handle_private_->advertiseService(
      "extract_segments", &Controller::extractSegmentsCallback, this);
}

void Controller::segmentPointCloudCallback(
    const sensor_msgs::PointCloud2::Ptr& segment_point_cloud_msg) {
  ROS_INFO("Segment pointcloud callback n.%u", ++callback_count_);
  ROS_INFO("Timestamp of segment: %f.",
           segment_point_cloud_msg->header.stamp.toSec());

  // Message timestamps are used to detect when all
  // segment messages from a certain frame have arrived.
  // Since segments from the same frame all have the same timestamp,
  // the start of a new frame is detected when the message timestamp changes.
  // TODO(grinvalm): need additional check for the last frame to be integrated.
  if (segment_point_cloud_msg->header.stamp != last_segment_msg_timestamp_) {
    ROS_INFO("Deciding labels for %lu pointclouds.",
             segments_to_integrate_.size());
    ros::WallTime start = ros::WallTime::now();

    integrator_->decideLabelPointClouds(&segments_to_integrate_,
                                        &segment_label_candidates);

    ros::WallTime end = ros::WallTime::now();
    ROS_INFO("Finished deciding labels in %f seconds.", (end - start).toSec());

    bool merge = false;
    constexpr bool kIsFreespacePointcloud = false;

    if (merge) {
      ROS_INFO("Merging %lu pointclouds.", segments_to_integrate_.size());
      // Merge pointclouds
      voxblox::Pointcloud points_C_;
      voxblox::Transformation T_G_C_;
      voxblox::Colors colors_;
      voxblox::Labels labels_;

      for (const auto& segment : segments_to_integrate_) {
        points_C_.insert(points_C_.end(), segment->points_C_.begin(),
                         segment->points_C_.end());
        colors_.insert(colors_.end(), segment->colors_.begin(),
                       segment->colors_.end());
        labels_.insert(labels_.end(), segment->labels_.begin(),
                       segment->labels_.end());
        T_G_C_ = segment->T_G_C_;
      }

      ROS_INFO("Integrating %lu merged pointclouds with %lu points.",
               segments_to_integrate_.size(), points_C_.size());
      start = ros::WallTime::now();

      integrator_->integratePointCloud(T_G_C_, points_C_, colors_, labels_,
                                       kIsFreespacePointcloud);

      end = ros::WallTime::now();
      ROS_INFO("Finished integrating merged pointclouds in %f seconds.",
               (end - start).toSec());
    } else {
      ROS_INFO("Integrating %lu pointclouds.", segments_to_integrate_.size());
      start = ros::WallTime::now();

      for (const auto& segment : segments_to_integrate_) {
        integrator_->integratePointCloud(segment->T_G_C_, segment->points_C_,
                                         segment->colors_, segment->labels_,
                                         kIsFreespacePointcloud);
      }
      integrator_->mergeLabels();

      end = ros::WallTime::now();
      ROS_INFO("Finished integrating pointclouds in %f seconds.",
               (end - start).toSec());
    }

    ROS_INFO("Clearing candidates and memory.");
    start = ros::WallTime::now();

    segment_label_candidates.clear();
    for (voxblox::Segment* segment : segments_to_integrate_) {
      delete segment;
    }
    segments_to_integrate_.clear();

    end = ros::WallTime::now();
    ROS_INFO("Finished clearing memory in %f seconds.", (end - start).toSec());
  }

  last_segment_msg_timestamp_ = segment_point_cloud_msg->header.stamp;

  // Look up transform from camera frame to world frame.
  voxblox::Transformation T_G_C;
  std::string world_frame_id = "world";
  std::string camera_frame_id = "/scenenet_camera_frame";
  // TODO(grinvalm): nicely parametrize the frames.
  //  std::string camera_frame_id = segment_point_cloud_msg->header.frame_id;
  //  std::string camera_frame_id = "/openni_depth_optical_frame";
  //  std::string camera_frame_id = "/camera_depth_optical_frame";

  if (lookupTransform(camera_frame_id, world_frame_id,
                      segment_point_cloud_msg->header.stamp, &T_G_C)) {
    voxblox::Segment* segment = new voxblox::Segment();
    segments_to_integrate_.push_back(segment);

    // Convert the PCL pointcloud into voxblox format.
    // TODO(helenol): improve...
    // Horrible hack fix to fix color parsing colors in PCL.
    for (size_t d = 0; d < segment_point_cloud_msg->fields.size(); ++d) {
      if (segment_point_cloud_msg->fields[d].name == std::string("rgb")) {
        segment_point_cloud_msg->fields[d].datatype =
            sensor_msgs::PointField::FLOAT32;
      }
    }

    modelify::PointCloudType point_cloud;
    pcl::fromROSMsg(*segment_point_cloud_msg, point_cloud);

    // Filter out NaNs. :|
    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(point_cloud, point_cloud, indices);

    for (size_t i = 0; i < point_cloud.points.size(); ++i) {
      if (std::isnan(point_cloud.points[i].x) ||
          std::isnan(point_cloud.points[i].y) ||
          std::isnan(point_cloud.points[i].z)) {
        continue;
      }

      segment->points_C_.push_back(voxblox::Point(point_cloud.points[i].x,
                                                  point_cloud.points[i].y,
                                                  point_cloud.points[i].z));

      segment->colors_.push_back(
          voxblox::Color(point_cloud.points[i].r, point_cloud.points[i].g,
                         point_cloud.points[i].b, point_cloud.points[i].a));
    }

    segment->T_G_C_ = T_G_C;

    ROS_INFO("Computing label candidates for a pointcloud of size %lu.",
             segment->points_C_.size());

    ros::WallTime start = ros::WallTime::now();
    integrator_->computeSegmentLabelCandidates(segment,
                                               &segment_label_candidates);

    ros::WallTime end = ros::WallTime::now();
    ROS_INFO("Finished computing label candidates in %f seconds.",
             (end - start).toSec());

    ROS_INFO_STREAM("Timings: " << std::endl
                                << voxblox::timing::Timing::Print());
  }
}

bool Controller::validateMergedObjectCallback(
    modelify_msgs::ValidateMergedObject::Request& request,
    modelify_msgs::ValidateMergedObject::Response& response) {
  typedef voxblox::TsdfVoxel VoxelType;
  typedef voxblox::Transformation Transformation;
  typedef kindr::minimal::RotationQuaternionTemplate<float> RotationQuaternion;
  typedef voxblox::Layer<VoxelType> Layer;
  // TODO(ff): Do the following afterwards in modelify.
  // - Check if merged object agrees with whole map (at all poses).
  // - If it doesn't agree at all poses try the merging again with the
  // reduced set of objects.
  // - Optionally put the one that doesn't agree with
  // the others in a list not to merge with the other merged ones

  // Extract TSDF layer of merged object.
  std::shared_ptr<Layer> merged_object_layer_O;
  CHECK(voxblox::deserializeMsgToLayer(request.gsm_update.object.tsdf_layer,
                                       merged_object_layer_O.get()))
      << "Deserializing of TSDF layer from merged object message failed.";

  // Extract transformations.
  std::vector<Transformation> transforms_W_O;
  // TODO(ff): I guess transforms should be part of object and not of the
  // gsm_update.
  for (geometry_msgs::Transform transform : request.gsm_update.transforms) {
    RotationQuaternion quaternion(transform.rotation.w, transform.rotation.x,
                                  transform.rotation.y, transform.rotation.z);
    Eigen::Vector3f translation(transform.translation.x,
                                transform.translation.y,
                                transform.translation.z);
    Transformation t(quaternion, translation);
    transforms_W_O.emplace_back(quaternion, translation);
  }

  voxblox::utils::VoxelEvaluationMode voxel_evaluation_mode =
      voxblox::utils::VoxelEvaluationMode::kEvaluateAllVoxels;
  voxblox::utils::VoxelEvaluationDetails voxel_evaluation_details;
  size_t idx = 0u;
  // Check if world TSDF layer agrees with merged object at all object poses.
  for (Transformation transform_W_O : transforms_W_O) {
    std::shared_ptr<Layer> merged_object_layer_W;

    // Transform merged object into the world frame.
    voxblox::transformLayer<VoxelType>(*merged_object_layer_O.get(),
                                       transform_W_O.inverse(),
                                       merged_object_layer_W.get());

    // Evaluate the RMSE of the merged object layer in the world layer.
    voxblox::utils::evaluateLayersRmse(
        *(map_->getTsdfLayerPtr()), *merged_object_layer_W,
        voxel_evaluation_mode, &voxel_evaluation_details);
    // TODO(ff): Move this to modelify_ros or voxblox_ros conversions.h.
    response.voxel_evaluation_details[idx].rmse = voxel_evaluation_details.rmse;
    response.voxel_evaluation_details[idx].max_error =
        voxel_evaluation_details.max_error;
    response.voxel_evaluation_details[idx].min_error =
        voxel_evaluation_details.min_error;
    response.voxel_evaluation_details[idx].num_evaluated_voxels =
        voxel_evaluation_details.num_evaluated_voxels;
    response.voxel_evaluation_details[idx].num_ignored_voxels =
        voxel_evaluation_details.num_ignored_voxels;
    response.voxel_evaluation_details[idx].num_overlapping_voxels =
        voxel_evaluation_details.num_overlapping_voxels;
    response.voxel_evaluation_details[idx].num_non_overlapping_voxels =
        voxel_evaluation_details.num_non_overlapping_voxels;
    ++idx;
    CHECK_LT(idx, transforms_W_O.size());
  }
  return true;
}

bool Controller::generateMeshCallback(
    std_srvs::Empty::Request& request,
    std_srvs::Empty::Response& response) {  // NOLINT
  voxblox::timing::Timer generate_mesh_timer("mesh/generate");
  boost::mutex::scoped_lock updateMeshLock(updateMeshMutex);
  const bool clear_mesh = true;
  if (clear_mesh) {
    constexpr bool only_mesh_updated_blocks = false;
    constexpr bool clear_updated_flag = true;
    mesh_integrator_->generateMesh(only_mesh_updated_blocks,
                                   clear_updated_flag);
  } else {
    constexpr bool only_mesh_updated_blocks = true;
    constexpr bool clear_updated_flag = true;
    mesh_integrator_->generateMesh(only_mesh_updated_blocks,
                                   clear_updated_flag);
  }
  generate_mesh_timer.Stop();

  voxblox::timing::Timer publish_mesh_timer("mesh/publish");
  visualization_msgs::MarkerArray marker_array;
  marker_array.markers.resize(1);
  fillMarkerWithMesh(mesh_layer_, voxblox::ColorMode::kColor,
                     &marker_array.markers[0]);
  // TODO(grinvalm): world frame param.
  voxblox::ColorMode color_mode_ = voxblox::ColorMode::kColor;
  std::string world_frame_ = "world";
  marker_array.markers[0].header.frame_id = world_frame_;
  mesh_pub_->publish(marker_array);

  publish_mesh_timer.Stop();

  // TODO(grinvalm): param.
  std::string mesh_filename_ = "voxblox_gsm_mesh.ply";

  if (!mesh_filename_.empty()) {
    voxblox::timing::Timer output_mesh_timer("mesh/output");
    bool success = outputMeshLayerAsPly(mesh_filename_, *mesh_layer_);
    output_mesh_timer.Stop();
    if (success) {
      ROS_INFO("Output file as PLY: %s", mesh_filename_.c_str());
    } else {
      ROS_INFO("Failed to output mesh as PLY: %s", mesh_filename_.c_str());
    }
  }
  // TODO(grinvalm): flag rename or remove?

  updatedMesh = true;
  updateMeshLock.unlock();

  ROS_INFO_STREAM("Mesh Timings: " << std::endl
                                   << voxblox::timing::Timing::Print());
  return true;
}

void Controller::updateMeshEvent(const ros::TimerEvent& e) {
  boost::mutex::scoped_lock updateMeshLock(updateMeshMutex);
  bool updated = false;

  voxblox::timing::Timer generate_mesh_timer("mesh/update");
  constexpr bool only_mesh_updated_blocks = true;
  constexpr bool clear_updated_flag = true;
  mesh_integrator_->generateMesh(only_mesh_updated_blocks, clear_updated_flag);

  updateMeshLock.unlock();

  generate_mesh_timer.Stop();

  // TODO(helenol): also think about how to update markers incrementally?
  voxblox::timing::Timer publish_mesh_timer("mesh/publish");
  visualization_msgs::MarkerArray marker_array;
  marker_array.markers.resize(1);
  fillMarkerWithMesh(mesh_layer_, voxblox::ColorMode::kColor,
                     &marker_array.markers[0]);
  marker_array.markers[0].header.frame_id = "world";
  mesh_pub_->publish(marker_array);

  publish_mesh_timer.Stop();
}

bool Controller::extractSegmentsCallback(std_srvs::Empty::Request& request,
                                         std_srvs::Empty::Response& response) {
  // Get list of all labels in the map.
  std::vector<voxblox::Label> labels = integrator_->getLabelsList();

  // TODO(grinvalm): there are more segments extracted than there really are.
  // Seems like their voxel count is greater than 0, but when meshed they are
  // too small to result in any polygon. Find a fix, maybe with a size
  // threshold.

  for (auto& label : labels) {
    voxblox::Layer<voxblox::TsdfVoxel> tsdf_layer(map_config_.voxel_size,
                                                  map_config_.voxels_per_side);
    voxblox::Layer<voxblox::LabelVoxel> label_layer(
        map_config_.voxel_size, map_config_.voxels_per_side);
    // Extract the TSDF and label layers corresponding to a segment.
    extractSegmentLayers(label, &tsdf_layer, &label_layer);

    voxblox::MeshLayer mesh_layer(map_->block_size());
    voxblox::MeshLabelIntegrator mesh_integrator(mesh_config_, &tsdf_layer,
                                                 &label_layer, &mesh_layer);
    constexpr bool only_mesh_updated_blocks = false;
    constexpr bool clear_updated_flag = true;
    mesh_integrator_->generateMesh(only_mesh_updated_blocks,
                                   clear_updated_flag);

    boost::filesystem::path segments_dir("segments");
    boost::filesystem::create_directory(segments_dir);

    std::string mesh_filename =
        "segments/voxblox_gsm_mesh_label_" + std::to_string(label) + ".ply";

    bool success = voxblox::outputMeshLayerAsPly(mesh_filename, mesh_layer);
    if (success) {
      ROS_INFO("Output segment file as PLY: %s", mesh_filename.c_str());
    } else {
      ROS_INFO("Failed to output mesh as PLY: %s", mesh_filename.c_str());
    }
  }
  return true;
}

void Controller::extractSegmentLayers(
    voxblox::Label label, voxblox::Layer<voxblox::TsdfVoxel>* tsdf_layer,
    voxblox::Layer<voxblox::LabelVoxel>* label_layer) {
  voxblox::BlockIndexList all_label_blocks;
  map_->getTsdfLayerPtr()->getAllAllocatedBlocks(&all_label_blocks);

  for (const voxblox::BlockIndex& block_index : all_label_blocks) {
    voxblox::Block<voxblox::TsdfVoxel>::Ptr global_tsdf_block =
        map_->getTsdfLayerPtr()->getBlockPtrByIndex(block_index);
    voxblox::Block<voxblox::LabelVoxel>::Ptr global_label_block =
        map_->getLabelLayerPtr()->getBlockPtrByIndex(block_index);

    size_t vps = global_label_block->voxels_per_side();
    for (int i = 0; i < vps * vps * vps; i++) {
      voxblox::LabelVoxel& global_label_voxel =
          global_label_block->getVoxelByLinearIndex(i);
      if (global_label_voxel.label == label) {
        voxblox::Block<voxblox::TsdfVoxel>::Ptr tsdf_block;
        voxblox::Block<voxblox::LabelVoxel>::Ptr label_block;

        tsdf_block = tsdf_layer->allocateBlockPtrByIndex(block_index);
        label_block = label_layer->allocateBlockPtrByIndex(block_index);

        voxblox::TsdfVoxel& tsdf_voxel = tsdf_block->getVoxelByLinearIndex(i);
        voxblox::LabelVoxel& label_voxel =
            label_block->getVoxelByLinearIndex(i);

        voxblox::TsdfVoxel& global_tsdf_voxel =
            global_tsdf_block->getVoxelByLinearIndex(i);

        tsdf_voxel = global_tsdf_voxel;
        label_voxel = global_label_voxel;
      }
    }
  }
}

bool Controller::lookupTransform(const std::string& from_frame,
                                 const std::string& to_frame,
                                 const ros::Time& timestamp,
                                 voxblox::Transformation* transform) {
  tf::StampedTransform tf_transform;
  ros::Time time_to_lookup = timestamp;

  // If this transform isn't possible at the time, then try to just look up
  // the latest (this is to work with bag files and static transform
  // publisher, etc).
  if (!tf_listener_.canTransform(to_frame, from_frame, time_to_lookup)) {
    time_to_lookup = ros::Time(0);
    LOG(ERROR) << "Using latest TF transform instead of timestamp match.";
  }

  try {
    tf_listener_.lookupTransform(to_frame, from_frame, time_to_lookup,
                                 tf_transform);
  } catch (tf::TransformException& ex) {
    LOG(ERROR) << "Error getting TF transform from sensor data: " << ex.what();
    return false;
  }

  tf::transformTFToKindr(tf_transform, transform);
  return true;
}

}  // namespace voxblox_gsm
/* includes //{ */

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <libcamera_ros/libcamera_ros.h>

#include <libcamera_ros_driver/utils/clamp.h>
#include <libcamera_ros_driver/utils/format_mapping.h>
#include <libcamera_ros_driver/utils/stream_mapping.h>
#include <libcamera_ros_driver/utils/control_mapping.h>
#include <libcamera_ros_driver/utils/pretty_print.h>
#include <libcamera_ros_driver/utils/type_extent.h>
#include <libcamera_ros_driver/utils/types.h>
#include <libcamera_ros_driver/utils/pv_to_cv.h>
#include <libcamera_ros_driver/utils/is_vector.h>

#include <ros/ros.h>
#include <nodelet/nodelet.h>
#include <camera_info_manager/camera_info_manager.h>
#include <image_transport/image_transport.h>

#include <std_msgs/Header.h>
#include <sensor_msgs/CameraInfo.h>

//}

namespace libcamera_ros_driver
{

/* ParamCheck() method //{ */
template <typename T>
bool getOptionalParamCheck(const ros::NodeHandle &nh, const std::string &node_name, const std::string &param_name, T &param_out) {

  if (!nh.getParam(param_name, param_out)) {
    return false;
  }

  ROS_INFO_STREAM("[LibcameraRosDriver]: " << param_name << "': " << param_out);

  return true;
}

template <typename T>
bool getCompulsoryParamCheck(const ros::NodeHandle &nh, const std::string &node_name, const std::string &param_name, T &param_out) {

  const bool res = nh.getParam(param_name, param_out);

  if (!res) {
    ROS_ERROR_STREAM("[LibcameraRosDriver]: Could not load compulsory parameter '" << param_name << "'");
  } else {
    ROS_INFO_STREAM("[LibcameraRosDriver]: Loaded parameter " << param_name << "': " << param_out);
  }

  return res;
}

template <typename T>
bool getCompulsoryParamCheck(const ros::NodeHandle &nh, const std::string &node_name, const std::string &param_name, T &param_out, const T &param_default) {

  const bool res = nh.getParam(param_name, param_out);

  if (!res) {
    param_out = param_default;
  }

  ROS_INFO_STREAM("[LibcameraRosDriver]: Loaded parameter '" << param_name << "': " << param_out);

  return res;
}

//}

/* class LibcameraRosDriver //{ */

class LibcameraRosDriver : public nodelet::Nodelet {
public:
  virtual void onInit();
  ~LibcameraRosDriver();

private:
  ros::NodeHandle nh_;

  libcamera::CameraManager                         camera_manager_;
  std::shared_ptr<libcamera::Camera>               camera_;
  libcamera::Stream *                              stream_;
  std::shared_ptr<libcamera::FrameBufferAllocator> allocator_;
  std::vector<std::unique_ptr<libcamera::Request>> requests_;
  std::mutex                                       request_lock_;

  std::string frame_id_;

  bool _use_ros_time_ = false;
  bool remove_stride_ = false;

  ros::Duration start_time_offset_;
  bool          start_time_offset_obtained_ = false;

  struct buffer_info_t
  {
    void * data;
    size_t size;
  };
  std::unordered_map<const libcamera::FrameBuffer *, buffer_info_t> buffer_info_;

  std::shared_ptr<camera_info_manager::CameraInfoManager> cinfo_;

  image_transport::CameraPublisher image_pub_;
  std::mutex                       image_pub_mutex_;

  // map parameter names to libcamera control id
  std::unordered_map<std::string, const libcamera::ControlId *> parameter_ids_;
  // parameters that are to be set for every request
  std::unordered_map<unsigned int, libcamera::ControlValue> parameters_;

  void declareControlParameters();
  void requestComplete(libcamera::Request *request);

  bool updateControlParameter(const libcamera::ControlValue &value, const libcamera::ControlId *id);
};

//}

/* LibcameraRosDriver::onInit method //{ */

void LibcameraRosDriver::onInit() {

  /* obtain node handle */
  nh_ = nodelet::Nodelet::getMTPrivateNodeHandle();

  /* waits for the ROS to publish clock */
  ros::Time::waitForValid();

  /* load parameters //{ */

  bool        success = true;
  std::string camera_name;
  std::string stream_role;
  std::string pixel_format;
  std::string calib_url;
  int         camera_id;
  int         resolution_width;
  int         resolution_height;
  
  remove_stride_ = false;

  success = success && getCompulsoryParamCheck(nh_, "LibcameraRosDriver", "camera_name", camera_name);
  success = success && getOptionalParamCheck(nh_, "LibcameraRosDriver", "camera_id", camera_id);
  success = success && getCompulsoryParamCheck(nh_, "LibcameraRosDriver", "stream_role", stream_role);
  success = success && getCompulsoryParamCheck(nh_, "LibcameraRosDriver", "pixel_format", pixel_format);
  success = success && getCompulsoryParamCheck(nh_, "LibcameraRosDriver", "frame_id", frame_id_);
  success = success && getCompulsoryParamCheck(nh_, "LibcameraRosDriver", "calib_url", calib_url);
  success = success && getCompulsoryParamCheck(nh_, "LibcameraRosDriver", "resolution/width", resolution_width);
  success = success && getCompulsoryParamCheck(nh_, "LibcameraRosDriver", "resolution/height", resolution_height);
  success = success && getCompulsoryParamCheck(nh_, "LibcameraRosDriver", "use_ros_time", _use_ros_time_);
  success = success && getOptionalParamCheck(nh_, "LibcameraRosDriver", "remove_stride", remove_stride_);


  if (!success) {
    ROS_ERROR("[LibcameraRosDriver]: Some compulsory parameters were not loaded successfully, ending the node");
    ros::shutdown();
    return;
  }

  //}

  // start camera manager and check for cameras
  camera_manager_.start();
  if (camera_manager_.cameras().empty()) {
    ROS_ERROR("[LibcameraRosDriver]: no cameras available");
    ros::shutdown();
    return;
  }

  if (!camera_name.empty()) {
    std::vector<std::string> available_cameras;
    ROS_INFO_STREAM("[LibcameraRosDriver]: Available cameras:");
    for (int i = 0; i < camera_manager_.cameras().size(); i++) {
      available_cameras.push_back(camera_manager_.cameras().at(i)->id());
    }
    for (int i = 0; i < available_cameras.size(); i++) {
      if (available_cameras.at(i).find(camera_name) != std::string::npos) {
        ROS_INFO_STREAM("[LibcameraRosDriver]: found camera: " << camera_name << " index: " << i << " at: " << available_cameras.at(i));
        camera_id = i;
        break;
      }
    }
  }

  if (camera_id >= camera_manager_.cameras().size()) {
    ROS_INFO_STREAM(camera_manager_);
    ROS_ERROR_STREAM("[LibcameraRosDriver]: camera with id " << camera_name << " does not exist");
    ros::shutdown();
    return;
  }
  camera_ = camera_manager_.cameras().at(camera_id);
  ROS_INFO_STREAM("[LibcameraRosDriver]: Use camera by id: " << camera_id);

  if (!camera_) {
    ROS_INFO_STREAM("[LibcameraRosDriver]: " << camera_manager_);
    ROS_ERROR_STREAM("[LibcameraRosDriver]: "
                     << "camera with name " << camera_name << " does not exist");
    ros::shutdown();
    return;
  }

  if (camera_->acquire()) {
    ROS_ERROR("[LibcameraRosDriver]: failed to acquire camera");
    ros::shutdown();
    return;
  }

  // configure camera stream
  std::unique_ptr<libcamera::CameraConfiguration> cfg = camera_->generateConfiguration({get_role(stream_role)});

  if (!cfg) {
    ROS_ERROR("[LibcameraRosDriver]: failed to generate configuration");
    ros::shutdown();
    return;
  }

  libcamera::StreamConfiguration &scfg = cfg->at(0);

  // get common pixel formats that are supported by the camera and the node
  const libcamera::StreamFormats            stream_formats = get_common_stream_formats(scfg.formats());
  const std::vector<libcamera::PixelFormat> common_fmt     = stream_formats.pixelformats();

  if (common_fmt.empty()) {
    ROS_ERROR("[LibcameraRosDriver]: camera does not provide any of the supported pixel formats");
    ros::shutdown();
    return;
  }

  if (pixel_format.empty()) {

    // auto select first common pixel format
    scfg.pixelFormat = common_fmt.front();  // get pixel format from provided string
    ROS_INFO_STREAM("[LibcameraRosDriver]: " << stream_formats);
    ROS_WARN_STREAM("[LibcameraRosDriver]: no pixel format selected, using default: \"" << scfg.pixelFormat << "\"");
    ROS_WARN_STREAM("[LibcameraRosDriver]: set parameter 'pixel_format' to silent this warning");
  } else {

    // get pixel format from provided string
    const libcamera::PixelFormat format_requested = libcamera::PixelFormat::fromString(pixel_format);

    if (!format_requested.isValid()) {
      ROS_INFO_STREAM("[LibcameraRosDriver]: " << stream_formats);
      ROS_ERROR_STREAM("[LibcameraRosDriver]: invalid pixel format: \"" << pixel_format << "\"");
      ros::shutdown();
      return;
    }

    // check that the requested format is supported by camera and the node
    if (std::find(common_fmt.begin(), common_fmt.end(), format_requested) == common_fmt.end()) {
      ROS_INFO_STREAM("[LibcameraRosDriver]: " << stream_formats);
      ROS_ERROR_STREAM("[LibcameraRosDriver]: unsupported pixel format \"" << pixel_format << "\"");
      ros::shutdown();
      return;
    }

    scfg.pixelFormat = format_requested;
  }

  const libcamera::Size size(resolution_width, resolution_height);

  if (size.isNull()) {
    ROS_INFO_STREAM(scfg);
    scfg.size = scfg.formats().sizes(scfg.pixelFormat).back();
    ROS_WARN_STREAM("[LibcameraRosDriver]: no dimensions selected, auto-selecting: \"" << scfg.size << "\"");
    ROS_WARN_STREAM("[LibcameraRosDriver]: set parameters 'resolution/width' and 'resolution/height' to silent this warning");
  } else {
    scfg.size = size;
  }

  // store selected stream configuration
  const libcamera::StreamConfiguration selected_scfg = scfg;

  switch (cfg->validate()) {

    case libcamera::CameraConfiguration::Valid: {
      break;
    }

    case libcamera::CameraConfiguration::Adjusted: {

      if (selected_scfg.pixelFormat != scfg.pixelFormat) {
        ROS_INFO_STREAM(stream_formats);
      }

      if (selected_scfg.size != scfg.size) {
        ROS_INFO_STREAM(scfg);
      }

      ROS_WARN_STREAM("[LibcameraRosDriver]: stream configuration adjusted from \"" << selected_scfg.toString() << "\" to \"" << scfg.toString() << "\"");

      break;
    }

    case libcamera::CameraConfiguration::Invalid: {

      ROS_ERROR("[LibcameraRosDriver]: failed to valid stream configuration");
      ros::shutdown();

      return;
      break;
    }
  }

  if (camera_->configure(cfg.get()) < 0) {
    ROS_ERROR("[LibcameraRosDriver]: failed to configure streams");
    ros::shutdown();
    return;
  }

  ROS_INFO_STREAM("[LibcameraRosDriver]: camera \"" << camera_->id() << "\" configured with " << scfg.toString() << " stream");

  declareControlParameters();

  int              param_int;
  float            param_float;
  std::string      param_string;
  bool             param_bool;
  std::vector<int> param_vector_int;

  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/exposure_time", param_int)) {
    updateControlParameter(pv_to_cv(param_int, parameter_ids_["ExposureTime"]->type()), parameter_ids_["ExposureTime"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/fps", param_float)) {
    int64_t frame_time = 1000000 / param_float;
    updateControlParameter(pv_to_cv(std::vector<int64_t>{frame_time, frame_time}, parameter_ids_["FrameDurationLimits"]->type()),
                           parameter_ids_["FrameDurationLimits"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/ae_constraint_mode", param_string)) {
    updateControlParameter(pv_to_cv(get_ae_constraint_mode(param_string), parameter_ids_["AeConstraintMode"]->type()), parameter_ids_["AeConstraintMode"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/brightness", param_float)) {
    updateControlParameter(pv_to_cv(param_float, parameter_ids_["Brightness"]->type()), parameter_ids_["Brightness"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/sharpness", param_float)) {
    updateControlParameter(pv_to_cv(param_float, parameter_ids_["Sharpness"]->type()), parameter_ids_["Sharpness"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/awb_enable", param_bool)) {
    if (parameter_ids_["AwbEnable"])  // if the parameter is set when not available, we would get a segmentation fault upon extracting its ->type()
      updateControlParameter(pv_to_cv(param_bool, parameter_ids_["AwbEnable"]->type()), parameter_ids_["AwbEnable"]);
    else
      ROS_ERROR_STREAM("[LibcameraRosDriver]: Parameter AwbEnable is not available! Maybe the selected camera is grayscale");
  }
  /* updateControlParameter<std::vector<float>>(std::string("control/colour_gains"), parameter_ids_["ColourGains"]); */
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/ae_enable", param_bool)) {
    updateControlParameter(pv_to_cv(param_bool, parameter_ids_["AeEnable"]->type()), parameter_ids_["AeEnable"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/saturation", param_float)) {
    updateControlParameter(pv_to_cv(param_float, parameter_ids_["Saturation"]->type()), parameter_ids_["Saturation"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/contrast", param_float)) {
    updateControlParameter(pv_to_cv(param_float, parameter_ids_["Contrast"]->type()), parameter_ids_["Contrast"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/exposure_value", param_float)) {
    updateControlParameter(pv_to_cv(param_float, parameter_ids_["ExposureValue"]->type()), parameter_ids_["ExposureValue"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/analogue_gain", param_float)) {
    updateControlParameter(pv_to_cv(param_float, parameter_ids_["AnalogueGain"]->type()), parameter_ids_["AnalogueGain"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/awb_mode", param_string)) {
    updateControlParameter(pv_to_cv(get_awb_mode(param_string), parameter_ids_["AwbMode"]->type()), parameter_ids_["AwbMode"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/ae_metering_mode", param_string)) {
    updateControlParameter(pv_to_cv(get_ae_metering_mode(param_string), parameter_ids_["AeMeteringMode"]->type()), parameter_ids_["AeMeteringMode"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/scaler_crop", param_vector_int)) {
    updateControlParameter(pv_to_cv(std::vector<int64_t>{param_vector_int.begin(), param_vector_int.end()}, parameter_ids_["ScalerCrop"]->type()),
                           parameter_ids_["ScalerCrop"]);
  }
  if (getOptionalParamCheck(nh_, "LibcameraRosDriver", "control/control", param_string)) {
    updateControlParameter(pv_to_cv(get_ae_exposure_mode(param_string), parameter_ids_["AeExposureMode"]->type()), parameter_ids_["AeExposureMode"]);
  }

  // allocate stream buffers and create one request per buffer
  stream_ = scfg.stream();

  allocator_ = std::make_shared<libcamera::FrameBufferAllocator>(camera_);
  allocator_->allocate(stream_);

  for (const std::unique_ptr<libcamera::FrameBuffer> &buffer : allocator_->buffers(stream_)) {

    std::unique_ptr<libcamera::Request> request = camera_->createRequest();

    if (!request) {
      ROS_ERROR("[LibcameraRosDriver]: Can't create request");
      ros::shutdown();
      return;
    }

    // multiple planes of the same buffer use the same file descriptor
    size_t buffer_length = 0;
    int    fd            = -1;
    for (const libcamera::FrameBuffer::Plane &plane : buffer->planes()) {

      if (plane.offset == libcamera::FrameBuffer::Plane::kInvalidOffset) {
        ROS_ERROR("[LibcameraRosDriver]: invalid offset");
        ros::shutdown();
        return;
      }

      buffer_length = std::max<size_t>(buffer_length, plane.offset + plane.length);

      if (!plane.fd.isValid()) {
        ROS_ERROR("[LibcameraRosDriver]: file descriptor is not valid");
        ros::shutdown();
        return;
      }

      if (fd == -1) {
        fd = plane.fd.get();
      } else if (fd != plane.fd.get()) {
        ROS_ERROR("[LibcameraRosDriver]: plane file descriptors differ");
        ros::shutdown();
        return;
      }
    }

    // memory-map the frame buffer planes
    void *data = mmap(nullptr, buffer_length, PROT_READ, MAP_SHARED, fd, 0);

    if (data == MAP_FAILED) {
      ROS_ERROR_STREAM("[LibcameraRosDriver]: mmap failed: " << std::string(std::strerror(errno)));
      ros::shutdown();
      return;
    }

    buffer_info_[buffer.get()] = {data, buffer_length};

    if (request->addBuffer(stream_, buffer.get()) < 0) {
      ROS_ERROR("[LibcameraRosDriver]: Can't set buffer for request");
      ros::shutdown();
      return;
    }

    // set modified control parameters
    for (const auto &[id, value] : parameters_) {
      request->controls().set(id, value);
    }

    requests_.push_back(std::move(request));
  }

  cinfo_ = std::make_shared<camera_info_manager::CameraInfoManager>(nh_, camera_name, calib_url);

  /* initialize publishers //{ */

  image_transport::ImageTransport it(nh_);
  image_pub_ = it.advertiseCamera("image_raw", 5);

  //}

  // register callback
  camera_->requestCompleted.connect(this, &LibcameraRosDriver::requestComplete);

  // start camera and queue all requests
  if (camera_->start()) {
    ROS_ERROR("[LibcameraRosDriver]: failed to start camera");
    ros::shutdown();
    return;
  }

  for (std::unique_ptr<libcamera::Request> &request : requests_) {
    camera_->queueRequest(request.get());
  }

  // | --------------------- finish the init -------------------- |

  ROS_INFO("[LibcameraRosDriver]: initialized");
}
//}

/* LibcameraRosDriver::~LibcameraRosDriver() //{ */

LibcameraRosDriver::~LibcameraRosDriver() {

  camera_->requestCompleted.disconnect();

  {
    std::scoped_lock lock(request_lock_);

    if (camera_->stop()) {
      ROS_ERROR("[LibcameraRosDriver]: failed to stop camera");
    }
  }

  camera_->release();
  camera_manager_.stop();

  for (const auto &e : buffer_info_) {
    if (munmap(e.second.data, e.second.size) == -1) {
      ROS_ERROR_STREAM("[LibcameraRosDriver]: "
                       << "munmap failed: " << std::strerror(errno));
    }
  }
}

//}

/* LibcameraRosDriver::declareControlParameters() //{ */

void LibcameraRosDriver::declareControlParameters() {

  ROS_INFO("[LibcameraRosDriver]: available control parameters:");

  for (const auto &[id, info] : camera_->controls()) {

    std::size_t extent;
    try {
      extent = get_extent(id);
    }
    catch (const std::runtime_error &e) {
      // ignore
      ROS_INFO_STREAM("[LibcameraRosDriver]:     " << id->name() << " : Not handled by the current version of the libcamera SDK");
      continue;
    }

    // store control id with name
    parameter_ids_[id->name()] = id;

    if (info.min().numElements() != info.max().numElements()) {
      ROS_ERROR("[LibcameraRosDriver]: minimum and maximum parameter array sizes do not match");
      ros::shutdown();
      return;
    }

    ROS_INFO_STREAM("[LibcameraRosDriver]:     " << id->name() << " : " << info.toString()
                                                 << (info.def().isNone() ? "" : " (default: {" + info.def().toString() + "})"));
  }
}

//}

/* LibcameraRosDriver::updateControlParameter() //{ */

bool LibcameraRosDriver::updateControlParameter(const libcamera::ControlValue &value, const libcamera::ControlId *id) {

  if (value.isNone()) {
    ROS_ERROR_STREAM("[LibcameraRosDriver]: " << id->name().c_str() << " : parameter type not defined");
    return false;
  }

  // verify parameter type and dimension against default
  const libcamera::ControlInfo &ci = camera_->controls().at(id);

  if (value.type() != id->type()) {
    ROS_ERROR_STREAM("[LibcameraRosDriver]: " << id->name().c_str() << " : parameter types mismatch, expected '" << std::to_string(id->type()).c_str()
                                              << "', got '" << std::to_string(value.type()).c_str() << "'");
    return false;
  }

  const std::size_t extent = get_extent(id);
  if ((value.isArray() && (extent > 0)) && value.numElements() != extent) {
    ROS_ERROR_STREAM("[LibcameraRosDriver]: " << id->name().c_str() << " : parameter dimensions mismatch, expected " << std::to_string(extent).c_str()
                                              << ", got " << std::to_string(value.numElements()).c_str());
    return false;
  }

  // check bounds and return error
  // it seems that for exposition the 0 is used for maximum value, which means infinity
  // therefore, we are checking if max > min. If yes, check  if the value is lower than max.
  if (value < ci.min() || (ci.max() > ci.min() ? value > ci.max() : false)) {
    ROS_ERROR_STREAM("[LibcameraRosDriver]: " << id->name().c_str() << " : parameter value " << value.toString().c_str()
                                              << " outside of range: " << ci.toString().c_str());
    return false;
  }

  parameters_[id->id()] = value;

  return true;
}

//}

/* LibcameraRosDriver::requestComplete() //{ */

void LibcameraRosDriver::requestComplete(libcamera::Request *request) {

  std::scoped_lock lock(request_lock_);

  if (request->status() == libcamera::Request::RequestComplete) {

    assert(request->buffers().size() == 1);

    // get the stream and buffer from the request
    const libcamera::FrameBuffer *  buffer    = request->findBuffer(stream_);
    const libcamera::FrameMetadata &metadata  = buffer->metadata();
    size_t                          bytesused = 0;

    for (const libcamera::FrameMetadata::Plane &plane : metadata.planes()) {
      bytesused += plane.bytesused;
    }

    // send image data
    std_msgs::Header hdr;

    hdr.stamp = ros::Time().fromNSec(metadata.timestamp);
    if (_use_ros_time_) {
      if (!start_time_offset_obtained_) {
        start_time_offset_          = ros::Time::now() - hdr.stamp;
        start_time_offset_obtained_ = true;
      }
      hdr.stamp += start_time_offset_;
    }

    hdr.frame_id                              = frame_id_;
    const libcamera::StreamConfiguration &cfg = stream_->configuration();

    sensor_msgs::Image image_msg;

    if (format_type(cfg.pixelFormat) == FormatType::RAW) {
      // raw uncompressed image
      assert(buffer_info_[buffer].size == bytesused);
      image_msg.header       = hdr;
      image_msg.width        = cfg.size.width;
      image_msg.height       = cfg.size.height;
      image_msg.encoding     = get_ros_encoding(cfg.pixelFormat);
      image_msg.is_bigendian = (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__);
      if (!remove_stride_)
      {
        image_msg.step = cfg.stride;
        image_msg.data.resize(buffer_info_[buffer].size);
        memcpy(image_msg.data.data(), buffer_info_[buffer].data, buffer_info_[buffer].size);
      }
      else{
        // TODO: Change 3 by the number of bytes per pixel
        // TODO: Little endian vs big endian
        image_msg.step = cfg.size.width * 3;
        image_msg.data.resize(cfg.size.width * cfg.size.height * 3);

        // each row of the image is stored in memory as RGBRGBRGB...00000 with stride padding
        // remove the padding to get the correct image
        for (int i = 0; i < cfg.size.height; i++)
        {
          //memcpy(image_msg.data.data() + i * cfg.size.width * 3, buffer_info_[buffer].data + i * cfg.stride, cfg.size.width * 3);
          // the previous line causes error arithmetic on a pointer to void
          memcpy(image_msg.data.data() + i * cfg.size.width * 3, static_cast<uint8_t*>(buffer_info_[buffer].data) + i * cfg.stride, cfg.size.width * 3);
        }
      }

    } else {
      ROS_ERROR_STREAM("[LibcameraRosDriver]: unsupported pixel format: " << stream_->configuration().pixelFormat.toString());
      return;
    }

    sensor_msgs::CameraInfo cinfo_msg = cinfo_->getCameraInfo();
    cinfo_msg.header                  = hdr;

    {
      std::scoped_lock lock(image_pub_mutex_);

      image_pub_.publish(image_msg, cinfo_msg);
    }

  } else if (request->status() == libcamera::Request::RequestCancelled) {
    ROS_ERROR_STREAM("[LibcameraRosDriver]: request '" << request->toString() << "' cancelled");
  }

  // queue the request again for the next frame
  request->reuse(libcamera::Request::ReuseBuffers);
  camera_->queueRequest(request);
}

//}

}  // namespace libcamera_ros_driver

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(libcamera_ros_driver::LibcameraRosDriver, nodelet::Nodelet);

#include "mecanum_robot.hpp"
namespace RoverRobotics {
MecanumRobot::MecanumRobot(const char *device, 
                                     float wheel_radius,
                                     float wheel_base,
                                     float robot_length,
                                     Control::pid_gains pid,
                                     Control::angular_scaling_params angular_scale) {


  /* create object to load/store persistent parameters (ie trim) */
  persistent_params_ = std::make_unique<Utilities::PersistentParams>(ROBOT_PARAM_PATH);

  /* set comm mode: can vs serial vs other */
  comm_type_ = "CAN";

  /* clear main data structure for holding robot status and commands */
  robotstatus_ = {0};

  /* scaling of angular command vs linear speed; useful for teleop */
  angular_scaling_params_ = angular_scale;

  /* Set robot geometry */
  robot_geometry_ = {.intra_axle_distance = robot_length,
                     .wheel_base = wheel_base,
                     .wheel_radius = wheel_radius,
                     .center_of_mass_x_offset = 0,
                     .center_of_mass_y_offset = 0};

  /* clear estop and zero out all motors */
  estop_ = false;
  motors_speeds_[VESC_IDS::FRONT_LEFT] = MOTOR_NEUTRAL_;
  motors_speeds_[VESC_IDS::FRONT_RIGHT] = MOTOR_NEUTRAL_;
  motors_speeds_[VESC_IDS::BACK_LEFT] = MOTOR_NEUTRAL_;
  motors_speeds_[VESC_IDS::BACK_RIGHT] = MOTOR_NEUTRAL_;

  /* make an object to decode and encode motor controller messages*/
  vescArray_ = vesc::BridgedVescArray(
      std::vector<uint8_t>{VESC_IDS::FRONT_LEFT, VESC_IDS::FRONT_RIGHT,
                           VESC_IDS::BACK_LEFT, VESC_IDS::BACK_RIGHT});
  
  /* register the pid gains for closed-loop modes */
  pid_ = pid;

  /* make and initialize the motion logic object */
  mecanum_control__ = std::make_unique<Control::MecanumMotionController>(
      Control::INDEPENDENT_WHEEL, robot_geometry_, pid_, MOTOR_MAX_, MOTOR_MIN_,
      left_trim_, right_trim_, geometric_decay_);

  /* MUST be done after skid control is constructed */
  load_persistent_params();

  mecanum_control__->setOperatingMode(Control::INDEPENDENT_WHEEL);
  mecanum_control__->setAccelerationLimits(
          {LINEAR_JERK_LIMIT_, std::numeric_limits<float>::max()});


  /* set up the comm port */
  register_comm_base(device);

  /* create a dedicated write thread to send commands to the robot on fixed
   * interval */
  write_to_robot_thread_ = std::thread([this]() { this->send_command(30); });

  /* create a dedicate thread to compute the desired robot motion, runs on fixed
   * interval */
  motor_speed_update_thread_ =
      std::thread([this]() { this->motors_control_loop(30); });
}

void MecanumRobot::send_estop(bool estop) {
  robotstatus_mutex_.lock();
  estop_ = estop;
  robotstatus_mutex_.unlock();
}

robotData MecanumRobot::status_request() { 
  robotstatus_mutex_.lock();
  auto returnData = robotstatus_;
  robotstatus_mutex_.unlock();
  return returnData; 
}

robotData MecanumRobot::info_request() { 
  return status_request(); 
}

void MecanumRobot::set_robot_velocity(double *control_array) {
  robotstatus_mutex_.lock();
  robotstatus_.cmd_linear_vel = control_array[0];
  robotstatus_.cmd_angular_vel = control_array[1];
  robotstatus_.cmd_trans_vel = control_array[3];
  robotstatus_.cmd_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  robotstatus_mutex_.unlock();
}

void MecanumRobot::unpack_comm_response(std::vector<uint8_t> robotmsg) {
  auto parsedMsg = vescArray_.parseReceivedMessage(robotmsg);
  if (parsedMsg.dataValid) {
    robotstatus_mutex_.lock();
    switch (parsedMsg.vescId) {
      case (VESC_IDS::FRONT_LEFT):
        robotstatus_.motor1_rpm = parsedMsg.rpm;
        robotstatus_.motor1_id = parsedMsg.vescId;
        robotstatus_.motor1_current = parsedMsg.current;
        break;
      case (VESC_IDS::FRONT_RIGHT):
        robotstatus_.motor2_rpm = parsedMsg.rpm;
        robotstatus_.motor2_id = parsedMsg.vescId;
        robotstatus_.motor2_current = parsedMsg.current;
        break;
      case (VESC_IDS::BACK_LEFT):
        robotstatus_.motor3_rpm = parsedMsg.rpm;
        robotstatus_.motor3_id = parsedMsg.vescId;
        robotstatus_.motor3_current = parsedMsg.current;
        break;
      case (VESC_IDS::BACK_RIGHT):
        robotstatus_.motor4_rpm = parsedMsg.rpm;
        robotstatus_.motor4_id = parsedMsg.vescId;
        robotstatus_.motor4_current = parsedMsg.current;
        break;
      default:
        break;
    }
    robotstatus_mutex_.unlock();
  }
}

bool MecanumRobot::is_connected() { return comm_base_->is_connected(); }

void MecanumRobot::register_comm_base(const char *device) {
  std::vector<uint8_t> setting;
  if (comm_type_ == "CAN") {
    try {
      if(strcmp(device, "internal") != 0) {
        comm_base_ = std::make_unique<CommCan>(
            device, [this](std::vector<uint8_t> c) { unpack_comm_response(c); },
            setting);
      } else {
        /*
        comm_base_ = std::make_unique<CommCanSPI>(
            device, [this](std::vector<uint8_t> c) { unpack_comm_response(c); },
            setting);
        */
      }  
    } catch (int i) {
      throw(i);
    }
  } else {
    throw(-2);
  }
}

void MecanumRobot::send_command(int sleeptime) {
  while (true) {

    /* loop over the motors */
    for (uint8_t vid = VESC_IDS::FRONT_LEFT; vid <= VESC_IDS::BACK_RIGHT;
         vid++) {

      robotstatus_mutex_.lock();
      auto signedMotorCommand = motors_speeds_[vid];

      /* only use current control when robot is stopped to prevent wasted energy
       */
      bool useCurrentControl = motors_speeds_[vid] == MOTOR_NEUTRAL_ &&
                               robotstatus_.linear_vel == MOTOR_NEUTRAL_ &&
                               robotstatus_.trans_vel == MOTOR_NEUTRAL_ &&
                               robotstatus_.angular_vel == MOTOR_NEUTRAL_;

      robotstatus_mutex_.unlock();

      auto msg =  vescArray_.buildCommandMessage((vesc::vescChannelCommand){
              .vescId = vid,
              .commandType = (useCurrentControl ? vesc::vescPacketFlags::CURRENT
                                                : vesc::vescPacketFlags::DUTY),
              .commandValue = (useCurrentControl ? (float)MOTOR_NEUTRAL_ : signedMotorCommand)});

      comm_base_->write_to_device(msg);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(sleeptime));
  }
}

int MecanumRobot::cycle_robot_mode() {
    // ONLY CLOSED LOOP CONTROL FOR 4WD
    return -1;
}

void MecanumRobot::load_persistent_params() {
  
  /* trim (aka curvature correction) */
  if(auto param = persistent_params_->read_param("trim")){
    update_drivetrim(param.value());
    std::cout << "Loaded trim from persistent param file: " << param.value() << std::endl;
  }
}

void MecanumRobot::update_drivetrim(double delta) {

  if (-MAX_CURVATURE_CORRECTION_ < (trimvalue_ + delta) && (trimvalue_ + delta) < MAX_CURVATURE_CORRECTION_) {
    trimvalue_ += delta;

    /* reduce power to right wheels */
    if (trimvalue_ >= 0) {
      left_trim_ = 1;
      right_trim_ = 1 - trimvalue_;
    }
    /* reduce power to left wheels */
    else {
      right_trim_ = 1;
      left_trim_ = 1 + trimvalue_;
    }
    mecanum_control__->setTrim(left_trim_, right_trim_);
    std::cout << "writing trim " << trimvalue_ << " to file " << std::endl;
    persistent_params_->write_param("trim", trimvalue_);
  }
  
}

void MecanumRobot::motors_control_loop(int sleeptime) {
  float linear_vel_target, trans_vel_target, angular_vel_target, rpm_FL, rpm_FR, rpm_BL, rpm_BR;
  std::chrono::milliseconds time_last =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch());
  std::chrono::milliseconds time_from_msg;

  while (true) {
    std::chrono::milliseconds time_now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());

    /* collect user commands and various status */
    robotstatus_mutex_.lock();
    linear_vel_target = robotstatus_.cmd_linear_vel;
    trans_vel_target = robotstatus_.cmd_trans_vel;
    angular_vel_target = robotstatus_.cmd_angular_vel;
    rpm_FL = robotstatus_.motor1_rpm;
    rpm_FR = robotstatus_.motor2_rpm;
    rpm_BL = robotstatus_.motor3_rpm;
    rpm_BR = robotstatus_.motor4_rpm;
    time_from_msg = robotstatus_.cmd_ts;
    robotstatus_mutex_.unlock();

    /* compute motion targets if no estop and data is not stale */
    if (!estop_ &&
        (time_now - time_from_msg).count() <= CONTROL_LOOP_TIMEOUT_MS_) {
      
      /* compute motion targets (not using duty cycle input ATM) */
      auto wheel_speeds = mecanum_control__->runMotionControl(
          (Control::robot_velocities){.linear_velocity = linear_vel_target,
                                      .trans_velocity = trans_vel_target,
                                      .angular_velocity = angular_vel_target},
          (Control::motor_data){.fl = 0, .fr = 0, .rl = 0, .rr = 0},
          (Control::motor_data){
              .fl = rpm_FL, .fr = rpm_FR, .rl = rpm_BL, .rr = rpm_BR});

      
      
      /* compute velocities of robot from wheel rpms */
      auto velocities = mecanum_control__->getMeasuredVelocities((Control::motor_data){
              .fl = rpm_FL, .fr = rpm_FR, .rl = rpm_BL, .rr = rpm_BR});

      /* update the main data structure with both commands and status */
      robotstatus_mutex_.lock();
      motors_speeds_[VESC_IDS::FRONT_LEFT] = wheel_speeds.fl;
      motors_speeds_[VESC_IDS::FRONT_RIGHT] = wheel_speeds.fr;
      motors_speeds_[VESC_IDS::BACK_LEFT] = wheel_speeds.rl;
      motors_speeds_[VESC_IDS::BACK_RIGHT] = wheel_speeds.rr;
      robotstatus_.linear_vel = velocities.linear_velocity;
      robotstatus_.trans_vel = velocities.trans_velocity;
      robotstatus_.angular_vel = velocities.angular_velocity;
      robotstatus_mutex_.unlock();

    } else {

      /* COMMAND THE ROBOT TO STOP */
      auto wheel_speeds = mecanum_control__->runMotionControl(
          {0, 0, 0}, {0, 0, 0, 0}, {rpm_FL, rpm_FR, rpm_BL, rpm_BR});
      auto velocities = mecanum_control__->getMeasuredVelocities((Control::motor_data){
              .fl = rpm_FL, .fr = rpm_FR, .rl = rpm_BL, .rr = rpm_BR});

      /* update the main data structure with both commands and status */
      robotstatus_mutex_.lock();
      motors_speeds_[VESC_IDS::FRONT_LEFT] = MOTOR_NEUTRAL_;
      motors_speeds_[VESC_IDS::FRONT_RIGHT] = MOTOR_NEUTRAL_;
      motors_speeds_[VESC_IDS::BACK_LEFT] = MOTOR_NEUTRAL_;
      motors_speeds_[VESC_IDS::BACK_RIGHT] = MOTOR_NEUTRAL_;
      robotstatus_.linear_vel = velocities.linear_velocity;
      robotstatus_.trans_vel = velocities.trans_velocity;
      robotstatus_.angular_vel = velocities.angular_velocity;
      robotstatus_mutex_.unlock();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleeptime));
  }
}

}  // namespace RoverRobotics

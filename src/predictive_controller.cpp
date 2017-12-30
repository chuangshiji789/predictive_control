
//This file containts read parameter from server, callback, call class objects, control all class, objects of all class

#include <predictive_control/predictive_controller.h>

predictive_control::predictive_control()
{
;
}

predictive_control::~predictive_control()
{
  clearDataMember();
  //delete pd_config_;
  //delete kinematic_solver_;
  //delete collision_detect_;
}

void predictive_control::spinNode()
{
  ROS_INFO(" Predictive control node is running, now it's 'Spinning Node'");
  ros::spin();
}

// diallocated memory
void predictive_control::clearDataMember()
{
  //current_position_ = Eigen::VectorXd(1.0);
  last_position_ = Eigen::VectorXd(degree_of_freedom_);
  //current_velocity_ = Eigen::VectorXd(1.0);
  last_velocity_ = Eigen::VectorXd(degree_of_freedom_);

  // reset FK_Matrix and Jacobian Matrix
  const int jacobian_matrix_rows = 6, jacobian_matrix_columns = degree_of_freedom_;
  FK_Matrix_ = Eigen::Matrix4d::Identity();
  Jacobian_Matrix_.resize(jacobian_matrix_rows, jacobian_matrix_columns);
}

// initialize all helper class of predictive control and subscibe joint state and publish controlled joint velocity
bool predictive_control::initialize()
{
  ros::NodeHandle nh;

  // make sure node is still running
  if (ros::ok())
  {
    // initialize helper classes, make sure pd_config should be initialized first as mother of other class
    pd_config_.reset(new predictive_configuration());
    bool pd_config_success = pd_config_->initialize();

    kinematic_solver_.reset(new Kinematic_calculations());
    bool kinematic_success = kinematic_solver_->initialize();

    collision_detect_.reset(new CollisionRobot());
    bool collision_success = collision_detect_->initializeCollisionRobot();

    pd_trajectory_generator_.reset(new pd_frame_tracker());
    bool pd_traj_success = pd_trajectory_generator_->initialize();

    // check successfully initialization of all classes
    if (pd_config_success == false || kinematic_success == false
        || collision_success == false || pd_traj_success == false || pd_config_->initialize_success_ == false)
    {
      ROS_ERROR("predictive_control: FAILED TO INITILIZED!!");
      std::cout << "States: \n"
                << " pd_config: " << std::boolalpha << pd_config_success << "\n"
                << " kinematic solver: " << std::boolalpha << kinematic_success << "\n"
                << "collision detect: " << std::boolalpha << collision_success << "\n"
                << "pd traj generator: " << std::boolalpha << pd_traj_success << "\n"
                << "pd config init success: " << std::boolalpha << pd_config_->initialize_success_
                << std::endl;
      return false;
    }

    // initialize data member of class
    degree_of_freedom_ = pd_config_->degree_of_freedom_;
    clock_frequency_ = pd_config_->clock_frequency_;

    /// INFO: static function called transformStdVectorToEigenVector define in the predictive_trajectory_generator.h
    goal_tolerance_ = pd_frame_tracker::transformStdVectorToEigenVector<double>(pd_config_->goal_pose_tolerance_);

    /// 3 position and 3 orientation(rpy)
    goal_gripper_pose_.resize(6);
    getTransform(pd_config_->chain_root_link_, pd_config_->target_frame_, goal_gripper_pose_);

    current_gripper_pose_.resize(6);
    getTransform(pd_config_->chain_root_link_, pd_config_->tracking_frame_, current_gripper_pose_);

    cartesian_dist_ = double(0.0);
    rotation_dist_ = double(0.0);

    // DEBUG
    if (pd_config_->activate_output_)
    {
      ROS_WARN("===== GOAL TOLERANCE =====");
      std::cout << goal_tolerance_.transpose() << std::endl;
    }

    // resize position and velocity velocity vectors
    //current_position_ = Eigen::VectorXd(degree_of_freedom_);
    last_position_ = Eigen::VectorXd(degree_of_freedom_);
    //current_velocity_ = Eigen::VectorXd(degree_of_freedom_);
    last_velocity_ = Eigen::VectorXd(degree_of_freedom_);

    // initialize FK_Matrix and Jacobian Matrix
    const int jacobian_matrix_rows = 6, jacobian_matrix_columns = degree_of_freedom_;
    FK_Matrix_ = Eigen::Matrix4d::Identity();
    Jacobian_Matrix_.resize(jacobian_matrix_rows, jacobian_matrix_columns);

    // resize controlled velocity variable, that publishing
    controlled_velocity_.data.resize(degree_of_freedom_, 0.0);
    for (int i=0u; i < degree_of_freedom_; ++i)
      controlled_velocity_.data[i] = last_velocity_(i);

    // ros interfaces
    joint_state_sub_ = nh.subscribe("joint_states", 1, &predictive_control::jointStateCallBack, this);
    controlled_velocity_pub_ = nh.advertise<std_msgs::Float64MultiArray>("joint_group_velocity_controller/command", 1);

    ros::Duration(1).sleep();

    timer_ = nh.createTimer(ros::Duration(1/clock_frequency_), &predictive_control::runNode, this);
    timer_.start();

    ROS_WARN("PREDICTIVE CONTROL INTIALIZED!!");
    return true;
  }
  else
  {
    ROS_ERROR("predictive_control: Failed to initialize as ROS Node is shoutdown");
    return false;
  }
}

// update this function 1/colck_frequency
void predictive_control::runNode(const ros::TimerEvent &event)
{
  // get goal pose w.r.t root link
  getTransform(pd_config_->chain_root_link_, pd_config_->target_frame_, goal_gripper_pose_);

  // update collision ball according to joint angles
  collision_detect_->updateCollisionVolume(kinematic_solver_->FK_Homogenous_Matrix_, kinematic_solver_->Transformation_Matrix_);

  // solver optimal control problem, track frame
  std::cout << "current gripper pose: \n "<< current_gripper_pose_.transpose() << std::endl;
  std::cout << "goal gripper pose: \n "<< goal_gripper_pose_.transpose() << std::endl;

  pd_trajectory_generator_->solveOptimalControlProblem(Jacobian_Matrix_,
                                                       current_gripper_pose_,
                                                       goal_gripper_pose_,
                                                       controlled_velocity_);

  controlled_velocity_pub_.publish(controlled_velocity_);
    // publish zero controlled velocity
    //publishZeroJointVelocity();
}

// read current position and velocity of robot joints
void predictive_control::jointStateCallBack(const sensor_msgs::JointState::ConstPtr& msg)
{
  Eigen::VectorXd current_position = Eigen::VectorXd(degree_of_freedom_);
  Eigen::VectorXd current_velocity = Eigen::VectorXd(degree_of_freedom_);
  int count = 0;

  for (unsigned int i = 0; i < degree_of_freedom_; ++i)
  {
    for (unsigned int j = 0; j < msg->name.size(); ++j)
    {
      // 0 means the contents of both strings are equal
      if ( std::strcmp( msg->name[j].c_str(), pd_config_->joints_name_[i].c_str()) == 0 )
      {
        current_position(i) =  msg->position[j] ;
        current_velocity(i) =  msg->velocity[j] ;
        count++;
        break;
      }
    }
  }

  if (count != degree_of_freedom_)
  {
    ROS_WARN(" Joint names are mismatched, need to check yaml file or code ... joint_state_callBack ");
  }

  else
  {
    last_position_ = current_position;
    last_velocity_ = current_velocity;

    // calculate forward kinematic and Jacobian matrix using current joint values, get current gripper pose using FK_Matrix
    kinematic_solver_->calculateJacobianMatrix(last_position_, FK_Matrix_, Jacobian_Matrix_);
    kinematic_solver_->getGripperPoseVectorFromFK(FK_Matrix_, current_gripper_pose_);

    // Output is active, than only print joint state values
    if (pd_config_->activate_output_)
    {
      std::cout<< "\n --------------------------------------------- \n";
      std::cout << "Current joint position: [ " << current_position.transpose() << " ]" << std::endl;
      std::cout << "Current joint velocity: [ " << current_velocity.transpose() << " ]" << std::endl;
      /*std::cout << "Current joint position: [";
      for_each(current_position.begin(), current_position.end(), [](double& p)
                            { std::cout<< std::setprecision(5) << p << ", " ; }
      );
      std::cout<<"]"<<std::endl;

      std::cout << "Current joint velocity: [";
      for_each(current_velocity.begin(), current_velocity.end(), [](double& v)
                            { std::cout<< std::setprecision(5) << v << ", " ; }
      );
      std::cout<<"]"<<std::endl;*/
      std::cout<< "\n --------------------------------------------- \n";
    }
  }
}



/*
void predictive_control_node::run_node(const ros::TimerEvent& event)
{
	ros::Duration period = event.current_real - event.last_real;

	std::vector<double> current_position_vec_copy = current_position;

	// current pose of gripper using fk, jacobian matrix
	kinematic_solver_->compute_gripper_pose_and_jacobian(current_position_vec_copy, current_gripper_pose, Jacobian_Mat);

	std::vector<geometry_msgs::Vector3> link_length;
	kinematic_solver_->compute_and_get_each_joint_pose(current_position_vec_copy, tranformation_matrix_stamped, link_length);

	std::map<std::string, geometry_msgs::PoseStamped> self_collsion_matrix;
	kinematic_solver_->compute_and_get_each_joint_pose(current_position_vec_copy, self_collsion_matrix);

	std::cout<<"\033[20;1m" << "############"<< "links " << self_collsion_matrix.size() << "###########" << "\033[0m\n" << std::endl;


	int id=0u;
	for (auto it = self_collsion_matrix.begin(); it != self_collsion_matrix.end(); ++it, ++id)
	{
		pd_frame_tracker_->create_collision_ball(it->second, 0.15, id);
	}

	marker_pub.publish(pd_frame_tracker_->get_collision_ball_marker());

	//boost::thread mux_thread{pd_frame_tracker_->generate_self_collision_distance_matrix(self_collsion_matrix)};
	//pd_frame_tracker_->generate_self_collision_distance_matrix(self_collsion_matrix, collision_distance_matrix);
	//mux_thread.join();

	collision_distance_vector = pd_frame_tracker_->compute_self_collision_distance(self_collsion_matrix, 0.20, 0.01);

	// target poseStamped
	pd_frame_tracker_->get_transform("/arm_base_link", new_config.target_frame, target_gripper_pose);

	// optimal problem solver
	//pd_frame_tracker_->solver(J_Mat, current_gripper_pose, joint_velocity_data);
	pd_frame_tracker_->optimal_control_solver(Jacobian_Mat, current_gripper_pose, target_gripper_pose, joint_velocity_data); //, collision_distance_vector

	// error poseStamped, computation of euclidean distance error
	geometry_msgs::PoseStamped tip_Target_Frame_error_stamped;
	pd_frame_tracker_->get_transform(new_config.tip_link, new_config.target_frame, tip_Target_Frame_error_stamped);

	pd_frame_tracker_->compute_euclidean_distance(tip_Target_Frame_error_stamped.pose.position, cartesian_dist);
	pd_frame_tracker_->compute_rotation_distance(tip_Target_Frame_error_stamped.pose.orientation, rotation_dist);

	std::cout<<"\033[36;1m" << "***********************"<< "cartesian distance: " << cartesian_dist << "***********************" << std::endl
							<< "***********************"<< "rotation distance: " << rotation_dist << "***********************" << std::endl
			<< "\033[0m\n" << std::endl;


	if (cartesian_dist < 0.03 && rotation_dist < 0.05)
	{
			publish_zero_jointVelocity();
	}
	else
	{
		joint_velocity_pub.publish(joint_velocity_data);
	}

}
*/

void predictive_control::publishZeroJointVelocity()
{
  controlled_velocity_.data.resize(degree_of_freedom_, 0.0);
  controlled_velocity_.data[0] = 0.0;
  controlled_velocity_.data[1] = 0.0;
  controlled_velocity_.data[2] = 0.0;
  controlled_velocity_.data[3] = 0.0;
  controlled_velocity_.data[4] = 0.0;
  controlled_velocity_.data[5] = 0.0;
  controlled_velocity_.data[6] = 0.0;
  controlled_velocity_pub_.publish(controlled_velocity_);
}

bool predictive_control::getTransform(const std::string& from, const std::string& to, Eigen::VectorXd& stamped_pose)
{
  bool transform = false;
  stamped_pose = Eigen::VectorXd(6);
  tf::StampedTransform stamped_tf;

  // make sure source and target frame exist
  if (tf_listener_.frameExists(to) && tf_listener_.frameExists(from))
  {
    try
    {
      // find transforamtion between souce and target frame
      tf_listener_.waitForTransform(from, to, ros::Time(0), ros::Duration(0.02));
      tf_listener_.lookupTransform(from, to, ros::Time(0), stamped_tf);

      // translation
      stamped_pose(0) = stamped_tf.getOrigin().x();
      stamped_pose(1) = stamped_tf.getOrigin().y();
      stamped_pose(2) = stamped_tf.getOrigin().z();

      // convert quternion to rpy
      tf::Quaternion quat(stamped_tf.getRotation().getX(),
                          stamped_tf.getRotation().getY(),
                          stamped_tf.getRotation().getZ(),
                          stamped_tf.getRotation().getW()
                          );
      tf::Matrix3x3 quat_matrix(quat);
      quat_matrix.getRPY(stamped_pose(3), stamped_pose(4), stamped_pose(5));

      transform = true;
    }
    catch (tf::TransformException& ex)
    {
      ROS_ERROR("predictive_control::getTransform: %s", ex.what());
    }
  }

  else
  {
    ROS_WARN("predictive_control::getTransform: '%s' or '%s' frame doesn't exist, pass existing frame",
             from.c_str(), to.c_str());
  }

  return transform;
}

/*
bool predictive_control::getTransform(const std::string& from, const std::string& to, geometry_msgs::PoseStamped& stamped_pose)
{
  bool transform = false;
  tf::StampedTransform stamped_tf;

  // make sure source and target frame exist
  if (tf_listener_.frameExists(to) & tf_listener_.frameExists(from))
  {
    try
    {
      // find transforamtion between souce and target frame
      tf_listener_.waitForTransform(from, to, ros::Time(0), ros::Duration(0.2));
      tf_listener_.lookupTransform(from, to, ros::Time(0), stamped_tf);

      // rotation
      stamped_pose.pose.orientation.w =  stamped_tf.getRotation().getW();
      stamped_pose.pose.orientation.x =  stamped_tf.getRotation().getX();
      stamped_pose.pose.orientation.y =  stamped_tf.getRotation().getY();
      stamped_pose.pose.orientation.z =  stamped_tf.getRotation().getZ();

      // translation
      stamped_pose.pose.position.x = stamped_tf.getOrigin().x();
      stamped_pose.pose.position.y = stamped_tf.getOrigin().y();
      stamped_pose.pose.position.z = stamped_tf.getOrigin().z();

      // header frame_id should be parent frame
      stamped_pose.header.frame_id = stamped_tf.frame_id_;  //from or to
      stamped_pose.header.stamp = ros::Time(0);

      transform = true;
    }
    catch (tf::TransformException& ex)
    {
      ROS_ERROR("predictive_control_node::getTransform: \n%s", ex.what());
    }
  }

  else
  {
    ROS_WARN("%s or %s frame doesn't exist, pass existing frame", from.c_str(), to.c_str());
  }

  return transform;
}
*/

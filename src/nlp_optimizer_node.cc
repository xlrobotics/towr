/**
 @file    nlp_optimizer_node.cc
 @author  Alexander W. Winkler (winklera@ethz.ch)
 @date    Jul 21, 2016
 @brief   Defines the ROS node that initializes/calls the NLP optimizer.
 */

#include <xpp/ros/nlp_optimizer_node.h>
#include <std_msgs/Int32.h>

#include <kindr/Core>

#include <xpp/ros/ros_conversions.h>
#include <xpp/ros/topic_names.h>

#include <xpp_msgs/RobotStateCartesianTrajectory.h> // publish
#include <xpp_msgs/OptParameters.h> // publish

namespace xpp {
namespace ros {


NlpOptimizerNode::NlpOptimizerNode ()
{
  ::ros::NodeHandle n;

  user_command_sub_ = n.subscribe(xpp_msgs::goal_state_topic, 1,
                                &NlpOptimizerNode::UserCommandCallback, this);

  current_state_sub_ = n.subscribe(xpp_msgs::curr_robot_state_real,
                                    1, // take only the most recent information
                                    &NlpOptimizerNode::CurrentStateCallback, this,
                                    ::ros::TransportHints().tcpNoDelay());

  cart_trajectory_pub_  = n.advertise<xpp_msgs::RobotStateCartesianTrajectory>
                                    (xpp_msgs::robot_trajectory_cart, 1);

  opt_parameters_pub_  = n.advertise<xpp_msgs::OptParameters>
                                    (xpp_msgs::opt_parameters, 1);

  dt_          = RosConversions::GetDoubleFromServer("/xpp/trajectory_dt");
  rosbag_name_ = RosConversions::GetStringFromServer("/xpp/rosbag_name") + ".bag";

}

void
NlpOptimizerNode::CurrentStateCallback (const StateMsg& msg)
{
  auto curr_state = RosConversions::RosToXpp(msg);
  SetInitialState(curr_state);

//  ROS_INFO_STREAM("Received Current Real State");
//  std::cout << curr_state.GetBase() << std::endl;
//  for (auto p : curr_state.GetEEPos().ToImpl()) {
//    std::cout << p << std::endl;
//  }

//  OptimizeMotion();
//  PublishTrajectory();
}

void
NlpOptimizerNode::OptimizeMotion ()
{
  try {
    motion_optimizer_.SolveProblem(solver_type_);
  } catch (const std::runtime_error& e) {
    ROS_ERROR_STREAM("Optimization failed, not sending. " << e.what());
  }
}

void
NlpOptimizerNode::UserCommandCallback(const UserCommandMsg& msg)
{
  motion_optimizer_.final_base_.lin = RosConversions::RosToXpp(msg.goal_lin);
  motion_optimizer_.final_base_.ang = RosConversions::RosToXpp(msg.goal_ang);
  solver_type_ = msg.use_solver_snopt ? opt::Snopt : opt::Ipopt;

  Eigen::Vector3d vel_dis(msg.vel_disturbance.x, msg.vel_disturbance.y, msg.vel_disturbance.z);
  motion_optimizer_.inital_base_.lin.v_ += vel_dis;

  PublishOptParameters();

  if (msg.optimize) {
    OptimizeMotion();
    SaveOptimizationAsRosbag ();
//    SaveAsRosbag(BuildTrajectory(), BuildOptParameters());
  }

  if (msg.replay_trajectory || msg.optimize) {
    // PublishTrajectory();
    // play back the rosbag hacky like this, as I can't find appropriate C++ API.
    system(("rosbag play --quiet "+ rosbag_name_).c_str());
  }
}

void
NlpOptimizerNode::PublishOptParameters() const
{
  auto msg = BuildOptParameters();
  opt_parameters_pub_.publish(msg);
}

xpp_msgs::OptParameters
NlpOptimizerNode::BuildOptParameters() const
{
  auto params = motion_optimizer_.GetMotionParameters();

  xpp_msgs::OptParameters params_msg;
  auto max_dev_xyz = params->GetMaximumDeviationFromNominal();
  params_msg.ee_max_dev = RosConversions::XppToRos<geometry_msgs::Vector3>(max_dev_xyz);

  auto nominal_B = params->GetNominalStanceInBase();
  for (auto ee : nominal_B.ToImpl())
    params_msg.nominal_ee_pos.push_back(RosConversions::XppToRos<geometry_msgs::Point>(ee));

  params_msg.goal_lin = RosConversions::XppToRos(motion_optimizer_.final_base_.lin);
  params_msg.goal_ang = RosConversions::XppToRos(motion_optimizer_.final_base_.ang);

  params_msg.base_mass = params->GetMass();

  return params_msg;
}

void
NlpOptimizerNode::SetInitialState (const RobotStateCartesian& initial_state)
{
  motion_optimizer_.initial_ee_W_       = initial_state.GetEEPos();

  motion_optimizer_.inital_base_     = State3dEuler(); // zero
  motion_optimizer_.inital_base_.lin = initial_state.GetBase().lin;

  kindr::RotationQuaternionD quat(initial_state.GetBase().ang.q);
  kindr::EulerAnglesZyxD euler(quat);
  euler.setUnique(); // to express euler angles close to 0,0,0, not 180,180,180 (although same orientation)
  motion_optimizer_.inital_base_.ang.p_ = euler.toImplementation().reverse();
  // assume zero euler rates and euler accelerations
}

void
NlpOptimizerNode::SaveOptimizationAsRosbag () const
{
  rosbag::Bag bag;
  bag.open(rosbag_name_, rosbag::bagmode::Write);

  // save the a-priori fixed optimization variables
  bag.write(xpp_msgs::opt_parameters, ::ros::Time(0.001), BuildOptParameters());

  // save the trajectory of each iteration
  auto trajectories = motion_optimizer_.GetTrajectories(dt_);
  int n_iterations = trajectories.size();
  for (int i=0; i<n_iterations; ++i)
    SaveTrajectoryInRosbag(bag, trajectories.at(i), xpp_msgs::nlp_iterations_name + std::to_string(i));

  // save number of iterations the optimizer took
  std_msgs::Int32 m;
  m.data = n_iterations;
  bag.write(xpp_msgs::nlp_iterations_count, ::ros::Time(0.001), m);

  // save the final trajectory
  SaveTrajectoryInRosbag(bag, trajectories.back(), xpp_msgs::curr_robot_state);

  bag.close();
}

void
NlpOptimizerNode::SaveTrajectoryInRosbag (rosbag::Bag& bag,
                                          const RobotStateVec& traj,
                                          const std::string& topic) const
{
  for (const auto state : traj) {
    double t_curr = state.GetTime();
    auto timestamp = ::ros::Time(t_curr +1e-6); // to avoid t=0.0

    auto state_msg = ros::RosConversions::XppToRos(state);
    bag.write(topic, timestamp, state_msg);
  }
}

//void
//NlpOptimizerNode::PublishTrajectory () const
//{
//  auto cart_traj_msg = BuildTrajectory();
//  cart_trajectory_pub_.publish(cart_traj_msg);
//}

//NlpOptimizerNode::TrajMsg
//NlpOptimizerNode::BuildTrajectory() const
//{
//  auto opt_traj_cartesian = motion_optimizer_.GetTrajectory(dt_);
//  return RosConversions::XppToRosCart(opt_traj_cartesian);
//}

} /* namespace ros */
} /* namespace xpp */

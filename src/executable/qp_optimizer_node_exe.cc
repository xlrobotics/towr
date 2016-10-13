/**
@file    qp_optimizer_node.cc
@author  Alexander W. Winkler (winklera@ethz.ch)
@date    4.03.2016
@brief   Starts the qp optimizer with fixed footholds
 */

#include <xpp/ros/qp_optimizer_node.h>

int main(int argc, char *argv[])
{
	ros::init(argc, argv, "qp_optimizer_node");
	xpp::ros::QpOptimizerNode qp_optimizer_node;
	ros::spin();

	return 1;
}


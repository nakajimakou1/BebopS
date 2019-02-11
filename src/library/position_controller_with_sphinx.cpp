/*
 * Copyright 2019 Giuseppe Silano, University of Sannio in Benevento, Italy
 * Copytight 2019 Peter Griggs, MIT, USA
 * Copyright 2019 Pasquale Oppido, University of Sannio in Benevento, Italy
 * Copyright 2019 Luigi Iannelli, University of Sannio in Benevento, Italy
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bebopS/position_controller_with_bebop.h"
#include "bebopS/transform_datatypes.h"
#include "bebopS/Matrix3x3.h"
#include "bebopS/Quaternion.h" 
#include "bebopS/stabilizer_types.h"

#include "bebop_msgs/default_topics.h"

#include <math.h> 
#include <time.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <iterator>

#include <ros/ros.h>
#include <chrono>
#include <inttypes.h>

#include <nav_msgs/Odometry.h>
#include <ros/console.h>
#include <std_msgs/Empty.h>


#define M_PI                      3.14159265358979323846  /* pi */
#define TsP                       10e-3  /* Position control sampling time */
#define TsA                       5e-3 /* Attitude control sampling time */
#define TsE                       5 /* Refresh landing time*/

#define MAX_TILT_ANGLE            20  /* Current tilt max in degree */
#define MAX_VERT_SPEED            1  /* Current max vertical speed in m/s */
#define MAX_ROT_SPEED             100 /* Current max rotation speed in degree/s */

#define MAX_POS_X                 1 /* Max position before emergency state along x-axis */
#define MAX_POS_Y                 1 /* Max position before emergency state along y-axis */
#define MAX_POS_Z                 1 /* Max position before emergency state along z-axis */
#define MAX_VEL_ERR               1 /* Max velocity error before emergency state */

namespace bebopS {

PositionControllerWithSphinx::PositionControllerWithSphinx()
    : controller_active_(false),
      dataStoring_active_(false),
      waypointFilter_active_(true),
      EKF_active_(false),
      dataStoringTime_(0),
      stateEmergency_(false),
      e_x_(0),
      e_y_(0),
      e_z_(0),
      e_z_sum_(0),
      vel_command_(0),
      dot_e_x_(0),
      dot_e_y_(0), 
      dot_e_z_(0),
      e_phi_(0),
      e_theta_(0),
      e_psi_(0),
      dot_e_phi_(0),
      dot_e_theta_(0), 
      dot_e_psi_(0),
      u_T_(0),
      bf_(0),
      l_(0),
      bm_(0),
      m_(0),
      g_(0),
      Ix_(0),
      Iy_(0),
      Iz_(0),
      beta_x_(0),
      beta_y_(0),
      beta_z_(0),
      beta_phi_(0),
      beta_theta_(0),
      beta_psi_(0),
      alpha_x_(0),
      alpha_y_(0),
      alpha_z_(0),
      alpha_phi_(0),
      alpha_theta_(0),
      alpha_psi_(0),
      mu_x_(0),
      mu_y_(0),
      mu_z_(0),
      mu_phi_(0),
      mu_theta_(0),
      mu_psi_(0),
      K_x_1_(0),
      K_y_1_(0),
      K_z_1_(0),
      K_x_2_(0),
      K_y_2_(0),
      K_z_2_(0),
      lambda_x_(0),
      lambda_y_(0),
      lambda_z_(0),
      control_({0,0,0,0}), //pitch, roll, yaw rate, thrust
      state_({0,  //Position.x 
              0,  //Position.y
              0,  //Position.z
              0,  //Linear velocity x
              0,  //Linear velocity y
              0,  //Linear velocity z
              0,  //Quaternion x
              0,  //Quaternion y
              0,  //Quaternion z
              0,  //Quaternion w
              0,  //Angular velocity x
              0,  //Angular velocity y
              0}) //Angular velocity z)
              {  

            // Command signals initialization
            command_trajectory_.setFromYaw(0);
            command_trajectory_.position_W[0] = 0;
            command_trajectory_.position_W[1] = 0;
            command_trajectory_.position_W[2] = 0;

            // Kalman filter's parameters initialization
            filter_parameters_.dev_x_ = 0;
            filter_parameters_.dev_y_ = 0;
            filter_parameters_.dev_z_ = 0;
            filter_parameters_.dev_vx_ = 0;
            filter_parameters_.dev_vy_ = 0;
            filter_parameters_.dev_vz_ = 0;
            filter_parameters_.Qp_x_ = 0;
            filter_parameters_.Qp_y_ = 0;
            filter_parameters_.Qp_z_ = 0;
            filter_parameters_.Qp_vx_ = 0;
            filter_parameters_.Qp_vy_ = 0;
            filter_parameters_.Qp_vz_ = 0;
            filter_parameters_.Rp_ = Eigen::MatrixXf::Zero(6,6);
            filter_parameters_.Qp_ = Eigen::MatrixXf::Identity(6,6);

            //For publishing landing and reset commands
            land_pub_ = n4_.advertise<std_msgs::Empty>(bebop_msgs::default_topics::LAND, 1);
            reset_pub_ = n4_.advertise<std_msgs::Empty>(bebop_msgs::default_topics::RESET, 1);

            // Timers set the outer and inner loops working frequency
            timer1_ = n1_.createTimer(ros::Duration(TsA), &PositionControllerWithSphinx::CallbackAttitude, this, false, true);
            timer2_ = n2_.createTimer(ros::Duration(TsP), &PositionControllerWithSphinx::CallbackPosition, this, false, true); 

}

//The library Destructor
PositionControllerWithSphinx::~PositionControllerWithSphinx() {}

//The callback saves data come from simulation into csv files
void PositionControllerWithSphinx::CallbackSaveData(const ros::TimerEvent& event){

      if(!dataStoring_active_){
         return;
      }

      ofstream fileControllerGains;
      ofstream fileVehicleParameters;
      ofstream fileControlSignals;
      ofstream fileReferenceAngles;
      ofstream fileVelocityErrors;
      ofstream fileDroneAttiude;
      ofstream fileTrajectoryErrors;
      ofstream fileAttitudeErrors;
      ofstream fileDerivativeAttitudeErrors;
      ofstream fileTimeAttitudeErrors;
      ofstream fileTimePositionErrors;
      ofstream fileDroneAngularVelocitiesABC;
      ofstream fileDroneTrajectoryReference;
      ofstream fileDroneLinearVelocitiesABC;
      ofstream fileDronePosition;

      ROS_INFO("CallbackSavaData function is working. Time: %f seconds, %f nanoseconds", odometry_.timeStampSec, odometry_.timeStampNsec);
    
      fileControllerGains.open("/home/" + user_ + "/controllerGains.csv", std::ios_base::app);
      fileVehicleParameters.open("/home/" + user_ + "/vehicleParameters.csv", std::ios_base::app);
      fileControlSignals.open("/home/" + user_ + "/controlSignals.csv", std::ios_base::app);
      fileReferenceAngles.open("/home/" + user_ + "/referenceAngles.csv", std::ios_base::app);
      fileVelocityErrors.open("/home/" + user_ + "/velocityErrors.csv", std::ios_base::app);
      fileDroneAttiude.open("/home/" + user_ + "/droneAttitude.csv", std::ios_base::app);
      fileTrajectoryErrors.open("/home/" + user_ + "/trajectoryErrors.csv", std::ios_base::app);
      fileAttitudeErrors.open("/home/" + user_ + "/attitudeErrors.csv", std::ios_base::app);
      fileDerivativeAttitudeErrors.open("/home/" + user_ + "/derivativeAttitudeErrors.csv", std::ios_base::app);
      fileTimeAttitudeErrors.open("/home/" + user_ + "/timeAttitudeErrors.csv", std::ios_base::app);
      fileTimePositionErrors.open("/home/" + user_ + "/timePositionErrors.csv", std::ios_base::app);
      fileDroneAngularVelocitiesABC.open("/home/" + user_ + "/droneAngularVelocitiesABC.csv", std::ios_base::app);
      fileDroneTrajectoryReference.open("/home/" + user_ + "/droneTrajectoryReferences.csv", std::ios_base::app);
      fileDroneLinearVelocitiesABC.open("/home/" + user_ + "/droneLinearVelocitiesABC.csv", std::ios_base::app);
      fileDronePosition.open("/home/" + user_ + "/dronePosition.csv", std::ios_base::app);

      // Saving vehicle parameters in a file
      fileControllerGains << beta_x_ << "," << beta_y_ << "," << beta_z_ << "," << alpha_x_ << "," << alpha_y_ << "," << alpha_z_ << "," << beta_phi_ << ","
    		  << beta_theta_ << "," << beta_psi_ << "," << alpha_phi_ << "," << alpha_theta_ << "," << alpha_psi_ << "," << mu_x_ << "," << mu_y_ << ","
			  << mu_z_ << "," << mu_phi_ << "," << mu_theta_ << "," << mu_psi_ << "," << odometry_.timeStampSec << "," << odometry_.timeStampNsec << "\n";

      // Saving vehicle parameters in a file
      fileVehicleParameters << bf_ << "," << l_ << "," << bm_ << "," << m_ << "," << g_ << "," << Ix_ << "," << Iy_ << "," << Iz_ << ","
    		  << odometry_.timeStampSec << "," << odometry_.timeStampNsec << "\n";

      // Saving control signals in a file
      for (unsigned n=0; n < listControlSignals_.size(); ++n) {
          fileControlSignals << listControlSignals_.at( n );
      }

      // Saving the reference angles in a file
      for (unsigned n=0; n < listReferenceAngles_.size(); ++n) {
          fileReferenceAngles << listReferenceAngles_.at( n );
      }

      // Saving the velocity errors in a file
      for (unsigned n=0; n < listVelocityErrors_.size(); ++n) {
          fileVelocityErrors << listVelocityErrors_.at( n );
      }

      // Saving the drone attitude in a file
      for (unsigned n=0; n < listDroneAttitude_.size(); ++n) {
          fileDroneAttiude << listDroneAttitude_.at( n );
      }
 
      // Saving the trajectory errors in a file
      for (unsigned n=0; n < listTrajectoryErrors_.size(); ++n) {
          fileTrajectoryErrors << listTrajectoryErrors_.at( n );
      }

      // Saving the attitude errors in a file
      for (unsigned n=0; n < listAttitudeErrors_.size(); ++n) {
          fileAttitudeErrors << listAttitudeErrors_.at( n );
      }

      // Saving the derivative attitude errors in a file
      for (unsigned n=0; n < listDerivativeAttitudeErrors_.size(); ++n) {
          fileDerivativeAttitudeErrors << listDerivativeAttitudeErrors_.at( n );
      }

      // Saving the position and attitude errors along the time
      for (unsigned n=0; n < listTimeAttitudeErrors_.size(); ++n) {
          fileTimeAttitudeErrors << listTimeAttitudeErrors_.at( n );
      }

      // Saving
      for (unsigned n=0; n < listTimePositionErrors_.size(); ++n) {
          fileTimePositionErrors << listTimePositionErrors_.at( n );
      }

      // Saving the drone angular velocity in the aircraft body reference system
      for (unsigned n=0; n < listDroneAngularVelocitiesABC_.size(); ++n) {
    	  fileDroneAngularVelocitiesABC << listDroneAngularVelocitiesABC_.at( n );
      }

      // Saving the drone trajectory references (them coming from the waypoint filter)
      for (unsigned n=0; n < listDroneTrajectoryReference_.size(); ++n) {
    	  fileDroneTrajectoryReference << listDroneTrajectoryReference_.at( n );
      }

      // Saving drone linear velocity in the aircraft body center reference system
      for (unsigned n=0; n < listDroneLinearVelocitiesABC_.size(); ++n) {
    	  fileDroneLinearVelocitiesABC << listDroneLinearVelocitiesABC_.at( n );
      }

      // Saving the drone position along axes
      for (unsigned n=0; n < listDronePosition_.size(); ++n) {
        fileDronePosition << listDronePosition_.at( n );
      }

      // Closing all opened files
      fileControllerGains.close ();
      fileVehicleParameters.close ();
      fileControlSignals.close ();
      fileReferenceAngles.close();
      fileVelocityErrors.close();
      fileDroneAttiude.close();
      fileTrajectoryErrors.close();
      fileAttitudeErrors.close();
      fileDerivativeAttitudeErrors.close();
      fileTimeAttitudeErrors.close();
      fileTimePositionErrors.close();
      fileDroneAngularVelocitiesABC.close();
      fileDroneTrajectoryReference.close();
      fileDroneLinearVelocitiesABC.close();
      fileDronePosition.close();

      // To have a one shot storing
      dataStoring_active_ = false;

}

// The function moves the controller gains read from controller_bebop.yaml file to private variables of the class.
// These variables will be employed during the simulation
void PositionControllerWithSphinx::SetControllerGains(){

      beta_x_ = controller_parameters_.beta_xy_.x();
      beta_y_ = controller_parameters_.beta_xy_.y();
      beta_z_ = controller_parameters_.beta_z_;

      beta_phi_ = controller_parameters_.beta_phi_;
      beta_theta_ = controller_parameters_.beta_theta_;
      beta_psi_ = controller_parameters_.beta_psi_;

      alpha_x_ = 1 - beta_x_;
      alpha_y_ = 1 - beta_y_;
      alpha_z_ = 1 - beta_z_;

      alpha_phi_ = 1 - beta_phi_;
      alpha_theta_ = 1 - beta_theta_;
      alpha_psi_ = 1 - beta_psi_;

      mu_x_ = controller_parameters_.mu_xy_.x();
      mu_y_ = controller_parameters_.mu_xy_.y();
      mu_z_ = controller_parameters_.mu_z_;

      mu_phi_ = controller_parameters_.mu_phi_;
      mu_theta_ = controller_parameters_.mu_theta_;
      mu_psi_ = controller_parameters_.mu_psi_;  

      lambda_x_ = controller_parameters_.U_q_.x();
      lambda_y_ = controller_parameters_.U_q_.y();
      lambda_z_ = controller_parameters_.U_q_.z();
	  
      K_x_1_ = 1/mu_x_;
      K_x_2_ = -2 * (beta_x_/mu_x_);
	  
      K_y_1_ = 1/mu_y_;
      K_y_2_ = -2 * (beta_y_/mu_y_);
	  
      K_z_1_ = 1/mu_z_;
      K_z_2_ = -2 * (beta_z_/mu_z_);

}

// As SetControllerGains, the function is used to set the vehicle parameters into private variables of the class
void PositionControllerWithSphinx::SetVehicleParameters(){

      bf_ = vehicle_parameters_.bf_;
      l_ = vehicle_parameters_.armLength_;
      bm_ = vehicle_parameters_.bm_;
      m_ = vehicle_parameters_.mass_;
      g_ = vehicle_parameters_.gravity_;
      Ix_ = vehicle_parameters_.inertia_(0,0);
      Iy_ = vehicle_parameters_.inertia_(1,1);
      Iz_ = vehicle_parameters_.inertia_(2,2);

      // On the EKF object is invoked the method SeVehicleParameters. Such function allows to send the vehicle parameters to the EKF class.
      // Then, they are employed to set the filter matrices
      extended_kalman_filter_bebop_.SetVehicleParameters(m_, g_);

}

// Reading parameters come frame launch file
void PositionControllerWithSphinx::SetLaunchFileParameters(){

	// The boolean variable is used to inactive the logging if it is not useful
	if(dataStoring_active_){

           // Time after which the data storing function is turned on
	   timer3_ = n3_.createTimer(ros::Duration(dataStoringTime_), &PositionController::CallbackSaveData, this, false, true);

	   // Cleaning the string vector contents
	   listControlSignals_.clear();
	   listControlSignals_.clear();
	   listReferenceAngles_.clear();
	   listVelocityErrors_.clear();
	   listDroneAttitude_.clear();
	   listTrajectoryErrors_.clear();
	   listAttitudeErrors_.clear();
	   listDerivativeAttitudeErrors_.clear();
	   listTimeAttitudeErrors_.clear();
	   listTimePositionErrors_.clear();
	   listDroneAngularVelocitiesABC_.clear();
	   listDroneTrajectoryReference_.clear();
	   listDronePosition_.clear();
	  
	}

}

void PositionControllerWithSphinx::SetFilterParameters(){

      // The function is used to move the filter parameters from the YAML file to the filter class
      extended_kalman_filter_bebop_.SetFilterParameters(&filter_parameters_);

}

// The functions is used to convert the drone attitude from quaternion to Euler angles
void PositionControllerWithSphinx::Quaternion2Euler(double* roll, double* pitch, double* yaw) const {
    assert(roll);
    assert(pitch);
    assert(yaw);

    double x, y, z, w;
    x = odometry_.orientation.x();
    y = odometry_.orientation.y();
    z = odometry_.orientation.z();
    w = odometry_.orientation.w();
    
    tf::Quaternion q(x, y, z, w);
    tf::Matrix3x3 m(q);
    m.getRPY(*roll, *pitch, *yaw);
	
}

// When a new odometry message comes, the content of the message is stored in private variable. At the same time, the controller is going to be active.
// The attitude of the aircraft is computer (as we said before it move from quaterninon to Euler angles) and also the angular velocity is stored.
void PositionControllerWithSphinx::SetOdom(const EigenOdometry& odometry) {

   //+x forward, +y left, +z up, +yaw CCW
    odometry_ = odometry; 
    controller_active_= true;

    Quaternion2Euler(&state_.attitude.roll, &state_.attitude.pitch, &state_.attitude.yaw);

    state_.angularVelocity.x = odometry_.angular_velocity[0];
    state_.angularVelocity.y = odometry_.angular_velocity[1];
    state_.angularVelocity.z = odometry_.angular_velocity[2];

}

// The function allows to set the waypoint filter parameters
void PositionControllerWithSphinx::SetTrajectoryPoint() {

    waypoint_filter_.GetTrajectoryPoint(&command_trajectory_);

}

// The function sets the filter trajectory points
void PositionControllerWithSphinx::SetTrajectoryPoint(const mav_msgs::EigenTrajectoryPoint& command_trajectory_positionControllerNode) {

  // If the waypoint has been activated or not
  if(waypointFilter_active_){
    waypoint_filter_.SetTrajectoryPoint(command_trajectory_positionControllerNode);
  }
  else
    command_trajectory_ = command_trajectory_positionControllerNode;


}

// Just to plot the data during the simulation
void PositionControllerWithSphinx::GetTrajectory(nav_msgs::Odometry* smoothed_trajectory){

   smoothed_trajectory->pose.pose.position.x = command_trajectory_.position_W[0];
   smoothed_trajectory->pose.pose.position.y = command_trajectory_.position_W[1];
   smoothed_trajectory->pose.pose.position.z = command_trajectory_.position_W[2];

}

// Just to plot the data during the simulation
void PositionControllerWithSphinx::GetOdometry(nav_msgs::Odometry* odometry_filtered){

   *odometry_filtered = odometry_filtered_private_;

}

// Just to analyze the components that get uTerr variable
void PositionControllerWithSphinx::GetUTerrComponents(nav_msgs::Odometry* uTerrComponents){

  uTerrComponents->pose.pose.position.x = ( (alpha_z_/mu_z_) * dot_e_z_);
  uTerrComponents->pose.pose.position.y = - ( (beta_z_/pow(mu_z_,2)) * e_z_);
  uTerrComponents->pose.pose.position.z = ( g_ + ( (alpha_z_/mu_z_) * dot_e_z_) - ( (beta_z_/pow(mu_z_,2)) * e_z_) );

}

// Just to analyze the position and velocity errors
void PositionControllerWithSphinx::GetPositionAndVelocityErrors(nav_msgs::Odometry* positionAndVelocityErrors){

  positionAndVelocityErrors->pose.pose.position.x = e_x_;
  positionAndVelocityErrors->pose.pose.position.y = e_y_;
  positionAndVelocityErrors->pose.pose.position.z = e_z_;

  positionAndVelocityErrors->twist.twist.linear.x = dot_e_x_;
  positionAndVelocityErrors->twist.twist.linear.y = dot_e_y_;
  positionAndVelocityErrors->twist.twist.linear.z = dot_e_z_;

}

// Just to analyze the attitude and angular velocity errors
void PositionControllerWithSphinx::GetAngularAndAngularVelocityErrors(nav_msgs::Odometry* angularAndAngularVelocityErrors){

  angularAndAngularVelocityErrors->pose.pose.position.x = e_phi_;
  angularAndAngularVelocityErrors->pose.pose.position.y = e_theta_;
  angularAndAngularVelocityErrors->pose.pose.position.z = e_psi_;

  angularAndAngularVelocityErrors->twist.twist.linear.x = dot_e_phi_;
  angularAndAngularVelocityErrors->twist.twist.linear.y = dot_e_theta_;
  angularAndAngularVelocityErrors->twist.twist.linear.z = dot_e_psi_;

}

// Just to plot the data during the simulation
void PositionControllerWithSphinx::GetReferenceAngles(nav_msgs::Odometry* reference_angles){
   assert(reference_angles);

   double u_x, u_y, u_z, u_Terr;
   PosController(&control_.uT, &control_.phiR, &control_.thetaR, &u_x, &u_y, &u_z, &u_Terr);

   reference_angles->pose.pose.position.x = control_.phiR*180/M_PI;
   reference_angles->pose.pose.position.y = control_.thetaR*180/M_PI;
   reference_angles->pose.pose.position.z = control_.uT;

   reference_angles->twist.twist.linear.x = u_x;
   reference_angles->twist.twist.linear.y = u_y;
   reference_angles->twist.twist.linear.z = u_Terr;

}

// Just to plot the components make able to compute dot_e_z
void PositionControllerWithSphinx::GetVelocityAlongZComponents(nav_msgs::Odometry* zVelocity_components){
  assert(zVelocity_components);

  zVelocity_components->pose.pose.position.x = state_.linearVelocity.x;
  zVelocity_components->pose.pose.position.y = state_.linearVelocity.y;
  zVelocity_components->pose.pose.position.z = state_.linearVelocity.z;

  double phi, theta, psi;
  Quaternion2Euler(&phi, &theta, &psi);

  zVelocity_components->twist.twist.linear.x = (-sin(theta) * state_.linearVelocity.x);
  zVelocity_components->twist.twist.linear.y = ( sin(phi) * cos(theta) * state_.linearVelocity.y);
  zVelocity_components->twist.twist.linear.z = (cos(phi) * cos(theta) * state_.linearVelocity.z);

}

// The functions is used to get information about the estimated state when bias and noise affect both the accelerometer and angular rate measurements
void PositionControllerWithSphinx::SetOdometryEstimated() {

    extended_kalman_filter_bebop_.SetThrustCommand(control_.uT);

    //The EKF works or not in according to the value of the EKF_active_ variables
    if(EKF_active_)
    	extended_kalman_filter_bebop_.EstimatorWithNoise(&state_, &odometry_, &odometry_filtered_private_);
    else
    	extended_kalman_filter_bebop_.Estimator(&state_, &odometry_);

}


// The function computes the command signals
void PositionControllerWithSphinx::CalculateCommandSignals(geometry_msgs::Twist* ref_command_signals) {
    assert(ref_command_signals);
    
    //this serves to inactivate the controller if we don't recieve a trajectory
    if(!controller_active_){
        ref_command_signals->linear.x = 0;
        ref_command_signals->linear.y = 0;
        ref_command_signals->linear.z = 0;
        ref_command_signals->angular.z = 0; 
        return;
    }
    
    double u_phi, u_theta;
    AttitudeController(&u_phi, &u_theta, &control_.dotPsi);
    
    //The commands are normalized to take into account the real commands that can be send to the drone
    //Them range is between -1 and 1.
    double theta_ref_degree, phi_ref_degree, yawRate_ref_degree;
    theta_ref_degree = control_.thetaR * (180/M_PI);
    phi_ref_degree = control_.phiR * (180/M_PI);
    yawRate_ref_degree = control_.dotPsi * (180/M_PI);

    double linearX, linearY, linearZ, angularZ;
    linearX = theta_ref_degree/MAX_TILT_ANGLE;
    linearY = phi_ref_degree/MAX_TILT_ANGLE;
    CommandVelocity(&linearZ);
    angularZ = yawRate_ref_degree/MAX_ROT_SPEED;  

    //The command signals are saturated to take into the SDK constrains in sending commands
    if(!(linearZ > -1 && linearZ < 1))
        if(linearZ > 1)
           linearZ = 1;
        else
           linearZ = -1;

    if(!(linearX > -1 && linearX < 1))
        if(linearX > 1)
           linearX = 1;
        else
           linearX = -1;

    if(!(linearY > -1 && linearY < 1))
        if(linearY > 1)
           linearY = 1;
        else
           linearY = -1;

    if(!(angularZ > -1 && angularZ < 1))
        if(angularZ > 1)
           angularZ = 1;
        else
           angularZ = -1;

    ref_command_signals->linear.x = linearX;
    ref_command_signals->linear.y = linearY;
    ref_command_signals->linear.z = linearZ;
    ref_command_signals->angular.z = angularZ; 

}

void PositionControllerWithSphinx::CommandVelocity(double* vel_command){

    e_z_sum_ = e_z_sum_ + e_z_ * TsP;

    *vel_command = (( (alpha_z_/mu_z_) * e_z_) - ( (beta_z_/pow(mu_z_,2)) * e_z_sum_))/MAX_VERT_SPEED;

}


void PositionControllerWithSphinx::LandEmergency(){

    if(stateEmergency_){
       std_msgs::Empty empty_msg;
       land_pub_.publish(empty_msg);
    }
}

void PositionControllerWithSphinx::Emergency(){

    stateEmergency_ = true;
    timer3_ = n3_.createTimer(ros::Duration(TsE), &PositionControllerWithSphinx::CallbackLand, this, false, true); 
    std_msgs::Empty empty_msg;
    reset_pub_.publish(empty_msg);

}

void PositionControllerWithSphinx::ReferenceAngles(double* phi_r, double* theta_r){
   assert(phi_r);
   assert(theta_r);

   double psi_r;
   psi_r = command_trajectory_.getYaw();

   double u_x, u_y, u_Terr;
   PosController(&u_x, &u_y, &u_T_, &u_Terr);

   *theta_r = atan( ( (u_x * cos(psi_r) ) + ( u_y * sin(psi_r) ) )  / u_Terr );
   *phi_r = atan( cos(*theta_r) * ( ( (u_x * sin(psi_r)) - (u_y * cos(psi_r)) ) / (u_Terr) ) );
    
}

void PositionControllerWithSphinx::PosController(double* u_x, double* u_y, double* u_T, double* u_Terr){
   assert(u_x);
   assert(u_y);
   assert(u_T);

   *u_x = m_ * ( (alpha_x_/mu_x_) * dot_e_x_) - ( (beta_x_/pow(mu_x_,2)) * e_x_);
   *u_y = m_ * ( (alpha_y_/mu_y_) * dot_e_y_) -  ( (beta_y_/pow(mu_y_,2)) *  e_y_);
   *u_Terr = m_ * ( g_ + ( (alpha_z_/mu_z_) * dot_e_z_) - ( (beta_z_/pow(mu_z_,2)) * e_z_) );
   *u_T = sqrt( pow(*u_x,2) + pow(*u_y,2) + pow(*u_Terr,2) );
   
}

void PositionControllerWithSphinx::AttitudeController(double* u_phi, double* u_theta, double* u_psi){
   assert(u_phi);
   assert(u_theta);
   assert(u_psi);

   *u_phi = Ix_ * ( ( ( (alpha_phi_/mu_phi_) * dot_e_phi_) - ( (beta_phi_/pow(mu_phi_,2)) * e_phi_) ) - ( ( (Iy_ - Iz_)/(Ix_ * mu_theta_ * mu_psi_) ) * e_theta_ * e_psi_) );
   *u_theta = Iy_ * ( ( ( (alpha_theta_/mu_theta_) * dot_e_theta_) - ( (beta_theta_/pow(mu_theta_,2)) * e_theta_) ) - ( ( (Iz_ - Ix_)/(Iy_ * mu_phi_ * mu_psi_) ) * e_phi_ * e_psi_) );
   *u_psi = Iz_ * ( ( ( (alpha_psi_/mu_psi_) * dot_e_psi_) - ( (beta_psi_/pow(mu_psi_,2)) * e_psi_) ) - ( ( (Ix_ - Iy_)/(Iz_ * mu_theta_ * mu_phi_) ) * e_theta_ * e_phi_) );

}

void PositionControllerWithSphinx::AngularVelocityErrors(double* dot_e_phi, double* dot_e_theta, double* dot_e_psi){
   assert(dot_e_phi);
   assert(dot_e_theta);
   assert(dot_e_psi);

   double psi_r;
   psi_r = command_trajectory_.getYaw();
   
   double dot_phi, dot_theta, dot_psi;

   dot_phi = state_.angularVelocity.x + (sin(state_.attitude.roll)*tan(state_.attitude.pitch)*state_.angularVelocity.y)
                + (cos(state_.attitude.roll)*tan(state_.attitude.pitch)*state_.angularVelocity.z);
    
   dot_theta = (cos(state_.attitude.roll)*state_.angularVelocity.y) - (sin(state_.attitude.roll)*state_.angularVelocity.z);    

   dot_psi = ((sin(state_.attitude.roll)*state_.angularVelocity.y)/cos(state_.attitude.pitch)) +
                 ((cos(state_.attitude.roll)*state_.angularVelocity.z)/cos(state_.attitude.pitch));

   *dot_e_phi =  - dot_phi;
   *dot_e_theta = - dot_theta;
   *dot_e_psi = - dot_psi;

}

void PositionControllerWithSphinx::VelocityErrors(double* dot_e_x, double* dot_e_y, double* dot_e_z){
   assert(dot_e_x);
   assert(dot_e_y);
   assert(dot_e_z);
   
   *dot_e_x = - state_.linearVelocity.x;
   *dot_e_y = - state_.linearVelocity.y;
   *dot_e_z = - state_.linearVelocity.z;
   
}

void PositionControllerWithSphinx::PositionErrors(double* e_x, double* e_y, double* e_z){
   assert(e_x);
   assert(e_y); 
   assert(e_z);
   
   double x_r, y_r, z_r;
   x_r = command_trajectory_.position_W[0];
   y_r = command_trajectory_.position_W[1]; 
   z_r = command_trajectory_.position_W[2];

   *e_x = x_r - state_.position.x;
   *e_y = y_r - state_.position.y;
   *e_z = z_r - state_.position.z;

}

void PositionControllerWithSphinx::AttitudeErrors(double* e_phi, double* e_theta, double* e_psi){
   assert(e_phi);
   assert(e_theta);
   assert(e_psi);
   
   double psi_r;
   psi_r = command_trajectory_.getYaw();
   
   ReferenceAngles(&control_.phiR, &control_.thetaR);

   *e_phi = control_.phiR - state_.attitude.roll;
   *e_theta = control_.thetaR - state_.attitude.pitch;
   *e_psi = psi_r - state_.attitude.yaw;

}

void PositionControllerWithSphinx::CallbackAttitude(const ros::TimerEvent& event){
     
     AttitudeErrors(&e_phi_, &e_theta_, &e_psi_);
     AngularVelocityErrors(&dot_e_phi_, &dot_e_theta_, &dot_e_psi_);
     
}

void PositionControllerWithSphinx::CallbackPosition(const ros::TimerEvent& event){

     waypoint_filter_.TrajectoryGeneration();
     SetTrajectoryPoint();
     SetOdometryEstimated(); 
     PositionErrors(&e_x_, &e_y_, &e_z_);
     VelocityErrors(&dot_e_x_, &dot_e_y_, &dot_e_z_);

     if(abs(state_.position.x) > MAX_POS_X || abs(state_.position.y) > MAX_POS_Y || state_.position.z > MAX_POS_Z || abs(dot_e_z_) > MAX_VEL_ERR || abs(dot_e_y_) > MAX_VEL_ERR || abs(dot_e_x_) > MAX_VEL_ERR)
        Emergency();

 
}

void PositionControllerWithSphinx::CallbackLand(const ros::TimerEvent& event){

     LandEmergency();
   
}


}

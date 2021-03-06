#include "ros/ros.h"
#include <iostream>
#include "std_msgs/String.h"
#include "tf/tf.h"
#include "geometry_msgs/Twist.h"
#include "nav_msgs/Odometry.h"
#include "sensor_msgs/LaserScan.h"
#include "sensor_msgs/PointCloud.h"
#include "geometry_msgs/Pose2D.h"
#include "vector"
#include "algorithm"
#include "bug_algorithms/bugSwitch.h"
#include "utils.h"

static int rateHz = 20;
static bool isSimulation = true;
static bool isPoseReady = false;
static bool isLaserReady = false;
static bool isNodeInit = false;
static float linear_vel_, angular_vel_;

enum NodeStates {Waiting, Initializing, Executing, Pause, Stopping};
static int node_state_ = Waiting;

enum States {FixYaw, GoStraight, Done};
static int state_ = FixYaw;

static float max_laser_range = 4.0;
static float yaw_ = 0;
static geometry_msgs::Point position_ = geometry_msgs::Point();
static geometry_msgs::Point desired_position_ = geometry_msgs::Point();
static float yaw_precision_ = PI/36; // 5 degrees
static float dist_precision_ = 0.3;
static float dist_detection = 0.5;
static float initial_to_goal_distance = 0.0;
static float current_to_goal_distance = 0.0;

static map<string, float> regions_ = {
  {"right",0},
  {"front_right",0},
  {"front",0},
  {"front_left",0},
  {"left",0}
};

static map<int, string> node_state_desc = {
  {Waiting, "Waiting"},
  {Initializing, "Initializing"},
  {Executing, "Executing"},
  {Pause, "Pause"},
  {Stopping, "Stopping"}
};

static map<int, string> state_desc_ = {
  {FixYaw, "fix the yaw"},
  {GoStraight, "go straight ahead"},
  {Done, "done"}
};

geometry_msgs::Twist twist_msg;
ros::Publisher vel_pub;
ros::Subscriber odom_sub;
ros::Subscriber laser_sub;

bool goToPointSwitch(bug_algorithms::bugSwitchRequest& request, bug_algorithms::bugSwitchResponse& response){
  node_state_ = request.state;
  cout << "Go to point node state: " << node_state_ << " | " << node_state_desc[node_state_] << endl;

  return true;
}

void odomCallback(const nav_msgs::Odometry::ConstPtr& msg)
{
  position_ = msg->pose.pose.position;

  tf::Quaternion q(
        msg->pose.pose.orientation.x,
        msg->pose.pose.orientation.y,
        msg->pose.pose.orientation.z,
        msg->pose.pose.orientation.w);

  tf::Matrix3x3 m(q);
  double roll, pitch, yaw;
  m.getRPY(roll, pitch, yaw);

  yaw_ = yaw;

  isPoseReady = true;
}

void laserCallback(const sensor_msgs::LaserScan::ConstPtr& msg){
  // Check the utils.h file for the functions processRay and myfn
  regions_["right"] = *min_element(begin(msg->ranges), begin(msg->ranges)+200, myfn);
  regions_["right"] = processRay(regions_["right"], max_laser_range);
  regions_["front_right"] = *min_element(begin(msg->ranges)+200, begin(msg->ranges)+310, myfn);
  regions_["front_right"] = processRay(regions_["front_right"], max_laser_range);
  regions_["front"] = *min_element(begin(msg->ranges)+310, begin(msg->ranges)+420, myfn);
  regions_["front"] = processRay(regions_["front"], max_laser_range);
  regions_["front_left"] = *min_element(begin(msg->ranges)+420, begin(msg->ranges)+530, myfn);
  regions_["front_left"] = processRay(regions_["front_left"], max_laser_range);
  regions_["left"] = *min_element(begin(msg->ranges)+530, end(msg->ranges), myfn);
  regions_["left"] = processRay(regions_["left"], max_laser_range);

  isLaserReady = true;

  //ROS_INFO("IMP\nRight: %f \nFront_right: %f \nFront: %f \nFront_left: %f \nLeft: %f",
  //         regions_["right"], regions_["front_right"], regions_["front"], regions_["front_left"], regions_["left"]);
}

void waitPose(){
  /*This prevents the robot's pose from being 0*/

  bool wait = true;
  ROS_INFO("Waiting for pose...");

  while(!isPoseReady && node_state_ == Initializing){
    ros::spinOnce();
  }
  ROS_INFO("Robot: %f, %f", position_.x, position_.y);

}

void waitLaser(){
  /*This prevents the robot's pose from being 0*/

  bool wait = true;
  ROS_INFO("Waiting for laser...");

  while(!isLaserReady && node_state_ == Initializing){
    ros::spinOnce();
  }
  ROS_INFO("Laser ok!");
}

void changeState(int state){
  state_ = state;
  string s = state_desc_[state_];
  //ROS_INFO("Go to point - State changed to: %s", s.c_str());
}

// Simulating radially symmetric intensity
float getIntensity(){
  current_to_goal_distance = getDistance(position_, desired_position_);
  // Mapping the current to goal distance in the range [0,1] from initial to goal distance to 0
  return mapRangeFloat(current_to_goal_distance, initial_to_goal_distance, 0, 0, 1);
}

// Simulating align signal from the tower
bool isAlign(){
  float desired_yaw = atan2(desired_position_.y - position_.y, desired_position_.x - position_.x);
  float err_yaw = normalizeAngle(desired_yaw - yaw_);
  return (abs(err_yaw) <= yaw_precision_);
}

void fixYaw(geometry_msgs::Point des_position){

  //ROS_INFO("Yaw error: %f", err_yaw);

  twist_msg = geometry_msgs::Twist();

  if(!isAlign()){
    twist_msg.angular.z = angular_vel_;
    vel_pub.publish(twist_msg);
  } else{
    //ROS_INFO("Yaw error: %f", err_yaw);
    changeState(GoStraight);
  }
}

void goStraightAhead(geometry_msgs::Point des_position){

  float err_pos = getDistance(position_, des_position);

  if(getIntensity() <= 0.98){
    twist_msg = geometry_msgs::Twist();
    if(regions_["front"] < dist_detection+0.2 &&
       regions_["front_right"] < dist_detection+0.2 &&
       regions_["front"] < dist_detection+0.2){
      if(regions_["front"] < dist_detection-0.1)
        twist_msg.linear.x = 0.0;
      else
        twist_msg.linear.x = 0.2;
    } else
      twist_msg.linear.x = linear_vel_;

    ROS_INFO("Vel: %f | Reg: %f", twist_msg.linear.x, regions_["front"]);
    vel_pub.publish(twist_msg);
  } else{
    ROS_INFO("Position: %f | %f", position_.x, position_.y);
    ROS_INFO("Desired: %f | %f", des_position.x, des_position.y);
    ROS_INFO("Position error: %f", err_pos);
    changeState(Done);
  }

  if(!isAlign()){
    //ROS_INFO("Yaw error: %f", err_yaw);
    changeState(FixYaw);
  }
}

void stop(){
  twist_msg = geometry_msgs::Twist();
  twist_msg.linear.x = 0.0;
  twist_msg.angular.z = 0.0;
  vel_pub.publish(twist_msg);
  cout << "Go to point stop!\n";
}

void shutDownSubscribers(){
  odom_sub.shutdown();
  laser_sub.shutdown();
}

void initNode(ros::NodeHandle& nh){
  string vel_topic, odom_topic, laser_topic;
  nh.getParam("/simulation", isSimulation);
  nh.getParam("/desired_x", desired_position_.x);
  nh.getParam("/desired_y", desired_position_.y);
  nh.getParam("/velocity", linear_vel_);

  angular_vel_ = linear_vel_+0.1;
  isPoseReady = false;
  isLaserReady = false;

  if(isSimulation){
    cout << "Go_to_point: Using GazeboSim topics\n";
    vel_topic = "cmd_vel";
    odom_topic = "odom";
    laser_topic = "p3dx/laser/scan";
  } else{
    cout << "Go_to_point: Using RosAria topics\n";
    vel_topic = "RosAria/cmd_vel";
    odom_topic = "RosAria/pose";
    laser_topic = "scan";
  }

  vel_pub = nh.advertise<geometry_msgs::Twist>(vel_topic, rateHz);
  odom_sub = nh.subscribe(odom_topic, rateHz, odomCallback);
  laser_sub = nh.subscribe(laser_topic, rateHz, laserCallback);

  waitPose();
  waitLaser();

  initial_to_goal_distance = getDistance(position_, desired_position_);
  current_to_goal_distance = initial_to_goal_distance;

  // Check if the current state is Initializing for the triggered Stopping case
  if(node_state_ == Initializing){
    // Changing node state to Executing
    node_state_ = Executing;
    // Initialize state
    isNodeInit = true;

    changeState(FixYaw);
  }
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "go_to_point_intensity");
  ros::NodeHandle nh;
  bool pauseBand = true;

  ros::ServiceServer service = nh.advertiseService("goToPointIntensitySwitch", goToPointSwitch);

  ros::Rate rate(rateHz);

  while(!ros::isShuttingDown()){
    if(node_state_ == Waiting){
      ROS_INFO_ONCE("Go to point inactive");

    } else if(node_state_ == Initializing){
      initNode(nh);

    } else if(node_state_ == Executing){
      pauseBand = true;
      if(state_ == FixYaw)
        fixYaw(desired_position_);
      else if(state_ == GoStraight)
        goStraightAhead(desired_position_);
      else if(state_ == Done){
        stop();
        node_state_ = Waiting;
      }
      else
        ROS_INFO("Unknown state!");

    } else if(node_state_ == Pause){
      if(pauseBand){
        if(isNodeInit)
          stop();
        pauseBand = false;
      }
    } else if(node_state_ == Stopping){
      if(isNodeInit){
        stop();
        shutDownSubscribers();
        isNodeInit = false;
      }
      node_state_ = Waiting;
    }

    ros::spinOnce();
    rate.sleep();
  }

  return 0;
}

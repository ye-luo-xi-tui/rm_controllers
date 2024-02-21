/*******************************************************************************
 * BSD 3-Clause License
 *
 * Copyright (c) 2021, Qiayuan Liao
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *******************************************************************************/

//
// Created by qiayuan on 8/14/20.
//

#include "rm_gimbal_controllers/bullet_solver/bullet_solver.h"
#include <cmath>
#include <tf/transform_datatypes.h>
#include <rm_common/ori_tool.h>
#include <angles/angles.h>

namespace rm_gimbal_controllers
{
BulletSolver::BulletSolver(ros::NodeHandle& controller_nh)
{
  config_ = { .resistance_coff_qd_10 = getParam(controller_nh, "resistance_coff_qd_10", 0.),
              .resistance_coff_qd_15 = getParam(controller_nh, "resistance_coff_qd_15", 0.),
              .resistance_coff_qd_16 = getParam(controller_nh, "resistance_coff_qd_16", 0.),
              .resistance_coff_qd_18 = getParam(controller_nh, "resistance_coff_qd_18", 0.),
              .resistance_coff_qd_30 = getParam(controller_nh, "resistance_coff_qd_30", 0.),
              .g = getParam(controller_nh, "g", 0.),
              .delay = getParam(controller_nh, "delay", 0.),
              .dt = getParam(controller_nh, "dt", 0.),
              .timeout = getParam(controller_nh, "timeout", 0.) };
  max_track_target_vel_ = getParam(controller_nh, "max_track_target_vel", 5.0);
  windmill_radius_ = getParam(controller_nh, "windmill_radius", 0.7);
  config_rt_buffer_.initRT(config_);

  marker_desire_.header.frame_id = "odom";
  marker_desire_.ns = "model";
  marker_desire_.action = visualization_msgs::Marker::ADD;
  marker_desire_.type = visualization_msgs::Marker::POINTS;
  marker_desire_.scale.x = 0.02;
  marker_desire_.scale.y = 0.02;
  marker_desire_.color.r = 1.0;
  marker_desire_.color.g = 0.0;
  marker_desire_.color.b = 0.0;
  marker_desire_.color.a = 1.0;

  marker_real_ = marker_desire_;
  marker_real_.color.r = 0.0;
  marker_real_.color.g = 1.0;

  d_srv_ = new dynamic_reconfigure::Server<rm_gimbal_controllers::BulletSolverConfig>(controller_nh);
  dynamic_reconfigure::Server<rm_gimbal_controllers::BulletSolverConfig>::CallbackType cb =
      [this](auto&& PH1, auto&& PH2) { reconfigCB(PH1, PH2); };
  d_srv_->setCallback(cb);

  path_desire_pub_.reset(
      new realtime_tools::RealtimePublisher<visualization_msgs::Marker>(controller_nh, "model_desire", 10));
  path_real_pub_.reset(
      new realtime_tools::RealtimePublisher<visualization_msgs::Marker>(controller_nh, "model_real", 10));
  pub_.reset(new realtime_tools::RealtimePublisher<std_msgs::Float64>(controller_nh, "test", 10));
}

double BulletSolver::getResistanceCoefficient(double bullet_speed) const
{
  // bullet_speed have 5 value:10,15,16,18,30
  double resistance_coff;
  if (bullet_speed < 12.5)
    resistance_coff = config_.resistance_coff_qd_10;
  else if (bullet_speed < 15.5)
    resistance_coff = config_.resistance_coff_qd_15;
  else if (bullet_speed < 17)
    resistance_coff = config_.resistance_coff_qd_16;
  else if (bullet_speed < 24)
    resistance_coff = config_.resistance_coff_qd_18;
  else
    resistance_coff = config_.resistance_coff_qd_30;
  return resistance_coff;
}

void BulletSolver::input(geometry_msgs::Point pos, geometry_msgs::Vector3 vel, double bullet_speed, double yaw,
                         double v_yaw, double r1, double r2, double dz, int armors_num)
{
  config_ = *config_rt_buffer_.readFromRT();
  bullet_speed_ = bullet_speed;
  resistance_coff_ = getResistanceCoefficient(bullet_speed_) != 0 ? getResistanceCoefficient(bullet_speed_) : 0.001;

  double temp_z = pos.z;
  double target_rho = std::sqrt(std::pow(pos.x, 2) + std::pow(pos.y, 2));
  output_yaw_ = std::atan2(pos.y, pos.x);
  output_pitch_ = std::atan2(temp_z, std::sqrt(std::pow(pos.x, 2) + std::pow(pos.y, 2)));
  double rough_fly_time =
      (-std::log(1 - target_rho * resistance_coff_ / (bullet_speed_ * std::cos(output_pitch_)))) / resistance_coff_;
  double r = r1;
  track_target_ = std::abs(v_yaw) < max_track_target_vel_;
  double aim_range_front;
  double switch_armor_angle = track_target_ ?
                                  acos(r / target_rho) - M_PI / 12 +
                                      (-acos(r / target_rho) + M_PI / 6) * std::abs(v_yaw) / max_track_target_vel_ :
                                  M_PI / 12;
  aim_range_front = output_yaw_ + (v_yaw > 0 ? switch_armor_angle - 2 * M_PI / armors_num : -switch_armor_angle);
  double shortest_angular_distance = angles::shortest_angular_distance(yaw + v_yaw * rough_fly_time, aim_range_front);
  if (shortest_angular_distance < 0)
    shortest_angular_distance += 2 * M_PI;
  selected_armor_ =
      static_cast<SelectedArmor>(fmod((shortest_angular_distance / (2 * M_PI / armors_num) + 1), armors_num));
  if (armors_num == 4 && selected_armor_ != SelectedArmor::FRONT && selected_armor_ != SelectedArmor::BACK)
  {
    r = r2;
    pos.z += dz;
  }
  if (track_target_)
    target_kinematics_.reset(new TrackedTargetKinematics(
        pos, vel, yaw + static_cast<int>(selected_armor_) * 2 * M_PI / armors_num, v_yaw, r));
  else
    target_kinematics_.reset(new UntrackedTargetKinematic(
        pos, vel, yaw + static_cast<int>(selected_armor_) * 2 * M_PI / armors_num, v_yaw, r));
}

void BulletSolver::input(double theta, double theta_dot, double bullet_speed,
                         geometry_msgs::TransformStamped odom2windmill, geometry_msgs::TransformStamped odom2pitch)
{
  bullet_speed_ = bullet_speed;
  resistance_coff_ = getResistanceCoefficient(bullet_speed_) != 0 ? getResistanceCoefficient(bullet_speed_) : 0.001;
  track_target_ = true;
  target_kinematics_.reset(new WindmillKinematics(theta, theta_dot, windmill_radius_, odom2windmill, odom2pitch));
}

bool BulletSolver::solve()
{
  config_ = *config_rt_buffer_.readFromRT();
  int count{};
  double error = 999;

  target_pos_ = target_kinematics_->position(0.);

  double temp_z = target_pos_.z;
  while (error >= 0.001)
  {
    output_yaw_ = std::atan2(target_pos_.y, target_pos_.x);
    double target_rho = std::sqrt(std::pow(target_pos_.x, 2) + std::pow(target_pos_.y, 2));
    output_pitch_ = std::atan2(temp_z, target_rho);
    fly_time_ =
        (-std::log(1 - target_rho * resistance_coff_ / (bullet_speed_ * std::cos(output_pitch_)))) / resistance_coff_;
    double real_z = (bullet_speed_ * std::sin(output_pitch_) + (config_.g / resistance_coff_)) *
                        (1 - std::exp(-resistance_coff_ * fly_time_)) / resistance_coff_ -
                    config_.g * fly_time_ / resistance_coff_;

    target_pos_ = target_kinematics_->position(fly_time_);

    double target_yaw = std::atan2(target_pos_.y, target_pos_.x);
    double error_theta = target_yaw - output_yaw_;
    double error_z = target_pos_.z - real_z;
    temp_z += error_z;
    error = std::sqrt(std::pow(error_theta * target_rho, 2) + std::pow(error_z, 2));
    count++;

    if (count >= 20 || std::isnan(error))
      return false;
  }
  return true;
}

void BulletSolver::getYawVelAndAccelDes(double& vel_des, double& accel_des)
{
  geometry_msgs::Vector3 target_vel = target_kinematics_->velocity(fly_time_);
  geometry_msgs::Vector3 target_accel = target_kinematics_->acceleration(fly_time_);
  double yaw_vel_des =
      (target_pos_.x * target_vel.y - target_pos_.y * target_vel.x) / (pow(target_pos_.x, 2) + pow(target_pos_.y, 2));
  double yaw_accel_des =
      (pow(target_pos_.x, 3) * target_accel.y - pow(target_pos_.y, 3) * target_accel.x +
       2 * target_pos_.x * target_pos_.y * pow(target_vel.x, 2) -
       2 * target_pos_.x * target_pos_.y * pow(target_vel.y, 2) -
       pow(target_pos_.x, 2) * target_pos_.y * target_accel.x + target_pos_.x * pow(target_pos_.y, 2) * target_accel.y -
       2 * pow(target_pos_.x, 2) * target_vel.x * target_vel.y +
       2 * pow(target_pos_.y, 2) * target_vel.x * target_vel.y) /
      pow((pow(target_pos_.x, 2) + pow(target_pos_.y, 2)), 2);
  vel_des = yaw_vel_des;
  accel_des = yaw_accel_des;
}

void BulletSolver::getPitchVelAndAccelDes(double& vel_des, double& accel_des)
{
  double dt = 0.01;
  geometry_msgs::Point pos = target_kinematics_->position(fly_time_ + dt);
  double target_rho = std::sqrt(std::pow(pos.x, 2) + std::pow(pos.y, 2));
  double temp_z = target_rho * tan(output_pitch_);
  double output_pitch_next = output_pitch_;
  double error_z = 999;
  while (std::abs(error_z) >= 1e-9)
  {
    output_pitch_next = std::atan2(temp_z, target_rho);
    double fly_time = (-std::log(1 - target_rho * resistance_coff_ / (bullet_speed_ * std::cos(output_pitch_next)))) /
                      resistance_coff_;
    double real_z = (bullet_speed_ * std::sin(output_pitch_next) + (config_.g / resistance_coff_)) *
                        (1 - std::exp(-resistance_coff_ * fly_time)) / resistance_coff_ -
                    config_.g * fly_time / resistance_coff_;
    error_z = pos.z - real_z;
    temp_z += error_z;
  }
  double pitch_vel_des, pitch_accel_des;
  pitch_vel_des = (output_pitch_next - output_pitch_) / dt;
  ros::Time now = ros::Time::now();
  pitch_accel_des = (pitch_vel_des - last_pitch_vel_des_) / (now - last_pitch_vel_des_solve_time_).toSec();
  last_pitch_vel_des_ = pitch_vel_des;
  last_pitch_vel_des_solve_time_ = now;
  vel_des = -pitch_vel_des;
  accel_des = -pitch_accel_des;
  if (pub_->trylock())
  {
    pub_->msg_.data = vel_des;
    pub_->unlockAndPublish();
  }
}

void BulletSolver::bulletModelPub(const geometry_msgs::TransformStamped& odom2pitch, const ros::Time& time)
{
  marker_desire_.points.clear();
  marker_real_.points.clear();
  double roll{}, pitch{}, yaw{};
  quatToRPY(odom2pitch.transform.rotation, roll, pitch, yaw);
  geometry_msgs::Point point_desire{}, point_real{};
  double target_rho = std::sqrt(std::pow(target_pos_.x, 2) + std::pow(target_pos_.y, 2));
  int point_num = int(target_rho * 20);
  for (int i = 0; i <= point_num; i++)
  {
    double rt_bullet_rho = target_rho * i / point_num;
    double fly_time = (-std::log(1 - rt_bullet_rho * resistance_coff_ / (bullet_speed_ * std::cos(output_pitch_)))) /
                      resistance_coff_;
    double rt_bullet_z = (bullet_speed_ * std::sin(output_pitch_) + (config_.g / resistance_coff_)) *
                             (1 - std::exp(-resistance_coff_ * fly_time)) / resistance_coff_ -
                         config_.g * fly_time / resistance_coff_;
    point_desire.x = rt_bullet_rho * std::cos(output_yaw_) + odom2pitch.transform.translation.x;
    point_desire.y = rt_bullet_rho * std::sin(output_yaw_) + odom2pitch.transform.translation.y;
    point_desire.z = rt_bullet_z + odom2pitch.transform.translation.z;
    marker_desire_.points.push_back(point_desire);
  }
  for (int i = 0; i <= point_num; i++)
  {
    double rt_bullet_rho = target_rho * i / point_num;
    double fly_time =
        (-std::log(1 - rt_bullet_rho * resistance_coff_ / (bullet_speed_ * std::cos(-pitch)))) / resistance_coff_;
    double rt_bullet_z = (bullet_speed_ * std::sin(-pitch) + (config_.g / resistance_coff_)) *
                             (1 - std::exp(-resistance_coff_ * fly_time)) / resistance_coff_ -
                         config_.g * fly_time / resistance_coff_;
    point_real.x = rt_bullet_rho * std::cos(yaw) + odom2pitch.transform.translation.x;
    point_real.y = rt_bullet_rho * std::sin(yaw) + odom2pitch.transform.translation.y;
    point_real.z = rt_bullet_z + odom2pitch.transform.translation.z;
    marker_real_.points.push_back(point_real);
  }
  marker_desire_.header.stamp = time;
  if (path_desire_pub_->trylock())
  {
    path_desire_pub_->msg_ = marker_desire_;
    path_desire_pub_->unlockAndPublish();
  }
  marker_real_.header.stamp = time;
  if (path_real_pub_->trylock())
  {
    path_real_pub_->msg_ = marker_real_;
    path_real_pub_->unlockAndPublish();
  }
}

double BulletSolver::getGimbalError(double yaw_real, double pitch_real)
{
  double error;
  if (track_target_)
  {
    double bullet_rho =
        bullet_speed_ * std::cos(pitch_real) * (1 - std::exp(-resistance_coff_ * fly_time_)) / resistance_coff_;
    double bullet_x = bullet_rho * std::cos(yaw_real);
    double bullet_y = bullet_rho * std::sin(yaw_real);
    double bullet_z = (bullet_speed_ * std::sin(pitch_real) + (config_.g / resistance_coff_)) *
                          (1 - std::exp(-resistance_coff_ * fly_time_)) / resistance_coff_ -
                      config_.g * fly_time_ / resistance_coff_;
    error = std::sqrt(std::pow(target_pos_.x - bullet_x, 2) + std::pow(target_pos_.y - bullet_y, 2) +
                      std::pow(target_pos_.z - bullet_z, 2));
  }
  else
  {
    double delay = config_.delay;
    geometry_msgs::Point target_pos_after_fly_time_and_delay{};
    target_pos_after_fly_time_and_delay = target_kinematics_->position(fly_time_ + delay);
    error = std::sqrt(std::pow(target_pos_.x - target_pos_after_fly_time_and_delay.x, 2) +
                      std::pow(target_pos_.y - target_pos_after_fly_time_and_delay.y, 2) +
                      std::pow(target_pos_.z - target_pos_after_fly_time_and_delay.z, 2));
  }
  return error;
}

void BulletSolver::reconfigCB(rm_gimbal_controllers::BulletSolverConfig& config, uint32_t /*unused*/)
{
  ROS_INFO("[Bullet Solver] Dynamic params change");
  if (!dynamic_reconfig_initialized_)
  {
    Config init_config = *config_rt_buffer_.readFromNonRT();  // config init use yaml
    config.resistance_coff_qd_10 = init_config.resistance_coff_qd_10;
    config.resistance_coff_qd_15 = init_config.resistance_coff_qd_15;
    config.resistance_coff_qd_16 = init_config.resistance_coff_qd_16;
    config.resistance_coff_qd_18 = init_config.resistance_coff_qd_18;
    config.resistance_coff_qd_30 = init_config.resistance_coff_qd_30;
    config.g = init_config.g;
    config.delay = init_config.delay;
    config.dt = init_config.dt;
    config.timeout = init_config.timeout;
    dynamic_reconfig_initialized_ = true;
  }
  Config config_non_rt{ .resistance_coff_qd_10 = config.resistance_coff_qd_10,
                        .resistance_coff_qd_15 = config.resistance_coff_qd_15,
                        .resistance_coff_qd_16 = config.resistance_coff_qd_16,
                        .resistance_coff_qd_18 = config.resistance_coff_qd_18,
                        .resistance_coff_qd_30 = config.resistance_coff_qd_30,
                        .g = config.g,
                        .delay = config.delay,
                        .dt = config.dt,
                        .timeout = config.timeout };
  config_rt_buffer_.writeFromNonRT(config_non_rt);
}
}  // namespace rm_gimbal_controllers

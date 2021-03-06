/*
 * BaseAuto.cpp
 *
 *  Created on: Mar 7, 2015
 *      Author: Péter Fankhauser
 *   Institute: ETH Zurich, Autonomous Systems Lab
 */

#include "free_gait_core/base_motion/BaseAuto.hpp"

#include "free_gait_core/leg_motion/EndEffectorMotionBase.hpp"
#include "free_gait_core/pose_optimization/PoseOptimizationGeometric.hpp"

#include <math.h>

namespace free_gait {

BaseAuto::BaseAuto()
    : BaseMotionBase(BaseMotionBase::Type::Auto),
      ignoreTimingOfLegMotion_(false),
      averageLinearVelocity_(0.0),
      averageAngularVelocity_(0.0),
      duration_(0.0),
      supportMargin_(0.0),
      minimumDuration_(0.0),
      isComputed_(false),
      controlSetup_ { {ControlLevel::Position, true}, {ControlLevel::Velocity, true},
                      {ControlLevel::Acceleration, false}, {ControlLevel::Effort, false} }
{
}

BaseAuto::~BaseAuto()
{
}

BaseAuto::BaseAuto(const BaseAuto& other) :
    BaseMotionBase(other),
    ignoreTimingOfLegMotion_(other.ignoreTimingOfLegMotion_),
    averageLinearVelocity_(other.averageLinearVelocity_),
    averageAngularVelocity_(other.averageAngularVelocity_),
    supportMargin_(other.supportMargin_),
    minimumDuration_(other.minimumDuration_),
    start_(other.start_),
    target_(other.target_),
    duration_(other.duration_),
    nominalPlanarStanceInBaseFrame_(other.nominalPlanarStanceInBaseFrame_),
    controlSetup_(other.controlSetup_),
    trajectory_(other.trajectory_),
    footholdsToReach_(other.footholdsToReach_),
    footholdsInSupport_(other.footholdsInSupport_),
    nominalStanceInBaseFrame_(other.nominalStanceInBaseFrame_),
    poseOptimization_(other.poseOptimization_),
    isComputed_(other.isComputed_)
{
  if (other.height_) height_.reset(new double(*(other.height_)));
}

std::unique_ptr<BaseMotionBase> BaseAuto::clone() const
{
  std::unique_ptr<BaseMotionBase> pointer(new BaseAuto(*this));
  return pointer;
}

const ControlSetup BaseAuto::getControlSetup() const
{
  return controlSetup_;
}

void BaseAuto::updateStartPose(const Pose& startPose)
{
  isComputed_ = false;
  start_ = startPose;
}

bool BaseAuto::prepareComputation(const State& state, const Step& step, const StepQueue& queue, const AdapterBase& adapter)
{
  if (!height_) {
    if (!computeHeight(state, queue, adapter)) {
      std::cerr << "BaseAutoStepWiseBasicAlignment::compute: Could not compute height." << std::endl;
      return false;
    }
  }
  if (!generateFootholdLists(state, step, queue, adapter)) {
    std::cerr << "BaseAutoStepWiseBasicAlignment::compute: Could not generate foothold lists." << std::endl;
    return false;
  }
  if (!optimizePose(target_)) {
    std::cerr << "BaseAutoStepWiseBasicAlignment::compute: Could not compute pose optimization." << std::endl;
    return false;
  }
  computeDuration(step, adapter);
  computeTrajectory();
  return isComputed_ = true;
}

bool BaseAuto::needsComputation() const
{
  return false;
}

bool BaseAuto::isComputed() const
{
  return isComputed_;
}

void BaseAuto::reset()
{
  start_.setIdentity();
  target_.setIdentity();
  duration_ = 0.0;
  isComputed_ = false;
}

Pose BaseAuto::evaluatePose(const double time) const
{
  double timeInRange = time <= duration_ ? time : duration_;
  Pose pose;
  trajectory_.evaluate(pose, timeInRange);
  return pose;
}

Twist BaseAuto::evaluateTwist(const double time) const
{
  double timeInRange = time <= duration_ ? time : duration_;
  curves::CubicHermiteSE3Curve::DerivativeType derivative;
  trajectory_.evaluateDerivative(derivative, timeInRange, 1);
  Twist twist(derivative.getTranslationalVelocity().vector(),
              derivative.getRotationalVelocity().vector());
  return twist;
}

double BaseAuto::getDuration() const
{
  return duration_;
}

const std::string& BaseAuto::getFrameId(const ControlLevel& controlLevel) const
{
  return frameId_;
}

void BaseAuto::setHeight(const double height)
{
  height_.reset(new double(height));
}

double BaseAuto::getHeight() const
{
  if (height_) return *height_;
  throw std::runtime_error("Height of BaseAutoStepWiseBasicAlignment has not been set yet.");
}

void BaseAuto::setAverageLinearVelocity(const double averageLinearVelocity)
{
  averageLinearVelocity_ = averageLinearVelocity;
}

double BaseAuto::getAverageLinearVelocity() const
{
  return averageLinearVelocity_;
}

void BaseAuto::setAverageAngularVelocity(const double averageAngularVelocity)
{
  averageAngularVelocity_ = averageAngularVelocity;
}

double BaseAuto::getAverageAngularVelocity() const
{
  return averageAngularVelocity_;
}

double BaseAuto::getSupportMargin() const
{
  return supportMargin_;
}

void BaseAuto::setSupportMargin(double supportMargin)
{
  supportMargin_ = supportMargin;
}

bool BaseAuto::computeHeight(const State& state, const StepQueue& queue, const AdapterBase& adapter)
{
  if (queue.previousStepExists()) {
    if (queue.getPreviousStep().hasBaseMotion()) {
      if (queue.getPreviousStep().getBaseMotion().getType() == BaseMotionBase::Type::Auto) {
        const auto& previousBaseMotion = dynamic_cast<const BaseAuto&>(queue.getPreviousStep().getBaseMotion());
        height_.reset(new double(previousBaseMotion.getHeight()));
        return true;
      }
    }
  }

  unsigned n = 0;
  double heightSum = 0;
  for (const auto& limb : adapter.getLimbs()) {
    if (!state.isIgnoreForPoseAdaptation(limb)) {
      double legHeight = -adapter.getPositionBaseToFootInBaseFrame(limb, state.getJointPositionsForLimb(limb)).z();
      heightSum += legHeight;
      ++n;
    }
  }
  if (n == 0) return false;
  height_.reset(new double(heightSum / (double)(n)));
  return true;
}

bool BaseAuto::generateFootholdLists(const State& state, const Step& step, const StepQueue& queue, const AdapterBase& adapter)
{
  footholdsInSupport_.clear();
  bool prepareForNextStep = false;
  if (!step.hasLegMotion() && queue.size() > 1) {
    if (queue.getNextStep().hasLegMotion()) prepareForNextStep = true;
  }

  if (prepareForNextStep) {
    // Auto motion for preparation of next step.
    for (const auto& limb : adapter.getLimbs()) {
      if (!state.isIgnoreContact(limb) && !queue.getNextStep().hasLegMotion(limb)) {
        footholdsInSupport_[limb] = adapter.getPositionWorldToFootInWorldFrame(limb);
      }
    }
  } else {
    // Auto motion for current step.
    for (const auto& limb : adapter.getLimbs()) {
      if (!step.hasLegMotion(limb) && !state.isIgnoreContact(limb)) {
        footholdsInSupport_[limb] = adapter.getPositionWorldToFootInWorldFrame(limb);
      }
    }
  }

  footholdsToReach_.clear();
  for (const auto& limb : adapter.getLimbs()) {
    if (step.hasLegMotion(limb)) {
      if (step.getLegMotion(limb).isIgnoreForPoseAdaptation()) continue;
      // Double check if right format.
      if (step.getLegMotion(limb).getTrajectoryType() == LegMotionBase::TrajectoryType::EndEffector
          && step.getLegMotion(limb).getControlSetup().at(ControlLevel::Position)) {
        // Use target end effector position.
        const auto& legMotion = dynamic_cast<const EndEffectorMotionBase&>(step.getLegMotion(limb));
        footholdsToReach_[limb] = adapter.transformPosition(legMotion.getFrameId(ControlLevel::Position),
                                                            adapter.getWorldFrameId(),
                                                            legMotion.getTargetPosition());
      }
      else {
        return false;
      }
    } else {
      // Use current foot position.
      if (!state.isIgnoreForPoseAdaptation(limb)) {
        footholdsToReach_[limb] = adapter.getPositionWorldToFootInWorldFrame(limb);
      }
    }
  }

  nominalStanceInBaseFrame_.clear();
  for (const auto& stance : nominalPlanarStanceInBaseFrame_) {
    nominalStanceInBaseFrame_.emplace(stance.first, Position(stance.second(0), stance.second(1), -*height_));
  }

  return true;
}

bool BaseAuto::optimizePose(Pose& pose)
{
  // Define support region.
  grid_map::Polygon supportRegion;
  std::vector<Position> footholdsOrdered;
  getFootholdsCounterClockwiseOrdered(footholdsInSupport_, footholdsOrdered);
  for (auto foothold : footholdsOrdered) {
    supportRegion.addVertex(foothold.vector().head<2>());
  }
  supportRegion.offsetInward(supportMargin_);

  // Create initial pose from geometric alignment.
  pose = PoseOptimizationGeometric::optimize(footholdsToReach_, nominalStanceInBaseFrame_, supportRegion);

  // Optimize pose.
  poseOptimization_.setStance(footholdsToReach_);
  poseOptimization_.setNominalStance(nominalStanceInBaseFrame_);
  poseOptimization_.setSupportRegion(supportRegion);
  return poseOptimization_.optimize(pose);
}

void BaseAuto::computeDuration(const Step& step, const AdapterBase& adapter)
{
  if (!step.hasLegMotion() || ignoreTimingOfLegMotion_) {
    double distance = (target_.getPosition() - start_.getPosition()).norm();
    double translationDuration = distance / averageLinearVelocity_;
    double angle = fabs(target_.getRotation().getDisparityAngle(start_.getRotation()));
    double rotationDuration = angle / averageAngularVelocity_;
    duration_ = translationDuration > rotationDuration ? translationDuration : rotationDuration;
  } else {
    for (const auto& limb : adapter.getLimbs()) {
      if (step.hasLegMotion(limb)) {
        if (!step.getLegMotion(limb).isIgnoreForPoseAdaptation()) {
          if (duration_ < step.getLegMotion(limb).getDuration()) duration_ = step.getLegMotion(limb).getDuration();
        }
      }
    }
  }

  duration_ = duration_ < minimumDuration_ ? minimumDuration_ : duration_;
}

bool BaseAuto::computeTrajectory()
{
  std::vector<Time> times;
  std::vector<ValueType> values;

  times.push_back(0.0);
  values.push_back(start_);

  times.push_back(duration_);
  values.push_back(target_);

  trajectory_.fitCurve(times, values);
  return true;
}

std::ostream& operator<<(std::ostream& out, const BaseAuto& baseAuto)
{
  out << "Frame: " << baseAuto.frameId_ << std::endl;
  out << "Height: " << *(baseAuto.height_) << std::endl;
  out << "Ignore timing of leg motion: " << (baseAuto.ignoreTimingOfLegMotion_ ? "True" : "False") << std::endl;
  out << "Average Linear Velocity: " << baseAuto.averageLinearVelocity_ << std::endl;
  out << "Average Angular Velocity: " << baseAuto.averageAngularVelocity_ << std::endl;
  out << "Support Margin: " << baseAuto.supportMargin_ << std::endl;
  out << "Duration: " << baseAuto.duration_ << std::endl;
  out << "Start Position: " << baseAuto.start_.getPosition() << std::endl;
  out << "Start Orientation: " << baseAuto.start_.getRotation() << std::endl;
  out << "Start Orientation (yaw, pitch, roll) [deg]: " << 180.0 / M_PI * EulerAnglesZyx(baseAuto.start_.getRotation()).getUnique().vector().transpose() << std::endl;
  out << "Target Position: " << baseAuto.target_.getPosition() << std::endl;
  out << "Target Orientation: " << baseAuto.target_.getRotation() << std::endl;
  out << "Target Orientation (yaw, pitch, roll) [deg]: " << 180.0 / M_PI * EulerAnglesZyx(baseAuto.target_.getRotation()).getUnique().vector().transpose() << std::endl;
  out << "Footholds in support: ";
  for (const auto& f : baseAuto.footholdsInSupport_) out << f.first << " ";
  out << std::endl;
  out << "Footholds to reach: ";
  for (const auto& f : baseAuto.footholdsToReach_) out << f.first << " ";
  out << std::endl;
  return out;
}

} /* namespace */

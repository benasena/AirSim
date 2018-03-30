#include "VehiclePawnWrapper.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystemComponent.h"
#include "ConstructorHelpers.h"
#include "AirBlueprintLib.h"
#include "common/ClockFactory.hpp"
#include "common/AirSimSettings.hpp"
#include "NedTransform.h"

VehiclePawnWrapper::VehiclePawnWrapper()
{
    static ConstructorHelpers::FObjectFinder<UParticleSystem> collision_display(TEXT("ParticleSystem'/AirSim/StarterContent/Particles/P_Explosion.P_Explosion'"));
    if (!collision_display.Succeeded())
        collision_display_template = collision_display.Object;
    else
        collision_display_template = nullptr;
}

void VehiclePawnWrapper::setupCamerasFromSettings()
{
    typedef msr::airlib::Settings Settings;
    typedef msr::airlib::ImageCaptureBase::ImageType ImageType;
    typedef msr::airlib::AirSimSettings AirSimSettings;

    int image_count = static_cast<int>(Utils::toNumeric(ImageType::Count));
    for (int image_type = -1; image_type < image_count; ++image_type) {
        for (int camera_index = 0; camera_index < getCameraCount(); ++camera_index) {
            APIPCamera* camera = getCamera(camera_index);
            camera->setImageTypeSettings(image_type, AirSimSettings::singleton().capture_settings[image_type], 
                AirSimSettings::singleton().noise_settings[image_type]);
        }
    }
}


void VehiclePawnWrapper::onCollision(class UPrimitiveComponent* MyComp, class AActor* Other, class UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation, 
    FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
    // Deflect along the surface when we collide.
    //FRotator CurrentRotation = GetActorRotation(RootComponent);
    //SetActorRotation(FQuat::Slerp(CurrentRotation.Quaternion(), HitNormal.ToOrientationQuat(), 0.025f));

    UPrimitiveComponent* comp = Cast<class UPrimitiveComponent>(Other ? (Other->GetRootComponent() ? Other->GetRootComponent() : nullptr) : nullptr);

    state_.collision_info.has_collided = true;
    state_.collision_info.normal = NedTransform::toVector3r(Hit.ImpactNormal, 1, true);
    state_.collision_info.impact_point = NedTransform::toNedMeters(Hit.ImpactPoint);
    state_.collision_info.position = NedTransform::toNedMeters(getPosition());
    state_.collision_info.penetration_depth = NedTransform::toNedMeters(Hit.PenetrationDepth);
    state_.collision_info.time_stamp = msr::airlib::ClockFactory::get()->nowNanos();
    state_.collision_info.object_name = std::string(Other ? TCHAR_TO_UTF8(*(Other->GetName())) : "(null)");
    state_.collision_info.object_id = comp ? comp->CustomDepthStencilValue : -1;

    ++state_.collision_info.collision_count;


    UAirBlueprintLib::LogMessageString("Collision", Utils::stringf("#%d with %s - ObjID %d", 
        state_.collision_info.collision_count, 
        state_.collision_info.object_name.c_str(), state_.collision_info.object_id),
        LogDebugLevel::Failure);
}

APawn* VehiclePawnWrapper::getPawn()
{
    return pawn_;
}

void VehiclePawnWrapper::displayCollisionEffect(FVector hit_location, const FHitResult& hit)
{
    if (collision_display_template != nullptr && Utils::isDefinitelyLessThan(hit.ImpactNormal.Z, 0.0f)) {
        UParticleSystemComponent* particles = UGameplayStatics::SpawnEmitterAtLocation(pawn_->GetWorld(), collision_display_template, hit_location);
        particles->SetWorldScale3D(FVector(0.1f, 0.1f, 0.1f));
    }
}

const msr::airlib::Kinematics::State* VehiclePawnWrapper::getTrueKinematics()
{
    return kinematics_;
}

void VehiclePawnWrapper::setKinematics(const msr::airlib::Kinematics::State* kinematics)
{
    kinematics_ = kinematics;
}

std::string VehiclePawnWrapper::getVehicleConfigName() const 
{
    return getConfig().vehicle_config_name == "" ? msr::airlib::AirSimSettings::singleton().default_vehicle_config
        : getConfig().vehicle_config_name;
}

int VehiclePawnWrapper::getRemoteControlID() const
{
    typedef msr::airlib::AirSimSettings AirSimSettings;

    //find out which RC we should use
    AirSimSettings::VehicleSettings vehicle_settings =
        AirSimSettings::singleton().getVehicleSettings(getVehicleConfigName());

    msr::airlib::Settings settings;
    vehicle_settings.getRawSettings(settings);

    msr::airlib::Settings rc_settings;
    settings.getChild("RC", rc_settings);
    return rc_settings.getInt("RemoteControlID", -1);
}

int VehiclePawnWrapper::getLidarForwardPitch() const
{
    typedef msr::airlib::AirSimSettings AirSimSettings;

    //find out how far to bias lidar forward
    AirSimSettings::VehicleSettings vehicle_settings =
        AirSimSettings::singleton().getVehicleSettings(getVehicleConfigName());

    msr::airlib::Settings settings;
    vehicle_settings.getRawSettings(settings);

    return settings.getInt("LidarForwardPitch", 0);
}

void VehiclePawnWrapper::initialize(APawn* pawn, const std::vector<APIPCamera*>& cameras, const WrapperConfig& config)
{
    pawn_ = pawn;
    cameras_ = cameras;
    config_ = config;

    image_capture_.reset(new UnrealImageCapture(cameras_));

    if (!NedTransform::isInitialized())
        NedTransform::initialize(pawn_);

    //set up key variables
    home_point_ = msr::airlib::GeoPoint(config.home_lattitude, config.home_longitude, config.home_altitude);

    //initialize state
    pawn_->GetActorBounds(true, initial_state_.mesh_origin, initial_state_.mesh_bounds);
    initial_state_.ground_offset = FVector(0, 0, initial_state_.mesh_bounds.Z);
    initial_state_.transformation_offset = pawn_->GetActorLocation() - initial_state_.ground_offset;
    ground_margin_ = FVector(0, 0, 20); //TODO: can we explain pawn_ experimental setting? 7 seems to be minimum
    ground_trace_end_ = initial_state_.ground_offset + ground_margin_; 

    initial_state_.start_location = getPosition();
    initial_state_.last_position = initial_state_.start_location;
    initial_state_.last_debug_position = initial_state_.start_location;
    initial_state_.start_rotation = getOrientation();

    initial_state_.tracing_enabled = config.enable_trace;
    initial_state_.collisions_enabled = config.enable_collisions;
    initial_state_.passthrough_enabled = config.enable_passthrough_on_collisions;

    initial_state_.collision_info = CollisionInfo();

    initial_state_.was_last_move_teleport = false;
    initial_state_.was_last_move_teleport = canTeleportWhileMove();

    setupCamerasFromSettings();
}

VehiclePawnWrapper::WrapperConfig& VehiclePawnWrapper::getConfig()
{
    return config_;
}

const VehiclePawnWrapper::WrapperConfig& VehiclePawnWrapper::getConfig() const
{
    return config_;
}

APIPCamera* VehiclePawnWrapper::getCamera(int index)
{
    if (index < 0 || index >= cameras_.size())
        throw std::out_of_range("Camera id is not valid");
    //should be overridden in derived class
    return cameras_.at(index);
}

UnrealImageCapture* VehiclePawnWrapper::getImageCapture()
{
    return image_capture_.get();
}

int VehiclePawnWrapper::getCameraCount()
{
    return cameras_.size();
}

void VehiclePawnWrapper::reset()
{
    state_ = initial_state_;
    //if (pawn_->GetRootPrimitiveComponent()->IsAnySimulatingPhysics()) {
    //    pawn_->GetRootPrimitiveComponent()->SetSimulatePhysics(false);
    //    pawn_->GetRootPrimitiveComponent()->SetSimulatePhysics(true);
    //}
    pawn_->SetActorLocationAndRotation(state_.start_location, state_.start_rotation, false, nullptr, ETeleportType::TeleportPhysics);

    //TODO: delete below
    //std::ifstream sim_log("C:\\temp\\mavlogs\\circle\\sim_cmd_006_orbit 5 1.txt.pos.txt");
    //plot(sim_log, FColor::Purple, Vector3r(0, 0, -3));
    //std::ifstream real_log("C:\\temp\\mavlogs\\circle\\real_cmd_006_orbit 5 1.txt.pos.txt");
    //plot(real_log, FColor::Yellow, Vector3r(0, 0, -3));

    //std::ifstream sim_log("C:\\temp\\mavlogs\\square\\sim_cmd_005_square 5 1.txt.pos.txt");
    //plot(sim_log, FColor::Purple, Vector3r(0, 0, -3));
    //std::ifstream real_log("C:\\temp\\mavlogs\\square\\real_cmd_012_square 5 1.txt.pos.txt");
    //plot(real_log, FColor::Yellow, Vector3r(0, 0, -3));
}

const VehiclePawnWrapper::GeoPoint& VehiclePawnWrapper::getHomePoint() const
{
    return home_point_;
}

const VehiclePawnWrapper::CollisionInfo& VehiclePawnWrapper::getCollisionInfo() const
{
    return state_.collision_info;
}

const real_T VehiclePawnWrapper::getDistance() const
{
    return distance_;
}

FVector VehiclePawnWrapper::getPosition() const
{
    return pawn_->GetActorLocation(); // - state_.mesh_origin
}

FRotator VehiclePawnWrapper::getOrientation() const
{
    return pawn_->GetActorRotation();
}

void VehiclePawnWrapper::toggleTrace()
{
    state_.tracing_enabled = !state_.tracing_enabled;

    if (!state_.tracing_enabled)
        UKismetSystemLibrary::FlushPersistentDebugLines(pawn_->GetWorld());
    else {     
        state_.debug_position_offset = state_.current_debug_position - state_.current_position;
        state_.last_debug_position = state_.last_position;
    }
}

void VehiclePawnWrapper::allowPassthroughToggleInput()
{
    state_.passthrough_enabled = !state_.passthrough_enabled;
    UAirBlueprintLib::LogMessage("enable_passthrough_on_collisions: ", FString::FromInt(state_.passthrough_enabled), LogDebugLevel::Informational);
}


void VehiclePawnWrapper::plot(std::istream& s, FColor color, const Vector3r& offset)
{
    using namespace msr::airlib;

    Vector3r last_point = VectorMath::nanVector();
    uint64_t timestamp;
    float heading, x, y, z;
    while (s >> timestamp >> heading >> x >> y >> z) {
        std::string discarded_line;
        std::getline(s, discarded_line);

        Vector3r current_point(x, y, z);
        current_point += offset;
        if (!VectorMath::hasNan(last_point)) {
            UKismetSystemLibrary::DrawDebugLine(pawn_->GetWorld(), NedTransform::toNeuUU(last_point), NedTransform::toNeuUU(current_point), color, 0, 3.0F);
        }
        last_point = current_point;
    }

}

void VehiclePawnWrapper::printLogMessage(const std::string& message, const std::string& message_param, unsigned char severity)
{
    UAirBlueprintLib::LogMessageString(message, message_param, static_cast<LogDebugLevel>(severity));
}

//parameters in NED frame
VehiclePawnWrapper::Pose VehiclePawnWrapper::getPose() const
{
    return toPose(getPosition(), pawn_->GetActorRotation().Quaternion());
}

VehiclePawnWrapper::Pose VehiclePawnWrapper::toPose(const FVector& u_position, const FQuat& u_quat)
{
    const Vector3r& position = NedTransform::toNedMeters(u_position);
    const Quaternionr& orientation = NedTransform::toQuaternionr(u_quat, true);
    return Pose(position, orientation);
}

void VehiclePawnWrapper::setPose(const Pose& pose, bool ignore_collision)
{
    //translate to new VehiclePawnWrapper position & orientation from NED to NEU
    FVector position = NedTransform::toNeuUU(pose.position);
    state_.current_position = position;

    //quaternion formula comes from http://stackoverflow.com/a/40334755/207661
    FQuat orientation = NedTransform::toFQuat(pose.orientation, true);

    bool enable_teleport = ignore_collision || canTeleportWhileMove();

    //must reset collision before we set pose. Setting pose will immediately call NotifyHit if there was collision
    //if there was no collision than has_collided would remain false, else it will be set so its value can be
    //checked at the start of next tick
    state_.collision_info.has_collided = false;
    state_.was_last_move_teleport = enable_teleport;

    if (enable_teleport)
        pawn_->SetActorLocationAndRotation(position, orientation, false, nullptr, ETeleportType::TeleportPhysics);
    else
        pawn_->SetActorLocationAndRotation(position, orientation, true);

    if (state_.tracing_enabled && (state_.last_position - position).SizeSquared() > 0.25) {
        UKismetSystemLibrary::DrawDebugLine(pawn_->GetWorld(), state_.last_position, position, FColor::Purple, -1, 3.0f);
        state_.last_position = position;
    }
    else if (!state_.tracing_enabled) {
        state_.last_position = position;
    }

    //update ray tracing
    int range = 4000; //cm
    FVector start = getPosition();
    FVector aim = - pawn_->GetActorUpVector()
	    .RotateAngleAxis(-getLidarForwardPitch(), pawn_->GetActorRightVector());
    FVector end = start + aim * range;
    FHitResult dist_hit = FHitResult(ForceInit);

    bool is_hit = UAirBlueprintLib::GetObstacle(pawn_, start, end, dist_hit);
    distance_ = is_hit? dist_hit.Distance : range;
    FString hit_name = FString("None");

    if (dist_hit.GetActor())
        hit_name=dist_hit.GetActor()->GetName();

    float range_m = distance_*0.01;
    UAirBlueprintLib::LogMessage(FString("Distance to "), hit_name+FString(": ")+FString::SanitizeFloat(range_m), LogDebugLevel::Informational);
}

void VehiclePawnWrapper::setDebugPose(const Pose& debug_pose)
{
    state_.current_debug_position = NedTransform::toNeuUU(debug_pose.position);
    if (state_.tracing_enabled && !VectorMath::hasNan(debug_pose.position)) {
        FVector debug_position = state_.current_debug_position - state_.debug_position_offset;
        if ((state_.last_debug_position - debug_position).SizeSquared() > 0.25) {
            UKismetSystemLibrary::DrawDebugLine(pawn_->GetWorld(), state_.last_debug_position, debug_position, FColor(0xaa, 0x33, 0x11), -1, 10.0F);
            UAirBlueprintLib::LogMessage(FString("Debug Pose: "), debug_position.ToCompactString(), LogDebugLevel::Informational);
            state_.last_debug_position = debug_position;
        }
    }
    else if (!state_.tracing_enabled) {
        state_.last_debug_position = state_.current_debug_position - state_.debug_position_offset;
    }
}

bool VehiclePawnWrapper::canTeleportWhileMove()  const
{
    //allow teleportation
    //  if collisions are not enabled
    //  or we have collided but passthrough is enabled
    //     we will flip-flop was_last_move_teleport flag so on one tick we have passthrough and other tick we don't
    //     without flip flopping, collisions can't be detected
    return !state_.collisions_enabled || (state_.collision_info.has_collided && !state_.was_last_move_teleport && state_.passthrough_enabled);
}

void VehiclePawnWrapper::setLogLine(std::string line)
{
    log_line_ = line;
}

std::string VehiclePawnWrapper::getLogLine()
{
    return log_line_;
}

msr::airlib::Pose VehiclePawnWrapper::getActorPose(std::string actor_name)
{
    AActor* actor = UAirBlueprintLib::FindActor<AActor>(pawn_, FString(actor_name.c_str()));
    return actor ? toPose(actor->GetActorLocation(), actor->GetActorQuat())
        : Pose::nanPose();
}



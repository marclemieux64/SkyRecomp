#include "JoyConTranslator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>

extern "C" bool vr_sword_spatial(void);  // runtime/hle/vr_sword.c: the sword out in 1st person, its swings spatial

namespace skyrecomp::vr {
namespace {

using Vec3 = JoyConTranslator::Vec3;
using Quat = JoyConTranslator::Quat;

constexpr float TwoPi = 6.28318530718f;
constexpr float Gravity = 9.80665f;

// (test, tuning the motion feel) SKYRECOMP_VR_ACCEL_SCALE: linear acceleration sent = the hand's times this (1);
// SKYRECOMP_VR_ACCEL_SMOOTH_MS: low-pass time constant of the differentiated velocity (0: the per-sample
// AccelSmoothing, the earlier behaviour). Read once.
float accel_scale() {
    static float v = -1;
    if (v < 0) { const char *e = std::getenv("SKYRECOMP_VR_ACCEL_SCALE"); v = e ? std::max(0.0f, (float)std::atof(e)) : 1.0f; }
    return v;
}
float accel_smooth_s() {
    static float v = -1;
    if (v < 0) { const char *e = std::getenv("SKYRECOMP_VR_ACCEL_SMOOTH_MS"); v = e ? std::max(0.0f, (float)std::atof(e)) / 1000.0f : 0.0f; }
    return v;
}

Quat conjugate(const Quat &q) noexcept { return {-q[0], -q[1], -q[2], q[3]}; }

Quat multiply(const Quat &a, const Quat &b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Quat normalized(const Quat &q) noexcept {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-6f) || !std::isfinite(n)) return {0, 0, 0, 1};
    return {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
}

float length(const Vec3 &v) noexcept { return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); }

Vec3 rotate(const Quat &q, const Vec3 &v) noexcept {
    // v + 2 w (u x v) + 2 u x (u x v)
    const Vec3 u{q[0], q[1], q[2]};
    const Vec3 t{2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])};
    return {v[0] + q[3] * t[0] + (u[1] * t[2] - u[2] * t[1]),
            v[1] + q[3] * t[1] + (u[2] * t[0] - u[0] * t[2]),
            v[2] + q[3] * t[2] + (u[0] * t[1] - u[1] * t[0])};
}

// OpenXR axes (x right, y up, -z forward) -> the Joy-Con / player axes (x right, y forward, z up).
Vec3 toPlayerAxes(const Vec3 &v) noexcept { return {v[0], -v[2], v[1]}; }

float clampf(float v, float limit) noexcept { return std::isfinite(v) ? std::clamp(v, -limit, limit) : 0.0f; }

constexpr float SpatialBladeLength = 0.8f;  // metres: the blade's turn from the hand's movement (the game's: 80 units)

}  // namespace

Quat JoyConTranslator::yawOnly(const Quat &head) noexcept {
    const Vec3 f = rotate(normalized(head), {0, 0, -1});
    if (std::fabs(f[0]) < 1e-5f && std::fabs(f[2]) < 1e-5f) return {0, 0, 0, 1};  // (looking straight up / down)
    const float yaw = std::atan2(-f[0], -f[2]);
    return {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
}

JoyConTranslator::Mat3 JoyConTranslator::direction(const Quat &controller, const Quat &referenceYaw) noexcept {
    const Quat rel = multiply(conjugate(normalized(referenceYaw)), normalized(controller));
    // The Joy-Con's axes in the controller's own OpenXR axes: x right, y = pointing (-z), z = up (+y).
    const Vec3 axes[3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    Mat3 m{};
    for (int row = 0; row < 3; row++) {
        const Vec3 v = toPlayerAxes(rotate(rel, axes[row]));
        for (int col = 0; col < 3; col++) m[row * 3 + col] = v[col];
    }
    return m;
}

Vec3 JoyConTranslator::toJoyCon(const Vec3 &local, const Quat &controller) noexcept {
    return toPlayerAxes(rotate(conjugate(normalized(controller)), local));
}

float JoyConTranslator::deadzone(float v) noexcept {
    if (!std::isfinite(v)) return 0.0f;
    const float a = std::fabs(v);
    if (a <= StickDeadzone) return 0.0f;
    return std::copysign(std::min(1.0f, (a - StickDeadzone) / (1.0f - StickDeadzone)), v);
}

void JoyConTranslator::reset() noexcept {
    hands_[0] = hands_[1] = Hand{};
    state_ = {};
    zl_ = zr_ = l_ = r_ = false;
    lastTime_ = 0;
}

void JoyConTranslator::updateHand(int index, const SkyVrHand &in, const Quat &referenceYaw, float dt) noexcept {
    Hand &h = hands_[index];
    if (!in.tracked) {
        h.valid = h.haveVelocity = false;
        return;
    }
    const Quat raw = normalized({in.orientation[0], in.orientation[1], in.orientation[2], in.orientation[3]});
    const Vec3 linear{in.linear_velocity[0], in.linear_velocity[1], in.linear_velocity[2]};
    const Vec3 angular{in.angular_velocity[0], in.angular_velocity[1], in.angular_velocity[2]};
    const bool rest = in.velocity_valid && length(linear) < RestLinearSpeed && length(angular) < RestAngularSpeed;
    if (!rest) {
        h.restTime = 0;
    } else if (dt > 0 && dt < 0.1f) {
        h.restTime += dt;
        if (h.restTime >= RestSettleSeconds) {
            // OpenXR orientation is already an absolute, runtime-fused pose. Never offset or recenter it here.
            // Only remove a tiny stationary angular-velocity bias so the game's secondary gyro accumulator cannot
            // drift while the visible/controller direction remains exactly the tracked pose.
            const float biasAlpha = 1.0f - std::exp(-dt / 2.0f);
            for (int i = 0; i < 3; i++) h.gyroBias[i] += biasAlpha * (angular[i] - h.gyroBias[i]);
        }
    }
    const Quat q = raw;
    SkyJoyConSensor &s = h.sensor;
    const auto dir = direction(q, referenceYaw);
    std::copy(dir.begin(), dir.end(), s.direction);
    s.delta_ns = dt > 0 ? static_cast<int64_t>(dt * 1e9f) : 0;

    Vec3 gyro{};
    if (in.velocity_valid) {
        Vec3 w{angular[0] - h.gyroBias[0], angular[1] - h.gyroBias[1], angular[2] - h.gyroBias[2]};
        if (index == 1 && vr_sword_spatial()) {
            // The sword out (runtime/hle/vr_sword.c): the turn its blade makes in space, not the wrist's - the
            // controller's own turn without its twist about the blade (that doesn't move it), plus the turn the hand's
            // movement gives a blade of SpatialBladeLength (v x b / L): a swing of the arm, the wrist kept still, is
            // a slash too, in the way the blade moved; the game's slash then follows where the blade went.
            const Vec3 b = rotate(q, {0, 0, -1});  // (the blade: where the controller points)
            const float wb = w[0] * b[0] + w[1] * b[1] + w[2] * b[2];
            const Vec3 bv{b[1] * linear[2] - b[2] * linear[1], b[2] * linear[0] - b[0] * linear[2], b[0] * linear[1] - b[1] * linear[0]};
            for (int i = 0; i < 3; i++) w[i] = w[i] - wb * b[i] + bv[i] / SpatialBladeLength;
        }
        gyro = toJoyCon(w, q);
        const Vec3 v{in.linear_velocity[0], in.linear_velocity[1], in.linear_velocity[2]};
        if (h.haveVelocity && dt > 0.0005f && dt < 0.1f) {
            const float tau = accel_smooth_s();
            const float k = tau > 0 ? 1.0f - std::exp(-dt / tau) : AccelSmoothing;
            for (int i = 0; i < 3; i++)
                h.accel[i] += k * ((v[i] - h.velocity[i]) / dt - h.accel[i]);
        }
        else
            h.accel = {};
        h.velocity = v;
        h.haveVelocity = true;
    } else {
        h.haveVelocity = false;
        h.accel = {};
    }
    // Reported acceleration: gravity's direction minus the linear acceleration, in g, in the Joy-Con's axes.
    const Vec3 down = toJoyCon({0, -1, 0}, q);
    const float g = accel_scale() / Gravity;
    const Vec3 linearAccel = toJoyCon({h.accel[0] * g, h.accel[1] * g, h.accel[2] * g}, q);
    for (int i = 0; i < 3; i++) {
        s.gyro[i] = clampf(gyro[i] / TwoPi, MaxGyroRps);
        s.accel[i] = clampf(down[i] - linearAccel[i], MaxAccelG);
        h.angle[i] += s.gyro[i] * dt;
        s.angle[i] = h.angle[i];
    }
    h.valid = true;
}

void JoyConTranslator::update(const SkyVrControllers &in) noexcept {
    if (!in.active) {
        reset();
        return;
    }
    const float dt = lastTime_ && in.time_ns > lastTime_ ? static_cast<float>(in.time_ns - lastTime_) * 1e-9f : 0.0f;
    lastTime_ = in.time_ns;
    const Quat yaw = yawOnly({in.reference[0], in.reference[1], in.reference[2], in.reference[3]});
    for (int i = 0; i < 2; i++) updateHand(i, in.hand[i], yaw, dt);

    const auto hysteresis = [](float v, bool &held, float press, float release) {
        held = std::isfinite(v) && (held ? v > release : v >= press);
        return held;
    };
    uint32_t b = in.buttons;
    if (hysteresis(in.zl, zl_, TriggerPress, TriggerRelease)) b |= SKY_JC_ZL;
    if (hysteresis(in.zr, zr_, TriggerPress, TriggerRelease)) b |= SKY_JC_ZR;
    if (hysteresis(in.l, l_, GripPress, GripRelease)) b |= SKY_JC_L;
    if (hysteresis(in.r, r_, GripPress, GripRelease)) b |= SKY_JC_R;
    state_.buttons = b;
    for (int s = 0; s < 2; s++)
        for (int a = 0; a < 2; a++) state_.stick[s][a] = deadzone(in.stick[s][a]);
}

bool JoyConTranslator::sensor(int device, SkyJoyConSensor &out) const noexcept {
    if (device < 0 || device > 1 || !hands_[device].valid) return false;
    out = hands_[device].sensor;
    return true;
}

}  // namespace skyrecomp::vr

// ---- C bridge: one translator, written by the renderer thread (runtime/vr_input.c), read by the game's
// (runtime/hle/motion.c, runtime/hle/input.c).
namespace {
std::mutex lock;
skyrecomp::vr::JoyConTranslator translator;
bool active;
}  // namespace

extern "C" void sky_joycon_update(const SkyVrControllers *sample) {
    if (!sample) return;
    std::lock_guard<std::mutex> guard(lock);
    translator.update(*sample);
    active = sample->active;
}

extern "C" bool sky_joycon_state(SkyJoyConState *out) {
    std::lock_guard<std::mutex> guard(lock);
    if (!out || !active) return false;
    *out = translator.state();
    return true;
}

extern "C" bool sky_joycon_sensor(int device, SkyJoyConSensor *out) {
    std::lock_guard<std::mutex> guard(lock);
    return out && active && translator.sensor(device, *out);
}

extern "C" void sky_joycon_reset(void) {
    std::lock_guard<std::mutex> guard(lock);
    translator.reset();
    active = false;
}

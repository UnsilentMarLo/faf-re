#include "moho/render/lights/LightCollector.h"

#include <algorithm>
#include <cmath>

namespace moho::lights
{
  namespace
  {
    bool Finite3(const float (&v)[3])
    {
      return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    }

    // Which importance a flash of this size has at its peak.
    float FlashPeakPower(const Emission& e)
    {
      const float peak = std::max(e.size0, e.size1);
      return model::Power(model::FlashIntensity(peak), model::FlashRadius(peak));
    }

    // 0..1 envelope: linear fade-in over `inTicks` after birth, full during the life, linear fade-out over `outTicks` after it.
    float Envelope(const float age, const float life, const float inTicks, const float outTicks)
    {
      if (age < 0.0f) {
        return 0.0f;
      }
      float env = 1.0f;
      if (inTicks > 0.0f && age < inTicks) {
        env = age / inTicks;
      }
      if (age > life) {
        if (outTicks <= 0.0f) {
          return 0.0f;
        }
        const float out = 1.0f - (age - life) / outTicks;
        env = std::min(env, out);
      }
      return env < 0.0f ? 0.0f : env;
    }
  } // namespace

  float LightCollector::FadeInTicks() const
  {
    return model::kFadeInSeconds / secondsPerTick_;
  }

  float LightCollector::FadeOutTicks() const
  {
    return model::kFadeOutSeconds / secondsPerTick_;
  }

  void LightCollector::SetQuality(const int quality)
  {
    const int q = quality < model::kOff ? model::kOff : (quality > model::kUltra ? model::kUltra : quality);
    quality_ = q;
    // A lower level no longer admits some of what is stored (the class of an entry never changes, so one check is enough).
    pool_.erase(
      std::remove_if(pool_.begin(), pool_.end(), [q](const Entry& entry) { return !model::Admits(q, entry.peakPower); }),
      pool_.end()
    );
  }

  void LightCollector::SetSecondsPerTick(const float secondsPerTick)
  {
    // Outside this range the clock is paused or garbage; keep the last good value.
    if (secondsPerTick > 0.005f && secondsPerTick < 2.0f) {
      secondsPerTick_ = secondsPerTick;
    }
  }

  void LightCollector::PruneExpired(const float nowTick)
  {
    const float outTicks = FadeOutTicks();
    const std::size_t before = pool_.size();
    pool_.erase(
      std::remove_if(
        pool_.begin(), pool_.end(), [&](const Entry& entry) { return nowTick - entry.e.birth > entry.e.life + outTicks; }
      ),
      pool_.end()
    );
    stats_.expired += static_cast<std::uint32_t>(before - pool_.size());
  }

  bool LightCollector::Add(const Emission& emission)
  {
    ++stats_.offered;
    if (quality_ == model::kOff) {
      return false; // off is free: no validation, no counters beyond `offered`
    }

    const float peak = std::max(emission.size0, emission.size1);
    if (!Finite3(emission.pos) || !Finite3(emission.dir) || !Finite3(emission.accel) || !std::isfinite(emission.birth) ||
        !std::isfinite(emission.life) || !std::isfinite(emission.size0) || !std::isfinite(emission.size1) ||
        !(emission.size0 >= 0.0f) || !(emission.size1 >= 0.0f) || !(peak > 0.0f && peak < model::kMaxSpriteSize) ||
        !(emission.life > 0.0f && emission.life < model::kMaxLifeTicks)) {
      ++stats_.invalid;
      return false;
    }

    Entry entry{};
    entry.e = emission;
    for (float& c : entry.e.rgb) {
      c = model::Clamp(std::isfinite(c) ? c : 0.0f, 0.0f, 1.0f);
    }
    // Only flashes are fed in this stage; the other kinds get their own importance rule with their feeds.
    entry.peakPower = FlashPeakPower(emission);
    if (!model::Admits(quality_, entry.peakPower)) {
      ++stats_.gated;
      return false;
    }

    if (pool_.size() >= kPoolCapacity) {
      PruneExpired(lastNow_);
      if (pool_.size() >= kPoolCapacity) {
        ++stats_.poolFull;
        return false;
      }
    }

    entry.serial = nextSerial_++;
    pool_.push_back(entry);
    ++stats_.stored;
    return true;
  }

  void LightCollector::Update(const float nowTick, const std::size_t maxLights, std::vector<Light>& out)
  {
    out.clear();
    stats_.active = 0;
    stats_.culledByCap = 0;
    if (quality_ == model::kOff) {
      pool_.clear();
      return;
    }
    if (!std::isfinite(nowTick)) {
      return; // a broken clock changes nothing
    }
    lastNow_ = nowTick;
    PruneExpired(nowTick);

    const float inTicks = FadeInTicks();
    const float outTicks = FadeOutTicks();

    struct Ranked
    {
      Light light;
      float birth;
      std::uint32_t serial;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(pool_.size());

    for (const Entry& entry : pool_) {
      const Emission& e = entry.e;
      const float age = nowTick - e.birth;
      const float env = Envelope(age, e.life, inTicks, outTicks);
      if (env <= 0.0f) {
        continue;
      }

      // Size follows the particle: linear from size0 to size1 over the life, held after it.
      const float t01 = model::Clamp(age / e.life, 0.0f, 1.0f);
      const float size = e.size0 + (e.size1 - e.size0) * t01;
      const float radius = model::FlashRadius(size);
      const float intensity = model::FlashIntensity(size) * model::WeakenFactor(radius) * env;

      // Brightness below the visible threshold adds nothing: drop it before it takes a slot.
      const float maxChannel = std::max(e.rgb[0], std::max(e.rgb[1], e.rgb[2]));
      if (maxChannel * intensity < model::kMinVisible) {
        continue;
      }

      // The particle shader's motion rule, t in ticks (never before birth).
      const float t = age < 0.0f ? 0.0f : age;
      Ranked r{};
      r.light.pos[0] = e.pos[0] + e.dir[0] * t + 0.5f * e.accel[0] * t * t;
      r.light.pos[1] = e.pos[1] + e.dir[1] * t + 0.5f * e.accel[1] * t * t;
      r.light.pos[2] = e.pos[2] + e.dir[2] * t + 0.5f * e.accel[2] * t * t;
      r.light.radius = radius;
      r.light.rgb[0] = e.rgb[0] * intensity;
      r.light.rgb[1] = e.rgb[1] * intensity;
      r.light.rgb[2] = e.rgb[2] * intensity;
      r.light.kind = e.kind;
      r.light.power = model::Power(intensity, radius);
      r.birth = e.birth;
      r.serial = entry.serial;
      ranked.push_back(r);
    }

    stats_.active = static_cast<std::uint32_t>(ranked.size());
    const auto stronger = [](const Ranked& a, const Ranked& b) {
      if (a.light.power != b.light.power) {
        return a.light.power > b.light.power;
      }
      if (a.birth != b.birth) {
        return a.birth < b.birth;
      }
      return a.serial < b.serial;
    };
    std::sort(ranked.begin(), ranked.end(), stronger);

    const std::size_t take = std::min(maxLights, ranked.size());
    stats_.culledByCap = static_cast<std::uint32_t>(ranked.size() - take);
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
      out.push_back(ranked[i].light);
    }
  }

  void LightCollector::Clear()
  {
    pool_.clear();
  }
} // namespace moho::lights

#include "application/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "application/delay_estimator.hpp"
#include "application/erle_meter.hpp"
#include "application/timestamped_audio_buffer.hpp"

namespace echonull {
namespace {

std::int64_t milliseconds_to_hns(const double milliseconds) {
  return static_cast<std::int64_t>(std::llround(milliseconds * 10'000.0));
}

double duration_ms(const std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

}  // namespace

Engine::Engine(EngineSettings settings,
               IAudioCaptureSource& microphone,
               IAudioCaptureSource& reference,
               IAudioSink& output,
               IAecProcessor& aec,
               SnapshotHandler snapshot_handler)
    : settings_(std::move(settings)),
      microphone_(microphone),
      reference_(reference),
      output_(output),
      aec_(aec),
      snapshot_handler_(std::move(snapshot_handler)) {
  snapshot_.delay_ms = settings_.initial_delay_ms;
}

void Engine::publish(std::string message) {
  SnapshotHandler handler;
  EngineSnapshot current;
  {
    std::scoped_lock lock(snapshot_mutex_);
    snapshot_.microphone = microphone_.status();
    snapshot_.reference = reference_.status();
    snapshot_.output = output_.status();
    snapshot_.aec = aec_.status();
    if (!message.empty()) snapshot_.message = std::move(message);
    current = snapshot_;
    handler = snapshot_handler_;
  }
  if (handler) handler(current);
}

EngineSnapshot Engine::snapshot() const {
  std::scoped_lock lock(snapshot_mutex_);
  return snapshot_;
}

void Engine::stop_components() noexcept {
  try { microphone_.stop(); } catch (...) {}
  try { reference_.stop(); } catch (...) {}
  try { output_.stop(); } catch (...) {}
  try { aec_.reset(); } catch (...) {}
}

void Engine::run(std::atomic<bool>& stop_requested, const EngineHooks& hooks) {
  TimestampedAudioBuffer microphone_timeline(settings_.timeline_capacity_ms, settings_.sample_rate);
  TimestampedAudioBuffer reference_timeline(settings_.timeline_capacity_ms, settings_.sample_rate);
  DelayEstimator delay_estimator(settings_.sample_rate, settings_.initial_delay_ms,
                                 settings_.max_delay_ms);
  ErleMeter erle;

  try {
    publish("Loading NVIDIA NvAFX AEC");
    aec_.initialize();
    const auto aec_status = aec_.status();
    if (!aec_status.ready) throw std::runtime_error("NvAFX AEC did not become ready");
    if (aec_status.input_frame_samples != aec_status.output_frame_samples) {
      throw std::runtime_error("AEC input and output frame sizes must match for the live pipeline");
    }

    microphone_.start([&microphone_timeline](const CapturePacket& packet) {
      if (packet.discontinuity) microphone_timeline.clear();
      microphone_timeline.push(packet.timestamp_hns, packet.samples);
    });
    reference_.start([&reference_timeline](const CapturePacket& packet) {
      if (packet.discontinuity) reference_timeline.clear();
      reference_timeline.push(packet.timestamp_hns, packet.samples);
    });
    output_.start();

    if (!microphone_.wait_ready(5000)) {
      throw std::runtime_error("microphone failed to start: " + microphone_.status().error);
    }
    if (!reference_.wait_ready(5000)) {
      throw std::runtime_error("render-loopback reference failed to start: " + reference_.status().error);
    }
    if (!output_.wait_ready(5000)) {
      throw std::runtime_error("output endpoint failed to start: " + output_.status().error);
    }

    const std::size_t frame_samples = aec_status.input_frame_samples;
    const auto frame_hns = static_cast<std::int64_t>(std::llround(
        static_cast<double>(frame_samples) * kHundredNanosecondsPerSecond /
        static_cast<double>(settings_.sample_rate)));
    std::vector<float> near_end(frame_samples);
    std::vector<float> far_end(frame_samples);
    std::vector<float> unshifted_far(frame_samples);
    std::vector<float> processed(frame_samples);

    while (!stop_requested && (microphone_timeline.earliest_hns() == 0 ||
                               reference_timeline.earliest_hns() == 0)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (stop_requested) {
      stop_components();
      return;
    }

    double current_delay_ms = settings_.initial_delay_ms;
    std::int64_t cursor_hns = std::max(microphone_timeline.earliest_hns(),
                                      reference_timeline.earliest_hns() +
                                          milliseconds_to_hns(current_delay_ms));
    microphone_timeline.wait_until(cursor_hns + frame_hns, 1000);
    if (hooks.on_started) hooks.on_started();

    {
      std::scoped_lock lock(snapshot_mutex_);
      snapshot_.running = true;
      snapshot_.message = "AEC pipeline is running";
      snapshot_.delay_ms = current_delay_ms;
    }
    publish();

    auto next_publish = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(settings_.diagnostics_interval_ms);
    while (!stop_requested) {
      const auto mic_status = microphone_.status();
      const auto reference_status = reference_.status();
      const auto output_status = output_.status();
      if (mic_status.failed || reference_status.failed || output_status.failed) {
        throw std::runtime_error("an audio endpoint disconnected");
      }

      if (!microphone_timeline.wait_until(cursor_hns + frame_hns, 50)) {
        continue;
      }
      const double near_coverage = microphone_timeline.read(cursor_hns, near_end);
      if (near_coverage < 0.98) {
        std::scoped_lock lock(snapshot_mutex_);
        ++snapshot_.mic_underruns;
        const auto earliest = microphone_timeline.earliest_hns();
        if (earliest > cursor_hns) cursor_hns = earliest;
        continue;
      }

      const double unshifted_coverage = reference_timeline.read(cursor_hns, unshifted_far);
      if (unshifted_coverage >= 0.98 && settings_.auto_delay) {
        delay_estimator.add(near_end, unshifted_far);
        if (delay_estimator.has_estimate()) current_delay_ms = delay_estimator.delay_ms();
      }
      const auto reference_time = cursor_hns - milliseconds_to_hns(current_delay_ms);
      const double far_coverage = reference_timeline.read(reference_time, far_end);
      if (far_coverage < 0.98) {
        std::fill(far_end.begin(), far_end.end(), 0.0F);
        std::scoped_lock lock(snapshot_mutex_);
        ++snapshot_.reference_underruns;
      }

      const auto processing_start = std::chrono::steady_clock::now();
      aec_.process(near_end, far_end, processed);
      const double latency = duration_ms(std::chrono::steady_clock::now() - processing_start);
      const std::size_t dropped = output_.push(processed);
      erle.add(near_end, processed, far_end);

      if (hooks.on_frame) {
        hooks.on_frame(ProcessedFrame{cursor_hns, current_delay_ms, near_end, far_end, processed});
      }

      {
        std::scoped_lock lock(snapshot_mutex_);
        ++snapshot_.processed_frames;
        snapshot_.delay_ms = current_delay_ms;
        snapshot_.delay_confidence = delay_estimator.confidence();
        snapshot_.processing_latency_ms = snapshot_.processed_frames == 1
                                              ? latency
                                              : 0.95 * snapshot_.processing_latency_ms + 0.05 * latency;
        snapshot_.output_overruns += dropped;
        snapshot_.erle_db = erle.erle_db();
      }

      cursor_hns += frame_hns;
      const auto discard_before = cursor_hns - milliseconds_to_hns(settings_.max_delay_ms + 500.0);
      microphone_timeline.discard_before(discard_before);
      reference_timeline.discard_before(discard_before);

      const auto now = std::chrono::steady_clock::now();
      if (now >= next_publish) {
        publish();
        next_publish = now + std::chrono::milliseconds(settings_.diagnostics_interval_ms);
      }
    }

    stop_components();
    {
      std::scoped_lock lock(snapshot_mutex_);
      snapshot_.running = false;
      snapshot_.message = "AEC pipeline stopped";
    }
    publish();
  } catch (const std::exception& error) {
    stop_components();
    {
      std::scoped_lock lock(snapshot_mutex_);
      snapshot_.running = false;
      snapshot_.message = error.what();
    }
    publish();
    throw;
  }
}

}  // namespace echonull


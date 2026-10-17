#include "oot3d_host_audio_stream.h"
#include "oot3d_switch_threads.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace Oot3dNativeGame {
namespace {

int16_t RoundToS16(float value) {
    const long rounded = std::lrintf(value);
    return static_cast<int16_t>(std::clamp<long>(rounded, -32768L, 32767L));
}

// Normalized correlation of a[0..count) and b[0..count) (stride 1, mono).
double Correlation(const float* a, const float* b, size_t count) {
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (size_t index = 0; index < count; ++index) {
        ab += static_cast<double>(a[index]) * b[index];
        aa += static_cast<double>(a[index]) * a[index];
        bb += static_cast<double>(b[index]) * b[index];
    }
    const double energy = aa * bb;
    return energy > 1e-3 ? ab / std::sqrt(energy) : 0.0;
}

size_t FindPeriodLimited(const int16_t* stereo, size_t frames, bool anchorEnd,
                         size_t maximumPeriod) {
    constexpr size_t kMinimum = HostAudioStretcher::MinimumPeriod;
    const size_t maxPeriod = std::min(maximumPeriod, frames / 2U);
    if (maxPeriod < kMinimum) {
        return 0;
    }
    const size_t windowFrames = maxPeriod * 2U;
    const size_t base = anchorEnd ? frames - windowFrames : 0U;
    // Full-rate mono of the window and a 2:1 decimation for the coarse search.
    std::vector<float> mono(windowFrames);
    for (size_t frame = 0; frame < windowFrames; ++frame) {
        const size_t at = (base + frame) * 2U;
        mono[frame] = static_cast<float>(stereo[at]) + static_cast<float>(stereo[at + 1U]);
    }
    const size_t decimatedCount = windowFrames / 2U;
    std::vector<float> decimated(decimatedCount);
    for (size_t index = 0; index < decimatedCount; ++index) {
        decimated[index] = mono[index * 2U] + mono[index * 2U + 1U];
    }
    // The pair of periods sits at the end (insertion) or the start (removal).
    const auto pairCorrelation = [anchorEnd](const float* signal, size_t count,
                                             size_t period) {
        const float* first = anchorEnd ? signal + count - period * 2U : signal;
        return Correlation(first, first + period, period);
    };
    size_t bestCoarse = kMinimum / 2U;
    double bestScore = -2.0;
    for (size_t period = kMinimum / 2U; period <= maxPeriod / 2U; ++period) {
        const double score = pairCorrelation(decimated.data(), decimatedCount, period);
        if (score > bestScore) {
            bestScore = score;
            bestCoarse = period;
        }
    }
    const size_t low = std::max(kMinimum, bestCoarse * 2U >= 2U ? bestCoarse * 2U - 2U : 0U);
    const size_t high = std::min(maxPeriod, bestCoarse * 2U + 2U);
    size_t best = std::clamp(bestCoarse * 2U, kMinimum, maxPeriod);
    bestScore = -2.0;
    for (size_t period = low; period <= high; ++period) {
        const double score = pairCorrelation(mono.data(), windowFrames, period);
        if (score > bestScore) {
            bestScore = score;
            best = period;
        }
    }
    return best;
}

} // namespace

HostAudioStretcher::HostAudioStretcher() : HostAudioStretcher(Settings{}) {}

HostAudioStretcher::HostAudioStretcher(Settings settings) : mSettings(settings) {
    mSettings.RecoverFrames = std::max<size_t>(mSettings.RecoverFrames, 1U);
    mSettings.ConcealFadeFrames = std::max<size_t>(mSettings.ConcealFadeFrames, 1U);
    mSettings.HoldbackFrames = std::min(mSettings.HoldbackFrames, MaximumPeriod * 2U);
}

size_t HostAudioStretcher::FindPeriod(const int16_t* stereo, size_t frames, bool anchorEnd) {
    return FindPeriodLimited(stereo, frames, anchorEnd, MaximumPeriod);
}

void HostAudioStretcher::Reset() {
    mPending.clear();
    mHistory.clear();
    mCredit = 0.0;
    mCreditDirection = 0;
    mGain = 1.0F;
    mGainStep = 0.0F;
    mGainTarget = 1.0F;
    mConcealedRun = 0;
}

float HostAudioStretcher::ConcealGainTarget() const {
    if (mConcealedRun <= mSettings.ConcealFullFrames) {
        return 1.0F;
    }
    const double faded = static_cast<double>(mConcealedRun - mSettings.ConcealFullFrames) /
                         static_cast<double>(mSettings.ConcealFadeFrames);
    return static_cast<float>(std::max(0.0, 1.0 - faded));
}

void HostAudioStretcher::EmitFrame(float left, float right, std::vector<int16_t>& output) {
    if (mConcealedRun != 0) {
        ++mConcealedRun;
        mGain = std::min(mGain, ConcealGainTarget());
    } else if (mGain < mGainTarget) {
        mGain = std::min(mGainTarget, mGain + mGainStep);
    }
    output.push_back(RoundToS16(left * mGain));
    output.push_back(RoundToS16(right * mGain));
}

void HostAudioStretcher::Emit(const int16_t* stereo, size_t frames, std::vector<int16_t>& output) {
    if (frames == 0) {
        return;
    }
    if (mConcealedRun == 0 && mGain >= 1.0F) {
        output.insert(output.end(), stereo, stereo + frames * 2U);
    } else {
        for (size_t frame = 0; frame < frames; ++frame) {
            EmitFrame(stereo[frame * 2U], stereo[frame * 2U + 1U], output);
        }
    }
    mHistory.insert(mHistory.end(), stereo, stereo + frames * 2U);
    if (mHistory.size() > HistoryFrames * 4U) {
        mHistory.erase(mHistory.begin(),
                       mHistory.end() - static_cast<std::ptrdiff_t>(HistoryFrames * 2U));
    }
}

size_t HostAudioStretcher::InsertAtEnd(std::vector<int16_t>& output) {
    const size_t pendingFrames = PendingFrames();
    if (pendingFrames < MinimumPeriod) {
        return 0;
    }
    // Window: the last frames of history followed by the pending ones, ending
    // at the insertion point b (the end of the pending frames).
    const size_t historyFrames = std::min(mHistory.size() / 2U, HistoryFrames);
    mScratch.assign(mHistory.end() - static_cast<std::ptrdiff_t>(historyFrames * 2U), mHistory.end());
    mScratch.insert(mScratch.end(), mPending.begin(), mPending.end());
    const size_t windowFrames = mScratch.size() / 2U;
    const size_t period = FindPeriodLimited(mScratch.data(), windowFrames, true,
                                            std::min(MaximumPeriod, pendingFrames));
    if (period == 0) {
        return 0;
    }
    // Emit up to b - P; then y[k] = S[b-P+k](1-w) + S[b-2P+k]w, which starts
    // where S[b-P-1] left off and ends where S[b-P] continues.
    Emit(mPending.data(), pendingFrames - period, output);
    const int16_t* late = mScratch.data() + (windowFrames - period) * 2U;
    const int16_t* early = mScratch.data() + (windowFrames - period * 2U) * 2U;
    const size_t historyStart = mHistory.size();
    mHistory.resize(historyStart + period * 2U);
    for (size_t frame = 0; frame < period; ++frame) {
        const float weight = (static_cast<float>(frame) + 0.5F) / static_cast<float>(period);
        const float left = late[frame * 2U] * (1.0F - weight) + early[frame * 2U] * weight;
        const float right = late[frame * 2U + 1U] * (1.0F - weight) + early[frame * 2U + 1U] * weight;
        mHistory[historyStart + frame * 2U] = RoundToS16(left);
        mHistory[historyStart + frame * 2U + 1U] = RoundToS16(right);
        EmitFrame(left, right, output);
    }
    mPending.erase(mPending.begin(),
                   mPending.begin() + static_cast<std::ptrdiff_t>((pendingFrames - period) * 2U));
    mInsertedFrames += period;
    return period;
}

size_t HostAudioStretcher::RemoveAtStart(std::vector<int16_t>& output) {
    const size_t pendingFrames = PendingFrames();
    const size_t period = FindPeriodLimited(mPending.data(), pendingFrames, false,
                                            std::min(MaximumPeriod, pendingFrames / 2U));
    if (period == 0) {
        return 0;
    }
    // y[k] = x[k](1-w) + x[P+k]w replaces x[0..2P): it starts at x[0] and
    // ends where x[2P] continues.
    const size_t historyStart = mHistory.size();
    mHistory.resize(historyStart + period * 2U);
    for (size_t frame = 0; frame < period; ++frame) {
        const float weight = (static_cast<float>(frame) + 0.5F) / static_cast<float>(period);
        const int16_t* first = &mPending[frame * 2U];
        const int16_t* second = &mPending[(frame + period) * 2U];
        const float left = first[0] * (1.0F - weight) + second[0] * weight;
        const float right = first[1] * (1.0F - weight) + second[1] * weight;
        mHistory[historyStart + frame * 2U] = RoundToS16(left);
        mHistory[historyStart + frame * 2U + 1U] = RoundToS16(right);
        EmitFrame(left, right, output);
    }
    mPending.erase(mPending.begin(), mPending.begin() + static_cast<std::ptrdiff_t>(period * 4U));
    mRemovedFrames += period;
    return period;
}

void HostAudioStretcher::FlushPending(size_t keepFrames, std::vector<int16_t>& output) {
    const size_t pendingFrames = PendingFrames();
    if (pendingFrames <= keepFrames) {
        return;
    }
    const size_t emit = pendingFrames - keepFrames;
    Emit(mPending.data(), emit, output);
    mPending.erase(mPending.begin(), mPending.begin() + static_cast<std::ptrdiff_t>(emit * 2U));
}

void HostAudioStretcher::Drain(std::vector<int16_t>& output) {
    FlushPending(0, output);
}

void HostAudioStretcher::FadeOutTail(size_t frames, std::vector<int16_t>& output) {
    // Nothing to loop (no pending period): ramp the last emitted frame down
    // instead of stepping to silence, then hold silence.
    float left = 0.0F;
    float right = 0.0F;
    if (mGain > 0.0F && mHistory.size() >= 2U) {
        left = mHistory[mHistory.size() - 2U] * mGain;
        right = mHistory[mHistory.size() - 1U] * mGain;
    }
    constexpr size_t kRamp = 64;
    for (size_t frame = 0; frame < frames; ++frame) {
        const float scale =
            frame < kRamp ? 1.0F - (static_cast<float>(frame) + 1.0F) / kRamp : 0.0F;
        output.push_back(RoundToS16(left * scale));
        output.push_back(RoundToS16(right * scale));
    }
    if (frames != 0) {
        mGain = 0.0F;
        mConcealedRun += frames;
    }
}

void HostAudioStretcher::Process(std::span<const int16_t> input, double ratio,
                                 size_t concealFrames, std::vector<int16_t>& output) {
    const size_t inputFrames = input.size() / 2U;
    if (inputFrames != 0) {
        mPending.insert(mPending.end(), input.begin(), input.begin() + inputFrames * 2U);
        if (mConcealedRun != 0) {
            // Real audio again: continue from the held period, lifting a
            // faded gain back to unity.
            mConcealedRun = 0;
        }
        if (mGain < 1.0F) {
            mGainTarget = 1.0F;
            mGainStep = 1.0F / static_cast<float>(mSettings.RecoverFrames);
        }
        concealFrames = 0;
    }
    const bool slower = ratio > 1.0;
    const bool faster = ratio > 0.0 && ratio < 1.0;
    if (!slower && !faster) {
        mCredit = 0.0;
        mCreditDirection = 0;
    } else {
        // A period that overshoots the credit is repaid by waiting, never by
        // the opposite edit: removal only runs while a faster tempo is asked
        // and insertion only while a slower one is (the former code removed
        // right after inserting and vice versa, editing continuously), and a
        // change of direction starts from zero.
        constexpr double kMaximumCredit = static_cast<double>(MaximumPeriod) * 4.0;
        const int direction = slower ? 1 : -1;
        if (direction != mCreditDirection) {
            mCredit = 0.0;
            mCreditDirection = direction;
        }
        mCredit = std::clamp(mCredit + static_cast<double>(inputFrames) * (ratio - 1.0),
                             -kMaximumCredit, kMaximumCredit);
    }
    // One period always stays pending: a late input can then be concealed by
    // looping it seamlessly (an empty pending buffer left only a fade to
    // silence, since what was already played cannot be crossfaded).
    size_t holdback = mSettings.HoldbackFrames;
    if (slower || concealFrames != 0) {
        holdback = std::max(holdback, MaximumPeriod);
    } else if (faster) {
        holdback = std::max(holdback, MaximumPeriod * 2U);
    }

    for (int operations = 0;
         slower && operations < 4 && mCredit >= static_cast<double>(MinimumPeriod);
         ++operations) {
        const size_t period = InsertAtEnd(output);
        if (period == 0) {
            break;
        }
        mCredit -= static_cast<double>(period);
    }
    for (int operations = 0; faster && operations < 4 &&
                             mCredit <= -static_cast<double>(MinimumPeriod) &&
                             PendingFrames() >= MaximumPeriod * 2U;
         ++operations) {
        const size_t period = RemoveAtStart(output);
        if (period == 0) {
            break;
        }
        mCredit += static_cast<double>(period);
    }

    if (concealFrames != 0) {
        if (mConcealedRun == 0) {
            ++mConcealEvents;
            mConcealedRun = 1;
        }
        size_t produced = 0;
        while (produced < concealFrames) {
            if (mGain <= 0.0F) {
                // Faded out: silence until the input returns (history is
                // left alone so the held period still continues it).
                const size_t frames = concealFrames - produced;
                output.insert(output.end(), frames * 2U, int16_t{0});
                mConcealedRun += frames;
                produced = concealFrames;
                break;
            }
            const size_t before = output.size();
            const size_t period = InsertAtEnd(output);
            if (period == 0) {
                FadeOutTail(concealFrames - produced, output);
                produced = concealFrames;
                break;
            }
            produced += (output.size() - before) / 2U;
            mInsertedFrames -= period;
            mConcealedFrames += period;
        }
    }
    FlushPending(holdback, output);
}

HostAudioStream::HostAudioStream(Options options, Sink sink)
    : mOptions(options), mSink(std::move(sink)) {
    mOptions.TargetFrames = std::max<int32_t>(mOptions.TargetFrames, 1);
    mOptions.DeviceFrames = std::max<int32_t>(mOptions.DeviceFrames, 1);
    if (mOptions.InputRate != mOptions.OutputRate) {
        mResampler.emplace(mOptions.InputRate, mOptions.OutputRate);
    }
    if (mOptions.Thread) {
        mWorker = std::thread([this]() { WorkerMain(); });
    }
}

HostAudioStream::~HostAudioStream() {
    {
        std::lock_guard<std::mutex> lock(mInputMutex);
        mStop = true;
    }
    mWake.notify_all();
    if (mWorker.joinable()) {
        mWorker.join();
    }
}

void HostAudioStream::Submit(std::span<const int16_t> interleavedStereo) {
    if (interleavedStereo.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mInputMutex);
        mInput.insert(mInput.end(), interleavedStereo.begin(), interleavedStereo.end());
    }
    if (mOptions.Thread) {
        mWake.notify_one();
    } else {
        Pump();
    }
}

void HostAudioStream::Flush() {
    std::lock_guard<std::mutex> pump(mPumpMutex);
    {
        std::lock_guard<std::mutex> lock(mInputMutex);
        mInput.clear();
    }
    mStretcher.Reset();
    if (mResampler.has_value()) {
        mResampler->Reset();
    }
    mQueueEstimate = -1.0;
    mPrimed = false;
    mSpeedEstimate = 1.0;
    mSpeedWindowStart = -1.0;
    mSpeedWindowFrames = 0;
    mSlowGame = false;
    {
        std::lock_guard<std::mutex> stats(mStatsMutex);
        mLastQueuedAfter = -1;
    }
    if (mSink.Flush) {
        mSink.Flush();
    }
}

double HostAudioStream::NowSeconds() const {
    if (mOptions.Clock) {
        return mOptions.Clock();
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void HostAudioStream::UpdateSpeed(double now, size_t inputFrames) {
    // Game speed = DSP frames delivered per wall-clock second over the
    // nominal rate, sampled every 0.25 s and smoothed over ~1.5 s, so the
    // refresh bursts of normal play average out.
    if (mSpeedWindowStart < 0.0) {
        mSpeedWindowStart = now;
        mSpeedWindowFrames = 0;
    }
    mSpeedWindowFrames += inputFrames;
    const double elapsed = now - mSpeedWindowStart;
    if (elapsed < 0.25) {
        return;
    }
    const double sample = static_cast<double>(mSpeedWindowFrames) /
                          (elapsed * static_cast<double>(mOptions.InputRate));
    mSpeedEstimate += std::min(1.0, elapsed / 1.5) * (sample - mSpeedEstimate);
    mSpeedWindowStart = now;
    mSpeedWindowFrames = 0;
    if (!mSlowGame && mSpeedEstimate < kSlowGameEnterSpeed) {
        mSlowGame = true;
    } else if (mSlowGame && mSpeedEstimate > kSlowGameLeaveSpeed) {
        mSlowGame = false;
    }
}

int32_t HostAudioStream::CriticalFrames() const {
    // Below this the device is about to run dry: one device buffer (what the
    // SDL thread pulls at once) plus a pump period of margin, or a fifth of
    // the target.
    return std::max(mOptions.DeviceFrames + 256, mOptions.TargetFrames / 5);
}

double HostAudioStream::StretchRatio(double level) const {
    // Normal play never edits the stream: the +/-0.5% rate trim alone holds
    // the queue at its target. Whole periods are inserted only when the queue
    // is close to running dry (below 30% of the target, or 10% above the
    // concealment level, up to 1.5x duration near that level), and
    // removed only near the device queue's drop limit (above 2.6x the target,
    // down to 0.85x), pitch unchanged in both cases.
    const double target = static_cast<double>(mOptions.TargetFrames);
    const double critical = static_cast<double>(CriticalFrames()) / target;
    const double insertBelow = std::max(kInsertBelowLevel, critical + 0.1);
    if (level < insertBelow) {
        return 1.0 + 0.5 * std::min(1.0, (insertBelow - level) / (insertBelow - critical));
    }
    if (level > kRemoveAboveLevel) {
        return 1.0 - 0.15 * std::min(1.0, (level - kRemoveAboveLevel) / 0.6);
    }
    return 1.0;
}

void HostAudioStream::Pump() {
    std::lock_guard<std::mutex> pump(mPumpMutex);
    const auto start = std::chrono::steady_clock::now();
    mWork.clear();
    {
        std::lock_guard<std::mutex> lock(mInputMutex);
        mWork.swap(mInput);
    }
    const int32_t queued = mSink.Buffered ? std::max<int32_t>(mSink.Buffered(), 0) : 0;
    const double target = static_cast<double>(mOptions.TargetFrames);
    bool starved = false;
    if (static_cast<double>(queued) >= target * 0.9) {
        mPrimed = true;
    } else if (queued == 0 && mPrimed) {
        // The device drained its queue (the player re-primes): stay out of
        // the way until it plays again.
        mPrimed = false;
        starved = true;
    }
    const double level = static_cast<double>(queued) / target;
    UpdateSpeed(NowSeconds(), mWork.size() / 2U);
    double ratio = 1.0;
    if (mOptions.Stretch && mPrimed) {
        ratio = StretchRatio(level);
        if (mSlowGame && level < kSlowGameHoldLevel) {
            // A sustained slowdown (heavy scene): match the tempo to the
            // game's measured speed so the queue holds where it is, instead
            // of draining into repeated concealments.
            ratio = std::max(ratio, std::min(1.5, 1.0 / std::max(mSpeedEstimate, 0.01)));
        }
    }
    size_t concealFrames = 0;
    if (mOptions.Stretch && mPrimed && mWork.empty()) {
        const int32_t critical = CriticalFrames();
        if (queued < critical) {
            const double needOutput =
                static_cast<double>(critical + mOptions.DeviceFrames - queued);
            concealFrames = static_cast<size_t>(std::ceil(
                needOutput * mOptions.InputRate / static_cast<double>(mOptions.OutputRate)));
        }
    }
    mStretched.clear();
    mStretcher.Process(mWork, ratio, concealFrames, mStretched);
    if (!mStretched.empty()) {
        const std::vector<int16_t>* output = &mStretched;
        if (mResampler.has_value()) {
            if (!mWork.empty()) {
                // Same queue-holding trim as the inline path: at most 0.5%,
                // a few cents.
                mQueueEstimate = mQueueEstimate < 0.0
                                     ? static_cast<double>(queued)
                                     : mQueueEstimate + 0.05 * (static_cast<double>(queued) -
                                                                mQueueEstimate);
                mResampler->SetRateAdjustment(
                    std::clamp((mQueueEstimate - target) / target * 0.01, -0.005, 0.005));
            }
            mResampled.clear();
            mResampler->Process(mStretched, mResampled);
            output = &mResampled;
        }
        if (!output->empty() && mSink.Play) {
            mSink.Play(output->data(), output->size());
        }
    }
    // What the device really took since the previous pump: the queue left
    // then minus the queue now (frames the player dropped at its cap never
    // counted). Over a window this is the device's actual drain rate.
    const int32_t queuedAfter =
        mStretched.empty() || !mSink.Buffered ? queued : std::max<int32_t>(mSink.Buffered(), 0);
    const auto now = std::chrono::steady_clock::now();
    const double milliseconds =
        std::chrono::duration<double, std::milli>(now - start).count();
    {
        std::lock_guard<std::mutex> stats(mStatsMutex);
        if (mLastQueuedAfter >= 0 && queued <= mLastQueuedAfter) {
            mWindowConsumedFrames += static_cast<uint64_t>(mLastQueuedAfter - queued);
        }
        mLastQueuedAfter = queuedAfter;
        mWindowInputFrames += mWork.size() / 2U;
        mWindowQueuedSum += static_cast<double>(queued);
        ++mWindowQueuedCount;
        if (!mWindowStarted) {
            mWindowStarted = true;
            mWindowStart = NowSeconds();
        }
        mRateAdjustment = mResampler.has_value() ? mResampler->RateAdjustment() : 0.0;
    }
    std::lock_guard<std::mutex> stats(mStatsMutex);
    ++mStats.Pumps;
    mStats.InsertedFrames = mStretcher.InsertedFrames();
    mStats.RemovedFrames = mStretcher.RemovedFrames();
    mStats.ConcealedFrames = mStretcher.ConcealedFrames();
    mStats.ConcealEvents = mStretcher.ConcealEvents();
    mStats.GameSpeed = mSpeedEstimate;
    mStats.DeviceStarvations += starved ? 1U : 0U;
    mStats.PumpMilliseconds += milliseconds;
    mStats.MaximumPumpMilliseconds = std::max(mStats.MaximumPumpMilliseconds, milliseconds);
    if (mPrimed) {
        mWindowMinimumQueued =
            mWindowMinimumQueued < 0 ? queued : std::min(mWindowMinimumQueued, queued);
    }
}

HostAudioStream::Stats HostAudioStream::GetStats() const {
    std::lock_guard<std::mutex> stats(mStatsMutex);
    Stats copy = mStats;
    copy.MinimumQueued = mWindowMinimumQueued;
    return copy;
}

std::string HostAudioStream::PerfSummary() {
    Stats copy;
    {
        std::lock_guard<std::mutex> stats(mStatsMutex);
        copy = mStats;
        copy.MinimumQueued = mWindowMinimumQueued;
        mWindowMinimumQueued = -1;
    }
    const double inputMs = 1000.0 / static_cast<double>(std::max<uint32_t>(mOptions.InputRate, 1U));
    double deviceHz = 0.0;
    double inputHz = 0.0;
    double averageQueued = 0.0;
    double rateAdjustment = 0.0;
    {
        std::lock_guard<std::mutex> stats(mStatsMutex);
        const double now = NowSeconds();
        const double seconds = mWindowStarted ? now - mWindowStart : 0.0;
        if (seconds > 0.0) {
            deviceHz = static_cast<double>(mWindowConsumedFrames) / seconds;
            inputHz = static_cast<double>(mWindowInputFrames) / seconds;
        }
        averageQueued = mWindowQueuedCount != 0
                            ? mWindowQueuedSum / static_cast<double>(mWindowQueuedCount)
                            : 0.0;
        rateAdjustment = mRateAdjustment;
        mWindowStart = now;
        mWindowStarted = true;
        mWindowConsumedFrames = 0;
        mWindowInputFrames = 0;
        mWindowQueuedSum = 0.0;
        mWindowQueuedCount = 0;
    }
    std::ostringstream text;
    text << "audio: thread=" << (mOptions.Thread ? 1 : 0) << " stretch=" << (mOptions.Stretch ? 1 : 0)
         << " min_queued=" << copy.MinimumQueued << " avg_queued=" << static_cast<int64_t>(averageQueued)
         << " device_hz=" << static_cast<int64_t>(deviceHz) << " input_hz=" << static_cast<int64_t>(inputHz)
         << " trim_pct=" << rateAdjustment * 100.0 << " game_speed=" << copy.GameSpeed
         << " inserted_ms=" << copy.InsertedFrames * inputMs
         << " removed_ms=" << copy.RemovedFrames * inputMs
         << " concealed_ms=" << copy.ConcealedFrames * inputMs << " conceal_events=" << copy.ConcealEvents
         << " starved=" << copy.DeviceStarvations << " pumps=" << copy.Pumps
         << " pump_ms_avg="
         << (copy.Pumps != 0 ? copy.PumpMilliseconds / static_cast<double>(copy.Pumps) : 0.0)
         << " pump_ms_max=" << copy.MaximumPumpMilliseconds;
    return text.str();
}

void HostAudioStream::WorkerMain() {
#if defined(__SWITCH__)
    // Above the render worker (0x2D) and the game's other threads (0x2C), like
    // SDL's own audio thread: it only runs for a fraction of a millisecond.
    if (mOptions.SwitchPriority != 0) {
        svcSetThreadPriority(CUR_THREAD_HANDLE, mOptions.SwitchPriority);
    }
    // Core 1 with the PICA plan worker, off the render worker's core 2.
    (void)SetSwitchThreadCore(kSwitchAudioThreadCore);
#endif
    const auto poll = std::chrono::microseconds(std::max<uint32_t>(mOptions.PollMicroseconds, 500U));
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mInputMutex);
            mWake.wait_for(lock, poll, [this]() { return mStop || !mInput.empty(); });
            if (mStop) {
                break;
            }
        }
        Pump();
    }
}

} // namespace Oot3dNativeGame

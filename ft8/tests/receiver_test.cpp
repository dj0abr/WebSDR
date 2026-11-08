#include "SDRQUEUE.h"
#include <complex>
#include "FT8Receiver.h"
#include "SDRInputResampler.h"
#include "FT8WebMessage.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>

namespace {
int checks = 0;
constexpr std::int64_t utc = 1700000010; // Divisible by 15.
constexpr std::uint64_t rate = 480000;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}
SDRStreamInfo band(std::uint32_t start, std::uint32_t end) {
    SDRStreamInfo info;
    info.startHz = start; info.endHz = std::min(end, start + 480000);
    info.centerHz = start + 240000; info.generation = 1;
    return info;
}
void testClock() {
    const auto segment = sdrSegment(14000000,14350000);
    const auto start = utc*1000000000LL;
    const std::int64_t mono = 1000000000000LL;
    SDRCaptureClock clock;
    auto a = clock.observe(0xfffffff0U,32,start+sdrSampleTime(32,2400000),
                           mono+sdrSampleTime(32,2400000),segment);
    auto b = clock.observe(16,32,start+sdrSampleTime(64,2400000),
                           mono+sdrSampleTime(64,2400000),segment);
    require(a.discontinuity && !b.discontinuity && a.generation == b.generation,
            "hardware counter wrap was mistaken for loss");
    require(b.firstSample == 32 && b.utcNanoseconds == start+sdrSampleTime(32,2400000),
            "capture timestamps do not follow sample count");
    auto gap = clock.observe(80,32,start+sdrSampleTime(128,2400000),
                             mono+sdrSampleTime(128,2400000),segment);
    require(!gap.discontinuity && gap.generation == b.generation &&
            gap.firstSample==96 && gap.utcNanoseconds==start+sdrSampleTime(96,2400000) &&
            gap.changeReason==SDRStreamChange::HardwareSampleGap && gap.sampleDelta==32,
            "forward hardware gap did not preserve missing capture time");
    auto afterGap = clock.observe(112,32,start+sdrSampleTime(160,2400000),
                                  mono+sdrSampleTime(160,2400000),segment);
    require(!afterGap.discontinuity && afterGap.firstSample==128 &&
            afterGap.changeReason==SDRStreamChange::Startup && afterGap.sampleDelta==0,
            "forward hardware gap was reported again on continuous samples");
    auto backwardsCounter = clock.observe(100,32,start+sdrSampleTime(192,2400000),
                                          mono+sdrSampleTime(192,2400000),segment);
    require(backwardsCounter.discontinuity && backwardsCounter.generation!=gap.generation &&
            backwardsCounter.changeReason==SDRStreamChange::HardwareSampleGap &&
            backwardsCounter.sampleDelta==-44,"backward sample counter was bridged");
    auto afterBackwards = clock.observe(132,32,start+sdrSampleTime(224,2400000),
                                       mono+sdrSampleTime(224,2400000),segment);
    require(!afterBackwards.discontinuity && afterBackwards.sampleDelta==-44 &&
            afterBackwards.changeReason==SDRStreamChange::HardwareSampleGap,
            "counter reset diagnostics lost after first block");
    auto reset = clock.observe(0,32,start+sdrSampleTime(192,2400000),
                               mono+sdrSampleTime(192,2400000),segment,SDRStreamChange::HardwareReset);
    require(reset.discontinuity && reset.changeReason==SDRStreamChange::HardwareReset,"hardware reset missed");
    auto retune = clock.observe(32,32,start+sdrSampleTime(224,2400000),
                                mono+sdrSampleTime(224,2400000),sdrSegment(7000000,7200000));
    require(retune.discontinuity && retune.centerHz == 7240000 &&
            retune.changeReason==SDRStreamChange::BandChange,"band epoch missed");
    auto rf = clock.observe(64,32,start+sdrSampleTime(256,2400000),
                            mono+sdrSampleTime(256,2400000),sdrSegment(7000000,7200000),
                            SDRStreamChange::FrequencyChange);
    require(rf.discontinuity && rf.changeReason==SDRStreamChange::FrequencyChange,"RF event lost");
    auto fs = clock.observe(96,32,start+sdrSampleTime(288,2400000),
                            mono+sdrSampleTime(288,2400000),sdrSegment(7000000,7200000),
                            SDRStreamChange::SampleRateChange);
    require(fs.discontinuity && fs.changeReason==SDRStreamChange::SampleRateChange,"sample-rate event lost");

    // Continuous hardware samples delivered late and then in fast catch-up
    // bursts must retain their original capture timestamps. The old clock
    // falsely reset the stream whenever delivery latency exceeded 250 ms.
    SDRCaptureClock delayed;
    std::int64_t lastArrival = mono;
    bool continuous = true;
    for (unsigned i=0;i<4000;++i) {
        auto arrival = std::max<std::int64_t>(mono+(i+1)*1000000LL,lastArrival+1000);
        if (i==20) arrival += 450000000LL;
        if (i==1200) arrival += 2000000000LL;
        auto info = delayed.observe(i*2400,2400,start+arrival-mono,arrival,segment);
        continuous &= info.generation==1 && info.discontinuity==(i==0) &&
                      info.firstSample==i*2400 && info.utcNanoseconds==start+i*1000000LL;
        lastArrival = arrival;
    }
    require(continuous,"callback scheduling latency discarded continuous samples");
    // A real UTC step still resets the slot, and must not add driver backlog
    // to the new sample timestamps.
    lastArrival += 2001000000LL;
    auto step = delayed.observe(4000*2400,2400,start+lastArrival-mono+1000000000LL,
                                lastArrival,segment);
    require(step.discontinuity && step.changeReason==SDRStreamChange::ClockStep &&
            step.clockDeltaNs==1000000000LL,"real UTC step missed");
    require(step.utcNanoseconds==start+5000000000LL,"UTC step incorporated callback backlog");
    lastArrival += 1000;
    auto catchUp = delayed.observe(4001*2400,2400,start+lastArrival-mono+1000000000LL,
                                   lastArrival,segment);
    require(!catchUp.discontinuity && catchUp.utcNanoseconds==start+5001000000LL,
            "backlog after UTC step caused another reset");
    lastArrival += 1000;
    auto backwards = delayed.observe(4002*2400,2400,start+lastArrival-mono,lastArrival,segment);
    require(backwards.discontinuity && backwards.clockDeltaNs==-1000000000LL &&
            backwards.utcNanoseconds==start+4002000000LL,"backward UTC step missed");

    SDRCaptureClock preempted;
    preempted.observe(0,2400,start+1000000LL,mono+1000000LL,segment);
    auto unreliable = preempted.observe(2400,2400,start+2000000LL,mono+402000000LL,
                                        segment,SDRStreamChange::None,800000000LL);
    auto reliable = preempted.observe(4800,2400,start+802001000LL,mono+802001000LL,segment);
    require(!unreliable.discontinuity && !reliable.discontinuity &&
            reliable.utcNanoseconds==start+2000000LL,"preemption between clock reads looked like UTC step");

    SDRCaptureClock simultaneous;
    simultaneous.observe(0,2400,start+1000000LL,mono+1000000LL,segment);
    auto both = simultaneous.observe(4800,2400,start+1503000000LL,mono+503000000LL,segment);
    require(both.discontinuity && both.changeReason==SDRStreamChange::ClockStep &&
            both.utcNanoseconds==start+1002000000LL,
            "sample gap hid UTC step or introduced callback backlog");
}

void testInputResampler() {
    constexpr std::uint64_t rawRate = 2400000;
    std::complex<float> zeros[1344]{};
    struct Harness {
        std::vector<SDRStreamInfo> resets;
        std::uint64_t output = 0;
        std::int64_t anchor = 0;
        bool continuous = true;
        SDRInputResampler resampler{[this](const void*,std::size_t count,const SDRStreamInfo& info) {
            if (info.discontinuity) {
                resets.push_back(info); output=0; anchor=info.utcNanoseconds;
            }
            continuous &= info.firstSample==output &&
                info.utcNanoseconds==anchor+sdrSampleTime(output,480000);
            output+=count;
        }};
    };
    auto makeInfo = [&](std::uint64_t sample) {
        auto info=band(14000000,14350000);
        info.firstSample=sample;
        info.utcNanoseconds=utc*1000000000LL+sdrSampleTime(sample,rawRate);
        return info;
    };
    for (const std::int64_t gap : {236,1408,12000,12001,-1}) {
        Harness h;
        h.resampler.process(zeros,1344,makeInfo(0));
        h.resampler.process(zeros,1344,makeInfo(1344+gap));
        h.resampler.process(zeros,1344,makeInfo(2688+gap));
        const bool repair = gap>0 && gap<=12000;
        const auto stats=h.resampler.stats();
        require(h.resets.size()==(repair?1U:2U) && h.continuous,
                "gap repair/reset or output timestamps incorrect");
        require(stats.filledGaps==(repair?1U:0U) &&
                stats.filledSamples==(repair?static_cast<std::uint64_t>(gap):0) &&
                stats.rejectedGaps==(repair?0U:1U),"input gap size limits not enforced");
        if (repair) {
            Harness reference;
            std::uint64_t remaining=4032+gap, sample=0;
            while (remaining) {
                const auto count=std::min<std::uint64_t>(remaining,1344);
                reference.resampler.process(zeros,count,makeInfo(sample));
                sample+=count; remaining-=count;
            }
            require(h.output==reference.output,"zero-filled gap compressed the output sample clock");
        } else require(h.resets.back().changeReason==SDRStreamChange::QueueSampleGap &&
                       h.resets.back().sampleDelta==gap,"rejected gap diagnostics lost");
    }
    Harness budget;
    std::uint64_t n=0;
    budget.resampler.process(zeros,1344,makeInfo(n));
    for (int i=0;i<6;++i) {
        n+=1344+12000;
        budget.resampler.process(zeros,1344,makeInfo(n));
    }
    require(budget.resampler.stats().filledGaps==4 &&
            budget.resampler.stats().filledSamples==48000 &&
            budget.resampler.stats().rejectedGaps==2,
            "20 ms slot budget exceeded or reset by a rejected gap");
    n=15*rawRate;
    budget.resampler.process(zeros,1344,makeInfo(n)); // Large gap: reset, then next slot budget.
    n+=1344+12000;
    budget.resampler.process(zeros,1344,makeInfo(n));
    require(budget.resampler.stats().filledGaps==5,"new UTC slot did not regain its gap budget");

    Harness boundary;
    const auto nearBoundary=(utc+15)*1000000000LL-3060000;
    auto boundaryInfo=[&](std::uint64_t sample) {
        auto info=makeInfo(sample); info.utcNanoseconds=nearBoundary+sdrSampleTime(sample,rawRate);
        return info;
    };
    n=0;
    boundary.resampler.process(zeros,1344,boundaryInfo(n));
    for (const auto gap : {12000U,12000U,12000U,12000U,6000U,1U}) {
        n+=1344+gap;
        boundary.resampler.process(zeros,1344,boundaryInfo(n));
    }
    require(boundary.resampler.stats().filledSamples==54000 &&
            boundary.resampler.stats().filledGaps==5 &&
            boundary.resampler.stats().rejectedGaps==1,
            "gap spanning UTC boundary did not charge both slot budgets");

    for (int event=0;event<4;++event) {
        Harness h;
        h.resampler.process(zeros,1344,makeInfo(0));
        auto info=makeInfo(1344+236);
        if (event==0) {
            info.discontinuity=true; ++info.generation;
            info.changeReason=SDRStreamChange::HardwareReset;
        } else if (event==1) {
            info=band(7000000,7200000); info.firstSample=1344+236;
            info.utcNanoseconds=makeInfo(1344+236).utcNanoseconds;
        } else if (event==2) {
            ++info.generation; info.changeReason=SDRStreamChange::ClockStep;
        } else info.utcNanoseconds+=10000000;
        h.resampler.process(zeros,1344,info);
        require(h.resets.size()==2 && h.resampler.stats().filledSamples==0,
                "hardware reset, retune or time step was zero-filled");
    }
}

void testBroadcastMetadata() {
    SDRQUEUE::reset();
    SDRQUEUE::Sample samples[2]{};
    auto info = band(14000000,14350000);
    info.generation = 7; info.firstSample = 12800;
    info.utcNanoseconds = utc*1000000000LL; info.discontinuity = true;
    info.changeReason=SDRStreamChange::HardwareSampleGap; info.sampleDelta=1344;
    require(SDRQUEUE::pushData(samples,2,info),"cannot publish capture metadata");
    for (std::size_t consumer=0;consumer<3;++consumer) {
        SDRQUEUE::View view;
        require(SDRQUEUE::tryPop(consumer,view) && view.count==2 &&
                view.info.generation==7 && view.info.firstSample==12800 &&
                view.info.utcNanoseconds==info.utcNanoseconds &&
                view.info.discontinuity && view.info.centerHz==14240000 &&
                view.info.changeReason==SDRStreamChange::HardwareSampleGap && view.info.sampleDelta==1344,
                "capture metadata lost in broadcast queue");
        SDRQUEUE::consume(consumer);
    }
    SDRQUEUE::reset();
}

void testWebProtocol() {
    FT8DecodedSlot result;
    result.utcSeconds=utc; result.dialHz=432174000;
    std::string expected;
    for (int i=0;i<1800;++i) {
        const auto line="221330 -12 0.200 1500.000 ~ CQ K"+std::to_string(i)+"ABC FN42";
        result.lines.push_back(line);
        expected+="D\t"+std::to_string(utc)+"\t432174000\t"+line+"\n";
    }
    const auto frames=FT8Web::encode(result);
    std::string received;
    bool valid=true;
    for (const auto& frame : frames) {
        valid &= frame[0]==7 && frame.back()==0;
        std::string text;
        for (std::size_t i=1;i<frame.size() && frame[i];++i) {
            valid &= frame[i]>=0 && frame[i]<=255 && frame[i]==std::floor(frame[i]);
            text+=static_cast<char>(frame[i]);
        }
        valid &= !text.empty() && text.back()=='\n';
        received+=text;
    }
    require(valid && frames.size()>100,"invalid FT8 binary frames or insufficient burst coverage");
    require(received==expected,"FT8 web encoding lost/truncated records or rounded dial frequency");
    require(FT8Web::status(5357000)[0]==7 && FT8Web::status(0)[1]=='S',"missing FT8 web status");
    result.lines.clear();
    require(FT8Web::encode(result).empty(),"empty decode created a data frame");
    result.lines={"bad\nrecord"};
    try { FT8Web::encode(result); throw std::logic_error("accepted invalid record"); }
    catch(const std::runtime_error&) { require(true,"invalid record rejected"); }
    result.lines={std::string(1024,'X')};
    try { FT8Web::encode(result); throw std::logic_error("accepted oversized record"); }
    catch(const std::runtime_error&) { require(true,"oversized record rejected"); }
}

// Exercise the same 480 kHz entry point used by ClientManager, with variable
// block sizes and explicit capture times. PCM is translated to the USB dial
// frequency in the wide IQ stream. Holding each 12 kHz sample for 40 IQ samples
// produces known image frequencies, which the receive anti-alias filter removes.
void feed(FT8Receiver& receiver, const std::vector<float>& pcm, double seconds,
          int scenario = 0, bool tones = false) {
    const auto startNs = (utc-1)*1000000000LL;
    const auto total = static_cast<std::uint64_t>(seconds*rate);
    const unsigned sizes[]{257,1024,409,67,268};
    std::vector<std::complex<float>> iq;
    SDRCaptureClock captureClock;
    std::int64_t lastArrival = 0;
    unsigned delays = 0;
    for (std::uint64_t n = 0, block = 0; n < total; ++block) {
        if (scenario == 1 && n >= 4*rate && n < 4*rate+4800) n = 4*rate+4800;
        const auto count = std::min<std::uint64_t>(sizes[block%5],total-n);
        auto info = band(14000000,14350000);
        if (scenario == 2 && n >= 4*rate) {
            info = band(7000000,7200000); info.generation = 2;
        }
        info.firstSample = n;
        info.utcNanoseconds = startNs + sdrSampleTime(n,rate);
        if (scenario == 3 && n >= 4*rate) info.utcNanoseconds += 500000000LL;
        if (scenario == 4) {
            auto arrival = std::max<std::int64_t>(startNs+sdrSampleTime(n+count,rate),lastArrival+1000);
            if ((delays==0 && n>=4*rate) || (delays==1 && n>=10*rate)) {
                arrival += delays==0 ? 450000000LL : 2000000000LL;
                ++delays;
            }
            info = captureClock.observe(static_cast<std::uint32_t>(n*5),count*5,
                arrival,arrival-startNs, sdrSegment(14000000,14350000));
            info.firstSample /= 5; // Same 480 kHz entry point, raw clock runs at 2.4 MHz.
            lastArrival = arrival;
        }
        iq.resize(count);
        for (std::uint64_t j = 0; j < count; ++j) {
            const auto sample = n+j;
            const double t = double(sample%rate)/rate;
            const auto carrier = std::polar(1.0, 2*M_PI*(-166000)*t);
            if (tones) {
                const auto upper = std::polar(0.15,2*M_PI*1000*t);
                const auto lower = std::polar(0.2,-2*M_PI*1700*t);
                iq[j] = (upper+lower)*carrier;
            } else {
                const auto index = static_cast<std::int64_t>(sample/40)-12000;
                const float audio = index >= 0 && static_cast<std::size_t>(index)<pcm.size() ? pcm[index] : 0;
                iq[j] = double(audio)*carrier;
            }
        }
        receiver.process(iq.data(),count,info);
        n += count;
    }
}

// Exercise the production 2.4 MHz resampler as well as the FT8 receiver.
// Missing source samples are skipped, exactly as reported by firstSampleNum.
void feedRaw(SDRInputResampler& receiver, const std::vector<float>& pcm,
             double seconds, int scenario) {
    constexpr std::uint64_t rawRate=2400000;
    const auto startNs=(utc-1)*1000000000LL;
    const auto total=static_cast<std::uint64_t>(seconds*rawRate);
    const std::uint64_t positions[]{6*rawRate,8*rawRate,10*rawRate,12*rawRate};
    const std::uint64_t gaps[]{1408,236,396,764};
    const unsigned gapCount=scenario==1?4U:(scenario==2?1U:0U);
    std::complex<float> iq[1344];
    const auto increment=std::polar(1.0,2*M_PI*(-166000)/rawRate);
    SDRCaptureClock clock;
    unsigned gapIndex=0;
    for (std::uint64_t n=0;n<total;) {
        if (gapIndex<gapCount && n==positions[gapIndex]) {
            n+=scenario==2?24000:gaps[gapIndex];
            ++gapIndex;
        }
        auto count=std::min<std::uint64_t>(1344,total-n);
        if (gapIndex<gapCount) count=std::min(count,positions[gapIndex]-n);
        const auto info=clock.observe(static_cast<std::uint32_t>(n),count,
            startNs+sdrSampleTime(n+count,rawRate),sdrSampleTime(n+count,rawRate),
            sdrSegment(14000000,14350000));
        auto carrier=std::polar(1.0,2*M_PI*(-166000)*double(n%rawRate)/rawRate);
        for (std::uint64_t j=0;j<count;++j) {
            const auto index=static_cast<std::int64_t>((n+j)/200)-12000;
            const float audio=index>=0 && static_cast<std::size_t>(index)<pcm.size() ? pcm[index] : 0;
            iq[j]=double(audio)*carrier;
            carrier*=increment;
        }
        receiver.process(iq,count,info);
        n+=count;
    }
}

double magnitude(const std::vector<float>& audio, double frequency) {
    std::complex<double> sum{};
    for (std::size_t i=12000;i<24000;++i)
        sum += double(audio[i])*std::polar(1.0,-2*M_PI*frequency*i/12000);
    return std::abs(sum);
}
template<class F> void fails(F action, const std::string& expected) {
    try { action(); }
    catch (const std::exception& e) {
        require(std::string(e.what()).find(expected)!=std::string::npos,
                "unexpected error: "+std::string(e.what()));
        return;
    }
    throw std::runtime_error("expected failure: "+expected);
}
}

int main(int argc,char** argv) {
    signal(SIGPIPE,SIG_IGN);
    try {
        require(argc == 4,"expected PCM test signal, decoder and fake-process directory");
        testClock();
        testInputResampler();
        testBroadcastMetadata();
        testWebProtocol();
        const std::uint32_t starts[]{1800000,3500000,5300000,7000000,10100000,14000000,
            18068000,21000000,24890000,28000000,50000000,70000000,144000000,432000000};
        const std::uint32_t expected[]{1840000,3573000,5357000,7074000,10136000,14074000,
            18100000,21074000,24915000,28074000,50313000,70154000,144174000,432174000};
        for (std::size_t i=0;i<14;++i)
            require(FT8Receiver::dialFrequency(band(starts[i],starts[i]+480000))==expected[i],
                    "wrong band frequency");
        for (auto start : {400000U,26965000U,28450000U,29100000U,144500000U,
                           145000000U,145500000U,431200000U,438800000U,446000000U,1089900000U})
            require(FT8Receiver::dialFrequency(band(start,start+480000))==0,"unexpected FT8 activation");
        require(FT8Receiver::dialFrequency(band(14000000,14078000))==0,"truncated passband enabled");
        std::ifstream input(argv[1],std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),{});
        require(bytes.size()==360000,"wrong test PCM length");
        std::vector<float> pcm(180000);
        for (std::size_t i=0;i<pcm.size();++i) {
            const unsigned bits=bytes[2*i] | (unsigned(bytes[2*i+1])<<8);
            pcm[i]=float(bits>=32768 ? int(bits)-65536 : int(bits))/32768;
        }
        std::vector<FT8Slot> slots;
        FT8Receiver receiver([&](FT8Slot&& slot) { slots.push_back(std::move(slot)); });
        feed(receiver,pcm,16.2);
        require(slots.size()==1,"partial startup slot was decoded, or complete slot lost");
        require(slots[0].audio.size()==180000 && slots[0].utcSeconds==utc &&
                slots[0].dialHz==14074000,"incorrect slot sample count, UTC or dial");
        std::vector<FT8Slot> delayedSlots;
        FT8Receiver delayed([&](FT8Slot&& slot) { delayedSlots.push_back(std::move(slot)); });
        feed(delayed,pcm,16.2,4);
        require(delayedSlots.size()==1 && delayedSlots[0].utcSeconds==utc,
                "callback scheduling delays discarded a valid FT8 slot");
        require(delayedSlots[0].audio==slots[0].audio,
                "callback scheduling delays changed FT8 audio or slot boundaries");
        std::atomic<bool> cancel{false};
        const auto output=FT8DecoderProcess::decode(slots[0],argv[2],std::chrono::seconds(45),cancel);
        require(output.find("CQ K1ABC FN42")!=std::string::npos,"FT8 signal lost in complete IQ→USB→process path");
        std::istringstream lines(output); std::string line;
        bool matched=false;
        while(std::getline(lines,line)) if(line.find("CQ K1ABC FN42")!=std::string::npos) {
            std::istringstream row(line); std::string time; int snr; float dt,frequency;
            row>>time>>snr>>dt>>frequency;
            require(std::abs(frequency-1500)<2,"incorrect USB frequency translation");
            require(std::abs(dt-0.2)<0.08,"incorrect DSP delay compensation");
            matched=true;
        }
        require(matched,"missing decoded row");
        std::printf("Complete IQ→USB→f32le→FT8: %s",output.c_str());
        std::vector<FT8Slot> rawSlots, gapSlots;
        std::uint64_t rawOutputs=0, gapOutputs=0;
        FT8Receiver rawReceiver([&](FT8Slot&& s) {rawSlots.push_back(std::move(s));});
        FT8Receiver gapReceiver([&](FT8Slot&& s) {gapSlots.push_back(std::move(s));});
        SDRInputResampler rawInput([&](const void* iq,std::size_t count,const SDRStreamInfo& info) {
            rawOutputs+=count; rawReceiver.process(iq,count,info);
        });
        SDRInputResampler gapInput([&](const void* iq,std::size_t count,const SDRStreamInfo& info) {
            gapOutputs+=count; gapReceiver.process(iq,count,info);
        });
        feedRaw(rawInput,pcm,16.2,0);
        feedRaw(gapInput,pcm,16.2,1);
        require(rawSlots.size()==1 && gapSlots.size()==1 &&
                gapSlots[0].audio.size()==180000 && gapSlots[0].utcSeconds==utc &&
                gapSlots[0].dialHz==14074000,"short hardware gaps discarded or shifted FT8 slot");
        const auto gapStats=gapInput.stats();
        require(gapStats.filledGaps==4 && gapStats.filledSamples==2804 && gapStats.rejectedGaps==0,
                "reported hardware gaps were not filled exactly at 2.4 MHz");
        require(rawOutputs==gapOutputs,"hardware gap repair compressed the 480 kHz timeline");
        const auto gapOutput=FT8DecoderProcess::decode(gapSlots[0],argv[2],std::chrono::seconds(45),cancel);
        std::istringstream gapLines(gapOutput);
        matched=false;
        while(std::getline(gapLines,line)) if(line.find("CQ K1ABC FN42")!=std::string::npos) {
            std::istringstream row(line); std::string time; int snr; float dt,frequency;
            row>>time>>snr>>dt>>frequency;
            require(std::abs(frequency-1500)<2 && std::abs(dt-0.2)<0.08,
                    "gap repair changed frequency or decode timing");
            matched=true;
        }
        require(matched,"FT8 decode failed after repairing all four reported SDR gaps");
        std::printf("2.4 MHz IQ with gaps +1408/+236/+396/+764 → FT8: %s",gapOutput.c_str());
        std::vector<FT8Slot> rawRecovered;
        FT8Receiver longGapReceiver([&](FT8Slot&& s) {rawRecovered.push_back(std::move(s));});
        SDRInputResampler longGapInput([&](const void* iq,std::size_t count,const SDRStreamInfo& info) {
            longGapReceiver.process(iq,count,info);
        });
        feedRaw(longGapInput,{},31.4,2);
        require(longGapInput.stats().filledSamples==0 && longGapInput.stats().rejectedGaps==1 &&
                rawRecovered.size()==1 && rawRecovered[0].utcSeconds==utc+15 &&
                rawRecovered[0].audio.size()==180000,
                "long raw gap was repaired, damaged slot decoded or next complete slot lost");
        std::vector<FT8Slot> toneSlots;
        FT8Receiver usb([&](FT8Slot&& s) {toneSlots.push_back(std::move(s));});
        feed(usb,{},16.2,0,true);
        require(toneSlots.size()==1,"missing USB tone slot");
        const auto desired=magnitude(toneSlots[0].audio,1000);
        const auto image=magnitude(toneSlots[0].audio,1700);
        require(desired>1 && desired>30*image,"USB did not reject lower sideband");
        std::printf("USB image rejection: %.1f dB\n",20*std::log10(desired/std::max(image,1e-15)));
        for(int scenario=1;scenario<=3;++scenario) {
            std::vector<FT8Slot> recovered;
            FT8Receiver interrupted([&](FT8Slot&& s) {recovered.push_back(std::move(s));});
            feed(interrupted,{},31.4,scenario);
            require(recovered.size()==1 && recovered[0].utcSeconds==utc+15,
                    "damaged slot not discarded or next full slot not recovered");
            require(recovered[0].dialHz==(scenario==2?7074000U:14074000U),"mixed band slot");
        }
        const auto fake=std::string(argv[3])+"/";
        std::mutex resultMutex;
        std::condition_variable resultReady;
        std::vector<FT8DecodedSlot> published;
        {
            FT8DecoderProcess worker(fake+"web.py",std::chrono::seconds(3),[&](FT8DecodedSlot&& result) {
                std::lock_guard<std::mutex> lock(resultMutex);
                published.push_back(std::move(result)); resultReady.notify_one();
            });
            require(worker.enqueue(FT8Slot(slots[0])),"cannot enqueue callback test slot");
            std::unique_lock<std::mutex> lock(resultMutex);
            require(resultReady.wait_for(lock,std::chrono::seconds(4),[&]{return !published.empty();}),
                    "decoder result callback not delivered");
            require(published.size()==1 && published[0].utcSeconds==utc &&
                    published[0].dialHz==14074000 && published[0].lines.size()==2 &&
                    published[0].lines[1].find("<...>")!=std::string::npos,
                    "decoder result callback lost text or original slot metadata");
            lock.unlock();
            worker.stop();
        }
        require(FT8DecoderProcess::decode(slots[0],fake+"inspect.py",std::chrono::seconds(3),cancel)=="OK\n",
                "invalid f32le bytes or decoder arguments");
        auto duplex=FT8DecoderProcess::decode(slots[0],fake+"duplex.py",std::chrono::seconds(3),cancel);
        require(duplex.size()==131075 && duplex.substr(duplex.size()-3)=="OK\n","pipe deadlock or truncated output");
        fails([&]{FT8DecoderProcess::decode(slots[0],fake+"missing",std::chrono::seconds(1),cancel);},"No such file");
        fails([&]{FT8DecoderProcess::decode(slots[0],fake+"early.py",std::chrono::seconds(1),cancel);},"decoder failed");
        fails([&]{FT8DecoderProcess::decode(slots[0],fake+"huge.py",std::chrono::seconds(1),cancel);},"exceeds");
        fails([&]{FT8DecoderProcess::decode(slots[0],fake+"hang.py",std::chrono::milliseconds(100),cancel);},"timeout");
        cancel=true;
        fails([&]{FT8DecoderProcess::decode(slots[0],fake+"hang.py",std::chrono::seconds(1),cancel);},"cancelled");
        cancel=false;
        FT8Slot silence;silence.audio.resize(180000);silence.utcSeconds=utc;
        require(FT8DecoderProcess::decode(silence,argv[2],std::chrono::seconds(2),cancel).empty(),"silence produced a decode");
        silence.audio[0]=std::numeric_limits<float>::quiet_NaN();
        fails([&]{FT8DecoderProcess::decode(silence,argv[2],std::chrono::seconds(1),cancel);},"non-finite");
        const auto before=std::chrono::steady_clock::now();
        {
            FT8DecoderProcess worker(fake+"hang.py");
            int accepted=0;
            for(int i=0;i<12;++i) { auto slot=slots[0];accepted+=worker.enqueue(std::move(slot)); }
            require(accepted<=3,"unbounded decode queue");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            worker.stop();
        }
        require(std::chrono::steady_clock::now()-before<std::chrono::seconds(2),"worker shutdown hung");
        require(waitpid(-1,nullptr,WNOHANG)==-1 && errno==ECHILD,"decoder process not reaped");
        std::printf("%d live-receiver checks passed\n",checks);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"FT8 live test: %s\n",e.what());return 1;
    }
}

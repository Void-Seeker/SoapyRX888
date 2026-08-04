
#include "SoapyRX888.hpp"
#include <SoapySDR/Time.hpp>
#include <SoapySDR/Types.hpp>
#include <algorithm>
#include <cmath>

// AD8370 VGA step index (0..126) -> gain in dB, matching the SDDC RX888R2 host.
static double rx888_if_index_to_db(int i)
{
    if (i > 18) return 20.0 * std::log10(0.409 * (i - 18 + 3));
    return 20.0 * std::log10(0.059 * (i + 1));
}

// Nearest VGA step index for a requested gain in dB. The mapping is monotonic,
// so the search can stop as soon as the error starts growing again.
static int rx888_if_db_to_index(double db)
{
    int best = 0;
    double bestErr = std::fabs(rx888_if_index_to_db(0) - db);
    for (int i = 1; i <= 126; i++)
    {
        double err = std::fabs(rx888_if_index_to_db(i) - db);
        if (err >= bestErr) break;
        bestErr = err; best = i;
    }
    return best;
}

SoapyRX888::SoapyRX888(const SoapySDR::Kwargs &args):
    deviceId(-1),
    dev(nullptr),
    rxFormat(RX888_RX_FORMAT_INT16),
    sampleRate(64000000),
    centerFrequency(0),
    rfAtten(0),
    ifGainIndex(50),
    numBuffers(DEFAULT_NUM_BUFFERS)
    
{
    if (args.count("label") != 0) SoapySDR_logf(SOAPY_SDR_INFO, "Opening %s...", args.at("label").c_str());

    //if a serial is not present, then findRX888 had zero devices enumerated
    if (args.count("serial") == 0) throw std::runtime_error("No RX888 devices found!");

    const auto serial = args.at("serial");
    deviceId = rx888_get_index_by_serial(serial.c_str());
    if (deviceId < 0) throw std::runtime_error("rx888_get_index_by_serial("+serial+") - " + std::to_string(deviceId));
    
    SoapySDR_logf(SOAPY_SDR_DEBUG, "RX888 opening device %d", deviceId);
    if (rx888_open(&dev, deviceId) != 0) {
        throw std::runtime_error("Unable to open RX888 device");
    }

    //rx888_open() starts the ADC with whatever rate the (zero-initialized)
    //device struct holds, so push our default down before anyone reads it back.
    if (rx888_set_sample_rate(dev, sampleRate) != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "rx888_set_sample_rate(%u) failed at open", sampleRate);
    }
    sampleRate = rx888_get_sample_rate(dev);
    SoapySDR_logf(SOAPY_SDR_DEBUG, "RX888 initial sample rate: %u Hz", sampleRate);
}

SoapyRX888::~SoapyRX888(void)
{
    //stop the async thread before closing: rx888_close() spins until the async
    //status goes inactive without cancelling it itself, and a still-joinable
    //thread member would terminate() on destruction.
    this->stopAsyncThread();

    //cleanup device handles
    rx888_close(dev);
}



/*******************************************************************
 * Identification API
 ******************************************************************/

std::string SoapyRX888::getDriverKey(void) const
{
    return "RX888";
}

std::string SoapyRX888::getHardwareKey(void) const
{
    return "RX888";
}

SoapySDR::Kwargs SoapyRX888::getHardwareInfo(void) const
{
    //key/value pairs for any useful information
    //this also gets printed in --probe
    SoapySDR::Kwargs args;

    args["origin"] = "https://github.com/qoelet/SoapyRX888";
    args["index"] = std::to_string(deviceId);

    return args;
}    

/*******************************************************************
 * Channels API
 ******************************************************************/

size_t SoapyRX888::getNumChannels(const int dir) const
{
    return (dir == SOAPY_SDR_RX) ? 1 : 0;
}

bool SoapyRX888::getFullDuplex(const int direction, const size_t channel) const
{
    (void)direction;
    (void)channel;
    return false;
}

/*******************************************************************
 * Antenna API
 ******************************************************************/

std::vector<std::string> SoapyRX888::listAntennas(const int direction, const size_t channel) const
{
    (void)direction;
    (void)channel;
    std::vector<std::string> antennas;
    antennas.push_back("RX");
    return antennas;
}

void SoapyRX888::setAntenna(const int direction, const size_t channel, const std::string &name)
{
    (void)channel;
    (void)name;
    if (direction != SOAPY_SDR_RX)
    {
        throw std::runtime_error("setAntena failed: RX888 only supports RX");
    }
}

std::string SoapyRX888::getAntenna(const int direction, const size_t channel) const
{
    (void)channel;
    (void)direction;
    return "RX";
}

/*******************************************************************
 * Frontend corrections API
 ******************************************************************/

bool SoapyRX888::hasDCOffsetMode(const int direction, const size_t channel) const
{
    (void)direction;
    (void)channel;
    return false;
}

bool SoapyRX888::hasFrequencyCorrection(const int direction, const size_t channel) const
{
    (void)direction;
    (void)channel;
    return false;
}

/*******************************************************************
 * Gain API
 ******************************************************************/

std::vector<std::string> SoapyRX888::listGains(const int direction, const size_t channel) const
{
    (void)direction;
    (void)channel;
    std::vector<std::string> gains;
    gains.push_back("IF");   // AD8370 VGA — the main HF gain
    gains.push_back("RF");   // DAT-31 input attenuator
    return gains;
}

bool SoapyRX888::hasGainMode(const int direction, const size_t channel) const
{
    (void)direction;
    (void)channel;
    return false;
}

void SoapyRX888::setGain(const int direction, const size_t channel, const std::string &name, const double value)
{
    (void)direction;
    (void)channel;
    if (name == "IF")
    {
        const int index = rx888_if_db_to_index(value);
        SoapySDR_logf(SOAPY_SDR_DEBUG, "Set IF (VGA) gain: %.1f dB -> index %d", value, index);
        if (rx888_set_if_gain(dev, index) != 0)
        {
            throw std::runtime_error("setGain(IF) failed: rx888_set_if_gain()");
        }
        ifGainIndex = index;
    }
    else if (name == "RF")
    {
        const double atten = std::min(0.0, value);
        SoapySDR_logf(SOAPY_SDR_DEBUG, "Set RF attenuation: %.1f dB", atten);
        if (rx888_set_hf_attenuation(dev, atten) != 0)
        {
            throw std::runtime_error("setGain(RF) failed: rx888_set_hf_attenuation()");
        }
        rfAtten = atten;
    }
    else
    {
        //warn rather than throw: a stray element name from a host application
        //should not take down the stream, but it should not vanish silently
        SoapySDR_logf(SOAPY_SDR_WARNING, "setGain: ignoring unknown gain element '%s'", name.c_str());
    }
}

double SoapyRX888::getGain(const int direction, const size_t channel, const std::string &name) const
{
    (void)direction;
    (void)channel;
    if (name == "IF") return rx888_if_index_to_db(ifGainIndex);
    if (name == "RF") return rfAtten;
    return 0;
}

SoapySDR::Range SoapyRX888::getGainRange(const int direction, const size_t channel, const std::string &name) const
{
    (void)direction;
    (void)channel;
    //NOTE: the AD8370 splits into a low- and a high-gain range, so the step
    //size is not uniform and there is a ~3.3 dB hole between the ranges
    //(index 18 = 1.0 dB, index 19 = 4.3 dB). Requests landing in the hole snap
    //to whichever end is nearer.
    if (name == "IF")  // AD8370 VGA, 127 steps
        return SoapySDR::Range(rx888_if_index_to_db(0), rx888_if_index_to_db(126));
    if (name == "RF")  // DAT-31 attenuator, 0.5 dB steps
        return SoapySDR::Range(-31.5, 0.0, 0.5);
    return SoapySDR::Range(0, 0);
}

void SoapyRX888::setFrequency(const int direction, const size_t channel, const std::string &name, const double frequency, const SoapySDR::Kwargs &args)
{
    (void)direction;
    (void)channel;
    (void)args;
    //The RX888 HF path is a direct-sampling receiver: the LTC2208 ADC digitizes
    //the whole 0..Fs/2 band at once, with no tuner LO to retune. There is no
    //hardware frequency to set, so we just record the requested center for the
    //host (gqrx reads it back) and let the application tune digitally in-band.
    if (name == "RF" || name.empty())
    {
        centerFrequency = frequency;
        SoapySDR_logf(SOAPY_SDR_DEBUG, "Set center frequency: %f Hz (direct sampling, informational)", frequency);
    }
}

double SoapyRX888::getFrequency(const int direction, const size_t channel, const std::string &name) const
{
    (void)direction;
    (void)channel;
    if (name == "RF" || name.empty()) return centerFrequency;
    return 0;
}

std::vector<std::string> SoapyRX888::listFrequencies(const int direction, const size_t channel) const
{
    (void)direction;
    (void)channel;
    std::vector<std::string> names;
    names.push_back("RF");
    return names;
}

SoapySDR::RangeList SoapyRX888::getFrequencyRange(const int direction, const size_t channel, const std::string &name) const
{
    (void)direction;
    (void)channel;
    SoapySDR::RangeList ranges;
    //first Nyquist zone of the direct-sampling ADC: DC .. Fs/2
    if (name == "RF" || name.empty())
        ranges.push_back(SoapySDR::Range(0, sampleRate / 2.0));
    return ranges;
}

SoapySDR::ArgInfoList SoapyRX888::getFrequencyArgsInfo(const int direction, const size_t channel) const
{
    (void)direction;
    (void)channel;
    SoapySDR::ArgInfoList freqArgs;

    // TODO: frequency arguments

    return freqArgs;
}

/*******************************************************************
 * Sample Rate API
 ******************************************************************/

void SoapyRX888::setSampleRate(const int direction, const size_t channel, const double rate)
{
    (void)direction;
    (void)channel;
    //reject before narrowing: an out-of-range double -> uint32_t is undefined
    if (not (rate > 10000.0) or rate > 150000000.0)
    {
        throw std::runtime_error("setSampleRate failed: RX888 does not support "
                + std::to_string(rate) + " Hz");
    }
    const uint32_t newRate = static_cast<uint32_t>(rate);

    const long long ns = SoapySDR::ticksToTimeNs(ticks, sampleRate);
    SoapySDR_logf(SOAPY_SDR_DEBUG, "Setting sample rate: %u", newRate);

    //only commit the cached rate once the device has accepted it
    int r = rx888_set_sample_rate(dev, newRate);
    if (r == -EINVAL)
    {
        throw std::runtime_error("setSampleRate failed: RX888 does not support this sample rate");
    }
    if (r != 0)
    {
        throw std::runtime_error("setSampleRate failed");
    }
    sampleRate = rx888_get_sample_rate(dev);
    ticks = SoapySDR::timeNsToTicks(ns, sampleRate);
    resetBuffer = true;
}

double SoapyRX888::getSampleRate(const int direction, const size_t channel) const
{
    (void)direction;
    (void)channel;
    return sampleRate;
}

std::vector<double> SoapyRX888::listSampleRates(const int direction, const size_t channel) const
{
    (void)direction;
    (void)channel;
    std::vector<double> results;

    results.push_back(250000);
    results.push_back(500000);
    results.push_back(1000000);
    results.push_back(2000000);
    results.push_back(4000000);
    results.push_back(8000000);
    results.push_back(16000000);
    results.push_back(32000000);
    results.push_back(64000000);
    results.push_back(128000000);
    results.push_back(150000000);

    return results;
}

/*******************************************************************
 * Time API
 ******************************************************************/

std::vector<std::string> SoapyRX888::listTimeSources(void) const
{
    std::vector<std::string> results;

    results.push_back("sw_ticks");

    return results;
}

std::string SoapyRX888::getTimeSource(void) const
{
    return "sw_ticks";
}

bool SoapyRX888::hasHardwareTime(const std::string &what) const
{
    return what == "" || what == "sw_ticks";
}

long long SoapyRX888::getHardwareTime(const std::string &what) const
{
    (void)what;
    return SoapySDR::ticksToTimeNs(ticks, sampleRate);
}

void SoapyRX888::setHardwareTime(const long long timeNs, const std::string &what)
{
    (void)what;
    ticks = SoapySDR::timeNsToTicks(timeNs, sampleRate);
}

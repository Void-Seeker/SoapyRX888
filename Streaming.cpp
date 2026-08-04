#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Formats.hpp>

#include <SoapySDR/Time.hpp>
#include <chrono>
#include <cstring>
#include "SoapyRX888.hpp"



/*******************************************************************
 * Stream API
 ******************************************************************/

std::vector<std::string> SoapyRX888::getStreamFormats(const int direction, const size_t channel) const {
    (void)channel; //unused
    (void)direction; //unused
    std::vector<std::string> formats;

    formats.push_back(SOAPY_SDR_CF32);
    formats.push_back(SOAPY_SDR_S16);

    return formats;
}

std::string SoapyRX888::getNativeStreamFormat(const int direction, const size_t channel, double &fullScale) const {
    (void)channel; //unused
    //check that direction is SOAPY_SDR_RX
     if (direction != SOAPY_SDR_RX) {
         throw std::runtime_error("RX888 is RX only, use SOAPY_SDR_RX");
     }

     fullScale = 32768;
     return SOAPY_SDR_S16;
}

SoapySDR::ArgInfoList SoapyRX888::getStreamArgsInfo(const int direction, const size_t channel) const {
    (void)channel; //unused
    //check that direction is SOAPY_SDR_RX
     if (direction != SOAPY_SDR_RX) {
         throw std::runtime_error("RX888 is RX only, use SOAPY_SDR_RX");
     }

    SoapySDR::ArgInfoList streamArgs;

    SoapySDR::ArgInfo bufflenArg;
    bufflenArg.key = "bufflen";
    bufflenArg.value = std::to_string(DEFAULT_BUFFER_LENGTH);
    bufflenArg.name = "Buffer Size";
    bufflenArg.description = "Number of bytes per buffer, multiples of 512 only.";
    bufflenArg.units = "bytes";
    bufflenArg.type = SoapySDR::ArgInfo::INT;

    streamArgs.push_back(bufflenArg);

    SoapySDR::ArgInfo buffersArg;
    buffersArg.key = "buffers";
    buffersArg.value = std::to_string(DEFAULT_NUM_BUFFERS);
    buffersArg.name = "Ring buffers";
    buffersArg.description = "Number of buffers in the ring.";
    buffersArg.units = "buffers";
    buffersArg.type = SoapySDR::ArgInfo::INT;

    streamArgs.push_back(buffersArg);

    SoapySDR::ArgInfo asyncbuffsArg;
    asyncbuffsArg.key = "asyncBuffs";
    asyncbuffsArg.value = "0";
    asyncbuffsArg.name = "Async buffers";
    asyncbuffsArg.description = "Number of async usb buffers (advanced).";
    asyncbuffsArg.units = "buffers";
    asyncbuffsArg.type = SoapySDR::ArgInfo::INT;

    streamArgs.push_back(asyncbuffsArg);

    return streamArgs;
}

/*******************************************************************
 * Async thread work
 ******************************************************************/

static void _rx_callback(unsigned char *buf, uint32_t len, void *ctx)
{
    //printf("_rx_callback\n");
    SoapyRX888 *self = static_cast<SoapyRX888*>(ctx);
    self->rx_callback(buf, len);
}

void SoapyRX888::rx_async_operation(void)
{
    //printf("rx_async_operation\n");
    //this is the thread entry point: report errors, never throw out of here
    const int ret = rx888_read_async(dev, &_rx_callback, this,
            static_cast<uint32_t>(asyncBuffs), static_cast<uint32_t>(bufferLength));
    if (ret != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "rx888_read_async() returned %d, streaming stopped", ret);
    }
    _asyncActive = false;
    //printf("rx_async_operation done!\n");
}

void SoapyRX888::stopAsyncThread(void)
{
    if (not _rx_async_thread.joinable()) return;

    //rx888_cancel_async() is a no-op (-2) until the worker has reached the
    //RX888_RUNNING state inside rx888_read_async(), so a single cancel issued
    //during thread startup would be lost and join() would block forever.
    //Retry until it takes, or until the worker has exited on its own.
    while (_asyncActive and rx888_cancel_async(dev) != 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    _rx_async_thread.join();
}

void SoapyRX888::rx_callback(unsigned char *buf, uint32_t len)
{
    //printf("_rx_callback %d _buf_head=%d, numBuffers=%d\n", len, _buf_head, _buf_tail);

    // atomically add len to ticks but return the previous value
    unsigned long long tick = ticks.fetch_add(len);

    //overflow condition: the caller is not reading fast enough
    if (_buf_count == numBuffers)
    {
        _overflowEvent = true;
        return;
    }

    //copy into the buffer queue
    auto &buff = _buffs[_buf_tail];
    buff.tick = tick;
    buff.data.resize(len);
    std::memcpy(buff.data.data(), buf, len);

    //increment the tail pointer
    _buf_tail = (_buf_tail + 1) % numBuffers;

    //increment buffers available under lock
    //to avoid race in acquireReadBuffer wait
    {
    std::lock_guard<std::mutex> lock(_buf_mutex);
    _buf_count++;

    }

    //notify readStream()
    _buf_cond.notify_one();
}

/*******************************************************************
 * Stream API
 ******************************************************************/

//Parse a positive integer stream argument, leaving the default in place
//(with a warning) when the value is missing, malformed or out of range.
static void parseSizeArg(const SoapySDR::Kwargs &args, const std::string &key, size_t &value)
{
    if (args.count(key) == 0) return;
    const std::string &raw = args.at(key);
    try
    {
        const int parsed = std::stoi(raw);
        if (parsed > 0)
        {
            value = static_cast<size_t>(parsed);
            return;
        }
        SoapySDR_logf(SOAPY_SDR_WARNING, "RX888 ignoring non-positive %s='%s'", key.c_str(), raw.c_str());
    }
    catch (const std::invalid_argument &)
    {
        SoapySDR_logf(SOAPY_SDR_WARNING, "RX888 ignoring malformed %s='%s'", key.c_str(), raw.c_str());
    }
    catch (const std::out_of_range &)
    {
        SoapySDR_logf(SOAPY_SDR_WARNING, "RX888 ignoring out-of-range %s='%s'", key.c_str(), raw.c_str());
    }
}

SoapySDR::Stream *SoapyRX888::setupStream(
        const int direction,
        const std::string &format,
        const std::vector<size_t> &channels,
        const SoapySDR::Kwargs &args)
{
    if (direction != SOAPY_SDR_RX)
    {
        throw std::runtime_error("RX888 is RX only, use SOAPY_SDR_RX");
    }

    //check the channel configuration
    if (channels.size() > 1 or (channels.size() > 0 and channels.at(0) != 0))
    {
        throw std::runtime_error("setupStream invalid channel selection");
    }

    //check the format
    if (format == SOAPY_SDR_S16)
    {
        SoapySDR_log(SOAPY_SDR_INFO, "Using format S16.");
        rxFormat = RX888_RX_FORMAT_INT16;
    }
    else if (format == SOAPY_SDR_CF32)
    {
        SoapySDR_log(SOAPY_SDR_INFO, "Using format CF32 with imaginary component set to zero.");
        rxFormat = RX888_RX_FORMAT_FLOAT32;
    }
    else
    {
        throw std::runtime_error(
                "setupStream invalid format '" + format
                        + "' -- Only S16 and CF32 with imag=0 are supported by SoapyRX888 module.");
    }

    bufferLength = DEFAULT_BUFFER_LENGTH;
    parseSizeArg(args, "bufflen", bufferLength);
    //the USB transfer length must be a multiple of 512. librx888 silently
    //falls back to its own default when it is not, which would leave our ring
    //buffers sized for one length while the callbacks deliver another --
    //reject it here instead of streaming with a mismatched MTU.
    if (bufferLength % 512 != 0)
    {
        throw std::runtime_error("setupStream: bufflen must be a multiple of 512, got "
                + std::to_string(bufferLength));
    }
    SoapySDR_logf(SOAPY_SDR_DEBUG, "RX888 Using buffer length %zu", bufferLength);

    numBuffers = DEFAULT_NUM_BUFFERS;
    parseSizeArg(args, "buffers", numBuffers);
    SoapySDR_logf(SOAPY_SDR_DEBUG, "RX888 Using %zu buffers", numBuffers);

    asyncBuffs = 0;
    parseSizeArg(args, "asyncBuffs", asyncBuffs);

    //clear async fifo counts
    _buf_tail = 0;
    _buf_count = 0;
    _buf_head = 0;

    //allocate buffers
    _buffs.resize(numBuffers);
    for (auto &buff : _buffs) buff.data.resize(bufferLength);

    return (SoapySDR::Stream *) this;
}

void SoapyRX888::closeStream(SoapySDR::Stream *stream)
{
    this->deactivateStream(stream, 0, 0);
    _buffs.clear();
}

size_t SoapyRX888::getStreamMTU(SoapySDR::Stream *stream) const
{
    (void) stream; //unused
    return bufferLength / BYTES_PER_SAMPLE;
}

int SoapyRX888::activateStream(
        SoapySDR::Stream *stream,
        const int flags,
        const long long timeNs,
        const size_t numElems)
{
    (void) stream; //unused
    (void) timeNs; //unused
    (void) numElems; //unused
    if (flags != 0) return SOAPY_SDR_NOT_SUPPORTED;
    resetBuffer = true;
    bufferedElems = 0;

    //start the async thread
    if (not _rx_async_thread.joinable())
    {
        //rx888_reset_buffer(dev);
        //flag before spawning so a cancel racing startup cannot be dropped
        _asyncActive = true;
        _rx_async_thread = std::thread(&SoapyRX888::rx_async_operation, this);
    }

    return 0;
}

int SoapyRX888::deactivateStream(SoapySDR::Stream *stream, const int flags, const long long timeNs)
{
    (void) timeNs; //unused
    (void) stream; //unused
    if (flags != 0) return SOAPY_SDR_NOT_SUPPORTED;
    this->stopAsyncThread();
    return 0;
}

int SoapyRX888::readStream(
        SoapySDR::Stream *stream,
        void * const *buffs,
        const size_t numElems,
        int &flags,
        long long &timeNs,
        const long timeoutUs)
{
    //drop remainder buffer on reset
    if (resetBuffer and bufferedElems != 0)
    {
        bufferedElems = 0;
        this->releaseReadBuffer(stream, _currentHandle);
    }

    //this is the user's buffer for channel 0
    void *buff0 = buffs[0];

    //are elements left in the buffer? if not, do a new read.
    if (bufferedElems == 0)
    {
        int ret = this->acquireReadBuffer(stream, _currentHandle, (const void **)&_currentBuff, flags, timeNs, timeoutUs);
        if (ret < 0) return ret;
        bufferedElems = ret;
    }

    //otherwise just update return time to the current tick count
    else
    {
        flags |= SOAPY_SDR_HAS_TIME;
        timeNs = SoapySDR::ticksToTimeNs(bufTicks, sampleRate);
    }

    size_t returnedElems = std::min(bufferedElems, numElems);

    //convert into user's buff0
    //NOTE: the samples are read out with memcpy rather than through an
    //int16_t* cast -- the ring buffer is a char array, so punning it violates
    //strict aliasing (this builds at -O3) and misaligns loads on ARM.
    if (rxFormat == RX888_RX_FORMAT_INT16)
    {
        //native format: a straight copy of the ADC samples
        std::memcpy(buff0, _currentBuff, returnedElems * BYTES_PER_SAMPLE);
    }
    else if (rxFormat == RX888_RX_FORMAT_FLOAT32)
    {
        float *ftarget = reinterpret_cast<float*>(buff0);
        for (size_t i = 0; i < returnedElems; i++)
        {
        int16_t val;
        std::memcpy(&val, _currentBuff + BYTES_PER_SAMPLE * i, sizeof(val));
        ftarget[i * 2] = float(val) / 32768.0f;   // scale int16_t to [-1, 1] range.
        ftarget[i * 2 + 1] = 0.0f; // imaginary part is zero
        }
    }
    //bump variables for next call into readStream
    bufferedElems -= returnedElems;
    _currentBuff += returnedElems * BYTES_PER_SAMPLE;
    bufTicks += returnedElems; //for the next call to readStream if there is a remainder

    //return number of elements written to buff0
    if (bufferedElems != 0) flags |= SOAPY_SDR_MORE_FRAGMENTS;
    else this->releaseReadBuffer(stream, _currentHandle);
    return returnedElems;
}

/*******************************************************************
 * Direct buffer access API
 ******************************************************************/

size_t SoapyRX888::getNumDirectAccessBuffers(SoapySDR::Stream *stream)
{
    (void)stream; //unused
    return _buffs.size();
}

int SoapyRX888::getDirectAccessBufferAddrs(SoapySDR::Stream *stream, const size_t handle, void **buffs)
{
    (void)stream; //unused
    buffs[0] = static_cast<void*>(_buffs[handle].data.data());
    return 0;
}

int SoapyRX888::acquireReadBuffer(
    SoapySDR::Stream *stream,
    size_t &handle,
    const void **buffs,
    int &flags,
    long long &timeNs,
    const long timeoutUs)
{
    (void)stream; //unused
    //reset is issued by various settings
    //to drain old data out of the queue
    if (resetBuffer)
    {
        //drain all buffers from the fifo
        _buf_head = (_buf_head + _buf_count.exchange(0)) % numBuffers;
        resetBuffer = false;
        _overflowEvent = false;
    }

    //handle overflow from the rx callback thread
    if (_overflowEvent)
    {
        //drain the old buffers from the fifo
        _buf_head = (_buf_head + _buf_count.exchange(0)) % numBuffers;
        _overflowEvent = false;
        SoapySDR::log(SOAPY_SDR_SSI, "O");
        return SOAPY_SDR_OVERFLOW;
    }

    //wait for a buffer to become available
    if (_buf_count == 0)
    {
        std::unique_lock <std::mutex> lock(_buf_mutex);
        _buf_cond.wait_for(lock, std::chrono::microseconds(timeoutUs), [this]{return _buf_count != 0;});
        if (_buf_count == 0) return SOAPY_SDR_TIMEOUT;
    }

    //extract handle and buffer
    handle = _buf_head;
    _buf_head = (_buf_head + 1) % numBuffers;
    bufTicks = _buffs[handle].tick;
    timeNs = SoapySDR::ticksToTimeNs(_buffs[handle].tick, sampleRate);
    buffs[0] = (void *)_buffs[handle].data.data();
    flags |= SOAPY_SDR_HAS_TIME;

    //return number available
    return _buffs[handle].data.size() / BYTES_PER_SAMPLE;
}

void SoapyRX888::releaseReadBuffer(
    SoapySDR::Stream *stream,
    const size_t handle)
{
    (void)stream; //unused
    (void)handle; //unused
    //TODO this wont handle out of order releases
    //Guard against releasing a handle whose buffer was already dropped by a
    //fifo drain (reset or overflow); an unconditional decrement would wrap the
    //unsigned count and permanently defeat the overflow check in rx_callback.
    //Only this thread drains or decrements, so the read-modify-write is safe.
    if (_buf_count > 0) _buf_count--;
}
